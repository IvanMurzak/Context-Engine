// The observer-viewport model (M5-F1, issue #164; R-EDIT-001 / R-UI-007 / L-41 / R-REND-002 /
// R-HEAD-002 / R-A11Y-001 / R-HUX-011): the headless, CEF-free logic of the native viewport panel.
// It summarizes the extracted render snapshot (the "live scene (3D+2D)" the viewport observes over
// context_render) and computes the L-41 present outcome — which per-platform CEF compositing surface
// the composited frame is handed to CEF through (context_gui_compositor), and the failure classes a
// present can hit. READ-ONLY OBSERVER: no writes into the world (in-viewport override editing is
// R-HUX-006 -> M8.5, explicitly out of M5). The whole model is CI-assertable WITHOUT booting CEF.

#pragma once

#include "context/editor/gui/compositor/surface.h"

#include "context/render/render_world.h" // render::RenderSnapshot — the observed live scene

#include <cstdint>
#include <string>

namespace context::editor::gui::viewport
{

// The reserved `viewport.*` error-domain block (M5-F1 mints it — the wave's single code-minter).
// Owned HERE as string constants (the promote-a-local-string pattern of bridge::kScopeDeniedCode /
// ts::kTs*Code / pkg::kInstall*Code) so this GUI lib does NOT link the editor/contract catalog;
// src/editor/contract/src/error_catalog.cpp registers the SAME strings (append-only tail). USED by
// present() below (and asserted by the panel's tests + the contract catalog test).
inline constexpr const char* kViewportSurfaceUnavailableCode = "viewport.surface_unavailable";
inline constexpr const char* kViewportRenderFailedCode = "viewport.render_failed";
inline constexpr const char* kViewportAdapterAbsentCode = "viewport.adapter_absent";

// A summary of the extracted render snapshot the observer viewport composites (the render-relevant
// state of one sim tick — L-39): the 3D drawables and the real-time lights. Deterministic.
struct ViewportSceneSummary
{
    std::uint32_t drawables = 0;          // render::RenderItem count (the 3D scene)
    std::uint32_t directional_lights = 0; // render::DirectionalLightItem count
    std::uint32_t point_lights = 0;       // render::PointLightItem count

    [[nodiscard]] bool empty() const noexcept
    {
        return drawables == 0 && directional_lights == 0 && point_lights == 0;
    }
};

// Summarize an extracted render snapshot into the observer viewport's scene summary.
[[nodiscard]] ViewportSceneSummary summarize(const context::render::RenderSnapshot& snapshot);

// The observer viewport's present outcome: whether a frame could be composited for the platform, the
// L-41 surface handoff the present uses (context_gui_compositor::make_handoff — the ratified
// per-platform CEF compositing mode + shared-texture handoff), the composited frame size, and (on
// failure) exactly one reserved `viewport.*` code. Read-only observer — no authoring.
struct ViewportPresent
{
    bool ok = false;
    std::string error_code;               // empty when ok; else one of the kViewport*Code constants
    compositor::SurfaceHandoff handoff{}; // the L-41 surface handoff selected for this present
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// Compute the L-41 present outcome for the observer viewport on `platform` given the probed compositor
// `caps`, whether a rendering `adapter_available`, and whether the last scene render read back
// (`scene_render_ok`). Deterministic + total. In order:
//   * no rendering adapter        -> viewport.adapter_absent  (R-HEAD-002: absence is REPORTED, the
//                                    observer viewport never fabricates a frame without a GPU);
//   * the selected L-41 mode needs a GPU shared-texture surface but there is no GPU compositor (the
//     macOS-IOSurface-without-a-GPU case — L-41 has no software-OSR fallback on macOS) ->
//                                    viewport.surface_unavailable (R-UI-007 / L-41);
//   * the scene render/readback failed -> viewport.render_failed (R-REND-002);
//   * otherwise ok, carrying the L-41 handoff + the composited `width`x`height`.
[[nodiscard]] ViewportPresent compute_present(compositor::HostPlatform platform,
                                              const compositor::SurfaceCapabilities& caps,
                                              bool adapter_available, bool scene_render_ok,
                                              std::uint32_t width, std::uint32_t height);

} // namespace context::editor::gui::viewport
