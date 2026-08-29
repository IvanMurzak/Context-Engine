// The macOS HYBRID-CHROME seam (editor-window-chrome c1, target design 02 §4) — the split that
// keeps every DECISION this task adds testable on all three legs while the AppKit calls stay in
// cocoa_window.mm, honestly untested off macOS (window.h states the discipline).
//
// WHAT c1 IS. On macOS the content extends under a TRANSPARENT titlebar
// (NSWindowStyleMaskFullSizeContentView + titlebarAppearsTransparent), the native traffic lights
// stay exactly where macOS puts them, and the a2 titlebar strip pads by a MEASURED inset so its
// content never sits under the buttons. A press on the strip's published `caption` region hands the
// drag to the OS (`performWindowDragWithEvent:`); a double-click on it is `zoom:` — the platform
// convention. `chrome.state` flips from the a1 interim `system`/0 to `hybrid`/measured in the same
// change that makes it true (tasks/README.md interim honesty).
//
// THE THREE HALVES, and where each lives:
//
//   * PURE DECISIONS (this header + cocoa_chrome.cpp, compiled and ctest-run on every OS):
//     `caption_press_action` — which decoded pointer press the Cocoa pump must hand to the OS —
//     and `ns_hybrid_controls_inset` — the traffic-light cluster's measured frame becoming the
//     physical-pixel inset `chrome.state.controlsInset` serves.
//   * THE APPKIT CALLS (cocoa_window.mm): reading `standardWindowButton:` frames, the live
//     style-mask truth, `performWindowDragWithEvent:` / `zoom:` inside the pump.
//   * THE QUERY/WIRING SURFACE (declared here, real in cocoa_window.mm, an honest refusal in
//     cocoa_window.cpp off macOS — the make_cocoa_window_backend linkable-symbol pattern): how the
//     composition root asks a backend for its hybrid-chrome truth without widening IWindowBackend.
//     Keyed on `IWindowBackend::name() == "cocoa"` — the one string only the real Cocoa backend
//     answers — rather than RTTI, so the downcast is confined to the TU that defines the type.
//     b1's Windows-frameless work is expected to grow its own platform seam beside this one; a
//     shared virtual on IWindowBackend is a later refactor once BOTH platform shapes exist.

#pragma once

#include "context/editor/shell/dpi.h"
#include "context/editor/shell/input.h"

#include <cstddef>
#include <cstdint>

namespace context::editor::shell
{

class IWindowBackend;

// What the Cocoa backend reports for `chrome.state` when its window really does hybrid chrome:
// the physical pixels the titlebar strip must reserve for the OS-drawn traffic lights (02 §1).
// Exactly one side is non-zero for a real cluster — left on an LTR desktop, right where macOS
// mirrors the buttons for an RTL system language.
struct CocoaChromeState
{
    std::uint32_t inset_left = 0;
    std::uint32_t inset_right = 0;
};

// What a decoded pointer PRESS over the arbiter's published region map asks the Cocoa pump to do.
enum class CaptionPressAction
{
    none,    // not the caption's press — decode and dispatch as always
    drag,    // single press on the caption drag surface: hand the NSEvent to performWindowDragWithEvent:
    zoom,    // double-click on the caption: `zoom:` (the platform convention, 02 §4)
    yielded, // ON the caption, but a live capture owns the press: leave it to the arbiter, which
             // routes it to the capture target (another button's drag) or swallows it (a modal
             // backdrop) — exactly as route_pointer will, because that verdict is what was asked
};

// Decide the caption consult for ONE decoded pointer event: the arbiter's OWN verdict, asked
// side-effect free through InputArbiter::preview_pointer, so the pump and the arbiter can never
// disagree about who owns a press. The region half is the same back-to-front last-match-wins
// hit-test (a control published after the caption wins without a carve-out token — input.h); the
// CAPTURE half is the same capture state route_pointer consults (a right-button orbit that wandered
// onto the strip keeps its press; an open dropdown's backdrop swallows it). Only a LEFT press can be
// the caption's: moves keep hovering the strip, releases belong to whatever owns the press, and the
// right button is the context-menu gesture the browser must keep seeing.
[[nodiscard]] CaptionPressAction caption_press_action(const PointerEvent& pointer,
                                                      const InputArbiter& arbiter);

// The traffic-light cluster's measured extent becoming the `controlsInset` physical pixels.
//
// Inputs are Cocoa POINTS in WINDOW coordinates (x from the window's LEFT edge — the y axis and its
// flip are irrelevant to a horizontal inset): `min_x`/`max_x` bound the union of the
// `standardWindowButton:` frames, `width` is the content width the strip spans. The inset is the
// cluster's far edge plus a SYMMETRIC pad (the same gap the cluster keeps from the window edge), on
// whichever side the cluster sits — macOS mirrors the buttons to the RIGHT for RTL system
// languages, and measuring rather than assuming is the whole point of this function. Physical px,
// rounded UP: a strip that pads one pixel short sits under a button; one pixel long is invisible.
// Degenerate input (non-finite, an empty cluster, a zero-width window) answers the honest {0, 0}.
[[nodiscard]] CocoaChromeState ns_hybrid_controls_inset(double min_x_points, double max_x_points,
                                                        double width_points, DpiScale dpi);

// What the pump consumed — and what it declined — for the windowed smoke's suppression assertions
// (the same observable pattern WindowBridge's counters serve the live CEF smokes). `yields` counts
// caption presses the consult left to the arbiter because a capture owned them.
struct CocoaCaptionStats
{
    std::size_t drags = 0;
    std::size_t zooms = 0;
    std::size_t yields = 0;
};

// --- the backend query/wiring surface (real on macOS, an honest refusal elsewhere) ---------------

// Ask `backend` for its hybrid-chrome truth. True — with the measured inset in `out` — ONLY when
// `backend` is the live Cocoa backend AND its window really carries the hybrid style right now
// (FullSizeContentView + a transparent titlebar, read back from the NSWindow at call time). That
// read-back is the interim-honesty rule made structural: revert the style and `chrome.state` falls
// back to `system` by construction, instead of reporting a mode the window no longer does. False
// for every other backend (headless, win32, x11) and in every non-macOS build.
[[nodiscard]] bool cocoa_hybrid_chrome(const IWindowBackend& backend, CocoaChromeState& out);

// Give a Cocoa backend read access to a window's LIVE input arbiter, so its pump can consult the
// published `caption` rects AND the live capture state at NSEvent time — the only moment
// `performWindowDragWithEvent:` still has the event in hand (dispatch-time consumers are too late;
// the NSEvent is gone). The pointer must outlive every pump() call (EditorWindow wires its own
// arbiter, whose lifetime this holds for). No-op for every non-Cocoa backend and in every non-macOS
// build; nullptr unbinds.
void cocoa_bind_caption_arbiter(IWindowBackend& backend, const InputArbiter* arbiter);

// Read what the pump consumed so far. True only for the live Cocoa backend, like the query above.
[[nodiscard]] bool cocoa_caption_stats(const IWindowBackend& backend, CocoaCaptionStats& out);

} // namespace context::editor::shell
