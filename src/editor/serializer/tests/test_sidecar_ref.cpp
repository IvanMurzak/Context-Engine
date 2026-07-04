// Sidecar-reference extraction tests (L-33, R-FILE-001): the strict decimal hash parse, the
// well-formed / malformed classification, leaf semantics, JSON-pointer locations, and nesting
// (R-QA-013 happy + edge + failure paths).

#include "context/editor/serializer/sidecar_ref.h"

#include "context/editor/serializer/json_parse.h"

#include "serializer_test.h"

#include <string>

using context::editor::serializer::collect_sidecar_refs;
using context::editor::serializer::Diagnostic;
using context::editor::serializer::is_sidecar_ref;
using context::editor::serializer::JsonValue;
using context::editor::serializer::parse_hash_string;
using context::editor::serializer::parse_json;
using context::editor::serializer::SidecarRef;

namespace
{

JsonValue parsed(const std::string& source)
{
    const auto result = parse_json(source);
    CHECK(result.ok);
    return result.root;
}

} // namespace

int main()
{
    // --- parse_hash_string: the strict canonical-decimal u64 parse ------------------------------
    CHECK(parse_hash_string("0") == 0ULL);
    CHECK(parse_hash_string("42") == 42ULL);
    // u64 max is representable in full (a JSON number could not round-trip it).
    CHECK(parse_hash_string("18446744073709551615") == 18446744073709551615ULL);
    CHECK(!parse_hash_string(""));                     // empty
    CHECK(!parse_hash_string("01"));                   // leading zero — not canonical
    CHECK(!parse_hash_string("-1"));                   // sign
    CHECK(!parse_hash_string("+1"));                   // sign
    CHECK(!parse_hash_string("12a"));                  // non-digit
    CHECK(!parse_hash_string("0x10"));                 // hex is not the encoding
    CHECK(!parse_hash_string("18446744073709551616")); // u64 max + 1 — overflow
    CHECK(!parse_hash_string("99999999999999999999")); // far overflow

    // --- is_sidecar_ref: shape classification ---------------------------------------------------
    CHECK(is_sidecar_ref(parsed(R"({"$sidecar": "tiles.bin", "hash": "123"})")));
    // Extra members are tolerated (forward-compatible with schema-blessed extensions).
    CHECK(is_sidecar_ref(parsed(R"({"$sidecar": "a.bin", "hash": "1", "note": "x"})")));
    CHECK(!is_sidecar_ref(parsed(R"({"$sidecar": "a.bin"})")));            // hash missing
    CHECK(!is_sidecar_ref(parsed(R"({"$sidecar": "", "hash": "1"})")));    // empty relpath
    CHECK(!is_sidecar_ref(parsed(R"({"$sidecar": 5, "hash": "1"})")));     // relpath not a string
    CHECK(!is_sidecar_ref(parsed(R"({"$sidecar": "a", "hash": 1})")));     // hash not a string
    CHECK(!is_sidecar_ref(parsed(R"({"$sidecar": "a", "hash": "01"})")));  // non-canonical decimal
    CHECK(!is_sidecar_ref(parsed(R"({"sidecar": "a", "hash": "1"})")));    // no "$sidecar" member
    CHECK(!is_sidecar_ref(parsed(R"(["$sidecar"])")));                     // not an object

    // --- collect: happy path, authored order, JSON pointers -------------------------------------
    {
        std::vector<Diagnostic> diagnostics;
        const JsonValue root = parsed(R"({
            "entities": [
                {"mesh": {"$sidecar": "meshes/rock.bin", "hash": "7"}},
                {"tiles": {"$sidecar": "tiles.bin", "hash": "9"}}
            ],
            "heightmap": {"$sidecar": "height.bin", "hash": "11"}
        })");
        const std::vector<SidecarRef> refs = collect_sidecar_refs(root, diagnostics);
        CHECK(diagnostics.empty());
        CHECK(refs.size() == 3);
        CHECK(refs[0].relpath == "meshes/rock.bin");
        CHECK(refs[0].hash == 7);
        CHECK(refs[0].json_pointer == "/entities/0/mesh");
        CHECK(refs[1].relpath == "tiles.bin");
        CHECK(refs[1].hash == 9);
        CHECK(refs[1].json_pointer == "/entities/1/tiles");
        CHECK(refs[2].relpath == "height.bin");
        CHECK(refs[2].hash == 11);
        CHECK(refs[2].json_pointer == "/heightmap");
    }

    // --- a ref object is a LEAF: nothing inside it is authored content --------------------------
    {
        std::vector<Diagnostic> diagnostics;
        // A malformed carrier ("hash" missing) whose members contain a would-be ref: the carrier is
        // diagnosed and NOT descended into, so the nested shape is not collected.
        const JsonValue root = parsed(
            R"({"broken": {"$sidecar": "a.bin", "nested": {"$sidecar": "b.bin", "hash": "1"}}})");
        const std::vector<SidecarRef> refs = collect_sidecar_refs(root, diagnostics);
        CHECK(refs.empty());
        CHECK(diagnostics.size() == 1);
        CHECK(diagnostics[0].code == "sidecar.ref_malformed");
        CHECK(diagnostics[0].message.find("/broken") == 0); // the pointer names the carrier
    }

    // --- malformed shapes: one diagnostic each, named by pointer --------------------------------
    {
        std::vector<Diagnostic> diagnostics;
        const JsonValue root = parsed(R"({
            "a": {"$sidecar": 5, "hash": "1"},
            "b": {"$sidecar": "x.bin"},
            "c": {"$sidecar": "y.bin", "hash": 9},
            "d": {"$sidecar": "z.bin", "hash": "007"},
            "ok": {"$sidecar": "w.bin", "hash": "3"}
        })");
        const std::vector<SidecarRef> refs = collect_sidecar_refs(root, diagnostics);
        CHECK(refs.size() == 1);
        CHECK(refs[0].relpath == "w.bin");
        CHECK(diagnostics.size() == 4);
        for (const Diagnostic& d : diagnostics)
            CHECK(d.code == "sidecar.ref_malformed");
    }

    // --- pointer escaping (RFC 6901: "~" -> "~0", "/" -> "~1") ----------------------------------
    {
        std::vector<Diagnostic> diagnostics;
        const JsonValue root = parsed(R"({"a/b": {"c~d": {"$sidecar": "e.bin", "hash": "5"}}})");
        const std::vector<SidecarRef> refs = collect_sidecar_refs(root, diagnostics);
        CHECK(diagnostics.empty());
        CHECK(refs.size() == 1);
        CHECK(refs[0].json_pointer == "/a~1b/c~0d");
    }

    // --- a document with no refs collects nothing ------------------------------------------------
    {
        std::vector<Diagnostic> diagnostics;
        const JsonValue root = parsed(R"({"version": 1, "entities": [1, 2, 3], "name": "x"})");
        CHECK(collect_sidecar_refs(root, diagnostics).empty());
        CHECK(diagnostics.empty());
    }

    // --- a whole-document ref (root IS the ref) has the empty pointer ---------------------------
    {
        std::vector<Diagnostic> diagnostics;
        const JsonValue root = parsed(R"({"$sidecar": "root.bin", "hash": "2"})");
        const std::vector<SidecarRef> refs = collect_sidecar_refs(root, diagnostics);
        CHECK(refs.size() == 1);
        CHECK(refs[0].json_pointer.empty()); // "" is the whole document (RFC 6901)
    }

    SERIALIZER_TEST_MAIN_END();
}
