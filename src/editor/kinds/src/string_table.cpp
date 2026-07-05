// String-table content-kind semantics — see string_table.h.

#include "context/editor/kinds/string_table.h"

#include <functional>
#include <string>

namespace context::editor::kinds
{

using serializer::JsonMember;
using serializer::JsonValue;

namespace
{

const JsonValue* member(const JsonValue& object, std::string_view key)
{
    if (object.type != JsonValue::Type::object)
        return nullptr;
    for (const JsonMember& m : object.members)
        if (m.key == key)
            return &m.value;
    return nullptr;
}

const JsonValue* array_member(const JsonValue& object, std::string_view key)
{
    const JsonValue* v = member(object, key);
    return (v != nullptr && v->type == JsonValue::Type::array) ? v : nullptr;
}

const JsonValue* string_member(const JsonValue& object, std::string_view key)
{
    const JsonValue* v = member(object, key);
    return (v != nullptr && v->type == JsonValue::Type::string) ? v : nullptr;
}

// The base-language subtag of a BCP-47 tag, lowercased ("pt-BR" -> "pt", "EN" -> "en").
std::string base_language(std::string_view locale)
{
    std::string base;
    for (const char c : locale)
    {
        if (c == '-' || c == '_')
            break;
        base.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
    }
    return base;
}

// The declared `fallback` array for locale `tag` in the table (nullptr when the tag is undeclared or
// declares no fallback).
const JsonValue* fallback_of(const JsonValue& doc, std::string_view tag)
{
    const JsonValue* locales = array_member(doc, "locales");
    if (locales == nullptr)
        return nullptr;
    for (const JsonValue& loc : locales->elements)
    {
        const JsonValue* l = string_member(loc, "locale");
        if (l != nullptr && l->string_value == tag)
            return array_member(loc, "fallback");
    }
    return nullptr;
}

bool contains(const std::vector<std::string>& v, const std::string& s)
{
    for (const std::string& e : v)
        if (e == s)
            return true;
    return false;
}

} // namespace

std::string_view plural_category_id(PluralCategory category) noexcept
{
    switch (category)
    {
    case PluralCategory::zero:
        return "zero";
    case PluralCategory::one:
        return "one";
    case PluralCategory::two:
        return "two";
    case PluralCategory::few:
        return "few";
    case PluralCategory::many:
        return "many";
    case PluralCategory::other:
        return "other";
    }
    return "other";
}

PluralCategory plural_category(std::string_view locale, long long count) noexcept
{
    // Magnitude of `count` computed in the UNSIGNED domain so LLONG_MIN cannot overflow (the
    // CI UBSan `sanitize` leg would flag a signed `-count`); the conversion is modular/well-defined.
    unsigned long long n = static_cast<unsigned long long>(count);
    if (count < 0)
        n = 0ULL - n;
    const std::string base = base_language(locale);
    const unsigned long long mod10 = n % 10;
    const unsigned long long mod100 = n % 100;

    // East-Asian (and kin): no plural distinction — everything is `other`.
    if (base == "ja" || base == "zh" || base == "ko" || base == "vi" || base == "th" ||
        base == "id")
        return PluralCategory::other;

    // French-like: 0 and 1 are singular.
    if (base == "fr")
        return (n == 0 || n == 1) ? PluralCategory::one : PluralCategory::other;

    // East-Slavic (ru, uk): one / few / many by the mod-10 / mod-100 rule.
    if (base == "ru" || base == "uk")
    {
        if (mod10 == 1 && mod100 != 11)
            return PluralCategory::one;
        if (mod10 >= 2 && mod10 <= 4 && !(mod100 >= 12 && mod100 <= 14))
            return PluralCategory::few;
        return PluralCategory::many; // mod10==0, mod10 in 5..9, or mod100 in 11..14
    }

    // Polish: one / few / many.
    if (base == "pl")
    {
        if (n == 1)
            return PluralCategory::one;
        if (mod10 >= 2 && mod10 <= 4 && !(mod100 >= 12 && mod100 <= 14))
            return PluralCategory::few;
        return PluralCategory::many;
    }

    // Arabic: the full six-way split.
    if (base == "ar")
    {
        if (n == 0)
            return PluralCategory::zero;
        if (n == 1)
            return PluralCategory::one;
        if (n == 2)
            return PluralCategory::two;
        if (mod100 >= 3 && mod100 <= 10)
            return PluralCategory::few;
        if (mod100 >= 11 && mod100 <= 99)
            return PluralCategory::many;
        return PluralCategory::other;
    }

    // English-like default (en, de, nl, sv, da, no, es, it, … AND every unrecognized locale):
    // `one` for exactly 1, `other` for all else.
    return (n == 1) ? PluralCategory::one : PluralCategory::other;
}

FallbackChain resolve_fallback_chain(const JsonValue& doc, std::string_view locale)
{
    FallbackChain chain;
    std::vector<std::string> on_path; // the active DFS recursion path (true-cycle detection)
    std::vector<std::string> added;   // every locale already placed (diamonds are not cycles)

    std::function<void(const std::string&)> visit = [&](const std::string& tag) {
        if (contains(on_path, tag))
        {
            chain.cycle = true; // a fallback edge back onto the active path
            return;
        }
        if (contains(added, tag))
            return; // already resolved via another branch — a diamond, not a cycle
        on_path.push_back(tag);
        added.push_back(tag);
        chain.order.push_back(tag);
        if (const JsonValue* fb = fallback_of(doc, tag); fb != nullptr)
            for (const JsonValue& f : fb->elements)
                if (f.type == JsonValue::Type::string)
                    visit(f.string_value);
        on_path.pop_back();
    };
    visit(std::string(locale));

    // The implicit final fallback: the table's authoritative source locale.
    if (const JsonValue* src = string_member(doc, "sourceLocale"); src != nullptr)
        if (!contains(chain.order, src->string_value))
            chain.order.push_back(src->string_value);
    return chain;
}

StringResolution resolve_string(const JsonValue& doc, std::string_view key, std::string_view locale,
                                std::optional<long long> count)
{
    StringResolution res;

    const JsonValue* keys = array_member(doc, "keys");
    if (keys == nullptr)
        return res;
    const JsonValue* values = nullptr;
    for (const JsonValue& entry : keys->elements)
    {
        const JsonValue* k = string_member(entry, "key");
        if (k != nullptr && k->string_value == key)
        {
            values = array_member(entry, "values");
            break;
        }
    }
    if (values == nullptr)
        return res;

    const FallbackChain chain = resolve_fallback_chain(doc, locale);
    for (const std::string& loc : chain.order)
    {
        const JsonValue* val = nullptr;
        for (const JsonValue& v : values->elements)
        {
            const JsonValue* vl = string_member(v, "locale");
            if (vl != nullptr && vl->string_value == loc)
            {
                val = &v;
                break;
            }
        }
        if (val == nullptr)
            continue;

        if (const JsonValue* text = string_member(*val, "text"); text != nullptr)
        {
            res.found = true;
            res.text = text->string_value;
            res.resolved_locale = loc;
            res.plural = false;
            return res;
        }

        const JsonValue* plural = member(*val, "plural");
        if (plural != nullptr && plural->type == JsonValue::Type::object)
        {
            PluralCategory cat =
                count.has_value() ? plural_category(loc, *count) : PluralCategory::other;
            const JsonValue* form = string_member(*plural, plural_category_id(cat));
            if (form == nullptr && cat != PluralCategory::other)
            {
                cat = PluralCategory::other; // CLDR: `other` is the guaranteed fallback form
                form = string_member(*plural, "other");
            }
            if (form != nullptr)
            {
                res.found = true;
                res.text = form->string_value;
                res.resolved_locale = loc;
                res.category = cat;
                res.plural = true;
                return res;
            }
            // A plural set with no usable `other` form is malformed (validate_string_table flags
            // it); fall through to the next fallback locale rather than returning empty.
        }
    }
    return res;
}

std::vector<KindDiagnostic> validate_string_table(const JsonValue& doc)
{
    std::vector<KindDiagnostic> out;

    // --- locales: duplicates, unknown fallbacks, fallback cycles --------------------------------
    std::vector<std::string> declared;
    if (const JsonValue* locales = array_member(doc, "locales"); locales != nullptr)
    {
        for (std::size_t i = 0; i < locales->elements.size(); ++i)
        {
            const JsonValue* l = string_member(locales->elements[i], "locale");
            if (l == nullptr)
                continue;
            if (contains(declared, l->string_value))
                out.push_back({"stringtable.locale_duplicate",
                               "/locales/" + std::to_string(i) + "/locale",
                               "duplicate locale \"" + l->string_value + "\""});
            else
                declared.push_back(l->string_value);
        }
        for (std::size_t i = 0; i < locales->elements.size(); ++i)
        {
            const JsonValue& loc = locales->elements[i];
            if (const JsonValue* fb = array_member(loc, "fallback"); fb != nullptr)
                for (std::size_t j = 0; j < fb->elements.size(); ++j)
                {
                    const JsonValue& f = fb->elements[j];
                    if (f.type == JsonValue::Type::string && !contains(declared, f.string_value))
                        out.push_back({"stringtable.fallback_unknown",
                                       "/locales/" + std::to_string(i) + "/fallback/" +
                                           std::to_string(j),
                                       "fallback names locale \"" + f.string_value +
                                           "\" not declared in `locales`"});
                }
            if (const JsonValue* l = string_member(loc, "locale");
                l != nullptr && resolve_fallback_chain(doc, l->string_value).cycle)
                out.push_back({"stringtable.fallback_cycle",
                               "/locales/" + std::to_string(i) + "/fallback",
                               "fallback chain for \"" + l->string_value + "\" contains a cycle"});
        }
    }

    // --- keys: duplicates, exactly-one-of text/plural, plural completeness ----------------------
    if (const JsonValue* keys = array_member(doc, "keys"); keys != nullptr)
    {
        std::vector<std::string> seen_keys;
        for (std::size_t i = 0; i < keys->elements.size(); ++i)
        {
            const JsonValue& entry = keys->elements[i];
            if (const JsonValue* k = string_member(entry, "key"); k != nullptr)
            {
                if (contains(seen_keys, k->string_value))
                    out.push_back({"stringtable.key_duplicate",
                                   "/keys/" + std::to_string(i) + "/key",
                                   "duplicate key \"" + k->string_value + "\""});
                else
                    seen_keys.push_back(k->string_value);
            }
            const JsonValue* values = array_member(entry, "values");
            if (values == nullptr)
                continue;
            for (std::size_t j = 0; j < values->elements.size(); ++j)
            {
                const JsonValue& v = values->elements[j];
                const std::string vptr =
                    "/keys/" + std::to_string(i) + "/values/" + std::to_string(j);
                const JsonValue* text = member(v, "text");
                const JsonValue* plural = member(v, "plural");
                if ((text != nullptr) == (plural != nullptr)) // neither, or both
                    out.push_back({"stringtable.value_invalid", vptr,
                                   "a translation is EXACTLY ONE of `text` or `plural`"});
                if (plural != nullptr && plural->type == JsonValue::Type::object &&
                    string_member(*plural, "other") == nullptr)
                    out.push_back({"stringtable.plural_incomplete", vptr + "/plural",
                                   "a plural set must define the required CLDR `other` category"});
            }
        }
    }
    return out;
}

} // namespace context::editor::kinds
