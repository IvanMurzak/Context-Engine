// The PURE half of the macOS hybrid-chrome seam (editor-window-chrome c1) — see cocoa_chrome.h for
// the split. Everything here is plain arithmetic over explicit inputs, compiled and ctest-run on
// all three legs (editor-shell-test_cocoa_chrome); the AppKit calls live in cocoa_window.mm.

#include "context/editor/shell/cocoa_chrome.h"

#include <algorithm>
#include <cmath>

namespace context::editor::shell
{

CaptionPressAction caption_press_action(const PointerEvent& pointer, const InputArbiter& arbiter)
{
    // Only a LEFT PRESS can be the caption's (cocoa_chrome.h states why moves, releases and the
    // right button are excluded). Everything else falls through to ordinary dispatch untouched —
    // the "for THAT press only" half of 02 §4's suppression rule.
    if (pointer.action != PointerAction::down || pointer.button != MouseButton::left)
    {
        return CaptionPressAction::none;
    }
    // The SAME hit-test the InputArbiter routes by (back-to-front last-match-wins), so the pump and
    // the arbiter can never disagree about who owns a press: a control published after the caption
    // wins here exactly as it wins there, with no carve-out token (input.h § RegionKind).
    const ShellRegion* hit = arbiter.regions().hit_test(pointer.position);
    if (hit == nullptr || hit->kind != RegionKind::caption)
    {
        return CaptionPressAction::none;
    }
    // THE CAPTURE HALF of the verdict. A press on the caption rect is the OS's only when the
    // arbiter would route it to the caption: while a live capture owns the stream — another
    // button's implicit drag (a right-button camera orbit that wandered onto the strip), or a
    // modal push_capture (an open dropdown's backdrop) — route_pointer sends the press to the
    // capture target or swallows it, and a window drag started here instead would steal the press
    // from that owner without its release ever arriving. preview_pointer is route_pointer's own
    // verdict, side-effect free, so the two cannot disagree; the press then flows on to the
    // arbiter, which applies exactly that verdict.
    const PointerPreview verdict = arbiter.preview_pointer(pointer);
    if (verdict.target != InputTarget::native || verdict.region != hit)
    {
        return CaptionPressAction::yielded;
    }
    // Cocoa counts clicks for us (NsEvent::click_count): the second press of a double-click
    // arrives with click_count 2, so the first press starts a drag — exactly what a native
    // titlebar does — and the second becomes the double-click, whose ACTION the pump reads from
    // the user's preference (caption_double_click_action).
    return pointer.click_count >= 2 ? CaptionPressAction::double_click : CaptionPressAction::drag;
}

CaptionDoubleClickAction caption_double_click_action(std::string_view apple_action_on_double_click)
{
    if (apple_action_on_double_click == "Minimize")
    {
        return CaptionDoubleClickAction::minimize;
    }
    if (apple_action_on_double_click == "None")
    {
        return CaptionDoubleClickAction::none;
    }
    // "Maximize" (the platform default, also what an UNSET preference means), "Fill" (no public
    // NSWindow API for the tile — `zoom:` is the closest), and any token a future macOS adds: the
    // default. A wrong guess here is a zoom, which the next double-click undoes.
    return CaptionDoubleClickAction::zoom;
}

CocoaChromeState ns_hybrid_controls_inset(double min_x_points, double max_x_points,
                                          double width_points, DpiScale dpi)
{
    CocoaChromeState state;
    // Refuse degenerate input with the honest {0, 0} rather than a garbage inset: NaN comparisons
    // are all false, so the explicit isfinite checks are what keeps a NaN from sailing through the
    // ordering checks below (the `!(x > 0)` lesson ns_extent_to_physical records).
    if (!std::isfinite(min_x_points) || !std::isfinite(max_x_points) ||
        !std::isfinite(width_points) || max_x_points <= min_x_points || width_points <= 0.0)
    {
        return state;
    }
    const double scale = static_cast<double>(dpi.factor());
    const double centre = (min_x_points + max_x_points) / 2.0;
    // Physical px, rounded UP and clamped into [0, width]: a pad one pixel short puts strip
    // content under a button, and a cluster reported outside the window (a mid-teardown read)
    // must not produce an inset wider than the window itself.
    const double max_inset = std::ceil(width_points * scale);
    const auto to_inset = [max_inset](double physical_px) -> std::uint32_t {
        return static_cast<std::uint32_t>(std::clamp(std::ceil(physical_px), 0.0, max_inset));
    };
    if (centre <= width_points / 2.0)
    {
        // The ordinary LTR cluster: inset = the cluster's far edge + the same gap it keeps from
        // the window's left edge, so the strip's padding reads symmetric around the buttons.
        const double pad = std::max(0.0, min_x_points);
        state.inset_left = to_inset((max_x_points + pad) * scale);
    }
    else
    {
        // The RTL mirror: macOS puts the traffic lights on the RIGHT for RTL system languages;
        // same symmetric-pad shape measured from the right edge.
        const double pad = std::max(0.0, width_points - max_x_points);
        state.inset_right = to_inset(((width_points - min_x_points) + pad) * scale);
    }
    return state;
}

} // namespace context::editor::shell
