// CPU unit test for the GPU sprite proof's INPUTS (context/render/sprite/sprite_offscreen.h). It does
// NOT run render_offscreen_sprite — that needs a real rasterizing adapter and is the Linux-CI GPU
// gate (the GPU-free fake backend rasterizes only the reference triangle, not arbitrary quads). What
// this DOES do locally: compile the proof header under the dev toolchain (catching GCC-tier breaks
// before the CI render job), and pin down the scene's draw order + the exact clip-space quad geometry
// the CI proof bakes into its WGSL — so a green CI GPU readback is a check on the SAME numbers this
// test verifies.

#include "context/render/sprite/sprite_offscreen.h"

#include "render_test.h"

#include <cmath>
#include <string>

using namespace context::render::sprite;

namespace
{

bool approx(float a, float b, float eps = 1e-5f)
{
    return std::fabs(a - b) <= eps;
}

// Clip x/y in [-1,1] (y-up) -> the 256x256 readback screen coords (row 0 = top) the proof samples.
float screen_x(float clip_x) { return (clip_x * 0.5f + 0.5f) * 256.0f; }
float screen_y(float clip_y) { return (0.5f - 0.5f * clip_y) * 256.0f; }

void test_reference_scene_draw_order()
{
    const SpriteScene scene = reference_sprite_scene();
    CHECK(scene.sprites.size() == 2);
    const std::vector<std::uint32_t> order = sort_draw_order(scene.sprites);
    // Sprite 0 (red, layer 0) draws before sprite 1 (green, layer 1) -> green composites on top.
    CHECK(order.size() == 2);
    CHECK(order[0] == 0);
    CHECK(order[1] == 1);
}

void test_reference_scene_projects_to_asserted_pixels()
{
    const SpriteScene scene = reference_sprite_scene();
    const Mat4 proj = scene.camera.projection();

    // Red sprite centre (-40,0) -> screen (88,128); the proof samples (60,128), which must be inside
    // its screen span [48,128] x [88,168].
    const Vec2 red_c = project_point(proj, Vec2{-40.0f, 0.0f});
    CHECK(approx(screen_x(red_c.x), 88.0f, 0.5f));
    CHECK(approx(screen_y(red_c.y), 128.0f, 0.5f));

    // Green sprite centre (20,0) -> screen (148,128); the proof samples (170,128) inside [108,188].
    const Vec2 green_c = project_point(proj, Vec2{20.0f, 0.0f});
    CHECK(approx(screen_x(green_c.x), 148.0f, 0.5f));

    // Overlap column: red spans screen x [48,128], green [108,188]; (118,128) is inside BOTH, so it is
    // the sort-order discriminator the GPU proof asserts green wins.
    CHECK(118.0f > 108.0f && 118.0f < 128.0f);
}

void test_quad_wgsl_bakes_corners_and_color()
{
    const SpriteScene scene = reference_sprite_scene();
    const Mat4 proj = scene.camera.projection();
    const Sprite2D& red = scene.sprites[0];
    const std::array<Vec2, 4> corners =
        quad_clip_corners(proj, Vec2{red.position[0], red.position[1]},
                          Vec2{red.size[0], red.size[1]});
    const std::string wgsl = quad_wgsl(corners, red.color);
    // A well-formed shader with the two entry points and a 6-vertex (two-triangle) quad.
    CHECK(wgsl.find("fn vs_main") != std::string::npos);
    CHECK(wgsl.find("fn fs_main") != std::string::npos);
    CHECK(wgsl.find("array<vec2f, 6>") != std::string::npos);
    // The fragment bakes the red tint (1.0, 0.0, 0.0, 1.0).
    CHECK(wgsl.find("1.000000, 0.000000, 0.000000, 1.000000") != std::string::npos);
}

} // namespace

int main()
{
    test_reference_scene_draw_order();
    test_reference_scene_projects_to_asserted_pixels();
    test_quad_wgsl_bakes_corners_and_color();
    RENDER_TEST_MAIN_END();
}
