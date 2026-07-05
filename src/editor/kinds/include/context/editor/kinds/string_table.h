// String-table content-kind semantics (R-I18N-001): CLDR plural-category selection, fallback-chain
// resolution, key lookup, and the semantic validation the ctx:string-table schema shape cannot express.

#pragma once

#include "context/editor/kinds/diagnostic.h"
#include "context/editor/serializer/json_tree.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace context::editor::kinds
{

// The CLDR cardinal plural categories (the R-I18N-001 ICU-style set). `other` is the required
// catch-all every locale defines; the rest appear only where a locale's rules produce them.
enum class PluralCategory
{
    zero,
    one,
    two,
    few,
    many,
    other
};

// The stable CLDR category id ("zero" / "one" / "two" / "few" / "many" / "other").
[[nodiscard]] std::string_view plural_category_id(PluralCategory category) noexcept;

// Select the CLDR cardinal plural category for a NON-NEGATIVE integer `count` in `locale` (BCP-47;
// the base-language subtag drives the rule — "pt-BR" uses "pt"). Implemented for a curated set of
// representative locales spanning the CLDR rule families (English-like one/other; French 0-or-1
// singular; East-Slavic ru/uk; Polish; Arabic's full six-way; East-Asian no-plural) — see
// string_table.cpp; an unrecognized locale falls back to the English-like {one iff count==1, else
// other} default. Rules are cardinal and INTEGER-ONLY (fractional CLDR operands v/f/t are out of v1
// scope); every category returned is one a matching integer selects. A negative count is treated as
// its magnitude.
[[nodiscard]] PluralCategory plural_category(std::string_view locale, long long count) noexcept;

// A locale's resolved fallback lookup order inside a parsed ctx:string-table.
struct FallbackChain
{
    // The locale itself, then its declared `fallback` chain (depth-first, authored order),
    // de-duplicated, with the table's `sourceLocale` appended as the implicit final fallback.
    std::vector<std::string> order;
    // True when the declared chain contains a cycle (a fallback edge back onto the active path); the
    // order is truncated at the back-edge so resolution still terminates.
    bool cycle = false;
};

// Resolve `locale`'s fallback lookup order. A locale absent from the table's `locales` still yields
// [locale, sourceLocale] (unknown locales resolve against the source). Pure over the parsed tree.
[[nodiscard]] FallbackChain resolve_fallback_chain(const serializer::JsonValue& doc,
                                                   std::string_view locale);

// The outcome of resolving one key in one locale — carries provenance (WHICH fallback locale and
// plural category produced the string), the R-CLI-006 spirit for localized lookups.
struct StringResolution
{
    bool found = false;
    std::string text;
    std::string resolved_locale;                     // the fallback-chain locale that supplied it
    PluralCategory category = PluralCategory::other;  // meaningful when `plural` is true
    bool plural = false;                              // the resolved value was a plural set
};

// Resolve `key` in `locale`, walking the fallback chain. When the matched value is a plural set and
// `count` is supplied, the CLDR category for (resolved_locale, *count) is chosen, falling back to
// `other` when the exact category is absent; with no `count`, a plural value resolves to its `other`
// (citation) form. Returns found=false when no chain locale defines the key. Pure over the tree.
[[nodiscard]] StringResolution resolve_string(const serializer::JsonValue& doc, std::string_view key,
                                              std::string_view locale,
                                              std::optional<long long> count = std::nullopt);

// Semantic validation of a parsed ctx:string-table (BEYOND schema validation):
//   - stringtable.locale_duplicate — two `locales` entries share a tag;
//   - stringtable.key_duplicate     — two `keys` entries share a key;
//   - stringtable.fallback_unknown  — a `fallback` names a locale not declared in `locales`;
//   - stringtable.fallback_cycle    — a locale's fallback chain revisits a locale (a cycle);
//   - stringtable.value_invalid     — a translation is not EXACTLY ONE of `text` / `plural`;
//   - stringtable.plural_incomplete — a plural set omits the required CLDR `other` category.
// Deterministic; diagnostics in document order. A schema-invalid document is skipped gracefully.
[[nodiscard]] std::vector<KindDiagnostic> validate_string_table(const serializer::JsonValue& doc);

} // namespace context::editor::kinds
