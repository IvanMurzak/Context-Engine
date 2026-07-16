// Shared test fixture for the runtime-content ctests: builds a ComposedScene + ContentUnitSet from
// in-memory scene docs, then produces BOTH feeds of the loading seam from that ONE source —
//   - a real v1 pack (the a01 writer)      -> the shipped-build feed (PackContentSource);
//   - a vector<LoadedUnit> from the flatten -> the play-in-editor feed (EditorContentSource).
// A TEST-ONLY use of the editor build side (context_compose + context_pack); the runtime loader
// library never depends on it (L-24 layering).

#pragma once

#include "context/runtime/content/content_source.h"

#include "context/editor/compose/content_unit.h"
#include "context/editor/compose/flatten.h"
#include "context/editor/compose/scene_model.h"
#include "context/editor/pack/pack_writer.h"
#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_parse.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace content_fixture
{

namespace compose = context::editor::compose;
namespace pack = context::editor::pack;
namespace serializer = context::editor::serializer;
namespace content = context::runtime::content;
using serializer::JsonValue;

// A resolver over an in-memory scene map (the compose test pattern).
class MapResolver final : public compose::SceneResolver
{
public:
    bool add(const std::string& path, const char* json)
    {
        serializer::ParseResult parsed = serializer::parse_json(json);
        if (!parsed.ok)
            return false;
        std::optional<compose::SceneDoc> doc = compose::build_scene_doc(path, parsed.root);
        if (!doc.has_value())
            return false;
        docs_[path] = std::move(*doc);
        return true;
    }
    [[nodiscard]] const compose::SceneDoc* resolve(std::string_view path) const override
    {
        auto it = docs_.find(std::string(path));
        return it == docs_.end() ? nullptr : &it->second;
    }

private:
    std::map<std::string, compose::SceneDoc, std::less<>> docs_;
};

// Build the v1 pack bytes for a flattened scene (the shipped-build feed). Returns "" on any failure.
[[nodiscard]] inline std::string make_pack(const compose::ContentUnitSet& units,
                                           const compose::ComposedScene& scene,
                                           const std::vector<pack::PackSidecar>& sidecars = {},
                                           std::uint64_t engine_version = 1)
{
    pack::PackWriteOptions options;
    options.engine_version = engine_version;
    pack::PackWriteResult written = pack::write_pack(units, scene, sidecars, options);
    return written.ok ? written.bytes : std::string{};
}

// Build the live in-editor derived units for a flattened scene (the play-in-editor feed) — the SAME
// identity + id-path + payload triples the packer serializes, taken straight from the ComposedScene
// (an independent path from the pack bytes, so the parity assertion is meaningful).
[[nodiscard]] inline std::vector<content::LoadedUnit>
make_editor_units(const compose::ContentUnitSet& units, const compose::ComposedScene& scene)
{
    std::vector<content::LoadedUnit> out;
    out.reserve(units.units.size());
    for (const compose::ContentUnit& u : units.units)
    {
        content::LoadedUnit lu;
        lu.unit_id = u.identity_hash;
        lu.parent_unit = 0;
        lu.is_root = u.is_root;
        lu.is_sidecar = false;
        std::uint64_t bytes = 0;
        for (std::size_t idx : u.entity_indices)
        {
            const compose::ComposedEntity& e = scene.entities[idx];
            content::UnitEntity ue;
            ue.identity = e.identity_hash;
            ue.id_path = e.id_path;
            ue.value = e.value;
            std::string canon;
            if (serializer::serialize_canonical(e.value, canon))
                bytes += canon.size();
            lu.entities.push_back(std::move(ue));
        }
        lu.resident_bytes = bytes; // an estimate — world_hash ignores it; only used for scheduling
        out.push_back(std::move(lu));
    }
    return out;
}

// The resident-byte cost a source's directory records for a unit (0 if the unit is unknown).
[[nodiscard]] inline std::uint64_t dir_bytes(const content::ContentSource& source,
                                             std::uint64_t unit_id)
{
    for (const content::UnitDescriptor& d : source.directory())
        if (d.unit_id == unit_id)
            return d.resident_bytes;
    return 0;
}

} // namespace content_fixture
