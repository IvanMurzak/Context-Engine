// String-table content kind (R-I18N-001): schema-fixture validation against the REAL
// engine_schemas() registration (valid + every invalid class), CLDR plural-category evaluation
// across the rule families, fallback-chain resolution (multi-hop + cycle + implicit source),
// key/plural resolution with provenance, the semantic validator, the canonical round-trip, and the
// AI-authorable author-headless-then-query-back smoke (R-QA-006 spirit).
// (R-QA-013: happy path, edge cases, AND failure paths.)

#include "context/editor/kinds/string_table.h"

#include "context/editor/schema/kind_schema.h"
#include "context/editor/schema/validator.h"
#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_parse.h"

#include "kinds_test.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kinds = context::editor::kinds;
namespace schema = context::editor::schema;
namespace serializer = context::editor::serializer;

using kinds::PluralCategory;

namespace
{

serializer::JsonValue parse(std::string_view text)
{
    serializer::ParseResult p = serializer::parse_json(text);
    CHECK(p.ok);
    return p.root;
}

schema::ValidationReport validate(std::string_view text)
{
    serializer::ParseResult p = serializer::parse_json(text);
    CHECK(p.ok);
    return schema::validate_document(p.root, text, schema::engine_schemas());
}

bool has(const schema::ValidationReport& r, std::string_view code)
{
    for (const schema::ValidationDiagnostic& d : r.diagnostics)
        if (d.code == code)
            return true;
    return false;
}

PluralCategory cat(std::string_view locale, long long n)
{
    return kinds::plural_category(locale, n);
}

// A schema-valid AND semantically-clean string table: en source, fr/de/ru fallbacks to en, a simple
// key and a plural key (en two-form, ru four-form).
constexpr std::string_view kValidStrings = R"({
  "$schema": "ctx:string-table",
  "version": 1,
  "sourceLocale": "en",
  "locales": [
    {"locale": "en", "name": "English"},
    {"locale": "fr", "name": "French", "fallback": ["en"]},
    {"locale": "de", "name": "German", "fallback": ["en"]},
    {"locale": "ru", "name": "Russian", "fallback": ["en"]}
  ],
  "keys": [
    {"key": "greeting", "values": [
      {"locale": "en", "text": "Hello"},
      {"locale": "fr", "text": "Bonjour"}
    ]},
    {"key": "items", "values": [
      {"locale": "en", "plural": {"one": "{n} item", "other": "{n} items"}},
      {"locale": "ru", "plural": {"one": "predmet", "few": "predmeta", "many": "predmetov", "other": "predmeta"}}
    ]}
  ]
})";

} // namespace

