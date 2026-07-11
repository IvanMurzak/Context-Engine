// Observer-viewport panel tests (R-QA-013): the headless uitree projection, the L-41 present seam,
// the R-HUX-011 gesture->viewport-update loop event, and observed-scene / present-state reflection.

#include "context/editor/gui/viewport/viewport_panel.h"

#include "context/editor/gui/compositor/surface.h"
#include "context/editor/gui/uitree/node.h"
#include "context/editor/gui/uitree/panel.h"
#include "context/render/render_world.h"

#include "viewport_test.h"

#include <cstdint>
#include <string>

using namespace context::editor::gui::viewport;
namespace compositor = context::editor::gui::compositor;
namespace uitree = context::editor::gui::uitree;
namespace render = context::render;

namespace
{

// Whether the panel's rendered HTML contains a substring — used to assert state reaches the surface.
[[nodiscard]] bool html_has(const uitree::Panel& panel, const std::string& needle)
{
    return uitree::render_html(panel).find(needle) != std::string::npos;
}

void test_default_panel_shape()
{
    ViewportPanel panel;
    CHECK(panel.scene().empty());
    CHECK(panel.view_generation() == 0);

    const uitree::Panel ui = panel.build_panel();
    CHECK(ui.id() == "viewport");
    CHECK(ui.title() == "Viewport");
    CHECK(ui.has_command(kFrameSceneCommand));
    // The default present is a presentable frame on this host (the CEF host overrides it).
    CHECK(panel.present().ok);
    // The frame-scene command has exactly one keyboard path (the focusable button).
    CHECK(uitree::focus_order(ui).size() == 1);
    CHECK(uitree::focus_order(ui).front() == "viewport.frame");
}

void test_present_env_reaches_status_and_surface()
{
    ViewportPanel panel;
    compositor::SurfaceCapabilities caps; // gpu on
    panel.set_present_env(compositor::HostPlatform::windows, caps, /*adapter=*/true,
                          /*render_ok=*/true, 256, 256);
    CHECK(panel.present().handoff.mode == compositor::CompositingMode::accelerated_osr);

    const uitree::Panel ui = panel.build_panel();
    // The L-41 compositing mode reaches the status line + the surface description (composited via ...).
    CHECK(html_has(ui, "accelerated-osr"));
    CHECK(html_has(ui, "shared-texture"));
    CHECK(html_has(ui, "composited via accelerated-osr"));

    // An adapter-absent present surfaces the reserved viewport.* code (no fabricated frame).
    panel.set_present_env(compositor::HostPlatform::linux_, caps, /*adapter=*/false,
                          /*render_ok=*/true, 256, 256);
    CHECK(panel.present().error_code == kViewportAdapterAbsentCode);
    const uitree::Panel down = panel.build_panel();
    CHECK(html_has(down, "viewport.adapter_absent"));
}

void test_snapshot_summary_reaches_panel()
{
    ViewportPanel panel;
    render::RenderSnapshot snapshot;
    snapshot.items.resize(3);
    snapshot.directional_lights.resize(1);
    panel.set_snapshot(snapshot);
    CHECK(panel.scene().drawables == 3);
    CHECK(panel.scene().directional_lights == 1);

    const uitree::Panel ui = panel.build_panel();
    CHECK(html_has(ui, "3 drawables"));
}

void test_frame_scene_fires_the_r_hux_011_loop()
{
    ViewportPanel panel;
    render::RenderSnapshot snapshot;
    snapshot.items.resize(2);
    panel.set_snapshot(snapshot);

    int seen = 0;
    std::uint64_t last_gen = 0;
    ViewportSceneSummary last_scene;
    panel.add_view_update_listener(
        [&](const ViewportUpdate& u)
        {
            ++seen;
            last_gen = u.view_generation;
            last_scene = u.scene;
        });

    panel.frame_scene(); // the R-HUX-011 gesture->viewport-update loop event fires
    CHECK(seen == 1);
    CHECK(last_gen == 1);
    CHECK(panel.view_generation() == 1);
    CHECK(last_scene.drawables == 2); // the loop event carries what the re-framed view shows

    panel.frame_scene();
    CHECK(seen == 2);
    CHECK(panel.view_generation() == 2);

    // The view generation reaches the status line (a re-frame is a visible, deterministic event).
    CHECK(html_has(panel.build_panel(), "view 2"));
}

void test_observer_only_no_extra_commands()
{
    // The observer viewport exposes exactly ONE command — the frame-scene view affordance. NO
    // authoring/gizmo/override commands (R-HUX-006 in-viewport editing is M8.5, out of scope).
    ViewportPanel panel;
    const uitree::Panel ui = panel.build_panel();
    CHECK(ui.commands().size() == 1);
    CHECK(ui.commands().front().id == kFrameSceneCommand);
}

} // namespace

int main()
{
    test_default_panel_shape();
    test_present_env_reaches_status_and_surface();
    test_snapshot_summary_reaches_panel();
    test_frame_scene_fires_the_r_hux_011_loop();
    test_observer_only_no_extra_commands();
    VIEWPORT_TEST_MAIN_END();
}
