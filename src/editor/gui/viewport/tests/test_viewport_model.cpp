// Observer-viewport model tests (R-QA-013): the render-snapshot summary + the L-41 present-outcome
// computation across the reserved viewport.* failure classes and the ratified per-platform modes.

#include "context/editor/gui/viewport/viewport_model.h"

#include "context/editor/gui/compositor/surface.h"
#include "context/render/render_world.h"

#include "viewport_test.h"

#include <cstdint>

using namespace context::editor::gui::viewport;
namespace compositor = context::editor::gui::compositor;
namespace render = context::render;

namespace
{

render::RenderSnapshot make_snapshot(std::uint32_t drawables, std::uint32_t dir_lights,
                                     std::uint32_t point_lights)
{
    render::RenderSnapshot snapshot;
    snapshot.items.resize(drawables);
    snapshot.directional_lights.resize(dir_lights);
    snapshot.point_lights.resize(point_lights);
    return snapshot;
}

void test_summarize()
{
    const ViewportSceneSummary empty = summarize(render::RenderSnapshot{});
    CHECK(empty.empty());
    CHECK(empty.drawables == 0 && empty.directional_lights == 0 && empty.point_lights == 0);

    const ViewportSceneSummary s = summarize(make_snapshot(4, 1, 2));
    CHECK(!s.empty());
    CHECK(s.drawables == 4);
    CHECK(s.directional_lights == 1);
    CHECK(s.point_lights == 2);
}

void test_present_ok_windows_accelerated()
{
    // Windows + a GPU compositor -> the L-41 accelerated-OSR shared-texture primary; a presentable
    // frame. external_begin_frame is ALWAYS false (L-41 / cef#4033).
    compositor::SurfaceCapabilities caps; // gpu_compositing = true by default
    const ViewportPresent p =
        compute_present(compositor::HostPlatform::windows, caps, /*adapter=*/true, /*render_ok=*/true, 256, 256);
    CHECK(p.ok);
    CHECK(p.error_code.empty());
    CHECK(p.handoff.mode == compositor::CompositingMode::accelerated_osr);
    CHECK(p.handoff.shared_texture);
    CHECK(!p.handoff.external_begin_frame);
    CHECK(p.width == 256 && p.height == 256);
}

void test_present_ok_linux_software()
{
    // Linux default (no Mesa/X11-ozone gate) -> software OSR is the shipped default; still presentable
    // (cpu-readback, not shared-texture).
    compositor::SurfaceCapabilities caps; // gpu_compositing true, mesa_x11_ozone false
    const ViewportPresent p =
        compute_present(compositor::HostPlatform::linux_, caps, /*adapter=*/true, /*render_ok=*/true, 256, 256);
    CHECK(p.ok);
    CHECK(p.handoff.mode == compositor::CompositingMode::software_osr);
    CHECK(!p.handoff.shared_texture);
}

void test_present_adapter_absent()
{
    // No rendering adapter -> viewport.adapter_absent (R-HEAD-002), regardless of platform/caps.
    compositor::SurfaceCapabilities caps;
    const ViewportPresent p = compute_present(compositor::HostPlatform::linux_, caps, /*adapter=*/false,
                                      /*render_ok=*/true, 256, 256);
    CHECK(!p.ok);
    CHECK(p.error_code == kViewportAdapterAbsentCode);
}

void test_present_surface_unavailable_macos_no_gpu()
{
    // macOS always selects IOSurface (shared-texture) and L-41 has NO software fallback there; with no
    // GPU compositor the surface can't be acquired -> viewport.surface_unavailable (R-UI-007 / L-41).
    compositor::SurfaceCapabilities caps;
    caps.gpu_compositing = false;
    const ViewportPresent p = compute_present(compositor::HostPlatform::macos, caps, /*adapter=*/true,
                                      /*render_ok=*/true, 256, 256);
    CHECK(!p.ok);
    CHECK(p.error_code == kViewportSurfaceUnavailableCode);
    CHECK(p.handoff.mode == compositor::CompositingMode::iosurface);

    // Windows/Linux fall back to software OSR when there is no GPU compositor, so they never hit this.
    const ViewportPresent win = compute_present(compositor::HostPlatform::windows, caps, /*adapter=*/true,
                                        /*render_ok=*/true, 256, 256);
    CHECK(win.ok);
    CHECK(win.handoff.mode == compositor::CompositingMode::software_osr);
}

void test_present_render_failed()
{
    // Surface acquirable + adapter present, but the scene render/readback failed -> viewport.render_failed.
    compositor::SurfaceCapabilities caps;
    const ViewportPresent p = compute_present(compositor::HostPlatform::windows, caps, /*adapter=*/true,
                                      /*render_ok=*/false, 256, 256);
    CHECK(!p.ok);
    CHECK(p.error_code == kViewportRenderFailedCode);
    // The failure still reports the surface it would have used + the intended frame size.
    CHECK(p.handoff.mode == compositor::CompositingMode::accelerated_osr);
    CHECK(p.width == 256 && p.height == 256);
}

void test_all_three_codes_distinct()
{
    // The reserved viewport.* block is three distinct codes (the wave's single code-minter).
    CHECK(std::string(kViewportSurfaceUnavailableCode) == "viewport.surface_unavailable");
    CHECK(std::string(kViewportRenderFailedCode) == "viewport.render_failed");
    CHECK(std::string(kViewportAdapterAbsentCode) == "viewport.adapter_absent");
    CHECK(std::string(kViewportSurfaceUnavailableCode) != kViewportRenderFailedCode);
    CHECK(std::string(kViewportRenderFailedCode) != kViewportAdapterAbsentCode);
}

} // namespace

int main()
{
    test_summarize();
    test_present_ok_windows_accelerated();
    test_present_ok_linux_software();
    test_present_adapter_absent();
    test_present_surface_unavailable_macos_no_gpu();
    test_present_render_failed();
    test_all_three_codes_distinct();
    VIEWPORT_TEST_MAIN_END();
}