int main()
{
    // --- schema validation: happy path -----------------------------------------------------------
    {
        schema::ValidationReport r = validate(kValidStrings);
        CHECK(r.schema_bound);
        CHECK(r.schema_id == "ctx:string-table");
        CHECK(r.ok);
        CHECK(r.diagnostics.empty());
    }

    // --- schema validation: every invalid class --------------------------------------------------
    {
        // Missing a required root property (keys).
        schema::ValidationReport r =
            validate(R"({"$schema": "ctx:string-table", "version": 1, "sourceLocale": "en",
                "locales": []})");
        CHECK(!r.ok);
        CHECK(has(r, "schema.required_missing"));
    }
    {
        // An undeclared property.
        schema::ValidationReport r =
            validate(R"({"$schema": "ctx:string-table", "version": 1, "sourceLocale": "en",
                "locales": [], "keys": [], "bogus": 1})");
        CHECK(!r.ok);
        CHECK(has(r, "schema.unknown_property"));
    }
    {
        // A value's locale is the wrong type.
        schema::ValidationReport r =
            validate(R"({"$schema": "ctx:string-table", "version": 1, "sourceLocale": "en",
                "locales": [{"locale": "en"}], "keys": [
                  {"key": "k", "values": [{"locale": 7, "text": "x"}]}]})");
        CHECK(!r.ok);
        CHECK(has(r, "schema.type_mismatch"));
    }
    {
        // An unknown plural category (the CLDR set is fixed; additionalProperties: false).
        schema::ValidationReport r =
            validate(R"({"$schema": "ctx:string-table", "version": 1, "sourceLocale": "en",
                "locales": [{"locale": "en"}], "keys": [
                  {"key": "k", "values": [{"locale": "en", "plural": {"lots": "x", "other": "y"}}]}]})");
        CHECK(!r.ok);
        CHECK(has(r, "schema.unknown_property"));
    }

    // --- CLDR plural categories across the rule families ----------------------------------------
    {
        // English-like (one iff n==1). Base-language extraction handles region subtags.
        CHECK(cat("en", 1) == PluralCategory::one);
        CHECK(cat("en", 0) == PluralCategory::other);
        CHECK(cat("en", 2) == PluralCategory::other);
        CHECK(cat("en-US", 1) == PluralCategory::one);
        // French: 0 and 1 are singular.
        CHECK(cat("fr", 0) == PluralCategory::one);
        CHECK(cat("fr", 1) == PluralCategory::one);
        CHECK(cat("fr", 2) == PluralCategory::other);
        // East-Slavic (ru).
        CHECK(cat("ru", 1) == PluralCategory::one);
        CHECK(cat("ru", 2) == PluralCategory::few);
        CHECK(cat("ru", 5) == PluralCategory::many);
        CHECK(cat("ru", 11) == PluralCategory::many);
        CHECK(cat("ru", 21) == PluralCategory::one);
        CHECK(cat("ru", 22) == PluralCategory::few);
        CHECK(cat("ru", 25) == PluralCategory::many);
        // Polish.
        CHECK(cat("pl", 1) == PluralCategory::one);
        CHECK(cat("pl", 2) == PluralCategory::few);
        CHECK(cat("pl", 5) == PluralCategory::many);
        CHECK(cat("pl", 12) == PluralCategory::many);
        CHECK(cat("pl", 22) == PluralCategory::few);
        // Arabic: the full six-way split.
        CHECK(cat("ar", 0) == PluralCategory::zero);
        CHECK(cat("ar", 1) == PluralCategory::one);
        CHECK(cat("ar", 2) == PluralCategory::two);
        CHECK(cat("ar", 3) == PluralCategory::few);
        CHECK(cat("ar", 11) == PluralCategory::many);
        CHECK(cat("ar", 100) == PluralCategory::other);
        // East-Asian: no plural distinction.
        CHECK(cat("ja", 0) == PluralCategory::other);
        CHECK(cat("ja", 1) == PluralCategory::other);
        CHECK(cat("zh", 5) == PluralCategory::other);
        // Unknown locale → English-like default.
        CHECK(cat("xx", 1) == PluralCategory::one);
        CHECK(cat("xx", 3) == PluralCategory::other);
        // The category ids round-trip.
        CHECK(kinds::plural_category_id(PluralCategory::few) == "few");
        CHECK(kinds::plural_category_id(PluralCategory::other) == "other");
    }

    // --- fallback-chain resolution --------------------------------------------------------------
    {
        serializer::JsonValue doc = parse(kValidStrings);
        kinds::FallbackChain fr = kinds::resolve_fallback_chain(doc, "fr");
        CHECK(!fr.cycle);
        CHECK(fr.order.size() == 2);
        CHECK(fr.order[0] == "fr");
        CHECK(fr.order[1] == "en"); // declared fallback == implicit source
        // An undeclared locale still resolves against the source.
        kinds::FallbackChain zz = kinds::resolve_fallback_chain(doc, "zz");
        CHECK(zz.order.size() == 2);
        CHECK(zz.order[0] == "zz");
        CHECK(zz.order[1] == "en");
    }
    {
        // Multi-hop: de -> fr -> en (each declared), source en appended (already present).
        serializer::JsonValue doc = parse(R"({"$schema": "ctx:string-table", "version": 1,
            "sourceLocale": "en", "locales": [
              {"locale": "en"}, {"locale": "fr", "fallback": ["en"]},
              {"locale": "de", "fallback": ["fr"]}], "keys": []})");
        kinds::FallbackChain de = kinds::resolve_fallback_chain(doc, "de");
        CHECK(!de.cycle);
        CHECK(de.order.size() == 3);
        CHECK(de.order[0] == "de");
        CHECK(de.order[1] == "fr");
        CHECK(de.order[2] == "en");
    }
    {
        // A cycle a -> b -> a is detected (and truncated so resolution still terminates).
        serializer::JsonValue doc = parse(R"({"$schema": "ctx:string-table", "version": 1,
            "sourceLocale": "en", "locales": [
              {"locale": "a", "fallback": ["b"]}, {"locale": "b", "fallback": ["a"]},
              {"locale": "en"}], "keys": []})");
        CHECK(kinds::resolve_fallback_chain(doc, "a").cycle);
    }

    // --- key + plural resolution (with provenance) ----------------------------------------------
    {
        serializer::JsonValue doc = parse(kValidStrings);
        // Direct hit.
        kinds::StringResolution g_en = kinds::resolve_string(doc, "greeting", "en");
        CHECK(g_en.found);
        CHECK(g_en.text == "Hello");
        CHECK(g_en.resolved_locale == "en");
        CHECK(!g_en.plural);
        kinds::StringResolution g_fr = kinds::resolve_string(doc, "greeting", "fr");
        CHECK(g_fr.found);
        CHECK(g_fr.text == "Bonjour");
        // Fallback: de has no greeting value → resolves via the chain to en.
        kinds::StringResolution g_de = kinds::resolve_string(doc, "greeting", "de");
        CHECK(g_de.found);
        CHECK(g_de.text == "Hello");
        CHECK(g_de.resolved_locale == "en");
        // Plural selection in the source locale.
        kinds::StringResolution one = kinds::resolve_string(doc, "items", "en", 1);
        CHECK(one.found);
        CHECK(one.plural);
        CHECK(one.category == PluralCategory::one);
        CHECK(one.text == "{n} item");
        kinds::StringResolution many = kinds::resolve_string(doc, "items", "en", 7);
        CHECK(many.category == PluralCategory::other);
        CHECK(many.text == "{n} items");
        // Plural selection under the ru rules, direct.
        CHECK(kinds::resolve_string(doc, "items", "ru", 1).category == PluralCategory::one);
        CHECK(kinds::resolve_string(doc, "items", "ru", 2).category == PluralCategory::few);
        CHECK(kinds::resolve_string(doc, "items", "ru", 5).category == PluralCategory::many);
        CHECK(kinds::resolve_string(doc, "items", "ru", 5).resolved_locale == "ru");
        // Plural via fallback: fr has no items → resolves in en under the en rules.
        kinds::StringResolution fr_items = kinds::resolve_string(doc, "items", "fr", 3);
        CHECK(fr_items.found);
        CHECK(fr_items.resolved_locale == "en");
        CHECK(fr_items.category == PluralCategory::other);
        // A missing key is not found.
        CHECK(!kinds::resolve_string(doc, "nope", "en").found);
    }

    // --- semantic validation: a clean table has no findings -------------------------------------
    {
        CHECK(kinds::validate_string_table(parse(kValidStrings)).empty());
    }

    // --- semantic validation: every finding class -----------------------------------------------
    {
        // Duplicate locale + duplicate key.
        std::vector<kinds::KindDiagnostic> d = kinds::validate_string_table(parse(R"({
            "$schema": "ctx:string-table", "version": 1, "sourceLocale": "en",
            "locales": [{"locale": "en"}, {"locale": "en"}],
            "keys": [{"key": "k", "values": []}, {"key": "k", "values": []}]})"));
        CHECK(kinds::has_code(d, "stringtable.locale_duplicate"));
        CHECK(kinds::has_code(d, "stringtable.key_duplicate"));
    }
    {
        // Fallback to an undeclared locale.
        std::vector<kinds::KindDiagnostic> d = kinds::validate_string_table(parse(R"({
            "$schema": "ctx:string-table", "version": 1, "sourceLocale": "en",
            "locales": [{"locale": "en"}, {"locale": "fr", "fallback": ["zz"]}],
            "keys": []})"));
        CHECK(kinds::has_code(d, "stringtable.fallback_unknown"));
    }
    {
        // A fallback cycle.
        std::vector<kinds::KindDiagnostic> d = kinds::validate_string_table(parse(R"({
            "$schema": "ctx:string-table", "version": 1, "sourceLocale": "en",
            "locales": [{"locale": "a", "fallback": ["b"]}, {"locale": "b", "fallback": ["a"]}],
            "keys": []})"));
        CHECK(kinds::has_code(d, "stringtable.fallback_cycle"));
    }
    {
        // A value that is neither text nor plural, and one that is both.
        std::vector<kinds::KindDiagnostic> d = kinds::validate_string_table(parse(R"({
            "$schema": "ctx:string-table", "version": 1, "sourceLocale": "en",
            "locales": [{"locale": "en"}], "keys": [
              {"key": "neither", "values": [{"locale": "en"}]},
              {"key": "both", "values": [{"locale": "en", "text": "x", "plural": {"other": "y"}}]}]})"));
        CHECK(kinds::has_code(d, "stringtable.value_invalid"));
    }
    {
        // A plural set missing the required CLDR `other` category.
        std::vector<kinds::KindDiagnostic> d = kinds::validate_string_table(parse(R"({
            "$schema": "ctx:string-table", "version": 1, "sourceLocale": "en",
            "locales": [{"locale": "en"}], "keys": [
              {"key": "k", "values": [{"locale": "en", "plural": {"one": "x"}}]}]})"));
        CHECK(kinds::has_code(d, "stringtable.plural_incomplete"));
    }

    // --- canonical round-trip (R-FILE-001 fixpoint) ---------------------------------------------
    {
        serializer::CanonicalizeResult once = serializer::canonicalize(kValidStrings);
        CHECK(once.is_json);
        CHECK(once.bytes == serializer::canonicalize(once.bytes).bytes);
        CHECK(validate(once.bytes).ok);
    }

    // --- AI-authorable proof: author headless, then query back ----------------------------------
    {
        // The document an agent authors headless (canonical JSON, no GUI). It validates on both the
        // schema AND the semantic layer, and every string it declared queries back correctly.
        serializer::JsonValue authored = parse(kValidStrings);
        CHECK(validate(kValidStrings).ok);
        CHECK(kinds::validate_string_table(authored).empty());
        CHECK(kinds::resolve_string(authored, "greeting", "fr").text == "Bonjour");
        CHECK(kinds::resolve_string(authored, "items", "ru", 2).text == "predmeta");
        CHECK(kinds::resolve_string(authored, "items", "ru", 5).text == "predmetov");
    }

    KINDS_TEST_MAIN_END();
}
