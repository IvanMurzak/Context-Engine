// The CPU PBR reference (the analytic oracle the GPU proof asserts against): physical sanity of
// the metallic-roughness BRDF — horizon cutoff, linear intensity response, roughness/metallic
// monotonicity — plus point-light attenuation edges and the shade_reference composition
// (ambient/emissive/shadow/lightmap-hook terms), R-REND-004/006.

#include "context/render/lit/pbr.h"

#include <cmath>

#include "render_test.h"

using namespace context::render::lit;

namespace
{

constexpr float kEps = 1.0e-6f;

bool near_f(float a, float b, float eps = kEps)
{
    return std::fabs(a - b) <= eps;
}

float lum(Vec3 c)
{
    return (c.x + c.y + c.z) / 3.0f;
}

PbrSurface gray_surface(float roughness = 0.5f, float metallic = 0.0f)
{
    PbrSurface s;
    s.base_color = {0.8f, 0.8f, 0.8f};
    s.metallic = metallic;
    s.roughness = roughness;
    return s;
}

void test_below_horizon_is_black()
{
    const Vec3 n{0, 1, 0};
    const Vec3 v{0, 1, 0};
    const Vec3 below{0, -1, 0}; // light coming from underneath the surface
    const Vec3 c = pbr_brdf(n, v, below, gray_surface());
    CHECK(near_f(c.x, 0.0f) && near_f(c.y, 0.0f) && near_f(c.z, 0.0f));

    const Vec3 grazing{1, 0, 0}; // exactly at the horizon: ndl = 0
    const Vec3 g = pbr_brdf(n, v, grazing, gray_surface());
    CHECK(near_f(g.x, 0.0f) && near_f(g.y, 0.0f) && near_f(g.z, 0.0f));
}

void test_brdf_positive_and_diffuse_dominated_when_rough()
{
    const Vec3 n{0, 1, 0};
    const Vec3 v{0, 1, 0};
    const Vec3 l = normalize({0.3f, 1.0f, 0.0f});
    const Vec3 c = pbr_brdf(n, v, l, gray_surface(0.9f));
    CHECK(c.x > 0.0f && c.y > 0.0f && c.z > 0.0f);
    // A rough dielectric is close to pure Lambert: brdf ~ base * ndl (within ~15%).
    const float ndl = dot(n, l);
    CHECK(lum(c) > 0.8f * 0.8f * ndl && lum(c) < 1.2f * 0.8f * ndl);
}

void test_roughness_softens_the_specular_peak()
{
    // At the mirror configuration (n == v == l) the GGX peak dominates: a smoother surface must
    // reflect strictly more than a rougher one.
    const Vec3 n{0, 1, 0};
    const Vec3 v{0, 1, 0};
    const Vec3 l{0, 1, 0};
    const Vec3 smooth = pbr_brdf(n, v, l, gray_surface(0.2f));
    const Vec3 rough = pbr_brdf(n, v, l, gray_surface(0.8f));
    CHECK(lum(smooth) > lum(rough));
}

void test_metallic_kills_diffuse()
{
    // Far off the specular peak (zenith view, near-grazing light, fairly smooth surface) the
    // specular lobe is negligible for BOTH, so the dielectric keeps its Lambert floor while the
    // pure metal — whose diffuse is energy-split away — reflects almost nothing.
    const Vec3 n{0, 1, 0};
    const Vec3 v{0, 1, 0};
    const Vec3 l = normalize({1.0f, 0.05f, 0.0f});
    const Vec3 dielectric = pbr_brdf(n, v, l, gray_surface(0.3f, 0.0f));
    const Vec3 metal = pbr_brdf(n, v, l, gray_surface(0.3f, 1.0f));
    CHECK(lum(dielectric) > 0.0f);
    CHECK(lum(metal) < 0.25f * lum(dielectric));
}

void test_point_attenuation_edges()
{
    CHECK(near_f(point_attenuation(0.0f, 2.0f), 1.0f));
    CHECK(near_f(point_attenuation(1.0f, 2.0f), 0.25f)); // (1 - 0.5)^2
    CHECK(near_f(point_attenuation(2.0f, 2.0f), 0.0f));  // at range
    CHECK(near_f(point_attenuation(5.0f, 2.0f), 0.0f));  // beyond range
    CHECK(near_f(point_attenuation(1.0f, 0.0f), 0.0f));  // degenerate range guard
    CHECK(near_f(point_attenuation(1.0f, -3.0f), 0.0f));
}

void test_shade_composition_terms()
{
    ShadeInputs in;
    in.surface = gray_surface(0.8f);
    in.ambient = {0.1f, 0.1f, 0.1f};

    // No lights: ambient * base only.
    const Vec3 ambient_only = shade_reference(in);
    CHECK(near_f(ambient_only.x, 0.08f) && near_f(ambient_only.z, 0.08f));

    // Directional light adds; doubling intensity doubles exactly the direct term.
    in.dir_enabled = true;
    in.dir_direction = normalize({0.0f, -1.0f, 0.2f});
    in.dir_intensity = 0.5f;
    const Vec3 lit = shade_reference(in);
    CHECK(lum(lit) > lum(ambient_only));
    in.dir_intensity = 1.0f;
    const Vec3 brighter = shade_reference(in);
    CHECK(near_f(lum(brighter) - lum(ambient_only), 2.0f * (lum(lit) - lum(ambient_only)),
                 1.0e-4f));

    // The shadow factor scales the directional term to zero.
    in.shadow_factor = 0.0f;
    const Vec3 shadowed = shade_reference(in);
    CHECK(near_f(shadowed.x, ambient_only.x) && near_f(shadowed.y, ambient_only.y));
    in.shadow_factor = 1.0f;

    // The lightmap hook adds exactly lightmap * base (R-REND-006 trivial sample path) — measured
    // on a dim ambient-only surface so the [0,1] clamp leaves the delta intact.
    ShadeInputs lm_in;
    lm_in.surface = gray_surface(0.8f);
    lm_in.ambient = {0.1f, 0.1f, 0.1f};
    const Vec3 lm_base = shade_reference(lm_in);
    lm_in.lightmap = {0.25f, 0.5f, 0.125f};
    lm_in.lightmap_enabled = true;
    const Vec3 with_lightmap = shade_reference(lm_in);
    CHECK(near_f(with_lightmap.x - lm_base.x, 0.25f * 0.8f, 1.0e-4f));
    CHECK(near_f(with_lightmap.y - lm_base.y, 0.5f * 0.8f, 1.0e-4f));
    CHECK(near_f(with_lightmap.z - lm_base.z, 0.125f * 0.8f, 1.0e-4f));

    // Emissive adds unconditionally; the output clamps at 1.
    in.surface.emissive = {5.0f, 0.0f, 0.0f};
    const Vec3 emissive = shade_reference(in);
    CHECK(near_f(emissive.x, 1.0f)); // clamped
    CHECK(near_f(emissive.y, brighter.y));

    // A point light at the surface's zenith contributes with attenuation; beyond range it does not.
    ShadeInputs point_in;
    point_in.surface = gray_surface(0.8f);
    point_in.point_enabled = true;
    point_in.point_position = {0.0f, 1.0f, 0.0f};
    point_in.point_range = 2.0f;
    point_in.shaded_position = {0.0f, 0.0f, 0.0f};
    const Vec3 point_lit = shade_reference(point_in);
    CHECK(lum(point_lit) > 0.0f);
    point_in.point_range = 1.0f; // now the light sits exactly at range: zero contribution
    const Vec3 point_out = shade_reference(point_in);
    CHECK(near_f(lum(point_out), 0.0f));
}

} // namespace

int main()
{
    test_below_horizon_is_black();
    test_brdf_positive_and_diffuse_dominated_when_rough();
    test_roughness_softens_the_specular_peak();
    test_metallic_kills_diffuse();
    test_point_attenuation_edges();
    test_shade_composition_terms();
    RENDER_TEST_MAIN_END();
}
