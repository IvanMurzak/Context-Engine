// The native viewport OBSERVER panel (M5-F1, issue #164; R-EDIT-001 / R-UI-007 / L-41 / R-REND-002 /
// R-A11Y-001 / R-HUX-011): projects the observer viewport into a headless context_gui_uitree Panel —
// a labelled render surface describing the "live scene (3D+2D)" the viewport composites over
// context_render(_wgpu), a status line carrying the L-41 compositing mode + scene summary + present
// outcome, and a keyboard-reachable "frame scene" observer affordance that drives the R-HUX-011
// gesture->viewport-update loop. READ-ONLY OBSERVER: no writes into the world, no gizmos, no
// in-viewport override editing (R-HUX-006 -> M8.5). The whole panel is CI-assertable WITHOUT booting
// CEF (the render pixels + golden-scene equivalence are the render-layer proof, viewport_scene.h).

#pragma once

#include "context/editor/gui/viewport/viewport_model.h"

#include "context/editor/gui/compositor/surface.h"
#include "context/editor/gui/uitree/panel.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace context::editor::gui::viewport
{

// The R-HUX-011 gesture->viewport-update loop event — the third core interactive loop R-HUX-011
// names, alongside the scene-tree selection loop and the inspector commit loop. Fired when the
// observer view re-frames. The real input->paint latency timestamp is captured at the CEF host around
// this seam (R-EDIT-001 / R-HUX-011 "instrumented timestamps in the real event path"); the headless
// panel ships the loop seam, exactly like SceneTreePanel's SceneSelection.
struct ViewportUpdate
{
    std::uint64_t view_generation = 0; // increments on each re-frame
    ViewportSceneSummary scene;        // what the re-framed observer view now shows
};

// The command a focusable node binds so the observer's "frame scene" affordance has a keyboard path
// (R-CLI-001 CLI-completeness as a structural accessibility property — every GUI action reachable
// without a pointer). OBSERVER-ONLY: re-frames the view camera; it never writes the world.
inline constexpr const char* kFrameSceneCommand = "viewport.frame-scene";

class ViewportPanel
{
public:
    // The R-EDIT-001 contribution id this built-in panel registers under.
    static constexpr const char* kContributionId = "builtin.viewport";

    ViewportPanel();

    // Replace the observed live scene (the extracted render snapshot the viewport composites).
    // Read-only — the panel only observes it. Re-summarizes; does NOT change the present outcome.
    void set_snapshot(const context::render::RenderSnapshot& snapshot);
    [[nodiscard]] const ViewportSceneSummary& scene() const noexcept { return scene_; }

    // Set the present environment: the host platform, the probed compositor caps, whether a rendering
    // adapter is available, the composited frame size, and whether the last scene render read back.
    // Recomputes the L-41 present outcome (context_gui_compositor). The CEF host supplies this from
    // its real probe; the headless default is a presentable software-composited frame.
    void set_present_env(compositor::HostPlatform platform,
                         const compositor::SurfaceCapabilities& caps, bool adapter_available,
                         bool scene_render_ok, std::uint32_t width, std::uint32_t height);
    [[nodiscard]] const ViewportPresent& present() const noexcept { return present_; }

    // The R-HUX-011 gesture->viewport-update loop: re-frame the observer view (frame the whole scene).
    // OBSERVER-ONLY (no world writes). Advances the view generation and notifies every listener with
    // the current scene summary — the loop event the host times input->paint latency around.
    void frame_scene();
    [[nodiscard]] std::uint64_t view_generation() const noexcept { return view_generation_; }

    // Register a listener other panels / the host use to react to view re-framing (R-HUX-011).
    using ViewUpdateListener = std::function<void(const ViewportUpdate&)>;
    void add_view_update_listener(ViewUpdateListener listener);

    // Build the headless uitree Panel for the current scene + present + view generation. Deterministic:
    // identical state produces a byte-identical Panel (uitree::render_html). a11y-conformant by
    // construction — uitree::audit_a11y returns no violations for any state.
    [[nodiscard]] uitree::Panel build_panel() const;

private:
    void notify() const;

    ViewportSceneSummary scene_;
    ViewportPresent present_;
    std::uint64_t view_generation_ = 0;
    std::vector<ViewUpdateListener> listeners_;
};

} // namespace context::editor::gui::viewport
