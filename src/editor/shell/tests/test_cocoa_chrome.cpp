// T1 for the macOS hybrid-chrome seam (editor-window-chrome c1, target design 02 §4).
//
// WHAT THIS PROVES, on ALL THREE legs (the AppKit half is honestly untested off macOS — window.h's
// discipline — and its windowed proof is `editor-shell-cocoa-window`'s c1 step):
//
//   1. THE CAPTION CONSULT IS THE ARBITER'S OWN VERDICT — regions AND capture. `caption_press_action`
//      decides over the REAL InputArbiter: the same back-to-front last-match-wins hit-test
//      route_pointer uses (a control published after the caption wins, a caption published above
//      a viewport wins), and the same live capture state, asked through preview_pointer — a press
//      on the caption while another button's implicit drag or a modal push_capture is live is
//      YIELDED to the arbiter, never consumed as a window drag, and becomes the caption's again the
//      moment that capture ends. Only a LEFT PRESS is ever the caption's — moves, releases, the
//      right and middle buttons all flow to the browser untouched (the "for THAT press only" half
//      of the suppression rule).
//   2. DOUBLE-CLICK IS THE USER'S MACOS PREFERENCE. Cocoa counts clicks on the event
//      (NsEvent::click_count), so the first press of a double-click drags and the second is the
//      double-click; what it DOES is `AppleActionOnDoubleClick` mapped by the pure
//      caption_double_click_action — Maximize/unset/Fill → zoom, Minimize → minimize, None → none
//      (the owner's 2026-08-30 decision over 02 §4's fixed zoom).
//   3. THE MEASURED INSET ARITHMETIC. Traffic-light frames (points, window coords) become the
//      physical `controlsInset` px: the symmetric pad, the ×scale, round-UP (one px short puts
//      strip content under a button), the RTL mirror (macOS moves the buttons right for RTL system
//      languages), and the honest {0,0} for degenerate input — NaN included, which every ordering
//      comparison silently waves through.
//   4. THE SURFACE REFUSES A NON-COCOA BACKEND — the linkable-symbol rule: on every leg the query
//      answers false and the wiring no-ops against the headless backend, so a composition root can
//      call them unconditionally (shell.cpp's EditorWindow constructor does).

#include "context/editor/shell/cocoa_chrome.h"

#include "context/editor/shell/window.h"

#include "shell_test.h"

#include <limits>
#include <vector>

using namespace context::editor::shell;

