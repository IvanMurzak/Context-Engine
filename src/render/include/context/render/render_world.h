// The render world — the double-buffered snapshot of render-relevant simulation state (L-39).
//
// L-39: an extract step copies render-relevant state out of the sim World into a separate,
// double-buffered render world; the render side reads ONLY that snapshot, so sim and render never
// tear and neither blocks the other. Headless = the extract step never runs (R-HEAD-002/003).
//
// Render defines its OWN render-relevant components (float, interpolatable) — distinct from the
// deterministic integer sim components in src/runtime/session/. A transform/renderable package
// populates them on entities; the extract reads them. The kernel does NOT depend on this header —
// render depends on the kernel, never the reverse.

#pragma once

#include "context/kernel/entity.h"

#include <cstdint>
#include <vector>

namespace context::render
{

// A render-relevant transform: float, world-space, interpolatable between fixed sim ticks (the
// render-side interpolation contract is R-SIM-002, a later wave). Rotation is a quaternion (xyzw).
struct Transform
{
    float position[3] = {0.0f, 0.0f, 0.0f};
    float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float scale[3] = {1.0f, 1.0f, 1.0f};
};

// A render-relevant tag: an entity carrying Renderable (alongside a Transform) is drawn. mesh_id is
// an opaque handle into a mesh/material registry (a later wave); color is a flat tint for now.
struct Renderable
{
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    std::uint32_t mesh_id = 0;
};

// The metallic-roughness PBR material state of one drawable (R-REND-004 baseline; the glTF-style
// parameter set the material contract names — see src/render/material/). Factors are unitless [0,1]
// per the R-DATA-006 vocabulary; texture inputs are opaque handles into an asset registry (a later
// wave) — 0 = "slot unused". The lightmap fields are the R-REND-006 INPUT hooks: a lightmap handle
// plus which UV channel it samples (channel 1 = the UV2 channel mesh import reserves); the baker
// that would fill them is explicitly post-v1.
struct PbrMaterial
{
    float base_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float metallic = 0.0f;
    float roughness = 0.5f;
    float emissive[3] = {0.0f, 0.0f, 0.0f};
    float occlusion_strength = 1.0f;
    std::uint32_t base_color_tex = 0;
    std::uint32_t metallic_roughness_tex = 0;
    std::uint32_t normal_tex = 0;
    std::uint32_t emissive_tex = 0;
    std::uint32_t occlusion_tex = 0;
    std::uint32_t lightmap_tex = 0;         // R-REND-006 hook: 0 = no baked lighting input
    std::uint32_t lightmap_uv_channel = 1;  // R-REND-006 hook: UV set index (1 = the reserved UV2)
};

// A directional real-time light (R-REND-004), a World component on any entity. direction is the
// world-space direction the light TRAVELS (toward the scene), normalized at extract; a zero-length
// authored direction is malformed and the extract skips the light. color is linear RGB; intensity a
// unitless multiplier (v1 baseline — photometric units are a later wave).
struct DirectionalLight
{
    float direction[3] = {0.0f, -1.0f, 0.0f};
    float color[3] = {1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    bool cast_shadows = true;
};

// A point real-time light (R-REND-004). Position comes from the entity's Transform — a PointLight
// on an entity WITHOUT a Transform has no position and is skipped by the extract. range is meters
// (SI, R-DATA-006); contribution fades to zero at range.
struct PointLight
{
    float color[3] = {1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float range = 10.0f;
};

// One extracted drawable: the entity it came from plus a copy of its render state. material is the
// entity's PbrMaterial when it carries one, else the default (drawables never require it).
struct RenderItem
{
    kernel::Entity entity;
    Transform transform;
    Renderable renderable;
    PbrMaterial material;
};

// One extracted directional light.
struct DirectionalLightItem
{
    kernel::Entity entity;
    DirectionalLight light; // direction normalized by the extract
};

// One extracted point light: the component plus the position copied from the entity's Transform.
struct PointLightItem
{
    kernel::Entity entity;
    PointLight light;
    float position[3] = {0.0f, 0.0f, 0.0f};
};

// A complete render-side snapshot of one sim tick's drawables + lights. A plain value type:
// copied/swapped by the double buffer, read by the render side with no reference back into the sim
// World (R-REND-003 — lights and materials are extracted state; render never mutates sim).
struct RenderSnapshot
{
    std::uint64_t sim_tick = 0;
    std::vector<RenderItem> items;
    std::vector<DirectionalLightItem> directional_lights;
    std::vector<PointLightItem> point_lights;

    void clear()
    {
        sim_tick = 0;
        items.clear();
        directional_lights.clear();
        point_lights.clear();
    }
};

// The L-39 double buffer: the sim/extract side writes the BACK snapshot; the render side reads the
// FRONT snapshot; swap() flips them at a tick boundary. Because the two never alias, the render side
// always reads a complete, consistent snapshot while the next one is being extracted — no tearing.
// Two buffers also give render-side interpolation its two endpoints (front = previous, back = next).
class RenderDoubleBuffer
{
public:
    // The snapshot the extract step writes into (the next frame).
    [[nodiscard]] RenderSnapshot& back() { return buffers_[back_index()]; }

    // The snapshot the render side reads (the current frame).
    [[nodiscard]] const RenderSnapshot& front() const { return buffers_[front_]; }

    // Publish the freshly-extracted back buffer as the new front. The old front becomes the next
    // back and is cleared before it is written again.
    void swap()
    {
        front_ = back_index();
        buffers_[back_index()].clear();
    }

private:
    [[nodiscard]] std::size_t back_index() const { return front_ ^ 1u; }

    RenderSnapshot buffers_[2];
    std::size_t front_ = 0;
};

} // namespace context::render
