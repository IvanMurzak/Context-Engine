// The windowed present path — ISurface / ISwapchain (M9 e03; design 03 §2, §4, §7).
//
// The compositor's frame loop is driven end to end on the GPU-free fake backend: create a surface
// from a native window, capability-check, configure, then acquire -> render -> present, resize, and
// tear down. The Outdated / Lost / Timeout acquire branches matter most here — a real adapter
// produces them only under a racing resize or a device loss, so a GPU CI leg is precisely the wrong
// place to be discovering that the recovery path is wrong.

#include "context/render/present/osr_composite.h"
#include "context/render/rhi.h"

#include "render_test.h"
#include "render_test_rhi.h"

#include <memory>

using namespace context::render;
using namespace context::render::present;
using rendertest::FakeQueue;
using rendertest::FakeRhi;
using rendertest::FakeSurface;
using rendertest::FakeSwapchain;

namespace
{

NativeWindowDesc fake_window(int& storage)
{
    NativeWindowDesc window;
    window.kind = NativeWindowKind::Win32Hwnd;
    window.handle = &storage;
    return window;
}

// ------------------------------------------------------------------------ surface acquisition

void test_surface_needs_a_real_window()
{
    FakeRhi rhi(1);
    // A descriptor naming no window is a clean absence — the Shell degrades to the CPU present
    // path — never a crash.
    CHECK(rhi.create_surface(NativeWindowDesc{}) == nullptr);

    NativeWindowDesc no_handle;
    no_handle.kind = NativeWindowKind::Win32Hwnd;
    CHECK(rhi.create_surface(no_handle) == nullptr);

    int window = 0;
    CHECK(rhi.create_surface(fake_window(window)) != nullptr);
    CHECK(rhi.surface_creations() == 1);
}

void test_probe_surface_answers_the_gpu_gate()
{
    int window = 0;

    // The surface-LESS probe must never claim presentability: "nobody asked" is not "yes".
    FakeRhi rhi(1);
    CHECK(!rhi.probe().can_present);

    std::unique_ptr<ISurface> surface = rhi.create_surface(fake_window(window));
    CHECK(surface != nullptr);
    if (surface != nullptr)
    {
        const AdapterProbe probe = rhi.probe_surface(*surface);
        CHECK(probe.has_adapter);
        CHECK(probe.can_present);
        // Still device-free (R-HEAD-002): the gate must not cost a device.
        CHECK(rhi.device_creations() == 0);
    }

    // A GPU-less box: an adapter-free probe cannot present, and that is a clean report.
    FakeRhi headless(0);
    std::unique_ptr<ISurface> headless_surface = headless.create_surface(fake_window(window));
    CHECK(headless_surface != nullptr);
    if (headless_surface != nullptr)
    {
        const AdapterProbe probe = headless.probe_surface(*headless_surface);
        CHECK(!probe.has_adapter);
        CHECK(!probe.can_present);
    }

    // An adapter that exists but cannot present to THIS surface — the case that drives C-F2.
    FakeRhi unpresentable(1);
    SurfaceCaps refuses;
    refuses.supported = false;
    unpresentable.set_surface_caps(refuses);
    std::unique_ptr<ISurface> bad = unpresentable.create_surface(fake_window(window));
    CHECK(bad != nullptr);
    if (bad != nullptr)
    {
        const AdapterProbe probe = unpresentable.probe_surface(*bad);
        CHECK(probe.has_adapter);
        CHECK(!probe.can_present);
    }
}

// ----------------------------------------------------------------------------- configuration

void test_capabilities_gate_the_configure()
{
    int window = 0;
    FakeRhi rhi(1);
    std::unique_ptr<IDevice> device = rhi.create_device();
    std::unique_ptr<ISurface> surface = rhi.create_surface(fake_window(window));
    CHECK(device != nullptr && surface != nullptr);
    if (device == nullptr || surface == nullptr)
    {
        return;
    }

    const SurfaceCaps caps = surface->capabilities();
    CHECK(caps.supported);
    // WebGPU guarantees Fifo on every surface — the safe default the spike configures with.
    CHECK(caps.supports_present_mode(PresentMode::Fifo));
    CHECK(caps.supports_format(TextureFormat::BGRA8Unorm));
    CHECK(!caps.supports_present_mode(PresentMode::Mailbox));

    SurfaceConfig config;
    config.size = {800, 600};
    config.format = TextureFormat::BGRA8Unorm;
    config.present_mode = PresentMode::Fifo;
    std::unique_ptr<ISwapchain> chain = surface->configure(*device, config);
    CHECK(chain != nullptr);
    if (chain != nullptr)
    {
        CHECK(chain->is_configured());
        CHECK(chain->size().width == 800);
        // The swapchain is BGRA8Unorm while viewport render targets stay RGBA8Unorm (03 §2).
        CHECK(chain->format() == TextureFormat::BGRA8Unorm);
        CHECK(chain->present_mode() == PresentMode::Fifo);
    }

    // An unsupported present mode is REFUSED, not silently substituted — the substitution would
    // present the whole editor at the wrong pacing with nothing reporting it.
    SurfaceConfig unsupported = config;
    unsupported.present_mode = PresentMode::Mailbox;
    CHECK(surface->configure(*device, unsupported) == nullptr);

    // The FORMAT half of the same gate. Asserted separately because the two are independent checks
    // in the backend: dropping either one leaves the other's test green.
    SurfaceConfig wrong_format = config;
    wrong_format.format = TextureFormat::Depth32Float; // not in the surface's reported formats
    CHECK(!caps.supports_format(TextureFormat::Depth32Float));
    CHECK(surface->configure(*device, wrong_format) == nullptr);

    SurfaceConfig zero = config;
    zero.size = {0, 0};
    CHECK(surface->configure(*device, zero) == nullptr);

    // Exactly ONE configure got through: a refusal must not have half-configured the surface.
    CHECK(static_cast<FakeSurface&>(*surface).configure_count() == 1);
}

// -------------------------------------------------------------------------------- frame loop

void test_frame_loop_acquire_render_present()
{
    int window = 0;
    FakeRhi rhi(1);
    std::unique_ptr<IDevice> device = rhi.create_device();
    std::unique_ptr<ISurface> surface = rhi.create_surface(fake_window(window));
    SurfaceConfig config;
    config.size = {64, 32};
    std::unique_ptr<ISwapchain> chain = surface->configure(*device, config);
    CHECK(chain != nullptr);
    if (chain == nullptr)
    {
        return;
    }

    for (int frame = 0; frame < 3; ++frame)
    {
        const AcquiredFrame acquired = chain->acquire();
        CHECK(acquired.status == AcquireStatus::Ok);
        CHECK(acquired.view != nullptr);
        if (acquired.view == nullptr)
        {
            continue;
        }
        // Render the OSR composite straight into the backbuffer — the real per-window frame (03 §4).
        std::unique_ptr<IRenderPipeline> pipeline =
            device->create_render_pipeline(make_composite_pipeline_desc(chain->format()));
        std::unique_ptr<ICommandEncoder> encoder = device->create_command_encoder();
        RenderPassDesc pass;
        ColorAttachment attachment;
        attachment.view = acquired.view;
        attachment.load = LoadOp::Clear;
        pass.color.push_back(attachment);
        std::unique_ptr<IRenderPassEncoder> render_pass = encoder->begin_render_pass(pass);
        render_pass->set_pipeline(*pipeline);
        render_pass->draw(kCompositeVertexCount, 1);
        render_pass->end();
        std::unique_ptr<ICommandBuffer> commands = encoder->finish();
        device->queue().submit(*commands);
        chain->present();
    }

    auto& fake = static_cast<FakeSwapchain&>(*chain);
    CHECK(fake.acquire_count() == 3);
    CHECK(fake.present_count() == 3);

    // The RENDER half, asserted rather than merely executed: counting acquires and presents alone
    // would stay green with the whole pass encoding gutted.
    CHECK(static_cast<FakeQueue&>(device->queue()).submit_count() == 3);
    // The fake rasterizes a reference triangle on a 3-vertex draw, so the backbuffer must no longer
    // be the clear colour — proof the pipeline + draw actually reached it.
    const std::vector<std::uint8_t>& backbuffer = fake.backbuffer().pixels();
    bool any_drawn = false;
    for (std::size_t i = 0; i + 3 < backbuffer.size(); i += 4)
    {
        if (backbuffer[i] != 0 || backbuffer[i + 1] != 0 || backbuffer[i + 2] != 0)
        {
            any_drawn = true;
            break;
        }
    }
    CHECK(any_drawn);
}

void test_outdated_and_lost_are_recoverable_not_errors()
{
    int window = 0;
    FakeRhi rhi(1);
    std::unique_ptr<IDevice> device = rhi.create_device();
    std::unique_ptr<ISurface> surface = rhi.create_surface(fake_window(window));
    SurfaceConfig config;
    config.size = {64, 32};
    std::unique_ptr<ISwapchain> chain = surface->configure(*device, config);
    CHECK(chain != nullptr);
    if (chain == nullptr)
    {
        return;
    }
    auto& fake = static_cast<FakeSwapchain&>(*chain);

    // A resize raced this frame: no view, and the contract is "reconfigure, then carry on".
    fake.script_acquire(AcquireStatus::Outdated);
    AcquiredFrame outdated = chain->acquire();
    CHECK(outdated.status == AcquireStatus::Outdated);
    CHECK(outdated.view == nullptr);
    // Presenting a frame that was never acquired must be a no-op, never a crash or a phantom frame.
    chain->present();
    CHECK(fake.present_count() == 0);

    chain->resize(Extent2D{128, 64});
    CHECK(chain->size().width == 128);
    AcquiredFrame recovered = chain->acquire();
    CHECK(recovered.status == AcquireStatus::Ok);
    CHECK(recovered.view != nullptr);
    chain->present();
    CHECK(fake.present_count() == 1);

    // Device loss and timeout: same shape — no view, recoverable by rebuilding (03 §7). The STATUS
    // is asserted, not just the null view: reporting either of these as a generic Error is exactly
    // the mis-classification rhi.h warns against, and a view-only check cannot see it.
    fake.script_acquire(AcquireStatus::Lost);
    const AcquiredFrame lost = chain->acquire();
    CHECK(lost.status == AcquireStatus::Lost);
    CHECK(lost.view == nullptr);

    fake.script_acquire(AcquireStatus::Timeout);
    const AcquiredFrame timed_out = chain->acquire();
    CHECK(timed_out.status == AcquireStatus::Timeout);
    CHECK(timed_out.view == nullptr);

    // Recovery after a loss works the same way it does after Outdated — the chain is not poisoned.
    const AcquiredFrame after_loss = chain->acquire();
    CHECK(after_loss.status == AcquireStatus::Ok);
    CHECK(after_loss.view != nullptr);
    chain->present();
    CHECK(fake.present_count() == 2);
}

void test_suboptimal_frame_is_still_presentable()
{
    // Suboptimal is NOT Outdated: the frame is usable and must be presented, then reconfigured.
    int window = 0;
    FakeRhi rhi(1);
    std::unique_ptr<IDevice> device = rhi.create_device();
    std::unique_ptr<ISurface> surface = rhi.create_surface(fake_window(window));
    SurfaceConfig config;
    config.size = {32, 32};
    std::unique_ptr<ISwapchain> chain = surface->configure(*device, config);
    CHECK(chain != nullptr);
    if (chain == nullptr)
    {
        return;
    }
    auto& fake = static_cast<FakeSwapchain&>(*chain);
    fake.set_suboptimal(true);

    const AcquiredFrame acquired = chain->acquire();
    CHECK(acquired.status == AcquireStatus::Ok);
    CHECK(acquired.view != nullptr);
    CHECK(acquired.suboptimal);
    chain->present();
    CHECK(fake.present_count() == 1);
}

void test_resize_and_teardown()
{
    int window = 0;
    FakeRhi rhi(1);
    std::unique_ptr<IDevice> device = rhi.create_device();
    std::unique_ptr<ISurface> surface = rhi.create_surface(fake_window(window));
    SurfaceConfig config;
    config.size = {64, 64};
    std::unique_ptr<ISwapchain> chain = surface->configure(*device, config);
    CHECK(chain != nullptr);
    if (chain == nullptr)
    {
        return;
    }
    auto& fake = static_cast<FakeSwapchain&>(*chain);

    chain->resize(Extent2D{200, 100});
    CHECK(chain->size().width == 200);
    CHECK(chain->size().height == 100);
    CHECK(fake.backbuffer().size().width == 200); // the backbuffer really was rebuilt

    // A minimized window reports a zero extent every frame; tearing the chain down and rebuilding
    // it on every one of those would thrash, so a zero resize is ignored.
    chain->resize(Extent2D{0, 0});
    CHECK(chain->size().width == 200);
    CHECK(fake.resize_count() == 1);

    chain->unconfigure();
    CHECK(!chain->is_configured());
    // Acquiring from an unconfigured chain is an error status, not undefined behavior.
    CHECK(chain->acquire().status == AcquireStatus::Error);
    // Idempotent teardown: the window-destroy path may unconfigure twice.
    chain->unconfigure();
    CHECK(fake.unconfigure_count() == 2);
}

} // namespace

int main()
{
    test_surface_needs_a_real_window();
    test_probe_surface_answers_the_gpu_gate();
    test_capabilities_gate_the_configure();
    test_frame_loop_acquire_render_present();
    test_outdated_and_lost_are_recoverable_not_errors();
    test_suboptimal_frame_is_still_presentable();
    test_resize_and_teardown();
    RENDER_TEST_MAIN_END();
}
