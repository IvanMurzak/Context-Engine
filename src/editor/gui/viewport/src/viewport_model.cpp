// Observer-viewport model: render-snapshot summary + the L-41 present-outcome computation.

#include "context/editor/gui/viewport/viewport_model.h"

namespace context::editor::gui::viewport
{

ViewportSceneSummary summarize(const context::render::RenderSnapshot& snapshot)
{
    ViewportSceneSummary summary;
    summary.drawables = static_cast<std::uint32_t>(snapshot.items.size());
    summary.directional_lights = static_cast<std::uint32_t>(snapshot.directional_lights.size());
    summary.point_lights = static_cast<std::uint32_t>(snapshot.point_lights.size());
    return summary;
}

ViewportPresent compute_present(compositor::HostPlatform platform,
                                const compositor::SurfaceCapabilities& caps, bool adapter_available,
                                bool scene_render_ok, std::uint32_t width, std::uint32_t height)
{
    ViewportPresent out;
    out.handoff = compositor::make_handoff(platform, caps);

    if (!adapter_available)
    {
        // R-HEAD-002: no GPU adapter — the observer viewport reports absence, never a fabricated frame.
        out.ok = false;
        out.error_code = kViewportAdapterAbsentCode;
        return out;
    }
    if (out.handoff.shared_texture && !caps.gpu_compositing)
    {
        // The selected L-41 mode demands a GPU shared-texture surface but no GPU compositor is present
        // and this platform has no software-OSR fallback (macOS IOSurface) — the surface can't be
        // acquired. On Windows/Linux the L-41 tree falls back to software OSR, so this never trips there.
        out.ok = false;
        out.error_code = kViewportSurfaceUnavailableCode;
        return out;
    }
    if (!scene_render_ok)
    {
        // The scene render / pixel readback for the observer viewport failed (R-REND-002).
        out.ok = false;
        out.error_code = kViewportRenderFailedCode;
        out.width = width;
        out.height = height;
        return out;
    }

    out.ok = true;
    out.width = width;
    out.height = height;
    return out;
}

} // namespace context::editor::gui::viewport
