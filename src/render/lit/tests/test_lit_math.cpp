// 3D math for the lit path: vector ops, look-at view, WebGPU-clip ortho projection, and the
// composed camera/light view-projections of the reference scene mapping known world points to the
// expected NDC — the CPU pinning of exactly what the GPU proof rasterizes (sprite-path pattern).

#include "context/render/lit/lit_math.h"
#include "context/render/lit/lit_scene.h"

#include <cmath>

#include "render_test.h"

using namespace context::render::lit;

namespace
{

constexpr float kEps = 1.0e-5f;

bool near_f(float a, float b, float eps = kEps)
{
    return std::fabs(a - b) <= eps;
}

void test_vec_ops()
{
    CHECK(near_f(dot({1, 2, 3}, {4, 5, 6}), 32.0f));
    const Vec3 c = cross({1, 0, 0}, {0, 1, 0});
    CHECK(near_f(c.x, 0.0f) && near_f(c.y, 0.0f) && near_f(c.z, 1.0f));
    CHECK(near_f(length({3, 4, 0}), 5.0f));
    const Vec3 n = normalize({0, -2, 0});
    CHECK(near_f(n.x, 0.0f) && near_f(n.y, -1.0f) && near_f(n.z, 0.0f));
    // Degenerate normalize is the zero vector, never NaN.
    const Vec3 z = normalize({0, 0, 0});
    CHECK(z.x == 0.0f && z.y == 0.0f && z.z == 0.0f);
}

void test_identity_and_mul()
{
    const Mat4 id;
    const Vec3 p = transform_point(id, {1.5f, -2.0f, 3.0f});
    CHECK(near_f(p.x, 1.5f) && near_f(p.y, -2.0f) && near_f(p.z, 3.0f));

    // mul(a, b) applies b first: translate-after-scale differs from scale-after-translate.
    Mat4 scale2;
    scale2.m[0] = 2.0f; // x *= 2
    Mat4 translate;
    translate.m[12] = 1.0f; // x += 1
    const Vec3 q = transform_point(mul(translate, scale2), {1, 0, 0});
    CHECK(near_f(q.x, 3.0f)); // (1*2) + 1
    const Vec3 r = transform_point(mul(scale2, translate), {1, 0, 0});
    CHECK(near_f(r.x, 4.0f)); // (1+1) * 2
}

void test_look_at_reference_camera()
{
    // The reference camera: eye (0,5,0) looking straight down, up hint -Z. World +X must map to
    // view +X, world +Z to view -Y (screen down), and the ground plane to view z = -5.
    const Mat4 view = look_at({0, 5, 0}, {0, 0, 0}, {0, 0, -1});
    const Vec3 origin = transform_point(view, {0, 0, 0});
    CHECK(near_f(origin.x, 0.0f) && near_f(origin.y, 0.0f) && near_f(origin.z, -5.0f));
    const Vec3 px = transform_point(view, {1, 0, 0});
    CHECK(near_f(px.x, 1.0f) && near_f(px.y, 0.0f));
    const Vec3 pz = transform_point(view, {0, 0, 1});
    CHECK(near_f(pz.x, 0.0f) && near_f(pz.y, -1.0f));
}

void test_ortho_maps_webgpu_clip()
{
    // View-space box [-2,2]^2, near 1, far 9 (forward is -Z): the near plane lands at clip z = 0,
    // the far plane at 1, and the box corners at x,y = +-1.
    const Mat4 proj = ortho(-2, 2, -2, 2, 1, 9);
    const Vec3 near_c = transform_point(proj, {2, 2, -1});
    CHECK(near_f(near_c.x, 1.0f) && near_f(near_c.y, 1.0f) && near_f(near_c.z, 0.0f));
    const Vec3 far_c = transform_point(proj, {-2, -2, -9});
    CHECK(near_f(far_c.x, -1.0f) && near_f(far_c.y, -1.0f) && near_f(far_c.z, 1.0f));
    const Vec3 mid = transform_point(proj, {0, 0, -5});
    CHECK(near_f(mid.z, 0.5f));
}

void test_reference_camera_view_proj()
{
    // The composed camera VP of the reference scene: world origin lands at NDC (0,0); the ground
    // half-extents land inside the [-1,1] square (camera half-extent 2.56 > ground half 2).
    const Mat4 vp = camera_view_proj();
    const Vec3 center = transform_point(vp, {0, 0, 0});
    CHECK(near_f(center.x, 0.0f) && near_f(center.y, 0.0f));
    CHECK(center.z > 0.0f && center.z < 1.0f);

    const Vec3 corner = transform_point(vp, {2, 0, 2});
    CHECK(near_f(corner.x, 2.0f / 2.56f, 1.0e-4f));
    CHECK(near_f(corner.y, -2.0f / 2.56f, 1.0e-4f)); // world +Z is screen-down (NDC -y)

    // A point ABOVE the ground (the blocker plane) is closer to the camera: smaller NDC depth.
    const Vec3 ground = transform_point(vp, {0, 0, 0});
    const Vec3 raised = transform_point(vp, {0, 1, 0});
    CHECK(raised.z < ground.z);
}

void test_reference_light_view_proj()
{
    // The light VP of the reference sun: every scene point lands inside the light frustum, and a
    // point on the blocker is CLOSER to the light (smaller depth) than the ground point its shadow
    // ray continues to — the depth relation shadow mapping relies on.
    const Vec3 sun = normalize({0.6f, -1.0f, 0.0f});
    const Mat4 vp = light_view_proj(sun);

    const Vec3 scene_pts[4] = {{-2, 0, -2}, {2, 0, 2}, {-1, 1, -0.4f}, {-0.2f, 1, 0.4f}};
    for (const Vec3& p : scene_pts)
    {
        const Vec3 ndc = transform_point(vp, p);
        CHECK(ndc.x > -1.0f && ndc.x < 1.0f);
        CHECK(ndc.y > -1.0f && ndc.y < 1.0f);
        CHECK(ndc.z > 0.0f && ndc.z < 1.0f);
    }

    // The blocker point above the shadowed ground probe, along the light ray.
    const Vec3 shadowed_ground = sample_world(SamplePoint::ShadowedGround);
    const float t = (1.0f - shadowed_ground.y) / (-sun.y);
    const Vec3 on_blocker = add(shadowed_ground, scale(sun, -t));
    CHECK(near_f(on_blocker.y, 1.0f, 1.0e-4f));

    const Vec3 ground_ndc = transform_point(vp, shadowed_ground);
    const Vec3 blocker_ndc = transform_point(vp, on_blocker);
    CHECK(blocker_ndc.z < ground_ndc.z); // occluder is closer to the light
    // ... and they project to the SAME light-space texel neighborhood (same shadow-map ray).
    CHECK(near_f(blocker_ndc.x, ground_ndc.x, 1.0e-3f));
    CHECK(near_f(blocker_ndc.y, ground_ndc.y, 1.0e-3f));
}

} // namespace

int main()
{
    test_vec_ops();
    test_identity_and_mul();
    test_look_at_reference_camera();
    test_ortho_maps_webgpu_clip();
    test_reference_camera_view_proj();
    test_reference_light_view_proj();
    RENDER_TEST_MAIN_END();
}
