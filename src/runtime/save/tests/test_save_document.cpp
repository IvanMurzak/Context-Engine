// Save document serialize/parse (R-DATA-005): the canonical round-trip fixpoint, the composed-
// identity hex codec, and the parse failure paths (R-QA-013 happy + edge + failure).

#include "context/runtime/save/save_document.h"

#include "context/editor/serializer/canonical.h"

#include "save_test.h"

#include <cstdint>
#include <string>

using namespace context::runtime::save;
namespace serializer = context::editor::serializer;

namespace
{
[[nodiscard]] serializer::JsonValue parse(const char* json)
{
    serializer::CanonicalizeResult r = serializer::canonicalize(json);
    CHECK(r.is_json);
    return r.root;
}
} // namespace

int main()
{
    // --- the composed-identity hex codec -------------------------------------------------------
    {
        CHECK(format_identity(0x0123456789abcdefULL) == "0123456789abcdef");
        CHECK(format_identity(0) == "0000000000000000");
        CHECK(format_identity(0xffffffffffffffffULL) == "ffffffffffffffff");
        std::uint64_t got = 0;
        CHECK(parse_identity("0123456789abcdef", got) && got == 0x0123456789abcdefULL);
        CHECK(parse_identity("0000000000000000", got) && got == 0);
        // Rejections: wrong length, uppercase (canonical is lowercase), non-hex.
        CHECK(!parse_identity("0123", got));
        CHECK(!parse_identity("0123456789abcdef0", got));
        CHECK(!parse_identity("0123456789ABCDEF", got));
        CHECK(!parse_identity("0123456789abcdeg", got));
        CHECK(!parse_identity("", got));
    }

    // --- the canonical round-trip fixpoint (serialize -> parse -> serialize is byte-identical) --
    {
        SaveDocument save;
        save.format_version = kSaveFormatVersion;
        save.engine_version = "0.0.1";
        save.back_compat_scope = 8;
        save.component_versions = {{"ctx:transform", 3}, {"ctx:health", 2}};

        SaveEntity e1;
        e1.identity = 0x0123456789abcdefULL;
        e1.components = parse(R"({"ctx:transform": {"position": [1, 2, 3]}, "ctx:health": {"hp": 100}})");
        SaveEntity e2;
        e2.identity = 0xfedcba9876543210ULL;
        e2.components = parse(R"({"ctx:transform": {"position": [0, 0, 0]}})");
        save.entities = {e1, e2};

        const std::string s1 = serialize_save(save);
        CHECK(!s1.empty());
        // The save-kind marker + versioned envelope are present (canonical ": " separator).
        CHECK(s1.find("\"$save\": \"ctx:save\"") != std::string::npos);
        CHECK(s1.find("\"saveFormatVersion\": 1") != std::string::npos);

        const SaveParseResult parsed = parse_save(s1);
        CHECK(parsed.ok);
        CHECK(parsed.save.format_version == 1);
        CHECK(parsed.save.engine_version == "0.0.1");
        CHECK(parsed.save.back_compat_scope == 8);
        CHECK(parsed.save.component_versions.size() == 2);
        // Header lookups are order-independent (the canonical writer sorts keys).
        const std::int64_t* tv = parsed.save.saved_version("ctx:transform");
        const std::int64_t* hv = parsed.save.saved_version("ctx:health");
        CHECK(tv != nullptr && *tv == 3);
        CHECK(hv != nullptr && *hv == 2);
        CHECK(parsed.save.saved_version("ctx:missing") == nullptr);
        // Entity array order is preserved; identities round-trip losslessly through the hex form.
        CHECK(parsed.save.entities.size() == 2);
        CHECK(parsed.save.entities[0].identity == 0x0123456789abcdefULL);
        CHECK(parsed.save.entities[1].identity == 0xfedcba9876543210ULL);

        const std::string s2 = serialize_save(parsed.save);
        CHECK(s1 == s2); // the canonical fixpoint: re-serializing a parsed save is byte-identical
    }

    // --- a minimal save (empty entities), defaults applied -------------------------------------
    {
        const SaveParseResult parsed =
            parse_save(R"({"$save": "ctx:save", "saveFormatVersion": 1, "entities": []})");
        CHECK(parsed.ok);
        CHECK(parsed.save.entities.empty());
        CHECK(parsed.save.back_compat_scope == kDefaultBackCompatScope); // default when absent
        CHECK(parsed.save.engine_version.empty());
    }

    // --- parse failure paths -------------------------------------------------------------------
    {
        CHECK(!parse_save("not json at all").ok);          // not strict JSON
        CHECK(!parse_save("[]").ok);                        // root not an object
        CHECK(!parse_save("{}").ok);                        // no $save marker
        CHECK(!parse_save(R"({"saveFormatVersion": 1, "entities": []})").ok); // missing $save
        CHECK(!parse_save(R"({"$save": "nope", "saveFormatVersion": 1, "entities": []})").ok);
        CHECK(!parse_save(R"({"$save": "ctx:save", "entities": []})").ok);    // missing format version
        CHECK(!parse_save(R"({"$save": "ctx:save", "saveFormatVersion": 1})").ok); // missing entities

        // A malformed identity is save.malformed.
        const SaveParseResult bad_id = parse_save(
            R"({"$save": "ctx:save", "saveFormatVersion": 1, "entities": [{"identity": "xyz", "components": {}}]})");
        CHECK(!bad_id.ok);
        CHECK(!bad_id.diagnostics.empty() && bad_id.diagnostics.back().code == "save.malformed");

        // Components must be an object.
        CHECK(!parse_save(
                   R"({"$save": "ctx:save", "saveFormatVersion": 1, "entities": [{"identity": "0123456789abcdef", "components": []}]})")
                   .ok);

        // A newer envelope is refused with save.format_unsupported (never best-effort).
        const SaveParseResult newer =
            parse_save(R"({"$save": "ctx:save", "saveFormatVersion": 99, "entities": []})");
        CHECK(!newer.ok);
        CHECK(!newer.diagnostics.empty() && newer.diagnostics.back().code == "save.format_unsupported");

        // The envelope version also has a FLOOR: 0 or a negative version is malformed (versions
        // start at 1), never silently accepted as "supported".
        const SaveParseResult zero_ver =
            parse_save(R"({"$save": "ctx:save", "saveFormatVersion": 0, "entities": []})");
        CHECK(!zero_ver.ok);
        CHECK(!zero_ver.diagnostics.empty() && zero_ver.diagnostics.back().code == "save.malformed");
        const SaveParseResult neg_ver =
            parse_save(R"({"$save": "ctx:save", "saveFormatVersion": -3, "entities": []})");
        CHECK(!neg_ver.ok);
        CHECK(!neg_ver.diagnostics.empty() && neg_ver.diagnostics.back().code == "save.malformed");

        // A non-object component PAYLOAD (not just the outer components map) is malformed: a scalar
        // payload would otherwise silently no-op through the migration runner and get re-stamped.
        const SaveParseResult scalar_payload = parse_save(
            R"({"$save": "ctx:save", "saveFormatVersion": 1, "entities": [{"identity": "0123456789abcdef", "components": {"ctx:transform": 42}}]})");
        CHECK(!scalar_payload.ok);
        CHECK(!scalar_payload.diagnostics.empty() &&
              scalar_payload.diagnostics.back().code == "save.malformed");
    }

    // --- a SUCCESSFUL parse still surfaces the parser's non-fatal encoding findings ---------------
    {
        // A leading UTF-8 BOM parses fine (the parser strips it) but must not be silently swallowed:
        // parse_json reports a non-fatal encoding.bom and parse_save propagates it on the ok path
        // (the json.* family the SaveParseResult contract documents).
        const SaveParseResult bom = parse_save(
            "\xEF\xBB\xBF" R"({"$save": "ctx:save", "saveFormatVersion": 1, "entities": []})");
        CHECK(bom.ok);
        bool saw_bom = false;
        for (const auto& d : bom.diagnostics)
            if (d.code == "encoding.bom")
                saw_bom = true;
        CHECK(saw_bom);
    }

    SAVE_TEST_MAIN_END();
}
