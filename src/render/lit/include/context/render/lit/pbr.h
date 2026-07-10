// The CPU reference of the metallic-roughness PBR shading the lit WGSL performs (R-REND-004
// baseline: Lambert diffuse + Cook-Torrance GGX specular, constant-ambient IBL stub — advanced
// lighting is v2). This C++ mirrors the WGSL in lit_scene.cpp FORMULA-FOR-FORMULA (same float
// precision, same clamps): the offscreen GPU proof asserts readback pixels against these CPU values
// within a tolerance, the same "unit-tested CPU math pins what the GPU draws" pattern the sprite
// path proved with its projection corners. Change one side only in lockstep with the other.
//
// Conventions: all vectors world-space and normalized where named `n`/`v`/`l`; colors linear RGB;
// factors unitless [0,1] (R-DATA-006); light intensity a unitless multiplier folding the punctual-
// light normalization (v1 baseline — photometric units are a later wave).

#pragma once

#include "context/render/lit/lit_math.h"

namespace context::render::lit
{

// The metallic-roughness surface parameters of one shaded point.
struct PbrSurface
{
    Vec3 base_color = {1.0f, 1.0f, 1.0f};
    float metallic = 0.0f;
    float roughness = 0.5f;
    Vec3 emissive = {0.0f, 0.0f, 0.0f};
};

// The Cook-Torrance direct term * ndotl for ONE light direction (no light color/intensity):
// Lambert diffuse (energy-split by metallic) + GGX-distribution / Schlick-GGX-geometry /
// Schlick-Fresnel specular. Returns zero when the light is at or below the horizon.
[[nodiscard]] Vec3 pbr_brdf(Vec3 n, Vec3 v, Vec3 l, const PbrSurface& surface);

// Point-light distance attenuation: (1 - d/range)^2, clamped; zero at/beyond range and for a
// non-positive range (the degenerate-light guard the WGSL applies identically).
[[nodiscard]] float point_attenuation(float dist, float range);

// Everything one shaded point needs — the exact input set the lit WGSL's fs_main consumes.
struct ShadeInputs
{
    Vec3 normal = {0.0f, 1.0f, 0.0f};
    Vec3 view = {0.0f, 1.0f, 0.0f}; // toward the camera
    PbrSurface surface;

    Vec3 ambient = {0.0f, 0.0f, 0.0f}; // constant-IBL stub, linear RGB

    // Directional light (R-REND-004): direction of TRAVEL (unit), color, unitless intensity.
    // enabled folds the "no directional light extracted" case (intensity path stays live).
    Vec3 dir_direction = {0.0f, -1.0f, 0.0f};
    Vec3 dir_color = {1.0f, 1.0f, 1.0f};
    float dir_intensity = 1.0f;
    bool dir_enabled = false;
    float shadow_factor = 1.0f; // 0 = fully shadowed, 1 = fully lit (the depth-pass PCF result)

    // Point light: position world-space, range meters (SI).
    Vec3 point_position = {0.0f, 0.0f, 0.0f};
    Vec3 point_color = {1.0f, 1.0f, 1.0f};
    float point_intensity = 1.0f;
    float point_range = 0.0f;
    bool point_enabled = false;
    Vec3 shaded_position = {0.0f, 0.0f, 0.0f}; // the world position being shaded

    // Lightmap INPUT hook (R-REND-006): the sampled lightmap texel (linear RGB) and whether the
    // hook is active (scene flag AND material slot). The trivial runtime sample path adds
    // lightmap * base_color; the baker producing real lightmaps is post-v1 and absent.
    Vec3 lightmap = {0.0f, 0.0f, 0.0f};
    bool lightmap_enabled = false;
};

// The full composition fs_main performs: ambient + emissive + lightmap-hook + shadowed directional
// + attenuated point, clamped to [0,1] (the RGBA8 render-target write).
[[nodiscard]] Vec3 shade_reference(const ShadeInputs& in);

} // namespace context::render::lit
