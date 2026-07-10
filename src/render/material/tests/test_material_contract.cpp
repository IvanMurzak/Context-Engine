// The M4 material contract (R-REND-004 metallic-roughness parameter surface + R-REND-006 lightmap
// INPUT hooks): param/texture directive parsing, canonical serialization round-trip, the malformed
// family, hash compatibility for contract-free documents, and the authored lit_pbr corpus contract
// (the lightmap slot on the reserved UV2 channel).

#include "context/render/material/material_ir.h"

#include <fstream>
#include <optional>
#include <sstream>
#include <string>

#include "material_test.h"

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

const char* kContractDoc = "shader probe\n"
                           "keyword FOG off on\n"
                           "param base_color vec4 1.0 1.0 1.0 1.0\n"
                           "param metallic float 0.0\n"
                           "param roughness float 0.5\n"
                           "texture albedo_tex base_color uv0\n"
                           "texture lightmap_tex lightmap uv1\n"
                           "stage vertex vs_main\n"
                           "void vs_main() {}\n"
                           "endstage\n";

void test_contract_parses()
{
    const std::optional<ShaderIr> ir = parse_shader(kContractDoc);
    CHECK(ir.has_value());
    if (!ir.has_value())
    {
        return;
    }
    CHECK(ir->params.size() == 3u);
    CHECK(ir->textures.size() == 2u);

    CHECK(ir->params[0].name == "base_color");
    CHECK(ir->params[0].type == MaterialParamType::Vec4);
    CHECK(ir->params[0].defaults.size() == 4u);
    CHECK(ir->params[0].defaults[0] == "1.0");
    CHECK(ir->params[1].name == "metallic");
    CHECK(ir->params[1].type == MaterialParamType::Float);
    CHECK(ir->params[1].defaults.size() == 1u);

    CHECK(ir->textures[0].name == "albedo_tex");
    CHECK(ir->textures[0].semantic == TextureSemantic::BaseColor);
    CHECK(ir->textures[0].uv_channel == 0u);

    // The R-REND-006 lightmap INPUT hook: the lightmap slot + its UV2-channel selection.
    CHECK(ir->textures[1].name == "lightmap_tex");
    CHECK(ir->textures[1].semantic == TextureSemantic::Lightmap);
    CHECK(ir->textures[1].uv_channel == 1u);
}

void test_contract_round_trips_canonically()
{
    const std::optional<ShaderIr> ir = parse_shader(kContractDoc);
    CHECK(ir.has_value());
    if (!ir.has_value())
    {
        return;
    }
    const std::string s1 = serialize_shader(*ir);
    const std::optional<ShaderIr> ir2 = parse_shader(s1);
    CHECK(ir2.has_value());
    if (!ir2.has_value())
    {
        return;
    }
    CHECK(*ir == *ir2);
    CHECK(serialize_shader(*ir2) == s1);
    CHECK(ir_content_hash(*ir) == ir_content_hash(*ir2));

    // Canonical form always spells the uv channel explicitly (an omitted channel is uv0).
    const std::optional<ShaderIr> defaulted =
        parse_shader("shader p\ntexture t base_color\nstage vertex v\nendstage\n");
    CHECK(defaulted.has_value());
    if (defaulted.has_value())
    {
        CHECK(defaulted->textures[0].uv_channel == 0u);
        CHECK(serialize_shader(*defaulted).find("texture t base_color uv0\n") !=
              std::string::npos);
    }
}

void test_contract_canonicalization_sorts_by_name()
{
    // Authored out of order, params and textures serialize name-sorted (like keyword axes).
    const std::optional<ShaderIr> ir =
        parse_shader("shader p\n"
                     "param zeta float 1.0\n"
                     "param alpha float 0.0\n"
                     "texture z_tex normal uv0\n"
                     "texture a_tex base_color uv0\n"
                     "stage vertex v\nendstage\n");
    CHECK(ir.has_value());
    if (!ir.has_value())
    {
        return;
    }
    const std::string s = serialize_shader(*ir);
    CHECK(s.find("param alpha") < s.find("param zeta"));
    CHECK(s.find("texture a_tex") < s.find("texture z_tex"));
    // The canonical form is a parse/serialize fixed point.
    const std::optional<ShaderIr> again = parse_shader(s);
    CHECK(again.has_value());
    if (again.has_value())
    {
        CHECK(serialize_shader(*again) == s);
    }
}

void test_contract_free_serialization_unchanged()
{
    // A document with NO contract serializes byte-identically to the pre-contract format — so
    // existing content hashes / R-FILE-010 cache keys are unchanged by the contract's existence.
    const std::optional<ShaderIr> ir =
        parse_shader("shader p\nkeyword FOG off on\nstage vertex v\nbody\nendstage\n");
    CHECK(ir.has_value());
    if (!ir.has_value())
    {
        return;
    }
    CHECK(ir->params.empty());
    CHECK(ir->textures.empty());
    const std::string expected = "shader p\n"
                                 "keyword FOG off on\n"
                                 "stage vertex v\n"
                                 "body\n"
                                 "endstage\n";
    CHECK(serialize_shader(*ir) == expected);
}

