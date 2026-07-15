// The panel-scoped mesh+UV data seam (M7 a10, R-UI-003; owner ruling d). Binds a UI panel's
// dynamic-texture target (its per-panel RTT, dynamic_texture.h) to a UV-mapped mesh
// (packages::ui::PanelMesh — world-space positions + ONE UV set + triangle indices). a9 sampled the
// panel's RTT onto a flat quad (render_world.h UiPanel); a10 generalizes the binding to an arbitrary
// UV-mapped mesh: the same dynamic texture, now sampled across the mesh's UV set.
//
// DELIBERATELY MINIMAL + PANEL-SCOPED (the T8 risk note): one mesh, one UV set, one texture handle,
// a flat tint — this is NOT the M8 asset-mesh registry (uploaded meshes / streamed geometry / a
// material graph). A panel owns its mesh; nothing here accretes a general mesh/material store. The
// mesh geometry itself is built provider-agnostically in the headless context_ui package
// (curved_panel.h's build_cylinder_panel_mesh), so the SAME mesh drives both this render binding and
// the a10 raycast->UV->events picking (raycast_panel.h) — one source of truth for the surface.

#pragma once

#include "context/packages/ui/curved_panel.h" // packages::ui::PanelMesh / PanelVertex / Vec3
#include "context/render/ui/dynamic_texture.h" // DynamicTextureId / kInvalidDynamicTexture

#include <cstddef>

namespace context::render::ui
{

// One panel's mesh binding: the UV-mapped surface the panel's dynamic texture is sampled onto, the
// handle of that texture (its RTT target in the DynamicTextureRegistry), and a flat tint multiplied
// over the sampled texels (1,1,1,1 == the panel's own colors — a9 UiPanel::tint parity). Presentation
// state (D6): the render side reads it to sample + place the panel; the sim World never sees it.
struct PanelMeshBinding
{
    packages::ui::PanelMesh mesh;                      // world-space verts + the panel-scoped UV set
    DynamicTextureId texture = kInvalidDynamicTexture; // the RTT the panel content renders into
    float tint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
};

// Bind `mesh` to the dynamic-texture `target` (its RTT handle). A convenience factory — the binding
// is a plain aggregate, but this documents the seam's intent at the call site.
[[nodiscard]] inline PanelMeshBinding bind_panel_mesh(packages::ui::PanelMesh mesh,
                                                      DynamicTextureId target,
                                                      const float tint[4] = nullptr)
{
    PanelMeshBinding b;
    b.mesh = std::move(mesh);
    b.texture = target;
    if (tint != nullptr)
    {
        b.tint[0] = tint[0];
        b.tint[1] = tint[1];
        b.tint[2] = tint[2];
        b.tint[3] = tint[3];
    }
    return b;
}

// A binding is renderable iff it has geometry AND a bound texture target.
[[nodiscard]] inline bool valid(const PanelMeshBinding& b) noexcept
{
    return !b.mesh.empty() && b.texture != kInvalidDynamicTexture;
}

// The world-space centroid of the bound mesh's vertices — the natural "look at the panel" probe point
// (the golden readback samples the panel texel here; a picker can aim a ray at it). {0,0,0} for an
// empty mesh.
[[nodiscard]] inline packages::ui::Vec3 panel_mesh_centroid(const packages::ui::PanelMesh& mesh)
{
    packages::ui::Vec3 c;
    if (mesh.vertices.empty())
    {
        return c;
    }
    for (const packages::ui::PanelVertex& v : mesh.vertices)
    {
        c.x += v.pos.x;
        c.y += v.pos.y;
        c.z += v.pos.z;
    }
    const float inv = 1.0f / static_cast<float>(mesh.vertices.size());
    return packages::ui::Vec3{c.x * inv, c.y * inv, c.z * inv};
}

} // namespace context::render::ui
