// UV-mapping unit tests (M7 a10, DoD item 1): the pure ray-vs-triangle intersection + barycentric UV
// interpolation + UV->panel-space mapping (curved_panel.h) — hit -> UV -> panel coords on a curved
// (cylinder-class) mesh, plus the edge wrap/clamp cases. context_ui foundation only: no GPU, no
// spatial (the broad-phase-pruned raycaster equivalence is covered in test_curved_panel_input.cpp).

#include "context/packages/ui/curved_panel.h"

#include "ui_test.h"

#include <cmath>

using namespace context::packages::ui;

namespace
{

constexpr float kPi = 3.14159265358979323846f; // M_PI is not a strict-C++20 macro

[[nodiscard]] bool approx(float a, float b, float eps = 1e-4f)
{
    return std::fabs(a - b) <= eps;
}

// --- ray-vs-triangle core (Moeller-Trumbore) -----------------------------------------------------
void test_ray_triangle_core()
{
    // A unit right triangle in the z=0 plane: a=(0,0,0) b=(1,0,0) c=(0,1,0).
    const Vec3 a{0, 0, 0};
    const Vec3 b{1, 0, 0};
    const Vec3 c{0, 1, 0};

    // A straight -Z ray through the interior point (0.25, 0.25): hit at t=1, bary u=v=0.25.
    float t = 0, u = 0, v = 0;
    CHECK(ray_triangle_intersect(Ray{Vec3{0.25f, 0.25f, 1.0f}, Vec3{0, 0, -1}}, a, b, c, t, u, v));
    CHECK(approx(t, 1.0f));
    CHECK(approx(u, 0.25f));
    CHECK(approx(v, 0.25f));

    // A ray OUTSIDE the triangle (2,2) misses.
    CHECK(!ray_triangle_intersect(Ray{Vec3{2.0f, 2.0f, 1.0f}, Vec3{0, 0, -1}}, a, b, c, t, u, v));

    // A ray PARALLEL to the triangle plane misses (dir in the z=0 plane).
    CHECK(!ray_triangle_intersect(Ray{Vec3{0.25f, 0.25f, 0.0f}, Vec3{1, 0, 0}}, a, b, c, t, u, v));

    // A hit BEHIND the origin (the triangle is behind a +Z-pointing ray) misses.
    CHECK(!ray_triangle_intersect(Ray{Vec3{0.25f, 0.25f, 1.0f}, Vec3{0, 0, 1}}, a, b, c, t, u, v));

    // Double-sided: the same interior ray from BELOW (origin z=-1, dir +Z) still hits.
    CHECK(ray_triangle_intersect(Ray{Vec3{0.25f, 0.25f, -1.0f}, Vec3{0, 0, 1}}, a, b, c, t, u, v));
    CHECK(approx(t, 1.0f));
}

// --- UV -> panel-space mapping: edge wrap/clamp (DoD item 1, "edge wrap/clamp cases") -------------
void test_uv_to_panel_wrap_clamp()
{
    const float w = 200.0f;
    const float h = 100.0f;

    // In-range: a direct (u*w, v*h) with no y-flip (UV (0,0) == top-left == panel (0,0)).
    Vec2 p = uv_to_panel_coords(Vec2{0.25f, 0.75f}, w, h);
    CHECK(approx(p.x, 50.0f) && approx(p.y, 75.0f));
    p = uv_to_panel_coords(Vec2{0.0f, 0.0f}, w, h);
    CHECK(approx(p.x, 0.0f) && approx(p.y, 0.0f));
    p = uv_to_panel_coords(Vec2{1.0f, 1.0f}, w, h);
    CHECK(approx(p.x, w) && approx(p.y, h));

    // Clamp (default): out-of-[0,1] extends the edge texel.
    p = uv_to_panel_coords(Vec2{-0.1f, 1.2f}, w, h, UvWrap::Clamp);
    CHECK(approx(p.x, 0.0f) && approx(p.y, h)); // -0.1 -> 0, 1.2 -> 1
    p = uv_to_panel_coords(Vec2{5.0f, -3.0f}, w, h, UvWrap::Clamp);
    CHECK(approx(p.x, w) && approx(p.y, 0.0f));

    // Repeat: wrap by fractional part (u - floor(u)).
    p = uv_to_panel_coords(Vec2{1.25f, 2.5f}, w, h, UvWrap::Repeat);
    CHECK(approx(p.x, 0.25f * w) && approx(p.y, 0.5f * h)); // 1.25 -> 0.25, 2.5 -> 0.5
    p = uv_to_panel_coords(Vec2{-0.25f, -0.75f}, w, h, UvWrap::Repeat);
    CHECK(approx(p.x, 0.75f * w) && approx(p.y, 0.25f * h)); // -0.25 -> 0.75, -0.75 -> 0.25
    // Exact integers wrap to 0 (fractional part of a whole number is 0).
    p = uv_to_panel_coords(Vec2{2.0f, 3.0f}, w, h, UvWrap::Repeat);
    CHECK(approx(p.x, 0.0f) && approx(p.y, 0.0f));
}

// --- the cylinder-class mesh + hit -> UV -> panel coords ------------------------------------------
void test_cylinder_mesh_structure()
{
    const std::uint32_t segments = 16;
    const PanelMesh mesh = build_cylinder_panel_mesh(segments, 1.0f, 1.0f,
                                                     kPi * 0.75f, Vec3{0, 0, 0});
    // 2 verts per column boundary, 2 triangles per column quad.
    CHECK(mesh.vertices.size() == static_cast<std::size_t>(segments + 1) * 2u);
    CHECK(mesh.triangles.size() == static_cast<std::size_t>(segments) * 2u);

    // UV endpoints: the leftmost column is u=0, the rightmost u=1; top row v=0, bottom row v=1.
    CHECK(approx(mesh.vertices.front().uv.x, 0.0f)); // column 0 top
    CHECK(approx(mesh.vertices[0].uv.y, 0.0f));      // top
    CHECK(approx(mesh.vertices[1].uv.y, 1.0f));      // bottom
    CHECK(approx(mesh.vertices[mesh.vertices.size() - 2].uv.x, 1.0f)); // last column top, u=1

    // Every triangle index is in range.
    for (const std::array<std::uint32_t, 3>& tri : mesh.triangles)
    {
        CHECK(tri[0] < mesh.vertices.size() && tri[1] < mesh.vertices.size() &&
              tri[2] < mesh.vertices.size());
    }

    // The panel bulges toward +Z (convex, facing the camera): the centre column reaches z=radius,
    // the edge columns recede. And the projected x span foreshortens toward the edges.
    // Centre-column (angle 0) z == radius; symmetric edge columns share the smallest z.
    bool found_center_z = false;
    for (const PanelVertex& vtx : mesh.vertices)
    {
        if (approx(vtx.pos.x, 0.0f))
        {
            found_center_z = true;
            CHECK(approx(vtx.pos.z, 1.0f)); // radius
        }
    }
    CHECK(found_center_z);
}

void test_cylinder_hit_uv_panel_coords()
{
    const std::uint32_t segments = 16;
    const float radius = 1.0f;
    const float height = 1.0f;
    const PanelMesh mesh = build_cylinder_panel_mesh(segments, radius, height,
                                                     kPi * 0.75f, Vec3{0, 0, 0});
    const float panel_w = 200.0f;
    const float panel_h = 120.0f;

    auto cast = [&](float x, float y) -> PanelRayHit
    { return raycast_panel_mesh(Ray{Vec3{x, y, 10.0f}, Vec3{0, 0, -1}}, mesh); };

    // Centre ray (x=0, y=0): hits the middle of the panel -> UV ~ (0.5, 0.5).
    const PanelRayHit c = cast(0.0f, 0.0f);
    CHECK(c.hit);
    CHECK(approx(c.uv.x, 0.5f, 2e-3f));
    CHECK(approx(c.uv.y, 0.5f, 2e-3f));
    // v is exactly linear in the world y (dir is -Z so the hit y == the ray y): v = 0.5 - y here.
    CHECK(approx(cast(0.0f, 0.4f).uv.y, 0.1f, 2e-3f)); // near the top
    CHECK(approx(cast(0.0f, -0.4f).uv.y, 0.9f, 2e-3f)); // near the bottom

    // Curvature-aware u is MONOTONIC across the arc: a ray to the right of centre samples a larger u,
    // to the left a smaller u (the panel wraps left->right). Robust to the polygonal chord vs. the
    // exact-arc parametrization (u is piecewise-linear per chord — monotonic, not a closed form).
    const PanelRayHit right = cast(0.35f, 0.0f);
    const PanelRayHit left = cast(-0.35f, 0.0f);
    CHECK(right.hit && left.hit);
    CHECK(right.uv.x > 0.5f);
    CHECK(left.uv.x < 0.5f);
    CHECK(approx(right.uv.x, 1.0f - left.uv.x, 5e-3f)); // symmetric about the centre

    // hit -> UV -> panel coords: the centre maps to the panel-space centre (a2 hit_test surface).
    const std::optional<Vec2> coords =
        raycast_panel_pointer(Ray{Vec3{0.0f, 0.0f, 10.0f}, Vec3{0, 0, -1}}, mesh, panel_w, panel_h);
    CHECK(coords.has_value());
    CHECK(approx(coords->x, 0.5f * panel_w, 1.0f));
    CHECK(approx(coords->y, 0.5f * panel_h, 1.0f));

    // A ray that misses the mesh entirely (far to the side) -> no hit -> no panel coords.
    CHECK(!cast(100.0f, 0.0f).hit);
    CHECK(!raycast_panel_pointer(Ray{Vec3{100.0f, 0.0f, 10.0f}, Vec3{0, 0, -1}}, mesh, panel_w,
                                 panel_h)
               .has_value());
}

} // namespace

int main()
{
    test_ray_triangle_core();
    test_uv_to_panel_wrap_clamp();
    test_cylinder_mesh_structure();
    test_cylinder_hit_uv_panel_coords();
    UI_TEST_MAIN_END();
}
