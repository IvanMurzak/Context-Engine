// CPU unit test: orthographic 2D projection math (context/render/sprite/ortho.h). No GPU — runs on
// the local dev gate under every toolchain.

#include "context/render/sprite/ortho.h"

#include "render_test.h"

#include <cmath>

using namespace context::render::sprite;

namespace
{

bool approx(float a, float b, float eps = 1e-5f)
{
    return std::fabs(a - b) <= eps;
}

void test_ortho_matrix_maps_box_to_clip()
{
    // A 256-wide/high box centred on origin -> WebGPU clip cube (x,y in [-1,1], z in [0,1]).
    const Mat4 m = ortho(-128.0f, 128.0f, -128.0f, 128.0f, 0.0f, 1.0f);
    CHECK(approx(m.at(0, 0), 2.0f / 256.0f));
    CHECK(approx(m.at(1, 1), 2.0f / 256.0f));
    CHECK(approx(m.at(2, 2), 1.0f)); // z: [0,1] -> [0,1]
    CHECK(approx(m.at(3, 0), 0.0f)); // symmetric box -> no x translation
    CHECK(approx(m.at(3, 1), 0.0f));
    CHECK(approx(m.at(3, 3), 1.0f));
}

void test_ortho_offcenter_translation()
{
    // A box [0,256]x[0,256] -> the centre (128,128) maps to clip origin, so translation is -1,-1.
    const Mat4 m = ortho(0.0f, 256.0f, 0.0f, 256.0f, 0.0f, 1.0f);
    CHECK(approx(m.at(3, 0), -1.0f));
    CHECK(approx(m.at(3, 1), -1.0f));
    const Vec2 centre = project_point(m, Vec2{128.0f, 128.0f});
    CHECK(approx(centre.x, 0.0f));
    CHECK(approx(centre.y, 0.0f));
}

void test_project_point_corners_and_yup()
{
    const Camera2D cam{{0.0f, 0.0f}, 128.0f, 128.0f, 0.0f, 1.0f};
    const Mat4 m = cam.projection();
    CHECK(approx(project_point(m, Vec2{0.0f, 0.0f}).x, 0.0f));
    CHECK(approx(project_point(m, Vec2{0.0f, 0.0f}).y, 0.0f));
    // Right edge -> clip +1; top edge (world y-up) -> clip +1 (y-up preserved, no flip).
    CHECK(approx(project_point(m, Vec2{128.0f, 0.0f}).x, 1.0f));
    CHECK(approx(project_point(m, Vec2{0.0f, 128.0f}).y, 1.0f));
    CHECK(approx(project_point(m, Vec2{-128.0f, 0.0f}).x, -1.0f));
    CHECK(approx(project_point(m, Vec2{0.0f, -128.0f}).y, -1.0f));
}

void test_quad_clip_corners()
{
    const Camera2D cam{{0.0f, 0.0f}, 128.0f, 128.0f, 0.0f, 1.0f};
    const Mat4 m = cam.projection();
    // The red sprite from the reference scene: centre (-40,0), size 80x80.
    const std::array<Vec2, 4> c = quad_clip_corners(m, Vec2{-40.0f, 0.0f}, Vec2{80.0f, 80.0f});
    // World corners bl(-80,-40) br(0,-40) tr(0,40) tl(-80,40) -> /128.
    CHECK(approx(c[0].x, -80.0f / 128.0f)); // bl.x
    CHECK(approx(c[0].y, -40.0f / 128.0f)); // bl.y
    CHECK(approx(c[1].x, 0.0f));            // br.x
    CHECK(approx(c[2].y, 40.0f / 128.0f));  // tr.y
    CHECK(approx(c[3].x, -80.0f / 128.0f)); // tl.x
    // CCW winding, y-up: bottom row y < top row y; left col x < right col x.
    CHECK(c[0].y < c[3].y);
    CHECK(c[0].x < c[1].x);
}

void test_degenerate_camera_is_finite()
{
    // A zero-extent camera must not divide by zero — it yields a finite (identity-scale) matrix.
    const Mat4 m = ortho(5.0f, 5.0f, 5.0f, 5.0f, 0.0f, 0.0f);
    CHECK(std::isfinite(m.at(0, 0)));
    CHECK(std::isfinite(m.at(1, 1)));
    CHECK(std::isfinite(m.at(2, 2)));
}

} // namespace

int main()
{
    test_ortho_matrix_maps_box_to_clip();
    test_ortho_offcenter_translation();
    test_project_point_corners_and_yup();
    test_quad_clip_corners();
    test_degenerate_camera_is_finite();
    RENDER_TEST_MAIN_END();
}
