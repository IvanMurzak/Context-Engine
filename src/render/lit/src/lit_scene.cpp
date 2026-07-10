// The reference lit scene — see lit_scene.h. The WGSL emitted here and the CPU reference (pbr.cpp)
// are mirrors: change either only in lockstep with the other.

#include "context/render/lit/lit_scene.h"

#include "context/kernel/world.h"

#include <cmath>
#include <cstring>
#include <locale>
#include <sstream>

namespace context::render::lit
{
namespace
{

// ----------------------------------------------------------------------------- scene constants

// Geometry (meters, y up).
constexpr float kGroundHalf = 2.0f;
constexpr float kBlockerY = 1.0f;
constexpr float kBlockerX0 = -1.0f;
constexpr float kBlockerX1 = -0.2f;
constexpr float kBlockerZ0 = -0.4f;
constexpr float kBlockerZ1 = 0.4f;

// Camera (orthographic top-down).
constexpr float kCameraHalf = 2.56f;
constexpr float kCameraNear = 0.1f;
constexpr float kCameraFar = 10.0f;

// Directional sun: authored direction of travel (deliberately UNNORMALIZED — the extract
// normalizes it, and that normalization is part of what the proof exercises).
constexpr float kSunDirRaw[3] = {0.6f, -1.0f, 0.0f};
constexpr float kSunColor[3] = {1.0f, 1.0f, 0.95f};
constexpr float kSunIntensity = 0.9f;

// Point lamp.
constexpr float kLampPos[3] = {-1.26f, 0.5f, -1.26f};
constexpr float kLampColor[3] = {1.0f, 0.85f, 0.6f};
constexpr float kLampIntensity = 1.0f;
constexpr float kLampRange = 1.5f;

// Ambient stub (constant IBL, v1 baseline).
constexpr float kAmbient[3] = {0.06f, 0.06f, 0.07f};

// Materials: 0 = ground, 1 = blocker.
constexpr float kGroundBase[3] = {0.75f, 0.75f, 0.75f};
constexpr float kGroundRoughness = 0.85f;
constexpr float kGroundMetallic = 0.0f;
constexpr float kBlockerBase[3] = {0.9f, 0.25f, 0.2f};
constexpr float kBlockerRoughness = 0.4f;
constexpr float kBlockerMetallic = 0.1f;

// The light-space ortho box + the shadow comparison bias (see lit_wgsl()).
constexpr float kLightHalf = 3.2f;
constexpr float kLightNear = 0.1f;
constexpr float kLightFar = 12.0f;
constexpr float kLightEyeDistance = 6.0f;
constexpr float kShadowBias = 0.005f;

// The constant lightmap texel (R-REND-006 input hook), as stored RGBA8 bytes.
constexpr std::uint8_t kLightmapTexel[4] = {64, 51, 26, 255};

[[nodiscard]] Vec3 sun_direction_unit()
{
    return normalize({kSunDirRaw[0], kSunDirRaw[1], kSunDirRaw[2]});
}

[[nodiscard]] PbrSurface ground_surface(const LitSceneConfig& config)
{
    PbrSurface s;
    s.base_color = config.override_ground_color
                       ? config.ground_color_override
                       : Vec3{kGroundBase[0], kGroundBase[1], kGroundBase[2]};
    s.metallic = kGroundMetallic;
    s.roughness = kGroundRoughness;
    return s;
}

[[nodiscard]] PbrSurface blocker_surface()
{
    PbrSurface s;
    s.base_color = {kBlockerBase[0], kBlockerBase[1], kBlockerBase[2]};
    s.metallic = kBlockerMetallic;
    s.roughness = kBlockerRoughness;
    return s;
}

void copy_mat4(float out[16], const Mat4& m)
{
    for (std::size_t i = 0; i < 16; ++i)
    {
        out[i] = m.m[i];
    }
}

} // namespace

Vec3 scene_vertex_position(std::uint32_t index)
{
    // Two CCW quads as triangle pairs (bl,br,tr / bl,tr,tl in the top-down view).
    const float g = kGroundHalf;
    const Vec3 table[12] = {
        // ground (y = 0), material 0
        {-g, 0.0f, -g}, {g, 0.0f, -g}, {g, 0.0f, g},
        {-g, 0.0f, -g}, {g, 0.0f, g},  {-g, 0.0f, g},
        // blocker (y = kBlockerY), material 1
        {kBlockerX0, kBlockerY, kBlockerZ0}, {kBlockerX1, kBlockerY, kBlockerZ0},
        {kBlockerX1, kBlockerY, kBlockerZ1}, {kBlockerX0, kBlockerY, kBlockerZ0},
        {kBlockerX1, kBlockerY, kBlockerZ1}, {kBlockerX0, kBlockerY, kBlockerZ1},
    };
    return table[index % 12u];
}

float scene_vertex_uv2_u(std::uint32_t index)
{
    if (index >= 6u) // blocker: constant uv2 (its lightmap hook is off)
    {
        return 0.5f;
    }
    const Vec3 p = scene_vertex_position(index);
    return (p.x + kGroundHalf) / (2.0f * kGroundHalf);
}

float scene_vertex_uv2_v(std::uint32_t index)
{
    if (index >= 6u)
    {
        return 0.5f;
    }
    const Vec3 p = scene_vertex_position(index);
    return (p.z + kGroundHalf) / (2.0f * kGroundHalf);
}

Mat4 camera_view_proj()
{
    const Mat4 view = look_at({0.0f, 5.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f});
    const Mat4 proj =
        ortho(-kCameraHalf, kCameraHalf, -kCameraHalf, kCameraHalf, kCameraNear, kCameraFar);
    return mul(proj, view);
}

Mat4 light_view_proj(Vec3 light_travel_dir)
{
    const Vec3 d = normalize(light_travel_dir);
    const Vec3 eye = scale(d, -kLightEyeDistance);
    // Up hint +Z: never parallel to the reference sun (which travels in the XZ-tilted-down plane).
    const Mat4 view = look_at(eye, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
    const Mat4 proj = ortho(-kLightHalf, kLightHalf, -kLightHalf, kLightHalf, kLightNear, kLightFar);
    return mul(proj, view);
}

const std::uint8_t* lightmap_texel_rgba()
{
    return kLightmapTexel;
}

Vec3 lightmap_texel_linear()
{
    return {static_cast<float>(kLightmapTexel[0]) / 255.0f,
            static_cast<float>(kLightmapTexel[1]) / 255.0f,
            static_cast<float>(kLightmapTexel[2]) / 255.0f};
}

void populate_reference_world(kernel::World& world)
{
    // Ground drawable: mesh_id 0, gray rough material carrying the lightmap INPUT hook (a nonzero
    // lightmap handle on the reserved UV2 channel — R-REND-006; the handle resolves to the proof's
    // single bound lightmap; a real asset registry is a later wave).
    const kernel::Entity ground = world.create();
    world.add<Transform>(ground, Transform{});
    world.add<Renderable>(ground, Renderable{{1.0f, 1.0f, 1.0f, 1.0f}, 0u});
    PbrMaterial ground_mat;
    ground_mat.base_color[0] = kGroundBase[0];
    ground_mat.base_color[1] = kGroundBase[1];
    ground_mat.base_color[2] = kGroundBase[2];
    ground_mat.metallic = kGroundMetallic;
    ground_mat.roughness = kGroundRoughness;
    ground_mat.lightmap_tex = 1u;
    ground_mat.lightmap_uv_channel = 1u;
    world.add<PbrMaterial>(ground, ground_mat);

    // Blocker drawable: mesh_id 1, red-ish material, no lightmap.
    const kernel::Entity blocker = world.create();
    world.add<Transform>(blocker, Transform{});
    world.add<Renderable>(blocker, Renderable{{1.0f, 1.0f, 1.0f, 1.0f}, 1u});
    PbrMaterial blocker_mat;
    blocker_mat.base_color[0] = kBlockerBase[0];
    blocker_mat.base_color[1] = kBlockerBase[1];
    blocker_mat.base_color[2] = kBlockerBase[2];
    blocker_mat.metallic = kBlockerMetallic;
    blocker_mat.roughness = kBlockerRoughness;
    world.add<PbrMaterial>(blocker, blocker_mat);

    // The shadow-casting sun (authored unnormalized; the extract normalizes).
    const kernel::Entity sun = world.create();
    DirectionalLight sun_light;
    sun_light.direction[0] = kSunDirRaw[0];
    sun_light.direction[1] = kSunDirRaw[1];
    sun_light.direction[2] = kSunDirRaw[2];
    sun_light.color[0] = kSunColor[0];
    sun_light.color[1] = kSunColor[1];
    sun_light.color[2] = kSunColor[2];
    sun_light.intensity = kSunIntensity;
    sun_light.cast_shadows = true;
    world.add<DirectionalLight>(sun, sun_light);

    // The point lamp (position via Transform, as the extract requires).
    const kernel::Entity lamp = world.create();
    Transform lamp_transform;
    lamp_transform.position[0] = kLampPos[0];
    lamp_transform.position[1] = kLampPos[1];
    lamp_transform.position[2] = kLampPos[2];
    world.add<Transform>(lamp, lamp_transform);
    PointLight lamp_light;
    lamp_light.color[0] = kLampColor[0];
    lamp_light.color[1] = kLampColor[1];
    lamp_light.color[2] = kLampColor[2];
    lamp_light.intensity = kLampIntensity;
    lamp_light.range = kLampRange;
    world.add<PointLight>(lamp, lamp_light);
}

SceneUniformData pack_scene_uniform(const RenderSnapshot& snapshot, const LitSceneConfig& config)
{
    SceneUniformData out;
    copy_mat4(out.camera_vp, camera_view_proj());

    // Directional slot: the FIRST extracted directional light; an empty list disables the slot
    // (the absent-light edge — the scene still renders on ambient alone). The direction always
    // stays a unit vector (a placeholder when absent) so the shader's normalize(-dir) can never
    // manufacture a NaN that a zero enable-flag would then fail to zero out.
    Vec3 sun_dir = {0.0f, -1.0f, 0.0f};
    const bool have_sun = !snapshot.directional_lights.empty();
    bool sun_casts_shadows = false;
    if (have_sun)
    {
        const DirectionalLight& sun = snapshot.directional_lights.front().light;
        sun_dir = {sun.direction[0], sun.direction[1], sun.direction[2]};
        out.dir_dir[3] = sun.intensity * config.dir_intensity_scale;
        out.dir_color[0] = sun.color[0];
        out.dir_color[1] = sun.color[1];
        out.dir_color[2] = sun.color[2];
        sun_casts_shadows = sun.cast_shadows;
    }
    out.dir_dir[0] = sun_dir.x;
    out.dir_dir[1] = sun_dir.y;
    out.dir_dir[2] = sun_dir.z;
    out.dir_color[3] = (have_sun && config.dir_enabled) ? 1.0f : 0.0f;
    copy_mat4(out.light_vp, light_view_proj(sun_dir));

    // Point slot: the FIRST extracted point light; disabled folds into intensity 0.
    if (!snapshot.point_lights.empty() && config.point_enabled)
    {
        const PointLightItem& lamp = snapshot.point_lights.front();
        out.point_pos[0] = lamp.position[0];
        out.point_pos[1] = lamp.position[1];
        out.point_pos[2] = lamp.position[2];
        out.point_pos[3] = lamp.light.range;
        out.point_color[0] = lamp.light.color[0];
        out.point_color[1] = lamp.light.color[1];
        out.point_color[2] = lamp.light.color[2];
        out.point_color[3] = lamp.light.intensity;
    }

    out.ambient[0] = kAmbient[0];
    out.ambient[1] = kAmbient[1];
    out.ambient[2] = kAmbient[2];
    out.ambient[3] = config.lightmap_enabled ? 1.0f : 0.0f;

    // Top-down ortho camera: the view vector toward the camera is +Y everywhere.
    out.view_shadow[0] = 0.0f;
    out.view_shadow[1] = 1.0f;
    out.view_shadow[2] = 0.0f;
    out.view_shadow[3] = (config.shadows_enabled && sun_casts_shadows) ? 1.0f : 0.0f;

    // Materials keyed by mesh_id (0 = ground, 1 = blocker).
    for (const RenderItem& item : snapshot.items)
    {
        if (item.renderable.mesh_id > 1u)
        {
            continue;
        }
        LitMaterialUniform& slot = out.materials[item.renderable.mesh_id];
        const PbrMaterial& m = item.material;
        for (int c = 0; c < 4; ++c)
        {
            slot.base_color[c] = m.base_color[c];
        }
        slot.factors[0] = m.metallic;
        slot.factors[1] = m.roughness;
        slot.factors[2] = (m.lightmap_tex != 0u) ? 1.0f : 0.0f;
        slot.factors[3] = 0.0f;
        for (int c = 0; c < 3; ++c)
        {
            slot.emissive[c] = m.emissive[c];
        }
        slot.emissive[3] = 0.0f;
    }
    if (config.override_ground_color)
    {
        out.materials[0].base_color[0] = config.ground_color_override.x;
        out.materials[0].base_color[1] = config.ground_color_override.y;
        out.materials[0].base_color[2] = config.ground_color_override.z;
    }
    return out;
}

std::string lit_wgsl()
{
    std::ostringstream w;
    w.imbue(std::locale::classic()); // force '.' decimals: non-classic locales emit invalid WGSL
    w.setf(std::ios::fixed);
    w.precision(6);

    w << "struct LitMaterial {\n"
         "    base_color : vec4f,\n"
         "    factors : vec4f, // x metallic, y roughness, z lightmap_on, w pad\n"
         "    emissive : vec4f,\n"
         "}\n"
         "\n"
         "struct Scene {\n"
         "    camera_vp : mat4x4f,\n"
         "    light_vp : mat4x4f,\n"
         "    dir_dir : vec4f,     // xyz direction of travel, w intensity\n"
         "    dir_color : vec4f,   // rgb, w enabled\n"
         "    point_pos : vec4f,   // xyz, w range\n"
         "    point_color : vec4f, // rgb, w intensity\n"
         "    ambient : vec4f,     // rgb, w lightmap_enabled\n"
         "    view_shadow : vec4f, // xyz view dir toward camera, w shadows_enabled\n"
         "    materials : array<LitMaterial, 2>,\n"
         "}\n"
         "\n"
         "@group(0) @binding(0) var<uniform> scene : Scene;\n"
         "@group(0) @binding(1) var shadow_map : texture_depth_2d;\n"
         "@group(0) @binding(2) var shadow_sampler : sampler_comparison;\n"
         "@group(0) @binding(3) var lightmap_tex : texture_2d<f32>;\n"
         "@group(0) @binding(4) var lightmap_sampler : sampler;\n"
         "\n";

    // The baked scene geometry — emitted from the SAME CPU tables the local unit tests pin.
    w << "fn scene_position(i : u32) -> vec3f {\n"
         "    var p = array<vec3f, 12>(\n";
    for (std::uint32_t i = 0; i < scene_vertex_count(); ++i)
    {
        const Vec3 p = scene_vertex_position(i);
        w << "        vec3f(" << p.x << ", " << p.y << ", " << p.z << ")"
          << (i + 1 < scene_vertex_count() ? "," : ");") << "\n";
    }
    w << "    return p[i];\n"
         "}\n"
         "\n"
         "fn scene_uv2(i : u32) -> vec2f {\n"
         "    var p = array<vec2f, 12>(\n";
    for (std::uint32_t i = 0; i < scene_vertex_count(); ++i)
    {
        w << "        vec2f(" << scene_vertex_uv2_u(i) << ", " << scene_vertex_uv2_v(i) << ")"
          << (i + 1 < scene_vertex_count() ? "," : ");") << "\n";
    }
    w << "    return p[i];\n"
         "}\n"
         "\n";

    w << "struct VsOut {\n"
         "    @builtin(position) clip : vec4f,\n"
         "    @location(0) world : vec3f,\n"
         "    @location(1) uv2 : vec2f,\n"
         "    @location(2) @interpolate(flat) mi : u32,\n"
         "}\n"
         "\n"
         "@vertex\n"
         "fn vs_main(@builtin(vertex_index) i : u32) -> VsOut {\n"
         "    let p = scene_position(i);\n"
         "    var v : VsOut;\n"
         "    v.clip = scene.camera_vp * vec4f(p, 1.0);\n"
         "    v.world = p;\n"
         "    v.uv2 = scene_uv2(i);\n"
         "    v.mi = select(0u, 1u, i >= 6u);\n"
         "    return v;\n"
         "}\n"
         "\n"
         "// The depth-only shadow pass (no fragment stage): the scene from the light's view.\n"
         "@vertex\n"
         "fn vs_shadow(@builtin(vertex_index) i : u32) -> @builtin(position) vec4f {\n"
         "    return scene.light_vp * vec4f(scene_position(i), 1.0);\n"
         "}\n"
         "\n";

    // The CPU mirror of pbr_brdf/point_attenuation/shade_reference lives in pbr.cpp — change only
    // in lockstep (the GPU proof asserts readback against those CPU values).
    w << "fn pbr_brdf(n : vec3f, v : vec3f, l : vec3f, base : vec3f, metallic : f32, "
         "roughness : f32) -> vec3f {\n"
         "    let ndl = max(dot(n, l), 0.0);\n"
         "    let ndv = max(dot(n, v), 0.0001);\n"
         "    let h = normalize(l + v);\n"
         "    let ndh = max(dot(n, h), 0.0);\n"
         "    let vdh = max(dot(v, h), 0.0);\n"
         "    let a = roughness * roughness;\n"
         "    let a2 = a * a;\n"
         "    let denom = ndh * ndh * (a2 - 1.0) + 1.0;\n"
         "    let d = a2 / (3.14159265 * denom * denom);\n"
         "    let k = (roughness + 1.0) * (roughness + 1.0) / 8.0;\n"
         "    let gv = ndv / (ndv * (1.0 - k) + k);\n"
         "    let gl = ndl / (ndl * (1.0 - k) + k);\n"
         "    let f0 = mix(vec3f(0.04, 0.04, 0.04), base, metallic);\n"
         "    let f = f0 + (vec3f(1.0, 1.0, 1.0) - f0) * pow(1.0 - vdh, 5.0);\n"
         "    let spec = f * (d * gv * gl / max(4.0 * ndv * ndl, 0.0001));\n"
         "    let diffuse = base * (1.0 - metallic);\n"
         "    return (diffuse + spec) * ndl;\n"
         "}\n"
         "\n"
         "fn point_atten(dist : f32, range : f32) -> f32 {\n"
         "    if (range <= 0.0) {\n"
         "        return 0.0;\n"
         "    }\n"
         "    let x = clamp(1.0 - dist / range, 0.0, 1.0);\n"
         "    return x * x;\n"
         "}\n"
         "\n";

    // Shadow factor: light-space project, 4-tap PCF over the hardware 2x2 comparison filter
    // (PCF-class, R-REND-004). Out-of-frustum points are lit; the bias absorbs depth-interpolation
    // error (tolerance-aware for lavapipe — software-Vulkan depth precision differs from hardware).
    w << "fn shadow_factor(world : vec3f) -> f32 {\n"
         "    let lp = scene.light_vp * vec4f(world, 1.0);\n"
         "    let uv = lp.xy * vec2f(0.5, -0.5) + vec2f(0.5, 0.5);\n"
         "    let ref_depth = lp.z - " << kShadowBias << ";\n"
         "    var s = textureSampleCompare(shadow_map, shadow_sampler, uv, ref_depth, "
         "vec2i(-1, -1));\n"
         "    s += textureSampleCompare(shadow_map, shadow_sampler, uv, ref_depth, vec2i(1, -1));\n"
         "    s += textureSampleCompare(shadow_map, shadow_sampler, uv, ref_depth, vec2i(-1, 1));\n"
         "    s += textureSampleCompare(shadow_map, shadow_sampler, uv, ref_depth, vec2i(1, 1));\n"
         "    s *= 0.25;\n"
         "    let in_bounds = all(uv >= vec2f(0.0, 0.0)) && all(uv <= vec2f(1.0, 1.0)) &&\n"
         "                    lp.z >= 0.0 && lp.z <= 1.0;\n"
         "    let inside = select(0.0, 1.0, in_bounds);\n"
         "    return mix(1.0, s, inside * scene.view_shadow.w);\n"
         "}\n"
         "\n";

    w << "@fragment\n"
         "fn fs_main(frag : VsOut) -> @location(0) vec4f {\n"
         "    let n = vec3f(0.0, 1.0, 0.0); // both quads face +Y (baked geometry)\n"
         "    let v = scene.view_shadow.xyz;\n"
         "    let m = scene.materials[frag.mi];\n"
         "    let base = m.base_color.rgb;\n"
         "    let shadow = shadow_factor(frag.world);\n"
         "    // R-REND-006 lightmap INPUT hook: trivial sample path on the uv2 channel.\n"
         "    let lm = textureSample(lightmap_tex, lightmap_sampler, frag.uv2).rgb;\n"
         "    var color = scene.ambient.rgb * base + m.emissive.rgb;\n"
         "    color += lm * base * (m.factors.z * scene.ambient.w);\n"
         "    let ld = normalize(-scene.dir_dir.xyz);\n"
         "    color += pbr_brdf(n, v, ld, base, m.factors.x, m.factors.y) * scene.dir_color.rgb *\n"
         "             (scene.dir_dir.w * scene.dir_color.w * shadow);\n"
         "    let to_light = scene.point_pos.xyz - frag.world;\n"
         "    let pdist = length(to_light);\n"
         "    let pl = to_light / max(pdist, 0.0001);\n"
         "    color += pbr_brdf(n, v, pl, base, m.factors.x, m.factors.y) * "
         "scene.point_color.rgb *\n"
         "             (scene.point_color.w * point_atten(pdist, scene.point_pos.w));\n"
         "    return vec4f(clamp(color, vec3f(0.0, 0.0, 0.0), vec3f(1.0, 1.0, 1.0)), 1.0);\n"
         "}\n";

    return w.str();
}

Vec3 sample_world(SamplePoint point)
{
    switch (point)
    {
    case SamplePoint::ShadowedGround:
        return {0.1f, 0.0f, 0.0f};
    case SamplePoint::LitGround:
        return {1.2f, 0.0f, 0.0f};
    case SamplePoint::BlockerTop:
        return {-0.5f, kBlockerY, 0.0f};
    case SamplePoint::PointLitGround:
        return {kLampPos[0], 0.0f, kLampPos[2]};
    case SamplePoint::PointFarGround:
        return {kLampPos[0], 0.0f, -kLampPos[2]};
    }
    return {0.0f, 0.0f, 0.0f};
}

PixelCoord sample_pixel(SamplePoint point)
{
    const Vec3 ndc = transform_point(camera_view_proj(), sample_world(point));
    const float size = static_cast<float>(lit_target_size());
    const float px = (ndc.x * 0.5f + 0.5f) * size;
    const float py = (0.5f - ndc.y * 0.5f) * size; // NDC y-up -> framebuffer row 0 at top
    // The probe points sit exactly ON pixel boundaries (world coordinates aligned to the 50px/unit
    // grid), where floor() would flip with the last float ulp across compilers/fp-contraction.
    // Round-to-nearest instead: maximally STABLE near-integer, and it picks the pixel just right/
    // below the boundary — whose center stays inside the same constant-color region for every probe.
    PixelCoord out;
    out.x = static_cast<std::uint32_t>(px + 0.5f);
    out.y = static_cast<std::uint32_t>(py + 0.5f);
    return out;
}

float expected_shadow_factor(Vec3 ground_point)
{
    const Vec3 d = sun_direction_unit();
    if (!(d.y < 0.0f))
    {
        return 1.0f; // degenerate (non-descending) sun — nothing casts onto the ground
    }
    // Walk from the point TOWARD the light (-d) up to the blocker plane and test its rectangle.
    const float t = (kBlockerY - ground_point.y) / (-d.y);
    if (t <= 0.0f)
    {
        return 1.0f; // at/above the blocker plane — the blocker cannot occlude it
    }
    const Vec3 hit = add(ground_point, scale(d, -t));
    const bool occluded =
        hit.x >= kBlockerX0 && hit.x <= kBlockerX1 && hit.z >= kBlockerZ0 && hit.z <= kBlockerZ1;
    return occluded ? 0.0f : 1.0f;
}

Vec3 expected_color(SamplePoint point, const LitSceneConfig& config)
{
    const bool on_blocker = point == SamplePoint::BlockerTop;

    ShadeInputs in;
    in.normal = {0.0f, 1.0f, 0.0f};
    in.view = {0.0f, 1.0f, 0.0f};
    in.surface = on_blocker ? blocker_surface() : ground_surface(config);
    in.ambient = {kAmbient[0], kAmbient[1], kAmbient[2]};
    in.shaded_position = sample_world(point);

    in.dir_enabled = config.dir_enabled;
    in.dir_direction = sun_direction_unit();
    in.dir_color = {kSunColor[0], kSunColor[1], kSunColor[2]};
    in.dir_intensity = kSunIntensity * config.dir_intensity_scale;
    in.shadow_factor = 1.0f;
    if (config.shadows_enabled && !on_blocker)
    {
        in.shadow_factor = expected_shadow_factor(in.shaded_position);
    }

    in.point_enabled = config.point_enabled;
    in.point_position = {kLampPos[0], kLampPos[1], kLampPos[2]};
    in.point_color = {kLampColor[0], kLampColor[1], kLampColor[2]};
    in.point_intensity = kLampIntensity;
    in.point_range = kLampRange;

    in.lightmap_enabled = config.lightmap_enabled && !on_blocker; // only the ground has the hook
    in.lightmap = lightmap_texel_linear();

    return shade_reference(in);
}

} // namespace context::render::lit
