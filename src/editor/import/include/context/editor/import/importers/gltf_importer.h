// glTF importer (R-ASSET-001 / R-REND-006). Parses glTF 2.0 — both `.gltf` (JSON) and `.glb` (binary
// container: 12-byte header + JSON chunk + optional BIN chunk) — into a canonical MESH DESCRIPTOR:
// meshes -> primitives -> vertex attributes (POSITION / NORMAL / TANGENT / TEXCOORD_0), with a
// RESERVED UV2 (lightmap) channel per R-REND-006 (populated from TEXCOORD_1 when authored, else
// reserved-empty). Geometry decode + quantization (meshopt) is the transcode follow-up; the
// descriptor + UV2 reservation is the M2 deliverable ("mesh import reserves UV2", ROADMAP §1 M2).

#pragma once

#include "context/editor/import/importer.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace context::editor::import
{

// One primitive's attribute presence + the UV2 reservation (R-REND-006).
struct GltfPrimitiveInfo
{
    bool has_position = false;
    bool has_normal = false;
    bool has_tangent = false;
    bool has_texcoord0 = false; // base-color UVs
    bool has_texcoord1 = false; // authored lightmap UVs (feed the reserved UV2 slot)
    // UV2 is ALWAYS reserved in the descriptor (the R-REND-006 hook is a format contract, present
    // whether or not the source authored TEXCOORD_1) — `has_texcoord1` says whether it is populated.
    bool uv2_reserved = true;
    std::int64_t index_count = -1; // -1 = non-indexed (draw arrays)
    std::int64_t material = -1;    // material index, -1 = none
};

struct GltfMeshInfo
{
    std::string name;
    std::vector<GltfPrimitiveInfo> primitives;
};

struct GltfInfo
{
    std::string version;      // asset.version string ("2.0")
    bool is_glb = false;      // parsed from the binary container vs raw JSON
    std::size_t accessor_count = 0;
    std::size_t material_count = 0;
    std::vector<GltfMeshInfo> meshes;
};

// Parse glTF/GLB. Returns true and fills `info` on a well-formed asset; false with a diagnostic on a
// bad GLB header/chunk, non-JSON or non-object root, a missing/!"2.0" `asset.version`, or malformed
// mesh structure. Reuses the engine's canonical JSON parser (no third-party glTF lib). Never throws.
[[nodiscard]] bool parse_gltf(std::string_view bytes, GltfInfo& info,
                             std::vector<ImportDiagnostic>& diagnostics);

// The glTF importer: source ".gltf"/".glb" -> one ArtifactKind::mesh descriptor (UV2 reserved).
class GltfImporter final : public Importer
{
public:
    [[nodiscard]] std::string_view id() const noexcept override { return "gltf"; }
    [[nodiscard]] std::uint32_t version() const noexcept override { return 1; }
    [[nodiscard]] std::vector<std::string> extensions() const override { return {".gltf", ".glb"}; }
    [[nodiscard]] std::uint32_t derived_format_version(ArtifactKind kind) const noexcept override;
    [[nodiscard]] ImportResult import(const ImportInput& input) const override;
};

} // namespace context::editor::import
