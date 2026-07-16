// EditorContentSource (see editor_source.h): the in-memory play-in-editor feed of the loading seam.

#include "context/runtime/content/editor_source.h"

#include <algorithm>
#include <utility>

namespace context::runtime::content
{

EditorContentSource::EditorContentSource(std::vector<LoadedUnit> units)
{
    for (LoadedUnit& u : units)
        units_.emplace(u.unit_id, std::move(u));
    rebuild_directory();
}

void EditorContentSource::replace_unit(LoadedUnit unit)
{
    const std::uint64_t id = unit.unit_id;
    units_[id] = std::move(unit);
    rebuild_directory();
}

bool EditorContentSource::remove_unit(std::uint64_t unit_id)
{
    if (units_.erase(unit_id) == 0)
        return false;
    rebuild_directory();
    return true;
}

bool EditorContentSource::contains(std::uint64_t unit_id) const
{
    return units_.find(unit_id) != units_.end();
}

bool EditorContentSource::load(std::uint64_t unit_id, LoadedUnit& out, std::string* error) const
{
    const auto it = units_.find(unit_id);
    if (it == units_.end())
    {
        if (error != nullptr)
            *error = "content.unknown_unit";
        return false;
    }
    out = it->second; // a copy — the loader owns the materialized unit independent of later re-derives
    return true;
}

void EditorContentSource::rebuild_directory()
{
    // A deterministic directory order (ascending unit id) — the seam's directory has no authored
    // order once units live in a map, and a stable order keeps the loader's planning reproducible.
    directory_.clear();
    directory_.reserve(units_.size());
    for (const auto& [id, unit] : units_)
    {
        UnitDescriptor desc;
        desc.unit_id = unit.unit_id;
        desc.parent_unit = unit.parent_unit;
        desc.is_root = unit.is_root;
        desc.is_sidecar = unit.is_sidecar;
        desc.entity_count = unit.entities.size();
        desc.resident_bytes = unit.resident_bytes;
        directory_.push_back(desc);
    }
    std::sort(directory_.begin(), directory_.end(),
              [](const UnitDescriptor& a, const UnitDescriptor& b) { return a.unit_id < b.unit_id; });
}

} // namespace context::runtime::content