void test_malformed_contract_documents()
{
    const char* kBad[] = {
        "shader p\nparam\nstage vertex v\nendstage\n",                       // no name/type
        "shader p\nparam x\nstage vertex v\nendstage\n",                     // no type
        "shader p\nparam x mat4 1.0\nstage vertex v\nendstage\n",            // unknown type
        "shader p\nparam x float\nstage vertex v\nendstage\n",               // missing default
        "shader p\nparam x float 1.0 2.0\nstage vertex v\nendstage\n",       // arity too high
        "shader p\nparam x vec3 1.0 2.0\nstage vertex v\nendstage\n",        // arity too low
        "shader p\nparam x float abc\nstage vertex v\nendstage\n",           // not a float literal
        "shader p\nparam x float .5\nstage vertex v\nendstage\n",            // no integer digit
        "shader p\nparam x float 1.\nstage vertex v\nendstage\n",            // trailing dot
        "shader p\nparam x float 1.0e\nstage vertex v\nendstage\n",          // empty exponent
        "shader p\nparam a=b float 1.0\nstage vertex v\nendstage\n",         // key delimiter
        "shader p\nparam x float 1.0\nparam x float 2.0\nstage vertex v\nendstage\n", // dup param
        "shader p\ntexture\nstage vertex v\nendstage\n",                     // no name
        "shader p\ntexture t\nstage vertex v\nendstage\n",                   // no semantic
        "shader p\ntexture t glow uv0\nstage vertex v\nendstage\n",          // unknown semantic
        "shader p\ntexture t lightmap uv9\nstage vertex v\nendstage\n",      // channel out of range
        "shader p\ntexture t lightmap uvx\nstage vertex v\nendstage\n",      // malformed channel
        "shader p\ntexture t lightmap uv1 extra\nstage vertex v\nendstage\n", // trailing token
        "shader p\ntexture t;u lightmap\nstage vertex v\nendstage\n",        // key delimiter
        "shader p\ntexture t normal\ntexture t lightmap\nstage vertex v\nendstage\n", // dup slot
    };
    for (const char* doc : kBad)
    {
        CHECK(!parse_shader(doc).has_value());
    }

    // Exponent forms and signs ARE valid float literals.
    CHECK(parse_shader("shader p\nparam x float -1.5e-3\nstage vertex v\nendstage\n").has_value());
    CHECK(parse_shader("shader p\nparam x float +2\nstage vertex v\nendstage\n").has_value());
}

void test_lit_pbr_corpus_carries_the_contract()
{
    // AC3: the REAL authored corpus material contract carries the lightmap inputs.
    const std::string dir = CONTEXT_MATERIAL_CORPUS_DIR;
    const std::optional<std::string> text = read_file(dir + "/lit_pbr.shader");
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

    // The metallic-roughness parameter surface (R-REND-004).
    CHECK(ir->params.size() == 5u);
    bool has_metallic = false;
    bool has_roughness = false;
    for (const MaterialParam& p : ir->params)
    {
        if (p.name == "metallic" && p.type == MaterialParamType::Float)
        {
            has_metallic = true;
        }
        if (p.name == "roughness" && p.type == MaterialParamType::Float)
        {
            has_roughness = true;
        }
    }
    CHECK(has_metallic && has_roughness);

    // The R-REND-006 lightmap INPUT hook: slot present, on the reserved UV2 channel (uv1).
    const TextureSlot* lightmap = nullptr;
    for (const TextureSlot& t : ir->textures)
    {
        if (t.semantic == TextureSemantic::Lightmap)
        {
            lightmap = &t;
        }
    }
    CHECK(lightmap != nullptr);
    if (lightmap != nullptr)
    {
        CHECK(lightmap->name == "lightmap_tex");
        CHECK(lightmap->uv_channel == 1u);
    }

    // The contract survives the canonical round-trip byte-for-byte (the corpus is authored in
    // canonical order), so its content hash — the R-FILE-010 cache-key component — is stable.
    const std::string s1 = serialize_shader(*ir);
    const std::optional<ShaderIr> ir2 = parse_shader(s1);
    CHECK(ir2.has_value());
    if (ir2.has_value())
    {
        CHECK(*ir == *ir2);
        CHECK(serialize_shader(*ir2) == s1);
    }

    // The variant space is untouched by the contract (still the 12 keyword permutations).
    CHECK(enumerate_variants(*ir).size() == 12u);
}

} // namespace

int main()
{
    test_contract_parses();
    test_contract_round_trips_canonically();
    test_contract_canonicalization_sorts_by_name();
    test_contract_free_serialization_unchanged();
    test_malformed_contract_documents();
    test_lit_pbr_corpus_carries_the_contract();
    MATERIAL_TEST_MAIN_END();
}
