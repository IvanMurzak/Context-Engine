// Canonical-writer tests: key sorting, array-order preservation, indentation/formatting shape,
// escaping, the NaN/Infinity ban, byte-level canonicalize (JSON + non-JSON paths), and the
// document header reader (R-FILE-001, L-32/L-33; R-QA-013 happy + edge + failure).

#include "context/editor/serializer/canonical.h"

#include "context/editor/serializer/json_parse.h"

#include "serializer_test.h"

#include <limits>
#include <string>

using context::editor::serializer::canonical_hash_of;
using context::editor::serializer::canonicalize;
using context::editor::serializer::CanonicalizeResult;
using context::editor::serializer::Diagnostic;
using context::editor::serializer::DocumentHeader;
using context::editor::serializer::JsonMember;
using context::editor::serializer::JsonValue;
using context::editor::serializer::parse_json;
using context::editor::serializer::read_document_header;
using context::editor::serializer::serialize_canonical;

namespace
{

std::string canon(const std::string& source)
{
    return canonicalize(source).bytes;
}

} // namespace

int main()
{
    // Formatting shape: 2-space indent, ": " separator, sorted keys, one trailing newline.
    CHECK(canon("{\"b\": 2, \"a\": 1}") == "{\n  \"a\": 1,\n  \"b\": 2\n}\n");
    CHECK(canon("[1, 2]") == "[\n  1,\n  2\n]\n");
    CHECK(canon("{}") == "{}\n");
    CHECK(canon("[]") == "[]\n");
    CHECK(canon("null") == "null\n");
    CHECK(canon("true") == "true\n");
    CHECK(canon("\"x\"") == "\"x\"\n");
    CHECK(canon("3") == "3\n");

    // Whitespace/formatting noise canonicalizes away; the canonical hash is formatting-blind.
    {
        const CanonicalizeResult a = canonicalize("{\"k\":1}");
        const CanonicalizeResult b = canonicalize("  {\n\t\"k\" :   1\n}  \n\n");
        CHECK(a.is_json);
        CHECK(b.is_json);
        CHECK(a.bytes == b.bytes);
        CHECK(a.canonical_hash == b.canonical_hash);
        CHECK(a.canonical_hash == canonical_hash_of(a.bytes));
    }

    // Key ORDER canonicalizes too: permuted members yield identical canonical bytes.
    CHECK(canon("{\"z\": 1, \"a\": 2, \"m\": 3}") == canon("{\"m\": 3, \"a\": 2, \"z\": 1}"));

    // Key sorting is UTF-8 BYTE order: "$schema" ('$' 0x24) sorts before ASCII letters; a
    // multi-byte key (é = C3 A9) sorts after every ASCII key.
    CHECK(canon("{\"version\": 1, \"$schema\": \"s\"}") ==
          "{\n  \"$schema\": \"s\",\n  \"version\": 1\n}\n");
    CHECK(canon("{\"\xC3\xA9\": 1, \"z\": 2}") == "{\n  \"z\": 2,\n  \"\xC3\xA9\": 1\n}\n");

    // ARRAY order is authored order — never sorted (stable array ordering; id-keyed child
    // collections are arrays-of-objects-with-id, so their order is content — L-33/R-FILE-001).
    CHECK(canon("[3, 1, 2]") == "[\n  3,\n  1,\n  2\n]\n");
    CHECK(canon("[{\"id\": \"b\"}, {\"id\": \"a\"}]") !=
          canon("[{\"id\": \"a\"}, {\"id\": \"b\"}]"));

    // Number canonicalization flows through the writer: doubles ECMA-format, integers lossless.
    CHECK(canon("[1.0, 1e2, -0.0, -0, 0.5, 9007199254740993]") ==
          "[\n  1,\n  100,\n  0,\n  0,\n  0.5,\n  9007199254740993\n]\n");

    // String escaping is minimal; non-ASCII stays raw NFC UTF-8; \u escapes collapse to it.
    CHECK(canon("\"\\u00e9\"") == "\"\xC3\xA9\"\n");
    CHECK(canon("\"e\\u0301\"") == "\"\xC3\xA9\"\n"); // NFC at parse (composed on the way in)
    CHECK(canon("\"tab\\tquote\\\"back\\\\slash\"") == "\"tab\\tquote\\\"back\\\\slash\"\n");
    CHECK(canon("\"\\u0001\"") == "\"\\u0001\"\n"); // control chars stay \u-escaped

    // Nested indentation shape (the exact bytes the corpus pins at scale).
    CHECK(canon("{\"a\": [1, {\"b\": []}]}") ==
          "{\n  \"a\": [\n    1,\n    {\n      \"b\": []\n    }\n  ]\n}\n");

    // Fixpoint: canonicalizing canonical output is byte-identical (R-FILE-001).
    {
        const std::string once = canon("{\"z\": [1.50, {\"y\": \"e\\u0301\"}], \"a\": 1e3}");
        CHECK(canonicalize(once).bytes == once);
    }

    // Non-JSON input: NO canonicalization pass — bytes pass through raw and the canonical hash
    // equals the raw-byte hash by construction (binary sidecars, R-FILE-001).
    {
        const std::string binary = std::string("BIN\x00\xFF sidecar payload", 20);
        const CanonicalizeResult r = canonicalize(binary);
        CHECK(!r.is_json);
        CHECK(r.bytes == binary);
        CHECK(r.canonical_hash == canonical_hash_of(binary));
        CHECK(!r.diagnostics.empty()); // the parse failure is attached for JSON-expecting callers
    }

    // Failure path: a programmatic tree holding NaN/Infinity is unrepresentable (banned).
    {
        JsonValue root;
        root.type = JsonValue::Type::number;
        root.number_value = std::numeric_limits<double>::quiet_NaN();
        std::string out;
        CHECK(!serialize_canonical(root, out));
        CHECK(out.empty());
        root.number_value = std::numeric_limits<double>::infinity();
        CHECK(!serialize_canonical(root, out));
    }

    // BOM/CRLF heal on canonicalize: the diagnostics surface AND the output is clean LF.
    {
        const CanonicalizeResult r = canonicalize("\xEF\xBB\xBF{\"a\": 1}\r\n");
        CHECK(r.is_json);
        CHECK(r.bytes == "{\n  \"a\": 1\n}\n");
        bool bom = false;
        bool crlf = false;
        for (const Diagnostic& d : r.diagnostics)
        {
            bom = bom || d.code == "encoding.bom";
            crlf = crlf || d.code == "encoding.crlf";
        }
        CHECK(bom);
        CHECK(crlf);
    }

    // Document header reader (L-32): happy path.
    {
        const auto parsed = parse_json("{\"$schema\": \"ctx://scene/1\", \"version\": 3, "
                                       "\"componentVersions\": {\"core:transform\": 2}, "
                                       "\"entities\": []}");
        CHECK(parsed.ok);
        std::vector<Diagnostic> diags;
        const DocumentHeader h = read_document_header(parsed.root, diags);
        CHECK(diags.empty());
        CHECK(h.has_schema);
        CHECK(h.schema == "ctx://scene/1");
        CHECK(h.has_version);
        CHECK(h.version == 3);
        CHECK(h.has_component_versions);
        CHECK(h.component_versions.size() == 1);
        CHECK(h.component_versions[0].first == "core:transform");
        CHECK(h.component_versions[0].second == 2);
    }

    // Header reader: missing fields are NOT diagnostics (per-kind strictness is the schema
    // model's job); wrong SHAPES are.
    {
        const auto parsed = parse_json("{\"entities\": []}");
        CHECK(parsed.ok);
        std::vector<Diagnostic> diags;
        const DocumentHeader h = read_document_header(parsed.root, diags);
        CHECK(diags.empty());
        CHECK(!h.has_schema);
        CHECK(!h.has_version);
        CHECK(!h.has_component_versions);
    }
    {
        const auto parsed = parse_json("{\"$schema\": 1, \"version\": \"x\", "
                                       "\"componentVersions\": [1]}");
        CHECK(parsed.ok);
        std::vector<Diagnostic> diags;
        const DocumentHeader h = read_document_header(parsed.root, diags);
        CHECK(diags.size() == 3);
        CHECK(!h.has_schema);
        CHECK(!h.has_version);
        CHECK(!h.has_component_versions);
    }
    {
        const auto parsed = parse_json("{\"componentVersions\": {\"core:transform\": \"two\"}}");
        CHECK(parsed.ok);
        std::vector<Diagnostic> diags;
        const DocumentHeader h = read_document_header(parsed.root, diags);
        CHECK(diags.size() == 1);
        CHECK(diags[0].code == "header.component_version_not_integer");
        CHECK(h.has_component_versions);
        CHECK(h.component_versions.empty());
    }

    // A non-object root has no header (arrays are legal documents).
    {
        const auto parsed = parse_json("[1, 2]");
        CHECK(parsed.ok);
        std::vector<Diagnostic> diags;
        const DocumentHeader h = read_document_header(parsed.root, diags);
        CHECK(diags.empty());
        CHECK(!h.has_schema);
    }

    SERIALIZER_TEST_MAIN_END();
}
