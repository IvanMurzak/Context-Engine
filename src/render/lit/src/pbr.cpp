// CPU reference of the lit WGSL's PBR shading — see pbr.h. Mirrors lit_scene.cpp's WGSL
// formula-for-formula; change only in lockstep.

#include "context/render/lit/pbr.h"

#include <algorithm>
#include <cmath>

namespace context::render::lit
{
namespace
{

constexpr float kPi = 3.14159265f;

[[nodiscard]] float clamp01(float x)
{
    return std::min(std::max(x, 0.0f), 1.0f);
}

} // namespace

Vec3 pbr_brdf(Vec3 n, Vec3 v, Vec3 l, const PbrSurface& surface)
{
    const float ndl = std::max(dot(n, l), 0.0f);
    const float ndv = std::max(dot(n, v), 1.0e-4f);
    const Vec3 h = normalize(add(l, v));
    const float ndh = std::max(dot(n, h), 0.0f);
    const float vdh = std::max(dot(v, h), 0.0f);

    // GGX normal distribution.
    const float a = surface.roughness * surface.roughness;
    const float a2 = a * a;
    const float denom = ndh * ndh * (a2 - 1.0f) + 1.0f;
    const float d = a2 / (kPi * denom * denom);

    // Schlick-GGX geometry (direct-lighting k).
    const float k = (surface.roughness + 1.0f) * (surface.roughness + 1.0f) / 8.0f;
    const float gv = ndv / (ndv * (1.0f - k) + k);
    const float gl = ndl / (ndl * (1.0f - k) + k);
    const float g = gv * gl;

    // Schlick Fresnel over the metallic-mixed F0.
    const float f0_dielectric = 0.04f;
    const Vec3 f0 = {f0_dielectric + (surface.base_color.x - f0_dielectric) * surface.metallic,
                     f0_dielectric + (surface.base_color.y - f0_dielectric) * surface.metallic,
                     f0_dielectric + (surface.base_color.z - f0_dielectric) * surface.metallic};
    const float fresnel = std::pow(1.0f - vdh, 5.0f);
    const Vec3 f = {f0.x + (1.0f - f0.x) * fresnel, f0.y + (1.0f - f0.y) * fresnel,
                    f0.z + (1.0f - f0.z) * fresnel};

    const float spec_scale = d * g / std::max(4.0f * ndv * ndl, 1.0e-4f);
    const Vec3 diffuse = scale(surface.base_color, 1.0f - surface.metallic);
    return scale(add(diffuse, scale(f, spec_scale)), ndl);
}

float point_attenuation(float dist, float range)
{
    if (range <= 0.0f)
    {
        return 0.0f;
    }
    const float x = clamp01(1.0f - dist / range);
    return x * x;
}

Vec3 shade_reference(const ShadeInputs& in)
{
    const Vec3 base = in.surface.base_color;

    // Ambient stub + emissive.
    Vec3 color = add(mul(in.ambient, base), in.surface.emissive);

    // Lightmap INPUT hook (R-REND-006): trivial sample path only — baker absent.
    if (in.lightmap_enabled)
    {
        color = add(color, mul(in.lightmap, base));
    }

    // Directional light, modulated by the shadow-map PCF factor.
    if (in.dir_enabled)
    {
        const Vec3 l = normalize(scale(in.dir_direction, -1.0f));
        const Vec3 direct = pbr_brdf(in.normal, in.view, l, in.surface);
        color = add(color, scale(mul(direct, in.dir_color),
                                 in.dir_intensity * in.shadow_factor));
    }

    // Point light with distance attenuation.
    if (in.point_enabled)
    {
        const Vec3 to_light = sub(in.point_position, in.shaded_position);
        const float dist = length(to_light);
        const Vec3 l = scale(to_light, 1.0f / std::max(dist, 1.0e-4f));
        const float atten = point_attenuation(dist, in.point_range);
        const Vec3 direct = pbr_brdf(in.normal, in.view, l, in.surface);
        color = add(color, scale(mul(direct, in.point_color), in.point_intensity * atten));
    }

    return {clamp01(color.x), clamp01(color.y), clamp01(color.z)};
}

} // namespace context::render::lit