namespace
{

[[nodiscard]] ShellRegion region(const char* id, RegionKind kind, std::uint32_t x, std::uint32_t y,
                                 std::uint32_t w, std::uint32_t h)
{
    return ShellRegion{id, shelltest::rect(x, y, w, h), kind};
}

[[nodiscard]] PointerEvent press_at(std::int32_t x, std::int32_t y,
                                    MouseButton button = MouseButton::left, std::int32_t clicks = 1,
                                    PointerAction action = PointerAction::down)
{
    PointerEvent pointer;
    pointer.action = action;
    pointer.position = PointI{x, y};
    pointer.button = button;
    pointer.click_count = clicks;
    return pointer;
}

// The a2 titlebar's real publish shape: the caption drag surface FIRST, controls after (02 §6) —
// published into a REAL arbiter, the object the consult reads, so every case here runs the same
// hit-test and the same capture state route_pointer does. A viewport below the strip is what a
// capture can be started on.
[[nodiscard]] std::vector<ShellRegion> titlebar_regions()
{
    return {region("scene", RegionKind::viewport, 0, 76, 1200, 724),
            region("chrome.caption", RegionKind::caption, 100, 0, 1000, 76),
            region("chrome.caption-min", RegionKind::caption_min, 900, 0, 60, 76),
            region("chrome.caption-max", RegionKind::caption_max, 960, 0, 60, 76),
            region("chrome.caption-close", RegionKind::caption_close, 1020, 0, 60, 76)};
}

[[nodiscard]] InputArbiter titlebar_arbiter()
{
    InputArbiter arbiter;
    arbiter.regions().publish(titlebar_regions());
    return arbiter;
}

void a_left_press_on_the_caption_drags_and_a_double_click_is_the_double_click()
{
    const InputArbiter map = titlebar_arbiter();
    CHECK(caption_press_action(press_at(400, 30), map) == CaptionPressAction::drag);
    CHECK(caption_press_action(press_at(400, 30, MouseButton::left, 2), map) ==
          CaptionPressAction::double_click);
    // A triple-click's press still carries click_count >= 2 — it repeats the double-click action
    // (a zoom toggles back), never a surprise drag mid-flurry.
    CHECK(caption_press_action(press_at(400, 30, MouseButton::left, 3), map) ==
          CaptionPressAction::double_click);
}

void the_double_click_action_is_the_users_own_macos_preference()
{
    // The owner's 2026-08-30 decision: honour System Settings › Desktop & Dock › "Double-click a
    // window's title bar to", read as the raw `AppleActionOnDoubleClick` default. The exact tokens
    // macOS writes, and the platform default for everything else — unset included.
    CHECK(caption_double_click_action("Maximize") == CaptionDoubleClickAction::zoom);
    CHECK(caption_double_click_action("Minimize") == CaptionDoubleClickAction::minimize);
    CHECK(caption_double_click_action("None") == CaptionDoubleClickAction::none);
    CHECK(caption_double_click_action("") == CaptionDoubleClickAction::zoom);
    // "Fill" (macOS 15) tiles to the visible screen; no public NSWindow API does exactly that, so
    // the closest one — zoom: — is the documented answer, not a silent no-op.
    CHECK(caption_double_click_action("Fill") == CaptionDoubleClickAction::zoom);
    // Machine-written tokens are exact: a casing or spelling variant is NOT one of the three, and
    // the honest answer for a token we do not know is the default, never a minimize by accident.
    CHECK(caption_double_click_action("minimize") == CaptionDoubleClickAction::zoom);
    CHECK(caption_double_click_action("Something new") == CaptionDoubleClickAction::zoom);
}

void a_live_capture_owns_the_press_so_the_consult_yields()
{
    // THE GAP c1 SHIPPED WITH, closed: the consult used to read only the region map, so a left
    // press on the caption while another button's drag was live became a window drag — the drag's
    // owner never saw that press, and its release later arrived to nobody. Now the consult asks
    // the arbiter's own verdict, capture included.
    InputArbiter arbiter = titlebar_arbiter();

    // A right-button orbit starts on the viewport and wanders up onto the strip; the left press
    // there belongs to the orbit's implicit capture (route_pointer would route it to "scene").
    (void)arbiter.route_pointer(press_at(600, 400, MouseButton::right), 1);
    CHECK(arbiter.has_pointer_capture());
    CHECK(caption_press_action(press_at(400, 30), arbiter) == CaptionPressAction::yielded);
    // ...and a double-click is no more the caption's than a single press while it is live.
    CHECK(caption_press_action(press_at(400, 30, MouseButton::left, 2), arbiter) ==
          CaptionPressAction::yielded);
    // The consult is side-effect free: asking did not disturb the live capture.
    CHECK(arbiter.has_pointer_capture());
    // The moment the orbit's release ends the capture, the caption is the caption's again.
    (void)arbiter.route_pointer(
        press_at(400, 30, MouseButton::right, 1, PointerAction::up), 2);
    CHECK(!arbiter.has_pointer_capture());
    CHECK(caption_press_action(press_at(400, 30), arbiter) == CaptionPressAction::drag);

    // A MODAL capture (an open dropdown): its backdrop swallows the press, so a window drag must
    // not start from it either — dismissing the dropdown is what the press is for.
    Capture modal;
    modal.region_id = "scene";
    modal.target = InputTarget::viewport;
    modal.modal = true;
    arbiter.push_capture(modal);
    CHECK(caption_press_action(press_at(400, 30), arbiter) == CaptionPressAction::yielded);
    CHECK(arbiter.preview_pointer(press_at(400, 30)).target == InputTarget::swallowed);
    CHECK(arbiter.pop_capture("scene"));
    CHECK(caption_press_action(press_at(400, 30), arbiter) == CaptionPressAction::drag);

    // An OVERLAY (non-modal) capture whose region the press is outside of falls through to
    // ordinary arbitration in route_pointer — so the caption keeps its press, exactly as there.
    Capture overlay = modal;
    overlay.modal = false;
    arbiter.push_capture(overlay);
    CHECK(caption_press_action(press_at(400, 30), arbiter) == CaptionPressAction::drag);
    CHECK(arbiter.pop_capture("scene"));

    // A press that is NOT on the caption is `none`, capture or no capture — `yielded` is reserved
    // for the caption's own rect, so the smoke's counter means what it says.
    (void)arbiter.route_pointer(press_at(600, 400, MouseButton::right), 3);
    CHECK(caption_press_action(press_at(600, 400), arbiter) == CaptionPressAction::none);
    CHECK(caption_press_action(press_at(50, 30), arbiter) == CaptionPressAction::none);
}

void only_a_left_press_is_ever_the_captions()
{
    const InputArbiter map = titlebar_arbiter();
    // A move keeps hovering the strip; a release belongs to whatever owns its press; the right
    // button is the context-menu gesture; middle is middle. All flow on untouched.
    CHECK(caption_press_action(press_at(400, 30, MouseButton::none, 1, PointerAction::move), map) ==
          CaptionPressAction::none);
    CHECK(caption_press_action(press_at(400, 30, MouseButton::left, 1, PointerAction::up), map) ==
          CaptionPressAction::none);
    CHECK(caption_press_action(press_at(400, 30, MouseButton::right), map) ==
          CaptionPressAction::none);
    CHECK(caption_press_action(press_at(400, 30, MouseButton::middle), map) ==
          CaptionPressAction::none);
    // A wheel or leave sample can never be the caption's either.
    CHECK(caption_press_action(press_at(400, 30, MouseButton::none, 1, PointerAction::wheel), map) ==
          CaptionPressAction::none);
}

void the_consult_is_the_arbiters_own_last_match_wins_verdict()
{
    const InputArbiter map = titlebar_arbiter();
    // A press on a CONTROL rect published after the caption is the control's, not the caption's —
    // the same precedence route_pointer resolves, with no carve-out token (input.h).
    CHECK(caption_press_action(press_at(930, 30), map) == CaptionPressAction::none);
    CHECK(caption_press_action(press_at(1050, 30), map) == CaptionPressAction::none);
    // Outside every region: the browser's; on the viewport below the strip: the viewport's.
    CHECK(caption_press_action(press_at(50, 30), map) == CaptionPressAction::none);
    CHECK(caption_press_action(press_at(400, 200), map) == CaptionPressAction::none);
    // A caption published ABOVE (after) a viewport region wins over it, same rule.
    InputArbiter layered;
    layered.regions().publish({region("scene", RegionKind::viewport, 0, 0, 1200, 800),
                               region("chrome.caption", RegionKind::caption, 0, 0, 1200, 76)});
    CHECK(caption_press_action(press_at(400, 30), layered) == CaptionPressAction::drag);
    // And a press routed to the viewport (below the caption strip) is NOT the caption's.
    CHECK(caption_press_action(press_at(400, 400), layered) == CaptionPressAction::none);
    // Rect edges match the arbiter's half-open contains: the origin is inside, origin+extent out.
    InputArbiter edges;
    edges.regions().publish({region("chrome.caption", RegionKind::caption, 100, 10, 200, 40)});
    CHECK(caption_press_action(press_at(100, 10), edges) == CaptionPressAction::drag);
    CHECK(caption_press_action(press_at(300, 10), edges) == CaptionPressAction::none);
    CHECK(caption_press_action(press_at(100, 50), edges) == CaptionPressAction::none);
    // An empty map — the `system`-mode publish, and every window before the first publish.
    const InputArbiter empty;
    CHECK(caption_press_action(press_at(400, 30), empty) == CaptionPressAction::none);
}

void the_measured_cluster_becomes_the_physical_inset()
{
    // The ordinary LTR cluster at 1x: far edge 72 + the 8-point leading gap mirrored as trailing
    // pad -> 80 physical px on the LEFT, nothing on the right.
    const CocoaChromeState ltr = ns_hybrid_controls_inset(8.0, 72.0, 1280.0, DpiScale{96});
    CHECK(ltr.inset_left == 80u);
    CHECK(ltr.inset_right == 0u);
    // Retina doubles the PHYSICAL answer — points in, pixels out is the whole contract; a missing
    // scale ships looking correct on a 1x display and halves the pad exactly where it matters.
    const CocoaChromeState retina = ns_hybrid_controls_inset(8.0, 72.0, 1280.0, DpiScale{192});
    CHECK(retina.inset_left == 160u);
    CHECK(retina.inset_right == 0u);
    // Fractional frames round UP: one px short sits strip content under a button.
    const CocoaChromeState fractional =
        ns_hybrid_controls_inset(7.5, 71.25, 1280.0, DpiScale{144});
    CHECK(fractional.inset_left == 119u); // (71.25 + 7.5) * 1.5 = 118.125 -> 119
    CHECK(fractional.inset_right == 0u);
    // The RTL mirror: the same cluster measured from the right edge insets the RIGHT.
    const CocoaChromeState rtl = ns_hybrid_controls_inset(1208.0, 1272.0, 1280.0, DpiScale{96});
    CHECK(rtl.inset_left == 0u);
    CHECK(rtl.inset_right == 80u); // (1280-1208) + (1280-1272) = 80
    // A cluster that pokes past the window's left edge clamps its pad to zero, never negative.
    const CocoaChromeState clipped = ns_hybrid_controls_inset(-4.0, 60.0, 1280.0, DpiScale{96});
    CHECK(clipped.inset_left == 60u);
    CHECK(clipped.inset_right == 0u);
}

void degenerate_cluster_input_answers_the_honest_zero()
{
    // `qnan`, not `nan`: <cmath> puts a global ::nan(const char*) in scope, and MSVC's /W4 C4459
    // (local hides global) is a /WX error the local GCC gate cannot see.
    const double qnan = std::numeric_limits<double>::quiet_NaN();
    const double huge = std::numeric_limits<double>::infinity();
    const CocoaChromeState from_nan_min = ns_hybrid_controls_inset(qnan, 72.0, 1280.0, DpiScale{96});
    CHECK(from_nan_min.inset_left == 0u && from_nan_min.inset_right == 0u);
    const CocoaChromeState from_nan_max = ns_hybrid_controls_inset(8.0, qnan, 1280.0, DpiScale{96});
    CHECK(from_nan_max.inset_left == 0u && from_nan_max.inset_right == 0u);
    const CocoaChromeState from_inf_width =
        ns_hybrid_controls_inset(8.0, 72.0, huge, DpiScale{96});
    CHECK(from_inf_width.inset_left == 0u && from_inf_width.inset_right == 0u);
    // An empty or inverted cluster (a mid-teardown read, no buttons) is {0, 0}.
    const CocoaChromeState empty = ns_hybrid_controls_inset(72.0, 72.0, 1280.0, DpiScale{96});
    CHECK(empty.inset_left == 0u && empty.inset_right == 0u);
    const CocoaChromeState inverted = ns_hybrid_controls_inset(72.0, 8.0, 1280.0, DpiScale{96});
    CHECK(inverted.inset_left == 0u && inverted.inset_right == 0u);
    const CocoaChromeState no_window = ns_hybrid_controls_inset(8.0, 72.0, 0.0, DpiScale{96});
    CHECK(no_window.inset_left == 0u && no_window.inset_right == 0u);
}

void the_surface_refuses_a_non_cocoa_backend_on_every_leg()
{
    // The headless backend — what every mac CI CEF smoke runs, and the shape of every non-Cocoa
    // backend to this surface. The query refuses, the stats refuse, and the wiring no-ops, which is
    // what makes shell.cpp's unconditional EditorWindow-constructor call safe everywhere.
    WindowDesc desc;
    HeadlessWindowBackend headless(desc);
    CocoaChromeState chrome;
    chrome.inset_left = 123u; // must be left untouched by a refusal
    CHECK(!cocoa_hybrid_chrome(headless, chrome));
    CHECK(chrome.inset_left == 123u);
    CocoaCaptionStats stats;
    CHECK(!cocoa_caption_stats(headless, stats));
    InputArbiter arbiter;
    cocoa_bind_caption_arbiter(headless, &arbiter); // a no-op, and must not crash
    cocoa_bind_caption_arbiter(headless, nullptr);
    // Still fully functional as a window afterwards — the no-op really was one.
    std::vector<ShellEvent> drained;
    CHECK(headless.pump(drained));
}

} // namespace

int main()
{
    a_left_press_on_the_caption_drags_and_a_double_click_is_the_double_click();
    the_double_click_action_is_the_users_own_macos_preference();
    a_live_capture_owns_the_press_so_the_consult_yields();
    only_a_left_press_is_ever_the_captions();
    the_consult_is_the_arbiters_own_last_match_wins_verdict();
    the_measured_cluster_becomes_the_physical_inset();
    degenerate_cluster_input_answers_the_honest_zero();
    the_surface_refuses_a_non_cocoa_backend_on_every_leg();
    SHELL_TEST_MAIN_END();
}
