// The reference lit scene: geometry tables, analytically-pinned sample-point projection and shadow
// occlusion, the World -> extract -> pack_scene_uniform path (R-REND-003 one-way; absent-light
// edges), the CPU-expected colors the GPU proof asserts against (incl. the margins it relies on),
// and the generated WGSL's shape (R-REND-004/006).

#include "context/render/extract.h"
#include "context/render/lit/lit_scene.h"

#include "context/kernel/world.h"

#include <cmath>
#include <string>

#include "render_test.h"

using namespace context::render::lit;
using context::render::RenderSnapshot;

namespace
{

bool near_f(float a, float b, float eps = 1.0e-5f)
{
    return std::fabs(a - b) <= eps;
}

float lum(Vec3 c)
{
    return (c.x + c.y + c.z) / 3.0f;
}

void test_geometry_tables()
{
    CHECK(scene_vertex_count() == 12u);
    for (std::uint32_t i = 0; i < 6u; ++i) // ground
    {
        CHECK(scene_vertex_position(i).y == 0.0f);
        CHECK(scene_vertex_uv2_u(i) >= 0.0f && scene_vertex_uv2_u(i) <= 1.0f);
        CHECK(scene_vertex_uv2_v(i) >= 0.0f && scene_vertex_uv2_v(i) <= 1.0f);
    }
    for (std::uint32_t i = 6u; i < 12u; ++i) // blocker
    {
        CHECK(scene_vertex_position(i).y == 1.0f);
        CHECK(scene_vertex_position(i).x >= -1.0f && scene_vertex_position(i).x <= -0.2f);
        CHECK(scene_vertex_position(i).z >= -0.4f && scene_vertex_position(i).z <= 0.4f);
    }
}

void test_sample_points_project_to_expected_pixels()
{
    // The top-down ortho camera maps world (x,z) to pixel ((x+2.56)*50, (z+2.56)*50) on the
    // 256x256 target — pinned here so the GPU proof reads back exactly these texels.
    const PixelCoord shadowed = sample_pixel(SamplePoint::ShadowedGround);
    CHECK(shadowed.x == 133u && shadowed.y == 128u);
    const PixelCoord lit = sample_pixel(SamplePoint::LitGround);
    CHECK(lit.x == 188u && lit.y == 128u);
    const PixelCoord blocker = sample_pixel(SamplePoint::BlockerTop);
    CHECK(blocker.x == 103u && blocker.y == 128u);
    const PixelCoord point_near = sample_pixel(SamplePoint::PointLitGround);
    CHECK(point_near.x == 65u && point_near.y == 65u);
    const PixelCoord point_far = sample_pixel(SamplePoint::PointFarGround);
    CHECK(point_far.x == 65u && point_far.y == 191u);

    // Every probe lands inside the target.
    for (SamplePoint p : {SamplePoint::ShadowedGround, SamplePoint::LitGround,
                          SamplePoint::BlockerTop, SamplePoint::PointLitGround,
                          SamplePoint::PointFarGround})
    {
        CHECK(sample_pixel(p).x < lit_target_size());
        CHECK(sample_pixel(p).y < lit_target_size());
    }
}

void test_analytic_shadow_occlusion()
{
    // The blocker footprint shifted +0.6 in x is the ground shadow (x in [-0.4, 0.4]).
    CHECK(expected_shadow_factor(sample_world(SamplePoint::ShadowedGround)) == 0.0f);
    CHECK(expected_shadow_factor(sample_world(SamplePoint::LitGround)) == 1.0f);
    CHECK(expected_shadow_factor(sample_world(SamplePoint::PointLitGround)) == 1.0f);
    CHECK(expected_shadow_factor(sample_world(SamplePoint::PointFarGround)) == 1.0f);
    CHECK(expected_shadow_factor({-0.3f, 0.0f, 0.0f}) == 0.0f); // inside the shifted footprint
    CHECK(expected_shadow_factor({0.5f, 0.0f, 0.0f}) == 1.0f);  // just past its +x edge
    CHECK(expected_shadow_factor({0.0f, 0.0f, 0.6f}) == 1.0f);  // past its +z edge
}

void test_world_extract_pack_roundtrip()
{
    context::kernel::World world;
    populate_reference_world(world);

    RenderSnapshot snap;
    context::render::extract_render_world(world, 1u, snap);
    CHECK(snap.items.size() == 2u);
    CHECK(snap.directional_lights.size() == 1u);
    CHECK(snap.point_lights.size() == 1u);

    const LitSceneConfig config;
    const SceneUniformData u = pack_scene_uniform(snap, config);

    // The packed sun mirrors the EXTRACTED (normalized) light.
    if (!snap.directional_lights.empty())
    {
        const auto& sun = snap.directional_lights.front().light;
        CHECK(near_f(u.dir_dir[0], sun.direction[0]));
        CHECK(near_f(u.dir_dir[1], sun.direction[1]));
        CHECK(near_f(u.dir_dir[2], sun.direction[2]));
        CHECK(near_f(u.dir_dir[3], sun.intensity));
        const float len_sq = u.dir_dir[0] * u.dir_dir[0] + u.dir_dir[1] * u.dir_dir[1] +
                             u.dir_dir[2] * u.dir_dir[2];
        CHECK(near_f(len_sq, 1.0f, 1.0e-4f)); // unit direction reached the GPU block
    }
    CHECK(u.dir_color[3] == 1.0f);   // enabled
    CHECK(u.view_shadow[3] == 1.0f); // the sun casts shadows and the config allows them

    // The packed lamp mirrors the extracted point light (position via Transform).
    if (!snap.point_lights.empty())
    {
        const auto& lamp = snap.point_lights.front();
        CHECK(near_f(u.point_pos[0], lamp.position[0]));
        CHECK(near_f(u.point_pos[1], lamp.position[1]));
        CHECK(near_f(u.point_pos[2], lamp.position[2]));
        CHECK(near_f(u.point_pos[3], lamp.light.range));
        CHECK(u.point_color[3] > 0.0f);
    }

    // Materials keyed by mesh_id; the ground (slot 0) carries the R-REND-006 lightmap hook.
    CHECK(u.materials[0].factors[2] == 1.0f); // ground lightmap_on
    CHECK(u.materials[1].factors[2] == 0.0f); // blocker has no hook
    CHECK(u.ambient[3] == 0.0f);              // hook present but the sample path is toggled off

    // Config toggles fold into the block without touching the snapshot.
    LitSceneConfig off = config;
    off.dir_enabled = false;
    off.shadows_enabled = false;
    off.lightmap_enabled = true;
    const SceneUniformData u2 = pack_scene_uniform(snap, off);
    CHECK(u2.dir_color[3] == 0.0f);
    CHECK(u2.view_shadow[3] == 0.0f);
    CHECK(u2.ambient[3] == 1.0f);

    // A light that does not cast shadows disables the shadow term even when the config allows it.
    RenderSnapshot no_shadow_snap = snap;
    if (!no_shadow_snap.directional_lights.empty())
    {
        no_shadow_snap.directional_lights.front().light.cast_shadows = false;
        const SceneUniformData u3 = pack_scene_uniform(no_shadow_snap, config);
        CHECK(u3.view_shadow[3] == 0.0f);
    }
}

void test_pack_absent_lights_edge()
{
    // An EMPTY snapshot (no lights at all) packs a disabled, NaN-free block: the scene renders on
    // ambient alone — the absent-light edge path.
    const RenderSnapshot empty;
    const SceneUniformData u = pack_scene_uniform(empty, LitSceneConfig{});
    CHECK(u.dir_color[3] == 0.0f);
    CHECK(u.point_color[3] == 0.0f);
    CHECK(u.view_shadow[3] == 0.0f);
    const float len_sq =
        u.dir_dir[0] * u.dir_dir[0] + u.dir_dir[1] * u.dir_dir[1] + u.dir_dir[2] * u.dir_dir[2];
    CHECK(near_f(len_sq, 1.0f)); // placeholder direction stays unit — normalize() can't NaN
    for (float f : u.light_vp)
    {
        CHECK(f == f); // no NaN reached the matrix either
    }
    // Default materials in both slots.
    CHECK(u.materials[0].base_color[0] == 1.0f);
    CHECK(u.materials[1].factors[2] == 0.0f);
}

void test_expected_colors_carry_the_gpu_margins()
{
    // Pre-verify ON CPU every margin the GPU proof asserts, so a scene/lighting retune that
    // erodes a margin fails HERE first (fast, local) rather than only on the CI GPU leg.
    const LitSceneConfig base;

    const float lit = lum(expected_color(SamplePoint::LitGround, base));
    const float shadowed = lum(expected_color(SamplePoint::ShadowedGround, base));
    CHECK(shadowed + 60.0f / 255.0f < lit); // shadow margin

    LitSceneConfig unlit = base;
    unlit.dir_enabled = false;
    unlit.point_enabled = false;
    CHECK(lum(expected_color(SamplePoint::LitGround, unlit)) + 60.0f / 255.0f < lit);

    LitSceneConfig dim = base;
    dim.dir_intensity_scale = 0.5f;
    CHECK(lum(expected_color(SamplePoint::LitGround, dim)) + 25.0f / 255.0f < lit);

    LitSceneConfig recolor = base;
    recolor.override_ground_color = true;
    const Vec3 recolored = expected_color(SamplePoint::LitGround, recolor);
    CHECK(recolored.z > recolored.x + 50.0f / 255.0f); // blue-dominant

    LitSceneConfig no_shadow = base;
    no_shadow.shadows_enabled = false;
    const Vec3 ns_shadowed = expected_color(SamplePoint::ShadowedGround, no_shadow);
    const Vec3 ns_lit = expected_color(SamplePoint::LitGround, no_shadow);
    CHECK(near_f(lum(ns_shadowed), lum(ns_lit), 1.0e-4f)); // both probes converge
    CHECK(lum(ns_shadowed) > shadowed + 60.0f / 255.0f);

    const float point_near = lum(expected_color(SamplePoint::PointLitGround, base));
    const float point_far = lum(expected_color(SamplePoint::PointFarGround, base));
    CHECK(point_near > point_far + 30.0f / 255.0f); // falloff margin

    // The lightmap hook delta is exactly lightmap * albedo on the ground, and the near-saturation
    // channel still has headroom (no hidden clamp corrupting the GPU delta assertion).
    LitSceneConfig lightmap = base;
    lightmap.lightmap_enabled = true;
    const Vec3 with_lm = expected_color(SamplePoint::LitGround, lightmap);
    const Vec3 without = expected_color(SamplePoint::LitGround, base);
    const Vec3 lm = lightmap_texel_linear();
    CHECK(near_f(with_lm.x - without.x, lm.x * 0.75f, 1.0e-4f));
    CHECK(near_f(with_lm.y - without.y, lm.y * 0.75f, 1.0e-4f));
    CHECK(near_f(with_lm.z - without.z, lm.z * 0.75f, 1.0e-4f));
    CHECK(with_lm.x < 0.99f); // headroom: the delta is not eaten by the [0,1] clamp
    const Vec3 blocker_with = expected_color(SamplePoint::BlockerTop, lightmap);
    const Vec3 blocker_without = expected_color(SamplePoint::BlockerTop, base);
    CHECK(near_f(blocker_with.x, blocker_without.x)); // hook-free material unaffected
    CHECK(expected_color(SamplePoint::PointLitGround, base).x < 0.995f); // clamp headroom
}

void test_wgsl_shape_and_determinism()
{
    const std::string wgsl = lit_wgsl();

    // The bind-group contract the proof builds against (reflected layouts — see rhi.h).
    CHECK(wgsl.find("@group(0) @binding(0) var<uniform> scene : Scene;") != std::string::npos);
    CHECK(wgsl.find("@group(0) @binding(1) var shadow_map : texture_depth_2d;") !=
          std::string::npos);
    CHECK(wgsl.find("@group(0) @binding(2) var shadow_sampler : sampler_comparison;") !=
          std::string::npos);
    CHECK(wgsl.find("@group(0) @binding(3) var lightmap_tex : texture_2d<f32>;") !=
          std::string::npos);
    CHECK(wgsl.find("@group(0) @binding(4) var lightmap_sampler : sampler;") != std::string::npos);

    // The three entry points (main pass + depth-only shadow pass).
    CHECK(wgsl.find("fn vs_main(") != std::string::npos);
    CHECK(wgsl.find("fn fs_main(") != std::string::npos);
    CHECK(wgsl.find("fn vs_shadow(") != std::string::npos);

    // The PCF comparison sampling and the lightmap sample path are present.
    CHECK(wgsl.find("textureSampleCompare(") != std::string::npos);
    CHECK(wgsl.find("textureSample(lightmap_tex") != std::string::npos);

    // Deterministic emission (a derivation-cache prerequisite for any authored-path reuse).
    CHECK(lit_wgsl() == wgsl);

    // Locale trap guard: every emitted decimal uses '.', never ',' (invalid WGSL).
    CHECK(wgsl.find("0,5") == std::string::npos);
    CHECK(wgsl.find(", ,") == std::string::npos);
}

} // namespace

int main()
{
    test_geometry_tables();
    test_sample_points_project_to_expected_pixels();
    test_analytic_shadow_occlusion();
    test_world_extract_pack_roundtrip();
    test_pack_absent_lights_edge();
    test_expected_colors_carry_the_gpu_margins();
    test_wgsl_shape_and_determinism();
    RENDER_TEST_MAIN_END();
}
