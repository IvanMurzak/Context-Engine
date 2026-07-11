// Observer viewport panel: the live scene (3D+2D) projected into a headless uitree Panel with the
// L-41 present outcome + the R-HUX-011 gesture->viewport-update loop seam.

#include "context/editor/gui/viewport/viewport_panel.h"

#include "context/editor/gui/uitree/node.h"

#include "context/render/offscreen_scene.h" // offscreen_triangle_size() — the composite target edge

#include <sstream>
#include <string>
#include <utility>

namespace context::editor::gui::viewport
{

namespace
{

// The grep-stable token for an L-41 compositing mode (mirrors the CEF host's mode_token).
[[nodiscard]] const char* mode_token(compositor::CompositingMode mode)
{
    switch (mode)
    {
    case compositor::CompositingMode::accelerated_osr:
        return "accelerated-osr";
    case compositor::CompositingMode::software_osr:
        return "software-osr";
    case compositor::CompositingMode::iosurface:
        return "iosurface";
    }
    return "software-osr";
}

// A human/AT-facing description of the observed scene's composited layers.
[[nodiscard]] std::string scene_text(const ViewportSceneSummary& scene)
{
    if (scene.empty())
    {
        return "empty scene";
    }
    std::ostringstream out;
    out << scene.drawables << (scene.drawables == 1 ? " drawable" : " drawables");
    const std::uint32_t lights = scene.directional_lights + scene.point_lights;
    if (lights > 0)
    {
        out << ", " << lights << (lights == 1 ? " light" : " lights");
    }
    return out.str();
}

// The status line: the L-41 compositing mode + shared-texture handoff, the observed scene summary,
// the view generation, and the present outcome (ready, or the reserved viewport.* code). Deterministic.
[[nodiscard]] std::string status_text(const ViewportPresent& present,
                                      const ViewportSceneSummary& scene,
                                      std::uint64_t view_generation)
{
    std::ostringstream out;
    out << "Viewport - " << mode_token(present.handoff.mode) << " ("
        << (present.handoff.shared_texture ? "shared-texture" : "cpu-readback") << ") - "
        << scene_text(scene) << " - view " << view_generation << " - "
        << (present.ok ? "ready" : present.error_code);
    return out.str();
}

// The render-surface description (the a11y analog of an image's alt text): what the composited
// observer frame shows, and how it is handed to CEF. Deterministic.
[[nodiscard]] std::string surface_text(const ViewportPresent& present,
                                       const ViewportSceneSummary& scene)
{
    std::ostringstream out;
    if (!present.ok)
    {
        out << "Observer viewport unavailable (" << present.error_code << ")";
        return out.str();
    }
    out << "Observer viewport: 3D scene (" << scene.drawables
        << (scene.drawables == 1 ? " drawable" : " drawables") << ") + 2D overlay, " << present.width
        << "x" << present.height << ", composited via " << mode_token(present.handoff.mode);
    return out.str();
}

} // namespace

ViewportPanel::ViewportPanel()
{
    // Default: a presentable software-composited frame on this host's platform (the CEF host overrides
    // this from its real GPU/compositor probe via set_present_env). Default caps -> gpu_compositing on.
    const std::uint32_t edge = context::render::offscreen_triangle_size();
    present_ = compute_present(compositor::current_platform(), compositor::SurfaceCapabilities{},
                               /*adapter_available=*/true, /*scene_render_ok=*/true, edge, edge);
}

void ViewportPanel::set_snapshot(const context::render::RenderSnapshot& snapshot)
{
    scene_ = summarize(snapshot);
}

void ViewportPanel::set_present_env(compositor::HostPlatform platform,
                                    const compositor::SurfaceCapabilities& caps,
                                    bool adapter_available, bool scene_render_ok, std::uint32_t width,
                                    std::uint32_t height)
{
    present_ = compute_present(platform, caps, adapter_available, scene_render_ok, width, height);
}

void ViewportPanel::frame_scene()
{
    ++view_generation_;
    notify();
}

void ViewportPanel::add_view_update_listener(ViewUpdateListener listener)
{
    listeners_.push_back(std::move(listener));
}

void ViewportPanel::notify() const
{
    const ViewportUpdate update{view_generation_, scene_};
    for (const ViewUpdateListener& listener : listeners_)
    {
        if (listener)
        {
            listener(update);
        }
    }
}

uitree::Panel ViewportPanel::build_panel() const
{
    using uitree::Role;
    using uitree::UiNode;

    uitree::Panel panel("viewport", "Viewport");
    // The observer's keyboard-reachable "frame scene" affordance (R-A11Y-001 / R-CLI-001). Always
    // exposed and bound to the focusable button below, so it is never an unreachable a11y orphan.
    panel.add_command(kFrameSceneCommand, "Frame scene");

    UiNode root(Role::region, "viewport.panel");
    root.set_label("Viewport");

    root.add_child(
        UiNode(Role::heading, "viewport.heading").set_label("Viewport").set_text("Viewport"));

    root.add_child(UiNode(Role::status, "viewport.status")
                       .set_label("Viewport status")
                       .set_text(status_text(present_, scene_, view_generation_)));

    // The composited render surface — a labelled landmark whose accessible name describes the scene
    // (the a11y equivalent of an <img> with alt text; the CEF host paints the real pixels here).
    UiNode surface(Role::region, "viewport.surface");
    const std::string surface_desc = surface_text(present_, scene_);
    surface.set_label(surface_desc);
    surface.add_child(UiNode(Role::text, "viewport.surface.desc").set_text(surface_desc));
    root.add_child(std::move(surface));

    // The observer "frame scene" control: focusable + command-bound, so the R-HUX-011 loop has a
    // complete keyboard path. Observer-only (re-frames the view; no world writes).
    root.add_child(UiNode(Role::button, "viewport.frame")
                       .set_label("Frame scene")
                       .set_text("Frame scene")
                       .set_focusable(true)
                       .set_command(kFrameSceneCommand));

    panel.set_root(std::move(root));
    return panel;
}

} // namespace context::editor::gui::viewport
