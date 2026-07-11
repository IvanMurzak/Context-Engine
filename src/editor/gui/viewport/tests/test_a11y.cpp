// The viewport panel's per-panel a11y scan + keyboard-only navigation assertion (R-A11Y-001 /
// R-EDIT-001), headless on the default matrix (no CEF). This is the M5-F1 half of the a11y coverage
// the M5-F6 harness reconciles (registered in a11y/registry.cpp + coverage.manifest.jsonl; the
// defensive fragment is a11y/coverage/viewport.json). Asserts the observer viewport has zero a11y
// violations AND its "frame scene" command has a complete keyboard path, across every present state
// (ready / adapter-absent / surface-unavailable / render-failed) and empty vs populated scenes.

#include "context/editor/gui/viewport/viewport_panel.h"

#include "context/editor/gui/compositor/surface.h"
#include "context/editor/gui/uitree/panel.h"
#include "context/render/render_world.h"

#include "viewport_test.h"

using namespace context::editor::gui::viewport;
namespace compositor = context::editor::gui::compositor;
namespace uitree = context::editor::gui::uitree;
namespace render = context::render;

namespace
{

// The a11y + keyboard-nav gate for a panel state: zero violations AND the frame-scene command reachable
// by keyboard (exactly one focusable node — the observer viewport's single view affordance).
void assert_a11y_clean(const ViewportPanel& panel)
{
    const uitree::Panel ui = panel.build_panel();
    CHECK(uitree::audit_a11y(ui).empty());
    CHECK(uitree::focus_order(ui).size() == 1);
    CHECK(ui.has_command(kFrameSceneCommand));
}

} // namespace

int main()
{
    // Default (presentable) state.
    {
        ViewportPanel panel;
        assert_a11y_clean(panel);
    }

    // Populated scene (drawables + lights) — the surface description carries the summary; still clean.
    {
        ViewportPanel panel;
        render::RenderSnapshot snapshot;
        snapshot.items.resize(5);
        snapshot.directional_lights.resize(1);
        snapshot.point_lights.resize(3);
        panel.set_snapshot(snapshot);
        assert_a11y_clean(panel);
    }

    // Each reserved-viewport.* failure present state stays a11y-clean (the error surfaces as text, not
    // as an unlabelled/unreachable node).
    {
        compositor::SurfaceCapabilities caps;

        // adapter absent
        ViewportPanel absent;
        absent.set_present_env(compositor::HostPlatform::linux_, caps, /*adapter=*/false,
                               /*render_ok=*/true, 256, 256);
        assert_a11y_clean(absent);

        // surface unavailable (macOS IOSurface + no GPU compositor)
        compositor::SurfaceCapabilities no_gpu;
        no_gpu.gpu_compositing = false;
        ViewportPanel unavailable;
        unavailable.set_present_env(compositor::HostPlatform::macos, no_gpu, /*adapter=*/true,
                                    /*render_ok=*/true, 256, 256);
        assert_a11y_clean(unavailable);

        // render failed
        ViewportPanel failed;
        failed.set_present_env(compositor::HostPlatform::windows, caps, /*adapter=*/true,
                               /*render_ok=*/false, 256, 256);
        assert_a11y_clean(failed);
    }

    // After re-framing (R-HUX-011 loop advanced) the panel is still a11y-clean.
    {
        ViewportPanel panel;
        panel.frame_scene();
        panel.frame_scene();
        assert_a11y_clean(panel);
    }

    VIEWPORT_TEST_MAIN_END();
}
