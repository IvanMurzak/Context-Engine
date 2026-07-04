// Meta sidecar tests: naming, canonical serialization (platforms reservation), parse + failures.

#include "context/editor/assetdb/meta.h"

#include "context/editor/serializer/canonical.h"

#include "assetdb_test.h"

#include <string>
#include <vector>

using namespace context::editor::assetdb;
namespace serializer = context::editor::serializer;

int main()
{
    // --- sidecar naming --------------------------------------------------------------------------
    CHECK(meta_path_for("scenes/level1.json") == "scenes/level1.json.meta.json");
    CHECK(meta_path_for("tex.png") == "tex.png.meta.json");
    CHECK(is_meta_path("scenes/level1.json.meta.json"));
    CHECK(!is_meta_path("scenes/level1.json"));
    CHECK(!is_meta_path(".meta.json")); // a bare suffix names no asset
    CHECK(asset_path_for("scenes/level1.json.meta.json") == "scenes/level1.json");
    CHECK(asset_path_for(meta_path_for("a/b/c.bin")) == "a/b/c.bin");
    // A sidecar's own sidecar path is still meta-shaped (the candidate filter excludes sidecars
    // from ever being treated as assets, so none is ever created).
    CHECK(is_meta_path("x.json.meta.json.meta.json"));

    // --- fresh-sidecar serialization (meta creation, R-FILE-003 write #1) ------------------------
    AssetMeta meta;
    meta.guid = "0123456789abcdef0123456789abcdef";
    meta.kind = "ctx:scene";
    const std::string bytes = serialize_meta(meta);

    // The written form carries the header, the identity, and the RESERVED platforms block (L-36).
    CHECK(bytes.find("\"$schema\": \"ctx:meta\"") != std::string::npos);
    CHECK(bytes.find("\"version\": 1") != std::string::npos);
    CHECK(bytes.find("\"guid\": \"0123456789abcdef0123456789abcdef\"") != std::string::npos);
    CHECK(bytes.find("\"kind\": \"ctx:scene\"") != std::string::npos);
    CHECK(bytes.find("\"importSettings\": {}") != std::string::npos);
    CHECK(bytes.find("\"platforms\": {}") != std::string::npos);

    // Canonical fixpoint: the written bytes ARE their own canonicalization (idempotent writes).
    const serializer::CanonicalizeResult canon = serializer::canonicalize(bytes);
    CHECK(canon.is_json);
    CHECK(canon.bytes == bytes);

    // Round-trip: the parse recovers the identity fields.
    std::vector<std::string> problems;
    const auto parsed = parse_meta(bytes, problems);
    CHECK(parsed.has_value());
    CHECK(problems.empty());
    CHECK(parsed->guid == meta.guid);
    CHECK(parsed->kind == meta.kind);

    // Unknown kind: the member is omitted and parses back as "".
    AssetMeta kindless;
    kindless.guid = "00000000000000000000000000000abc";
    const std::string kindless_bytes = serialize_meta(kindless);
    CHECK(kindless_bytes.find("\"kind\"") == std::string::npos);
    problems.clear();
    const auto kindless_parsed = parse_meta(kindless_bytes, problems);
    CHECK(kindless_parsed.has_value());
    CHECK(kindless_parsed->kind.empty());

    // --- parse tolerance (identity must survive newer-engine sidecars) ---------------------------
    problems.clear();
    const auto extra = parse_meta("{\"$schema\": \"ctx:meta\", \"version\": 1, \"guid\": "
                                  "\"0123456789abcdef0123456789abcdef\", \"kind\": \"ctx:scene\", "
                                  "\"futureField\": {\"x\": 1}}\n",
                                  problems);
    CHECK(extra.has_value()); // unknown members never break identity
    CHECK(extra->guid == "0123456789abcdef0123456789abcdef");

    problems.clear();
    const auto headerless = parse_meta(
        "{\"guid\": \"0123456789abcdef0123456789abcdef\"}\n", problems);
    CHECK(headerless.has_value()); // hand-stripped header: identity survives, with a note
    CHECK(!problems.empty());

    // --- parse hard failures ----------------------------------------------------------------------
    problems.clear();
    CHECK(!parse_meta("not json at all", problems).has_value());
    CHECK(!problems.empty());
    problems.clear();
    CHECK(!parse_meta("[1, 2, 3]\n", problems).has_value()); // root must be an object
    problems.clear();
    CHECK(!parse_meta("{\"version\": 1}\n", problems).has_value()); // guid missing
    problems.clear();
    CHECK(!parse_meta("{\"guid\": \"UPPERCASE9abcdef0123456789abcdef\"}\n", problems)
               .has_value()); // guid malformed
    problems.clear();
    CHECK(!parse_meta("{\"guid\": 42}\n", problems).has_value()); // guid wrong type

    ASSETDB_TEST_MAIN_END();
}
