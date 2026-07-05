// Content-unit boundary emission + the boundary-property proof (see content_unit.h).

#include "context/editor/compose/content_unit.h"

#include "context/editor/compose/stable_id.h"
#include "context/editor/serializer/canonical.h"

#include <algorithm>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace context::editor::compose
{

namespace
{
namespace serializer = context::editor::serializer;
using serializer::JsonMember;
using serializer::JsonValue;

// Canonical-JSON emitter helpers (mirrors flatten.cpp's local helpers so the read-side result
// shapes cannot drift; the canonical writer sorts object keys, so member insertion order is free).
[[nodiscard]] JsonValue json_string(std::string_view v)
{
    JsonValue out;
    out.type = JsonValue::Type::string;
    out.string_value = std::string(v);
    return out;
}

[[nodiscard]] JsonValue json_bool(bool v)
{
    JsonValue out;
    out.type = JsonValue::Type::boolean;
    out.boolean_value = v;
    return out;
}

[[nodiscard]] JsonValue json_uint(std::uint64_t v)
{
    JsonValue out;
    out.type = JsonValue::Type::unsigned_integer;
    out.uint_value = v;
    return out;
}

void set_member(JsonValue& object, std::string_view key, JsonValue value)
{
    JsonMember m;
    m.key = std::string(key);
    m.value = std::move(value);
    object.members.push_back(std::move(m));
}
} // namespace

ContentUnitSet partition_content_units(const ComposedScene& scene, const SceneResolver& resolver)
{
    ContentUnitSet out;
    out.root_path = scene.root_path;

    // The root scene's top-level instances are the boundary definers (docs/chunk-pack-format.md
    // §2). Build ordered unit slots: the root unit first, then one per top-level instance in
    // authored order — a deterministic order independent of how entities enumerate.
    const SceneDoc* root_doc = resolver.resolve(scene.root_path);

    std::vector<ContentUnit> slots;
    ContentUnit root_unit;
    root_unit.is_root = true;
    root_unit.boundary_segment.clear(); // the scene root: an empty id-path
    root_unit.source_scene = scene.root_path;
    root_unit.identity_hash = identity_hash_of(scene.root_path, {});
    root_unit.unit_id = format_stable_id(root_unit.identity_hash);
    slots.push_back(std::move(root_unit));

    // Map a top-level instance id -> its slot index. std::less<> lets std::string_view keys probe
    // it without allocating.
    std::map<std::string, std::size_t, std::less<>> instance_slot;
    if (root_doc != nullptr)
    {
        for (const SceneInstance& inst : root_doc->instances)
        {
            // Duplicate top-level instance ids are a blocking compose.duplicate_id upstream; guard
            // anyway so a best-effort composed view never double-registers a slot.
            if (instance_slot.find(inst.id) != instance_slot.end())
                continue;
            ContentUnit unit;
            unit.is_root = false;
            unit.boundary_segment = inst.id;
            unit.source_scene = inst.scene;
            unit.identity_hash = identity_hash_of(scene.root_path, {inst.id});
            unit.unit_id = format_stable_id(unit.identity_hash);
            instance_slot.emplace(inst.id, slots.size());
            slots.push_back(std::move(unit));
        }
    }

    // Assign every composed entity to its unit by its id-path root: a top-level instance id lands
    // in that instance's unit (the whole subtree, including structural adds); anything else is the
    // root scene's own content.
    for (std::size_t i = 0; i < scene.entities.size(); ++i)
    {
        std::size_t slot = 0; // the root unit
        const std::vector<std::string>& id_path = scene.entities[i].id_path;
        if (!id_path.empty())
        {
            auto it = instance_slot.find(id_path.front());
            if (it != instance_slot.end())
                slot = it->second;
        }
        slots[slot].entity_indices.push_back(i);
    }

    // Emit only units that carry content, preserving the deterministic slot order (root first).
    for (ContentUnit& unit : slots)
        if (!unit.entity_indices.empty())
            out.units.push_back(std::move(unit));

    return out;
}

std::string content_units_json(const ContentUnitSet& units, const ComposedScene& scene)
{
    JsonValue root;
    root.type = JsonValue::Type::object;
    set_member(root, "rootScene", json_string(units.root_path));
    set_member(root, "unitCount", json_uint(static_cast<std::uint64_t>(units.units.size())));

    JsonValue array;
    array.type = JsonValue::Type::array;
    for (const ContentUnit& unit : units.units)
    {
        JsonValue obj;
        obj.type = JsonValue::Type::object;
        set_member(obj, "unitId", json_string(unit.unit_id));
        // Empty for every M2 unit (top-level granularity; the reserved unit-tree field, §2.1).
        set_member(obj, "parentUnit", json_string(""));
        set_member(obj, "isRoot", json_bool(unit.is_root));
        set_member(obj, "sourceScene", json_string(unit.source_scene));
        set_member(obj, "entityCount", json_uint(static_cast<std::uint64_t>(unit.entity_indices.size())));
        JsonValue identities;
        identities.type = JsonValue::Type::array;
        for (std::size_t idx : unit.entity_indices)
            identities.elements.push_back(json_string(format_stable_id(scene.entities[idx].identity_hash)));
        set_member(obj, "identities", std::move(identities));
        array.elements.push_back(std::move(obj));
    }
    set_member(root, "units", std::move(array));

    // Built from parsed strings (valid UTF-8) and unsigned integers — no doubles, so canonical
    // serialization cannot fail (mirrors composed_scene_json).
    std::string out;
    (void)serializer::serialize_canonical(root, out);
    return out;
}

ContentUnitResidency::ContentUnitResidency(const ContentUnitSet& units, const ComposedScene& scene)
{
    units_.reserve(units.units.size());
    for (const ContentUnit& source : units.units)
    {
        Unit unit;
        unit.unit_id = source.unit_id;
        unit.resident = false;
        unit.identities.reserve(source.entity_indices.size());
        for (std::size_t idx : source.entity_indices)
            unit.identities.push_back(scene.entities[idx].identity_hash);
        units_.push_back(std::move(unit));
    }
}

bool ContentUnitResidency::load(std::string_view unit_id)
{
    for (Unit& unit : units_)
        if (std::string_view(unit.unit_id) == unit_id)
        {
            unit.resident = true; // idempotent
            return true;
        }
    return false;
}

bool ContentUnitResidency::unload(std::string_view unit_id)
{
    for (Unit& unit : units_)
        if (std::string_view(unit.unit_id) == unit_id)
        {
            if (!unit.resident)
                return false;
            unit.resident = false;
            return true;
        }
    return false;
}

bool ContentUnitResidency::is_known(std::string_view unit_id) const
{
    for (const Unit& unit : units_)
        if (std::string_view(unit.unit_id) == unit_id)
            return true;
    return false;
}

bool ContentUnitResidency::is_resident(std::string_view unit_id) const
{
    for (const Unit& unit : units_)
        if (std::string_view(unit.unit_id) == unit_id)
            return unit.resident;
    return false;
}

std::size_t ContentUnitResidency::resident_unit_count() const noexcept
{
    std::size_t count = 0;
    for (const Unit& unit : units_)
        if (unit.resident)
            ++count;
    return count;
}

std::size_t ContentUnitResidency::resident_entity_count() const noexcept
{
    std::size_t count = 0;
    for (const Unit& unit : units_)
        if (unit.resident)
            count += unit.identities.size();
    return count;
}

std::vector<std::uint64_t> ContentUnitResidency::resident_identities() const
{
    std::vector<std::uint64_t> out;
    for (const Unit& unit : units_)
        if (unit.resident)
            out.insert(out.end(), unit.identities.begin(), unit.identities.end());
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace context::editor::compose
