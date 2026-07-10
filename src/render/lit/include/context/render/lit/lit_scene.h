// The reference lit scene (R-REND-004/006): a ground plane + a floating blocker panel, one
// directional light (shadow-casting) + one point light, metallic-roughness materials, and a
// constant 2x2 lightmap INPUT. The scene is authored as sim World components (DirectionalLight /
// PointLight / PbrMaterial, render_world.h), flows through the L-39 extract (R-REND-003 one-way),
// and is packed here into the GPU uniform layout the lit WGSL consumes — so the offscreen GPU proof
// exercises the REAL sim->render path end to end. Geometry is baked into the WGSL from the CPU
// tables below (the mesh/asset registry is a later wave), exactly the vertex-index pattern the
// sprite proof established: a local unit test pins the same tables the Linux-CI GPU proof draws.
//
// Layout sketch (world units are meters, R-DATA-006; y up):
//   * ground quad: y = 0, x/z in [-2, 2], normal +Y, material 0 (gray, rough) — carries the
//     lightmap hook (uv2 spans [0,1]^2).
//   * blocker quad: y = 1, x in [-1, -0.2], z in [-0.4, 0.4], normal +Y, material 1 (red-ish).
//   * directional light travels (0.6, -1, 0)/|.| — the blocker's shadow lands on the ground at the
//     blocker footprint shifted +0.6 in x (x in [-0.4, 0.4]): the sub-range x in [-0.2, 0.4] is
//     SHADOWED GROUND VISIBLE from the top-down camera (not covered by the blocker).
//   * point light hovers 0.5 above the ground at (-1.26, 0.5, -1.26), range 1.5 — far from the
//     shadow region, so the shadow assertions and the falloff assertions never interact.
//   * camera: orthographic top-down (eye (0,5,0), up hint -Z), half-extent 2.56 over a 256x256
//     target => world (x,z) maps to pixel ((x+2.56)*50, (z+2.56)*50) — analytically pinned below.

#pragma once

#include "context/render/lit/lit_math.h"
#include "context/render/lit/pbr.h"
#include "context/render/render_world.h"

#include <cstdint>
#include <string>

namespace context::kernel
{
class World;
}

