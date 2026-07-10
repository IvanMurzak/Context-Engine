// Local guard for the WEB backend's offscreen parity set (M4 T6, issue #137; R-REND-002, R-QA-013).
//
// The emdawnwebgpu web harness (src/render/web/web_main.cpp) renders the SAME two T1 proofs the
// native offscreen exe renders — the 3D-clip-space triangle (offscreen_scene.h) and the 2D ortho
// sprite scene (sprite_offscreen.h) — through the SAME rhi.h object model (create_wgpu_rhi()). The
// emscripten build of that path is a CI-only gate (`render-web`; emcc is not on the local executor),
// so this test is the LOCALLY-RUNNABLE, dev-gate assertion that the exact proof set the web harness
// selects is coherent and unchanged:
//   (1) the triangle proof passes end-to-end through the RHI on the GPU-free fake backend — the same
//       code the browser WebGPU backend runs (the fake backend software-rasterizes the same triangle
//       the WGSL draws), so a green here == the abstraction the web backend implements is coherent;
//   (2) the sprite scene's draw order + baked clip-space quad geometry are pinned — the CPU inputs
//       the web harness's 2D proof bakes into its per-sprite WGSL (its GPU rasterization needs a real
//       adapter, so it is the browser/Linux-CI gate, not run on the fake backend).
// It also compile-guards both proof headers under the dev toolchain, catching GCC-tier breaks before
// the CI round-trip. Parity to native holds by construction: identical proof code + identical WGSL
// through the identical rhi.h (see web_main.cpp). Runtime browser parity (the golden-scene SSIM
// gate) is the M4 T7 follow-up.

#include "context/render/offscreen_scene.h"
#include "context/render/sprite/sprite_offscreen.h"

#include "render_test.h"
#include "render_test_rhi.h"

#include <array>
#include <cstdint>
#include <vector>

using namespace context::render;

namespace
{

// (1) The 3D render-pipeline proof the web harness runs — asserted through the RHI abstraction on the
// fake backend, exactly as test_rhi does for the native path.
void test_web_triangle_proof_through_abstraction()
{
    rendertest::FakeRhi rhi(/*adapter_count=*/1);
    const AdapterProbe probe = rhi.probe();
    CHECK(probe.has_adapter);

    std::unique_ptr<IDevice> device = rhi.create_device();
    CHECK(device != nullptr);
    if (device == nullptr)
    {
        return;
    }
    CHECK(render_offscreen_triangle(*device) == OffscreenResult::Pass);
}

// (2) The 2D sprite proof the web harness runs — pin its draw order + baked quad geometry (the GPU
// rasterization is the browser/Linux-CI gate, not the fake backend).
void test_web_sprite_scene_inputs_pinned()
{
    using namespace context::render::sprite;

    const SpriteScene scene = reference_sprite_scene();
    CHECK(scene.sprites.size() == 2);

    // Red (layer 0) draws before green (layer 1) -> green composites on top in the overlap column.
    const std::vector<std::uint32_t> order = sort_draw_order(scene.sprites);
    CHECK(order.size() == 2);
    CHECK(order[0] == 0);
    CHECK(order[1] == 1);

    // The per-sprite WGSL the web harness bakes: two entry points + a 6-vertex (two-triangle) quad
    // with the red tint — the same numbers the browser proof renders.
    const Mat4 proj = scene.camera.projection();
    const Sprite2D& red = scene.sprites[0];
    const std::array<Vec2, 4> corners = quad_clip_corners(
        proj, Vec2{red.position[0], red.position[1]}, Vec2{red.size[0], red.size[1]});
    const std::string wgsl = quad_wgsl(corners, red.color);
    CHECK(wgsl.find("fn vs_main") != std::string::npos);
    CHECK(wgsl.find("fn fs_main") != std::string::npos);
    CHECK(wgsl.find("array<vec2f, 6>") != std::string::npos);
    CHECK(wgsl.find("1.000000, 0.000000, 0.000000, 1.000000") != std::string::npos);
}

} // namespace

int main()
{
    test_web_triangle_proof_through_abstraction();
    test_web_sprite_scene_inputs_pinned();
    RENDER_TEST_MAIN_END();
}
