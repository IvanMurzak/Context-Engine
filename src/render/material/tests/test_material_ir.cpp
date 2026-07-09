// IR round-trip over the REAL authored corpus (corpus/*.shader) + a programmatic canonicalization
// round-trip + the malformed-document failure family. Backend-free (R-REND-005; issue #121).

#include "context/render/material/material_ir.h"

#include "material_test.h"

#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace context::render::material;

namespace
{

std::optional<std::string> read_file(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Parse each authored corpus shader, then assert the canonicalization + hashing invariants.
void test_corpus_round_trip()
{
    const std::string dir = CONTEXT_MATERIAL_CORPUS_DIR;
    const std::vector<std::string> names = {"unlit_color.shader", "lit_pbr.shader",
                                            "postprocess_blit.shader"};

    for (const std::string& name : names)
    {
        const std::optional<std::string> text = read_file(dir + "/" + name);
        CHECK(text.has_value());
        if (!text.has_value())
        {
            continue;
        }

        const std::optional<ShaderIr> ir = parse_shader(*text);
        CHECK(ir.has_value());
        if (!ir.has_value())
        {
            continue;
        }

        // A real authored shader has a name and at least one stage.
        CHECK(!ir->name.empty());
        CHECK(!ir->stages.empty());

        // serialize() is canonicalizing and idempotent: parse -> serialize -> parse -> serialize is a
        // fixed point, and the content hash is stable across the round-trip.
        const std::string s1 = serialize_shader(*ir);
        const std::optional<ShaderIr> ir2 = parse_shader(s1);
        CHECK(ir2.has_value());
        if (!ir2.has_value())
        {
            continue;
        }
        const std::string s2 = serialize_shader(*ir2);
        CHECK(s1 == s2);
        CHECK(*ir == *ir2); // corpus keywords are authored already-sorted, so equality is exact
        CHECK(ir_content_hash(*ir) == ir_content_hash(*ir2));

        // The verbatim authored source body survives the round-trip (the "#version 450" first line and
        // the in-body "#ifdef" lines are preserved, not stripped as comments).
        CHECK(ir->stages.front().source.find("#version 450") != std::string::npos);
    }
}

// A specific corpus shape check: unlit_color has exactly the two boolean keyword axes and two stages.
void test_unlit_color_shape()
{
    const std::string dir = CONTEXT_MATERIAL_CORPUS_DIR;
    const std::optional<std::string> text = read_file(dir + "/unlit_color.shader");
    CHECK(text.has_value());
    if (!text.has_value())
    {
        return;
    }
    const std::optional<ShaderIr> ir = parse_shader(*text);
    CHECK(ir.has_value());
    if (!ir.has_value())
    {
        return;
    }
    CHECK(ir->name == "unlit_color");
    CHECK(ir->keywords.size() == 2);
    CHECK(ir->stages.size() == 2);
    CHECK(ir->stages[0].kind == ShaderStageKind::Vertex);
    CHECK(ir->stages[0].entry_point == "vs_main");
    CHECK(ir->stages[1].kind == ShaderStageKind::Fragment);
}

// A programmatic round-trip with DELIBERATELY unsorted keywords proves serialize() canonicalizes
// (sorts) the keyword order.
void test_canonicalization_sorts_keywords()
{
    ShaderIr ir;
    ir.name = "probe";
    ir.keywords.push_back({"ZEBRA", {"off", "on"}});
    ir.keywords.push_back({"ALPHA", {"a", "b", "c"}});
    ShaderStage vs;
    vs.kind = ShaderStageKind::Vertex;
    vs.entry_point = "main";
    vs.source = "void main() {}";
    ir.stages.push_back(vs);

    const std::string s = serialize_shader(ir);
    // ALPHA must be emitted before ZEBRA regardless of authored order.
    CHECK(s.find("keyword ALPHA") < s.find("keyword ZEBRA"));

    const std::optional<ShaderIr> reparsed = parse_shader(s);
    CHECK(reparsed.has_value());
    if (!reparsed.has_value())
    {
        return;
    }
    // The reparsed IR carries the sorted order; serialize() is a fixed point from here on.
    CHECK(reparsed->keywords.front().name == "ALPHA");
    CHECK(serialize_shader(*reparsed) == s);
    // The two IRs differ only in authored keyword ORDER, so their CANONICAL hashes match.
    CHECK(ir_content_hash(ir) == ir_content_hash(*reparsed));
}

// Empty-source stage round-trips (serialize omits the body line; parse recovers an empty source).
void test_empty_source_stage_round_trip()
{
    ShaderIr ir;
    ir.name = "empty_body";
    ShaderStage cs;
    cs.kind = ShaderStageKind::Compute;
    cs.entry_point = "cs";
    cs.source = "";
    ir.stages.push_back(cs);

    const std::string s = serialize_shader(ir);
    const std::optional<ShaderIr> ir2 = parse_shader(s);
    CHECK(ir2.has_value());
    if (ir2.has_value())
    {
        CHECK(ir == *ir2);
        CHECK(ir2->stages.front().source.empty());
    }
}

// Failure family: malformed documents parse to nullopt.
void test_parse_failures()
{
    CHECK(!parse_shader("").has_value());               // no shader name
    CHECK(!parse_shader("keyword FOO off on").has_value()); // no shader name
    CHECK(!parse_shader("shader ").has_value());        // 'shader' directive with no name token
    CHECK(!parse_shader("shader a\nbogus directive").has_value()); // unknown directive
    CHECK(!parse_shader("shader a\nkeyword K").has_value());       // keyword with no value
    CHECK(!parse_shader("shader a\nstage vertex main\n<body>").has_value()); // stage w/o endstage
    CHECK(!parse_shader("shader a\nstage bogus main\nendstage").has_value()); // bad stage kind
    CHECK(!parse_shader("shader a\nstage vertex\nendstage").has_value());     // stage missing entry

    // Canonical-key hygiene: a ';' or '=' in a shader/keyword name or a keyword value is rejected —
    // otherwise two distinct variants could collide on one VariantKey::canonical() cache key.
    CHECK(!parse_shader("shader a;b\nstage vertex m\nx\nendstage").has_value());   // ';' in shader name
    CHECK(!parse_shader("shader a\nkeyword K;X on off").has_value());              // ';' in keyword name
    CHECK(!parse_shader("shader a\nkeyword K a=b").has_value());                   // '=' in a value
    CHECK(!parse_shader("shader a\nkeyword K a;b").has_value());                   // ';' in a value
    // Duplicate keyword name / duplicate value within a keyword are rejected (each would enumerate a
    // variant with a non-unique canonical key).
    CHECK(!parse_shader("shader a\nkeyword FOG off on\nkeyword FOG a b").has_value()); // dup keyword name
    CHECK(!parse_shader("shader a\nkeyword FOG on on").has_value());                   // dup value

    // Sanity: the minimal well-formed document DOES parse.
    const std::optional<ShaderIr> ok = parse_shader("shader a\nstage vertex main\nx\nendstage");
    CHECK(ok.has_value());
    if (ok.has_value())
    {
        CHECK(ok->name == "a");
        CHECK(ok->stages.size() == 1);
        CHECK(ok->stages.front().source == "x");
    }
}

} // namespace

int main()
{
    test_corpus_round_trip();
    test_unlit_color_shape();
    test_canonicalization_sorts_keywords();
    test_empty_source_stage_round_trip();
    test_parse_failures();
    MATERIAL_TEST_MAIN_END();
}
