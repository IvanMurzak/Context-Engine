// The M4 EXIT gate's headless criterion (ROADMAP §1 M4 exit; R-HEAD-002 / R-HEAD-004; issue #141),
// registered as the blocking `m4-exit-headless-no-render` ctest and run by the CI build job's "M4
// exit gate" named step on all three build-matrix legs — under CONTEXT_BUILD_RENDER_WGPU=OFF, i.e.
// with NO GPU backend compiled in at all. It asserts, at milestone-exit strength:
//   1. headless is the ABSENCE of a renderer (R-HEAD-002): probing creates no device; with no
//      adapter there is no Renderer; the engine (World + sim->render extract + double buffer)
//      steps on regardless, for many ticks, with the render module absent;
//   2. offscreen rendering is the OPT-IN path (R-HEAD-004): when an adapter exists a renderer can
//      attach and the corpus offscreen render works — and DETACHING it returns the engine to the
//      exact headless behavior of (1), so rendering is optional in both directions.
// The other two M4 exit criteria are CI-job gates on real backends: the golden-scene SSIM corpus
// (render + render-web jobs vs goldens/) and the min-spec floor bench (bench/minspec_floor.py).

#include "context/render/extract.h"
#include "context/render/golden.h"
#include "context/render/render_world.h"
#include "context/render/renderer.h"

#include "context/kernel/world.h"

#include "render_test.h"
#include "render_test_rhi.h"

using namespace context::render;
using context::kernel::Entity;
using context::kernel::World;

namespace
{

// Step the world N ticks through the extract + double buffer — the engine's render-facing loop,
// identical whether a renderer exists or not (L-39). Returns the last swapped-in snapshot tick.
std::uint64_t step_ticks(World& world, RenderDoubleBuffer& db, std::uint64_t from, int ticks)
{
    std::uint64_t tick = from;
    for (int i = 0; i < ticks; ++i)
    {
        ++tick;
        extract_render_world(world, tick, db.back());
        db.swap();
    }
    return tick;
}

void test_headless_no_render_module()
{
    // No adapter anywhere: probe must not create a device, attach must yield no renderer.
    rendertest::FakeRhi rhi(/*adapter_count=*/0);
    const AdapterProbe probe = rhi.probe();
    CHECK(!probe.has_adapter);
    CHECK(rhi.device_creations() == 0);
    CHECK(Renderer::try_attach(rhi) == nullptr);
    CHECK(rhi.device_creations() == 0);

    // The engine runs with the render module ABSENT: a populated world steps 100 ticks through
    // the extract/double-buffer loop with no renderer and no device ever created.
    World world;
    const Entity e = world.create();
    world.add<Transform>(e, Transform{{1.0f, 2.0f, 3.0f}, {0, 0, 0, 1}, {1, 1, 1}});
    world.add<Renderable>(e, Renderable{});

    RenderDoubleBuffer db;
    const std::uint64_t last = step_ticks(world, db, 0, 100);
    CHECK(last == 100u);
    CHECK(db.front().sim_tick == 100u);
    CHECK(db.front().items.size() == 1u);
    CHECK(world.alive_count() == 1u);
    CHECK(rhi.device_creations() == 0); // still: no render module, no device
}

void test_offscreen_render_is_opt_in_and_detachable()
{
    // With an adapter, rendering ATTACHES (R-HEAD-004's opt-in offscreen path)...
    rendertest::FakeRhi rhi(/*adapter_count=*/1);
    std::unique_ptr<Renderer> renderer = Renderer::try_attach(rhi);
    CHECK(renderer != nullptr);
    if (renderer == nullptr)
    {
        return;
    }
    CHECK(rhi.device_creations() == 1);

    World world;
    const Entity e = world.create();
    world.add<Transform>(e, Transform{});
    world.add<Renderable>(e, Renderable{});

    RenderDoubleBuffer db;
    std::uint64_t tick = step_ticks(world, db, 0, 3);
    renderer->render(db.front());
    CHECK(renderer->stats().frames == 1u);
    CHECK(renderer->stats().last_sim_tick == 3u);

    // ... and the corpus offscreen render works through the attached device (the golden-scene
    // render path the CI render/render-web jobs gate on real backends).
    golden::GoldenImage image;
    CHECK(golden::render_golden_scene(renderer->device(), "triangle3d", image));
    CHECK(image.width == offscreen_triangle_size());
    CHECK(image.rgba.size() == static_cast<std::size_t>(image.width) * image.height * 4u);
    // Interior triangle pixel is red, background is the clear color (the fake backend rasterizes
    // the reference triangle deterministically).
    const std::size_t row = static_cast<std::size_t>(image.width) * 4u;
    const std::uint8_t* tri = image.rgba.data() + 150u * row + 128u * 4u;
    const std::uint8_t* bg = image.rgba.data() + 8u * row + 8u * 4u;
    CHECK(tri[0] > 200 && tri[1] < 60 && tri[2] < 60);
    CHECK(bg[2] > bg[0]); // clear 0.1/0.2/0.3 is blue-dominant

    // DETACH: destroying the renderer returns the engine to headless — stepping continues with
    // the render module absent and no further device use.
    renderer.reset();
    tick = step_ticks(world, db, tick, 5);
    CHECK(tick == 8u);
    CHECK(db.front().sim_tick == 8u);
    CHECK(db.front().items.size() == 1u);
}

} // namespace

int main()
{
    test_headless_no_render_module();
    test_offscreen_render_is_opt_in_and_detachable();
    RENDER_TEST_MAIN_END();
}