namespace context::render::lit
{

// ------------------------------------------------------------------------- GPU uniform layout
// The C++ mirror of the WGSL `Scene` uniform block (lit_wgsl()). Plain floats, 16-byte-aligned
// vec4 slots, 48-byte material elements — byte-compatible with the WGSL uniform address space.

struct LitMaterialUniform
{
    float base_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float factors[4] = {0.0f, 0.5f, 0.0f, 0.0f};  // x metallic, y roughness, z lightmap_on, w pad
    float emissive[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // rgb emissive, w pad
};

struct SceneUniformData
{
    float camera_vp[16] = {};
    float light_vp[16] = {};
    float dir_dir[4] = {};      // xyz unit direction of travel, w intensity
    float dir_color[4] = {};    // rgb color, w enabled (0/1)
    float point_pos[4] = {};    // xyz position, w range (meters)
    float point_color[4] = {};  // rgb color, w intensity (0 when disabled/absent)
    float ambient[4] = {};      // rgb constant-IBL stub, w lightmap_enabled (0/1)
    float view_shadow[4] = {};  // xyz view dir toward camera, w shadows_enabled (0/1)
    LitMaterialUniform materials[2];
};

static_assert(sizeof(LitMaterialUniform) == 48, "must match the WGSL material element stride");
static_assert(sizeof(SceneUniformData) == 320, "must match the WGSL Scene uniform block size");

// --------------------------------------------------------------------------- render variants
// The per-render knobs of the proof. The World stays FIXED (one authored scene); each variant is a
// pack-time override — which is exactly how a real frame varies uniforms without touching the sim.
struct LitSceneConfig
{
    bool dir_enabled = true;
    bool point_enabled = true;
    bool shadows_enabled = true;
    bool lightmap_enabled = false;      // the R-REND-006 trivial sample path toggle
    float dir_intensity_scale = 1.0f;   // scales the EXTRACTED light's intensity (param response)
    bool override_ground_color = false; // material-parameter response probe
    Vec3 ground_color_override = {0.3f, 0.5f, 0.9f};
};

// ------------------------------------------------------------------------------ scene tables

// Render-target / shadow-map size (square).
[[nodiscard]] constexpr std::uint32_t lit_target_size()
{
    return 256;
}

// The 12 baked world-space vertices (two quads, CCW pairs of triangles): 0..5 ground, 6..11
// blocker. All normals are +Y. Index i -> material index (0 ground, 1 blocker) = i >= 6.
[[nodiscard]] Vec3 scene_vertex_position(std::uint32_t index);
[[nodiscard]] float scene_vertex_uv2_u(std::uint32_t index);
[[nodiscard]] float scene_vertex_uv2_v(std::uint32_t index);
[[nodiscard]] constexpr std::uint32_t scene_vertex_count()
{
    return 12;
}

// The camera and light view-projection matrices (column-major, WebGPU clip conventions).
[[nodiscard]] Mat4 camera_view_proj();
[[nodiscard]] Mat4 light_view_proj(Vec3 light_travel_dir);

// The lightmap INPUT texel (R-REND-006 hook): a constant 2x2 RGBA8 lightmap. Byte value and its
// exact linear float (byte / 255) so CPU expectations match the GPU sample bit-for-bit.
[[nodiscard]] const std::uint8_t* lightmap_texel_rgba(); // 4 bytes, every texel identical
[[nodiscard]] Vec3 lightmap_texel_linear();

// ------------------------------------------------------------------- authoring + extraction

// Populate `world` with the authored reference scene: ground + blocker drawables (Transform +
// Renderable + PbrMaterial), the shadow-casting directional sun (authored UNNORMALIZED — the
// extract normalizes), and the point lamp (Transform + PointLight). mesh_id 0 = ground quad,
// 1 = blocker quad (the baked-geometry registry stand-in).
void populate_reference_world(kernel::World& world);

// Pack an extracted snapshot + a variant config into the GPU uniform block. Reads ONLY snapshot
// state (R-REND-003): the first extracted directional light and first extracted point light drive
// the light slots (absent lights disable their slot — the absent-light edge path); materials come
// from the items' PbrMaterial keyed by mesh_id. `config` applies the render-variant overrides.
[[nodiscard]] SceneUniformData pack_scene_uniform(const RenderSnapshot& snapshot,
                                                  const LitSceneConfig& config);

// ------------------------------------------------------------------------------- WGSL source
// The lit shader module: vs_main/fs_main (the PBR + shadow-PCF + lightmap-hook main pass) and
// vs_shadow (the depth-only shadow pass) over ONE uniform block. Bind group 0:
//   @binding(0) uniform Scene, @binding(1) shadow depth texture, @binding(2) comparison sampler,
//   @binding(3) lightmap texture, @binding(4) lightmap sampler.
// Hand-authored WGSL, so these bindings are the shader's own contract; pipelines reflect their
// layouts from it (rhi.h auto-layout — see IBindGroupLayout for the T3d Tint-renumbering note).
[[nodiscard]] std::string lit_wgsl();

// -------------------------------------------------------------------------- analytic samples

// Named readback probe points of the proof, all well clear of shadow/penumbra/geometry edges.
enum class SamplePoint
{
    ShadowedGround, // (0.1, 0, 0): in the blocker's shadow, NOT under the blocker
    LitGround,      // (1.2, 0, 0): open ground, same material/normal as ShadowedGround
    BlockerTop,     // (-0.5, 1, 0): on the blocker's lit top face
    PointLitGround, // (-1.26, 0, -1.26): directly under the point lamp (0.5 below it)
    PointFarGround, // (-1.26, 0, 1.26): symmetric ground point beyond the lamp's range
};

[[nodiscard]] Vec3 sample_world(SamplePoint point);

struct PixelCoord
{
    std::uint32_t x = 0;
    std::uint32_t y = 0;
};

// The framebuffer pixel a sample point projects to (through camera_view_proj on the 256x256
// target) — the CPU-pinned mapping the GPU proof reads back at.
[[nodiscard]] PixelCoord sample_pixel(SamplePoint point);

// Analytic shadow factor of a GROUND point under the reference directional light: 0 when the ray
// toward the light hits the blocker rectangle, else 1. (Sample points sit far from the penumbra,
// so the PCF result at them is exactly 0 or 1.)
[[nodiscard]] float expected_shadow_factor(Vec3 ground_point);

// The CPU-reference expected linear color at a sample point under `config` — shade_reference()
// with the scene's analytic inputs. The GPU readback must match within a small unorm tolerance.
[[nodiscard]] Vec3 expected_color(SamplePoint point, const LitSceneConfig& config);

} // namespace context::render::lit
