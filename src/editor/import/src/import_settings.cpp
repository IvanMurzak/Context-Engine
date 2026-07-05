// Import-settings resolution: meta `importSettings` + the `platforms.<id>` override, canonicalized
// and hashed for the R-FILE-010 cache key.

#include "context/editor/import/import_settings.h"

#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_tree.h"

namespace context::editor::import
{
namespace
{
using serializer::JsonValue;

// The object member named `key`, or nullptr. Object members are authored-order; a canonical parse
// has at most one per key (duplicate-key is a parse diagnostic upstream), so first match is total.
const JsonValue* member(const JsonValue& obj, std::string_view key)
{
    if (obj.type != JsonValue::Type::object)
        return nullptr;
    for (const serializer::JsonMember& m : obj.members)
        if (m.key == key)
            return &m.value;
    return nullptr;
}

// Read a boolean member with a default (a missing or wrong-typed member keeps `fallback`).
bool bool_member(const JsonValue& obj, std::string_view key, bool fallback)
{
    const JsonValue* v = member(obj, key);
    if (v == nullptr || v->type != JsonValue::Type::boolean)
        return fallback;
    return v->boolean_value;
}

// Shallow-merge `over` onto `base` (both objects): every `over` member replaces or appends by key,
// preserving base order then appended-override order. Deterministic; the canonical writer re-sorts,
// so the final key order is stable regardless of authoring order.
JsonValue shallow_merge(const JsonValue& base, const JsonValue& over)
{
    JsonValue result;
    result.type = JsonValue::Type::object;
    if (base.type == JsonValue::Type::object)
        result.members = base.members;
    if (over.type == JsonValue::Type::object)
    {
        for (const serializer::JsonMember& om : over.members)
        {
            bool replaced = false;
            for (serializer::JsonMember& rm : result.members)
            {
                if (rm.key == om.key)
                {
                    rm.value = om.value;
                    replaced = true;
                    break;
                }
            }
            if (!replaced)
                result.members.push_back(om);
        }
    }
    return result;
}
} // namespace

ImportSettings resolve_import_settings(std::string_view meta_bytes, std::string_view platform_id)
{
    ImportSettings settings;

    // Parse the meta (total: non-JSON / non-object -> an empty effective settings object).
    serializer::CanonicalizeResult parsed = serializer::canonicalize(meta_bytes);
    JsonValue effective;
    effective.type = JsonValue::Type::object;
    if (parsed.is_json && parsed.root.type == JsonValue::Type::object)
    {
        const JsonValue* import_settings = member(parsed.root, "importSettings");
        if (import_settings != nullptr && import_settings->type == JsonValue::Type::object)
            effective = *import_settings;

        // Reserved-in-M2 `platforms.<platform_id>` block shallow-overrides importSettings for this
        // platform only (L-36) — so a per-platform knob re-keys that platform's entry alone.
        const JsonValue* platforms = member(parsed.root, "platforms");
        if (platforms != nullptr && !platform_id.empty())
        {
            const JsonValue* override_block = member(*platforms, platform_id);
            if (override_block != nullptr && override_block->type == JsonValue::Type::object)
                effective = shallow_merge(effective, *override_block);
        }
    }

    // Canonicalize the effective object -> the cache-key bytes + hash (R-FILE-001 fixpoint form, so
    // equivalent authorings key identically). serialize_canonical only fails on a non-finite double,
    // impossible in a parsed tree; the "{}\n" default already set on `settings` covers that.
    std::string canonical;
    if (serializer::serialize_canonical(effective, canonical))
        settings.canonical_bytes = std::move(canonical);
    settings.hash = serializer::canonical_hash_of(settings.canonical_bytes);

    // Typed convenience reads (defaults preserved when absent/wrong-typed).
    settings.srgb = bool_member(effective, "srgb", settings.srgb);
    settings.generate_mipmaps = bool_member(effective, "generateMipmaps", settings.generate_mipmaps);

    return settings;
}

} // namespace context::editor::import
