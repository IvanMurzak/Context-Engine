// R-HEAD-002: rendering is strictly optional and detachable.
//
// Proves three things WITHOUT a GPU: (1) probing for adapters creates NO device; (2) with no
// adapter, try_attach yields NO renderer (the headless path — render module absent, no render
// work); (3) the engine (kernel World + the sim->render extract) runs fully with the renderer
// absent. The complement — with an adapter a renderer IS created and consumes frames — is also
// checked, so "headless" is observably the ABSENCE of a renderer, not a flag on one.

#include "context/render/extract.h"
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

void test_probe_creates_no_device()
{
    rendertest::FakeRhi rhi(/*adapter_count=*/2);
    const AdapterProbe probe = rhi.probe();
    CHECK(probe.has_adapter);
    CHECK(probe.adapter_count == 2u);
    // R-HEAD-002: the probe path enumerates adapters but NEVER creates a device.
    CHECK(rhi.device_creations() == 0);
}

void test_no_adapter_is_headless()
{
    rendertest::FakeRhi rhi(/*adapter_count=*/0);
    const AdapterProbe probe = rhi.probe();
    CHECK(!probe.has_adapter);
    CHECK(probe.adapter_count == 0u);

    std::unique_ptr<Renderer> renderer = Renderer::try_attach(rhi);
    CHECK(renderer == nullptr);       // no adapter => no renderer
    CHECK(rhi.device_creations() == 0); // and no device was created anywhere
}

void test_engine_runs_headless_without_renderer()
{
    // No renderer exists at all here — the render module is ABSENT. The engine still runs: build a
    // World, "step" it, and extract render state through the double buffer. All GPU-free.
    World world;
    const Entity e = world.create();
    world.add<Transform>(e, Transform{{1.0f, 2.0f, 3.0f}, {0, 0, 0, 1}, {1, 1, 1}});
    world.add<Renderable>(e, Renderable{});

    RenderDoubleBuffer db;
    extract_render_world(world, 7u, db.back());
    db.swap();

    CHECK(db.front().items.size() == 1u);
    CHECK(db.front().sim_tick == 7u);
    CHECK(world.alive_count() == 1u);
}

void test_adapter_attaches_and_consumes_frames()
{
    rendertest::FakeRhi rhi(/*adapter_count=*/1);
    std::unique_ptr<Renderer> renderer = Renderer::try_attach(rhi);
    CHECK(renderer != nullptr);
    if (renderer == nullptr)
    {
        return;
    }
    CHECK(rhi.device_creations() == 1);

    // A rendered engine feeds the front buffer to the renderer each frame.
    World world;
    const Entity e = world.create();
    world.add<Transform>(e, Transform{});
    world.add<Renderable>(e, Renderable{});

    RenderDoubleBuffer db;
    extract_render_world(world, 3u, db.back());
    db.swap();
    renderer->render(db.front());

    CHECK(renderer->stats().frames == 1u);
    CHECK(renderer->stats().last_item_count == 1u);
    CHECK(renderer->stats().last_sim_tick == 3u);
}

} // namespace

int main()
{
    test_probe_creates_no_device();
    test_no_adapter_is_headless();
    test_engine_runs_headless_without_renderer();
    test_adapter_attaches_and_consumes_frames();
    RENDER_TEST_MAIN_END();
}
