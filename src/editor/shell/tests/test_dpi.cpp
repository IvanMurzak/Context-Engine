// Per-monitor DPI arithmetic (03 §1): scale derivation, the clamp, round-to-nearest, the
// never-collapse rule, the signed point conversions, — a1 — the OSR screen mapping the two
// `CefRenderHandler` geometry members answer with, and — a2 — the popup rect's DIP -> physical
// conversion.

#include "context/editor/shell/dpi.h"

// a1: win32_frameless_client_insets, for the client-origin (frameless inset) case.
#include "context/editor/shell/window.h"

#include "shell_test.h"

#include <cstdint>

using namespace context::editor::shell;
namespace render = context::render;

namespace
{

void test_scale_is_derived_from_dpi()
{
    CHECK(shelltest::near_eq(DpiScale{96}.factor(), 1.0f));
    CHECK(shelltest::near_eq(DpiScale{144}.factor(), 1.5f));
    CHECK(shelltest::near_eq(DpiScale{192}.factor(), 2.0f));
    CHECK(shelltest::near_eq(DpiScale{240}.factor(), 2.5f));
    // The default is the reference DPI, so an unset scale is the identity rather than zero.
    CHECK(DpiScale{}.dpi == kReferenceDpi);
    CHECK(shelltest::near_eq(DpiScale{}.factor(), 1.0f));
}

void test_clamp_rejects_os_nonsense()
{
    // A monitor reporting 0 would divide the whole layout to nothing.
    CHECK(make_dpi_scale(0).dpi == kMinDpi);
    CHECK(make_dpi_scale(1).dpi == kMinDpi);
    CHECK(make_dpi_scale(100000).dpi == kMaxDpi);
    // Real values pass through untouched.
    CHECK(make_dpi_scale(96).dpi == 96u);
    CHECK(make_dpi_scale(144).dpi == 144u);
    CHECK(make_dpi_scale(kMinDpi).dpi == kMinDpi);
    CHECK(make_dpi_scale(kMaxDpi).dpi == kMaxDpi);
}

void test_extent_round_trips()
{
    const DpiScale one_five{144};
    CHECK(shelltest::extent_eq(to_physical(render::Extent2D{1280, 800}, one_five),
                               render::Extent2D{1920, 1200}));
    CHECK(shelltest::extent_eq(to_logical(render::Extent2D{1920, 1200}, one_five),
                               render::Extent2D{1280, 800}));

    // At 1.0 both directions are the identity.
    const DpiScale one{96};
    CHECK(shelltest::extent_eq(to_physical(render::Extent2D{1280, 800}, one),
                               render::Extent2D{1280, 800}));
    CHECK(shelltest::extent_eq(to_logical(render::Extent2D{1280, 800}, one),
                               render::Extent2D{1280, 800}));
}

void test_rounding_is_nearest_not_truncating()
{
    // 100 logical at 1.25 == 125 exactly; 101 at 1.25 == 126.25 -> 126, which truncation would
    // report as 126 too, so use a case where they differ: 3 at 1.5 == 4.5 -> 5, truncation says 4.
    const DpiScale one_five{144};
    CHECK(to_physical(render::Extent2D{3, 3}, one_five).width == 5u);
    const DpiScale one_twentyfive{120};
    CHECK(to_physical(render::Extent2D{101, 101}, one_twentyfive).width == 126u);
}

void test_a_non_empty_extent_never_collapses()
{
    // The load-bearing rule: ISwapchain::resize IGNORES a zero extent (a minimized window reports
    // one every frame), so a 1x1 logical window rounding to 0 physical would leave the swapchain on
    // a stale size while the window really did change.
    const DpiScale half{kMinDpi}; // 0.5x
    const render::Extent2D physical = to_physical(render::Extent2D{1, 1}, half);
    CHECK(physical.width == 1u);
    CHECK(physical.height == 1u);
    CHECK(!render::is_empty(physical));

    // The same rule in the other direction, at the top of the clamp range.
    const DpiScale ten{kMaxDpi};
    const render::Extent2D logical = to_logical(render::Extent2D{1, 1}, ten);
    CHECK(logical.width == 1u);
    CHECK(!render::is_empty(logical));

    // An EMPTY extent stays empty — "never collapse" must not mean "never zero", or a minimized
    // window would report 1x1 and be reconfigured every frame.
    CHECK(render::is_empty(to_physical(render::Extent2D{0, 0}, half)));
    CHECK(to_physical(render::Extent2D{0, 8}, half).width == 0u);
}

void test_points_convert_symmetrically_across_zero()
{
    const DpiScale two{192};
    CHECK(to_logical_point(PointI{200, 100}, two) == (PointI{100, 50}));
    CHECK(to_physical_point(PointI{100, 50}, two) == (PointI{200, 100}));

    // A captured drag legitimately leaves the client area, so a negative coordinate must scale the
    // same way a positive one does rather than flooring away from zero.
    CHECK(to_logical_point(PointI{-200, -100}, two) == (PointI{-100, -50}));
    CHECK(to_physical_point(PointI{-100, -50}, two) == (PointI{-200, -100}));

    // Round-to-nearest, away from zero on a halfway case — symmetric about the origin.
    const DpiScale one_five{144};
    CHECK(to_physical_point(PointI{3, -3}, one_five) == (PointI{5, -5}));

    CHECK(to_logical_point(PointI{0, 0}, two) == (PointI{0, 0}));
}

void test_scale_equality_is_by_dpi()
{
    CHECK(DpiScale{144} == DpiScale{144});
    CHECK(DpiScale{144} != DpiScale{96});
}

} // namespace

// -------------------------------------------------------------- a2: the OSR popup rect (DIP -> px)

// `OnPopupSize` reports "the new location and size in view coordinates" — DIP — while the popup's
// own `OnPaint` buffer is PHYSICAL. `osr_popup_dest_rect` is the one place that split is resolved.
//
// A NON-INTEGRAL SCALE IS THE WHOLE POINT (again): at 1.0 the converted and the raw rect are the
// same bytes, so a test there passes with the conversion deleted.
void test_osr_popup_dest_rect_scales_the_origin_and_takes_the_texture_size()
{
    const DpiScale one_five{144}; // 150%
    // A dropdown 200x120 DIP, anchored 260x400 DIP into the view. CEF paints ceil(DIP x 1.5).
    const render::Rect2D popup_dip = shelltest::rect(260, 400, 200, 120);
    const render::Extent2D texture{300, 180};

    const render::Rect2D dest = osr_popup_dest_rect(popup_dip, texture, one_five);

    // The ORIGIN is converted: 260 x 1.5 = 390, 400 x 1.5 = 600.
    CHECK(dest.origin.x == 390u);
    CHECK(dest.origin.y == 600u);
    // The SIZE is the texture's, not the DIP rect's — here they agree numerically with the scaled
    // DIP size, and the case below is the one that tells them apart.
    CHECK(shelltest::extent_eq(dest.size, texture));

    // The three ways this has been or could be wrong, each of them a DIFFERENT rect at this scale
    // and NONE of them distinguishable at 1.0:
    CHECK(!shelltest::rect_eq(dest, popup_dip)); // the shipped bug: the DIP rect used raw
    CHECK(dest.origin.x != popup_dip.origin.x);  // the origin left unconverted
    CHECK(!shelltest::extent_eq(dest.size, popup_dip.size)); // the size left unconverted (cropped)

    // At 1.0 the conversion is the identity — the existing behaviour, kept pinned.
    const render::Rect2D at_one =
        osr_popup_dest_rect(popup_dip, popup_dip.size, DpiScale{kReferenceDpi});
    CHECK(shelltest::rect_eq(at_one, popup_dip));
}

// WHY THE SIZE COMES FROM THE TEXTURE RATHER THAN FROM round(DIP x scale). CEF paints
// ceil(DIP x scale) pixels; the two disagree whenever the product's fraction is under a half, and a
// destination one pixel off the texture crops a column of the menu (CPU path) or resamples the whole
// thing (GPU path).
void test_osr_popup_dest_rect_size_is_not_the_rounded_dip_size()
{
    const DpiScale one_two_five{120};                            // 125%
    const render::Rect2D popup_dip = shelltest::rect(0, 0, 1, 1); // 1 DIP: 1.25 px -> CEF paints 2
    const render::Extent2D texture{2, 2};

    const render::Rect2D dest = osr_popup_dest_rect(popup_dip, texture, one_two_five);
    CHECK(shelltest::extent_eq(dest.size, texture));
    // round(1 x 1.25) is 1 — the answer a "scale the DIP size" implementation gives, and it is a
    // pixel short of what CEF actually painted.
    CHECK(to_physical(popup_dip.size, one_two_five).width == 1u);
    CHECK(dest.size.width != to_physical(popup_dip.size, one_two_five).width);
}

// The origin ROUNDS TO NEAREST, and rounds the same way `osr_screen_point` does.
//
// Deliberately NOT a test that `to_physical` (the extent form, with its never-collapse clamp) would
// be wrong here: over the supported kMinDpi..kMaxDpi range the two agree on every non-negative
// integer, so such a test could not fail and would be asserting nothing. What CAN be wrong is
// TRUNCATION, which is what a hand-rolled `x * factor` cast gives.
void test_osr_popup_dest_rect_origin_rounds_to_nearest_like_the_screen_mapping()
{
    const DpiScale one_five{144};
    // 5 x 1.5 = 7.5 and 3 x 1.5 = 4.5 — both halfway, and both a different integer from the
    // truncating answer.
    const render::Rect2D popup_dip = shelltest::rect(5, 3, 40, 20);
    const render::Rect2D dest = osr_popup_dest_rect(popup_dip, render::Extent2D{60, 30}, one_five);
    CHECK(dest.origin.x == 8u); // truncation gives 7
    CHECK(dest.origin.y == 5u); // truncation gives 4

    // The SAME arithmetic the screen mapping uses, stated as an identity so the two cannot drift:
    // where the popup is drawn and where CEF is told that point lives must round together.
    const PointI as_offset = osr_screen_point(PointI{5, 3}, PointI{0, 0}, one_five, false);
    CHECK(static_cast<std::int32_t>(dest.origin.x) == as_offset.x);
    CHECK(static_cast<std::int32_t>(dest.origin.y) == as_offset.y);

    // A popup anchored at the view origin stays there, at the low clamp as well as at 1.5.
    CHECK(osr_popup_dest_rect(shelltest::rect(0, 0, 200, 120), render::Extent2D{100, 60},
                              DpiScale{48})
              .origin.x == 0u);
}

// An empty texture — a popup rect that arrived before its first paint — yields an empty destination
// rather than a rect the present paths would draw at. Both paths gate on `have_popup_frame_`, so
// this pins the accessor's own answer rather than the gate.
void test_osr_popup_dest_rect_with_no_texture_is_empty()
{
    const render::Rect2D dest =
        osr_popup_dest_rect(shelltest::rect(10, 20, 30, 40), render::Extent2D{}, DpiScale{144});
    CHECK(render::is_empty(dest.size));
}

// --------------------------------------------------------------- a1: the OSR screen mapping (D12)

// THE REPORTED BUG, pinned. Unimplemented, `GetScreenPoint` returns false and CEF then treats view
// coordinates AS screen coordinates, so the right-click menu opens at the cursor's view position
// measured from the SCREEN origin (docs/shell.md § 16, owner item #5).
//
// A NON-INTEGRAL SCALE IS THE WHOLE POINT: at 1.0 the device and DIP conventions coincide, so a test
// there passes with the per-platform split reversed, dropped, or applied twice.
void test_osr_screen_point_converts_view_dip_to_the_platform_convention()
{
    const DpiScale one_five{144}; // 150%
    const PointI view{40, 20};    // DIP, inside the view

    // Windows/Linux: screen DEVICE pixels, and the client sits at a NON-ZERO screen origin (without
    // one the missing offset cannot fail).
    const PointI client_device{300, 150};
    const PointI screen =
        osr_screen_point(view, client_device, one_five, /*screen_coords_are_dip*/ false);
    CHECK(screen == (PointI{360, 180})); // 300 + 40*1.5, 150 + 20*1.5

    // The three ways this has been or could be wrong, each distinguishable HERE and at no other
    // scale/origin combination:
    CHECK(screen != view);               // the un-overridden CEF default: view AS screen
    CHECK(screen != (PointI{60, 30}));   // the offset dropped: scaled, but from the screen origin
    CHECK(screen != (PointI{340, 170})); // the split dropped: DIP added to a device-pixel origin

    // macOS: screen DIP, so the view offset is added UNSCALED to an origin already in DIP.
    const PointI client_dip{200, 100}; // the same window, in that platform's units
    const PointI screen_mac =
        osr_screen_point(view, client_dip, one_five, /*screen_coords_are_dip*/ true);
    CHECK(screen_mac == (PointI{240, 120}));
    // Giving macOS the device-pixel treatment is the mistake this pair exists to prevent.
    CHECK(screen_mac != (PointI{260, 130}));

    // At 1.0 the two conventions coincide — which is exactly why a wrong choice ships unnoticed
    // until someone runs the editor on a scaled monitor.
    const DpiScale one{96};
    CHECK(osr_screen_point(view, client_device, one, false) ==
          osr_screen_point(view, client_device, one, true));

    // The view origin maps to the client origin itself, on both conventions: the browser's (0, 0)
    // IS the first pixel of the window's client area.
    CHECK(osr_screen_point(PointI{0, 0}, client_device, one_five, false) == client_device);
    CHECK(osr_screen_point(PointI{0, 0}, client_dip, one_five, true) == client_dip);
}

void test_osr_screen_point_handles_a_window_left_of_the_primary_monitor()
{
    // An ordinary multi-monitor arrangement: the secondary display sits LEFT of / above the primary,
    // so the client origin is negative. A conversion that clamped (render::Rect2D's origin is
    // unsigned, which is why this arithmetic does not use it) would report the menu at the screen
    // origin — on the wrong monitor entirely.
    const DpiScale one_five{144};
    CHECK(osr_screen_point(PointI{10, 10}, PointI{-1720, -300}, one_five, false) ==
          (PointI{-1705, -285}));

    // Round-to-nearest AWAY FROM ZERO on the offset, symmetric about the origin: CEF asks for view
    // points outside the view (a drag past the edge), and those must not floor away from zero.
    CHECK(osr_screen_point(PointI{3, -3}, PointI{0, 0}, one_five, false) == (PointI{5, -5}));
}

// `GetRootScreenRect` is "the root window rectangle in screen DIP coordinates" — DIP on EVERY
// platform, with no per-platform split (pinned cef_render_handler.h:70-78). Multiplying it by the
// scale factor on Windows/Linux is the same class of mistake a2 fixes and is likewise invisible at
// 1.0.
void test_osr_root_screen_rect_is_dip_on_every_platform_convention()
{
    const DpiScale one_five{144};
    const render::Extent2D logical{800, 500};

    // ONE physical window, described in each platform's own convention: 800x500 DIP whose client
    // starts 300x150 device pixels (== 200x100 DIP) from the screen origin.
    const ScreenRect from_device =
        osr_root_screen_rect(PointI{300, 150}, logical, one_five, /*screen_coords_are_dip*/ false);
    const ScreenRect from_dip =
        osr_root_screen_rect(PointI{200, 100}, logical, one_five, /*screen_coords_are_dip*/ true);

    // The SAME window must produce the SAME rect — that identity is the no-split rule stated in a
    // form a scaled implementation cannot also satisfy.
    CHECK(from_device == from_dip);
    CHECK(from_device.origin == (PointI{200, 100}));
    CHECK(shelltest::extent_eq(from_device.size, logical));

    // The size passes through UNSCALED, and at this scale that is a different number from the
    // device size — so the assertion above is not vacuous.
    CHECK(to_physical(logical, one_five).width == 1200u);
    CHECK(from_device.size.width != to_physical(logical, one_five).width);

    // The two members agree by construction: the root rect's origin, put back into the platform's
    // own convention, is the screen point of the view origin.
    CHECK(to_physical_point(from_device.origin, one_five) ==
          osr_screen_point(PointI{0, 0}, PointI{300, 150}, one_five, false));
    CHECK(from_dip.origin == osr_screen_point(PointI{0, 0}, PointI{200, 100}, one_five, true));

    // A second scale, so nothing here is pinned to 1.5 alone, and a negative origin again.
    const DpiScale two{192};
    const ScreenRect at_2x = osr_root_screen_rect(PointI{-300, 150}, logical, two, false);
    CHECK(at_2x.origin == (PointI{-150, 75}));
    CHECK(shelltest::extent_eq(at_2x.size, logical));
}

// THE CLIENT ORIGIN, NOT THE WINDOW RECT. The Shell's Win32 window is frameless (it takes the frame
// over in WM_NCCALCSIZE), so its client area is INSET from its window rect — feeding the window
// origin to the mapping above puts every context menu that inset away from the cursor. Unlike the
// DIP/device split, this error does NOT vanish at 100%: the border is 8 px there, 12 px at 150%.
void test_the_screen_mapping_lands_on_the_client_origin_not_the_window_rect()
{
    const PointI window_origin{1000, 500}; // physical screen pixels, from the OS window rect

    const std::uint32_t scales[] = {96u, 144u}; // 100% and 150%
    for (const std::uint32_t raw_dpi : scales)
    {
        const DpiScale dpi{raw_dpi};
        const std::int32_t border = win32_resize_border_thickness(dpi); // 8 at 96, 12 at 144

        // Restored: the left/right/bottom borders are real NC strips and the TOP inset is zero (the
        // web titlebar draws in the client's first rows), so the client origin moves in x only.
        const Win32FrameInsets restored = win32_frameless_client_insets(false, dpi);
        const PointI client_restored{window_origin.x + restored.left,
                                     window_origin.y + restored.top};
        CHECK(client_restored == (PointI{1000 + border, 500}));

        const PointI at_view_origin = osr_screen_point(PointI{0, 0}, client_restored, dpi, false);
        CHECK(at_view_origin == client_restored);
        // The window-rect answer is a DIFFERENT point, by exactly the border — the error the seam's
        // contract (`IWindowBackend::client_origin`) exists to prevent, at every scale.
        CHECK(at_view_origin != osr_screen_point(PointI{0, 0}, window_origin, dpi, false));
        CHECK(at_view_origin.x - window_origin.x == border);

        // Maximized: ALL sides are inset, so the y moves too — the state a placement-derived origin
        // gets wrong twice over (the restore rect is not where the window is, and the top inset is
        // no longer zero).
        const Win32FrameInsets maximized = win32_frameless_client_insets(true, dpi);
        const PointI client_maximized{window_origin.x + maximized.left,
                                      window_origin.y + maximized.top};
        CHECK(client_maximized == (PointI{1000 + border, 500 + border}));
        CHECK(osr_screen_point(PointI{0, 0}, client_maximized, dpi, false) != client_restored);

        // The root rect follows the client too, and stays DIP.
        const ScreenRect root =
            osr_root_screen_rect(client_restored, render::Extent2D{800, 500}, dpi, false);
        CHECK(root.origin == to_logical_point(client_restored, dpi));
        CHECK(shelltest::extent_eq(root.size, render::Extent2D{800, 500}));
    }
}

// b1 (D11): the OSR drag's own direction — SCREEN back to view DIP.
//
// The drag is the one part of the OSR contract that needs the mapping BACKWARDS: `StartDragging`
// reports its start point in screen coordinates while every injection that answers it takes view
// ones (osr_drag.h). The property that matters is that the two functions are genuinely inverse —
// not that each is separately plausible — so this asserts the ROUND TRIP, on both platform
// conventions, at a scale where they differ.
void test_osr_view_point_is_the_inverse_of_the_screen_mapping()
{
    const DpiScale one_five{144};
    const PointI client_device{300, 150}; // Windows/Linux: the client origin in DEVICE pixels
    const PointI client_dip{200, 100};    // macOS: the same window, in screen DIP

    // Round-trip at a scale where DIP and device pixels differ. Every sample is an EVEN DIP offset
    // so 1.5x lands on an integer — a round trip through a rounding pair is only exactly invertible
    // where the intermediate is representable, and asserting otherwise would be asserting the FPU.
    const PointI views[] = {PointI{0, 0}, PointI{40, 20}, PointI{2, 100}, PointI{800, 600}};
    for (const PointI view : views)
    {
        const PointI screen = osr_screen_point(view, client_device, one_five, false);
        CHECK(osr_view_point(screen, client_device, one_five, false) == view);
        const PointI screen_dip = osr_screen_point(view, client_dip, one_five, true);
        CHECK(osr_view_point(screen_dip, client_dip, one_five, true) == view);
    }

    // THE ORIGIN IS SUBTRACTED FIRST, and only the OFFSET is scaled. The bug this rules out is
    // converting the whole screen point and then subtracting: at 1.5x with a client origin of
    // {300, 150} device px, that spelling would answer {200, 100} - {300, 150} = {-100, -50} for
    // the view origin instead of {0, 0}, i.e. the client origin scaled a second time.
    CHECK(osr_view_point(client_device, client_device, one_five, false) == (PointI{0, 0}));
    CHECK(osr_view_point(client_dip, client_dip, one_five, true) == (PointI{0, 0}));
    CHECK(osr_view_point(client_device, client_device, one_five, false) !=
          to_logical_point(client_device, one_five));

    // A pointer that has left the view to the LEFT/ABOVE is an ordinary drag state (the gesture
    // travels past the window edge), so the answer is legitimately NEGATIVE rather than clamped —
    // the same reason `PointI` is signed at all.
    const PointI outside = osr_view_point(PointI{270, 120}, client_device, one_five, false);
    CHECK(outside.x == -20); // (270 - 300) / 1.5
    CHECK(outside.y == -20); // (120 - 150) / 1.5

    // At 1.0 the two conventions coincide, which is exactly why every bug in this family is
    // invisible on an unscaled monitor — pinned so the suite cannot be read as proving otherwise.
    const DpiScale one{96};
    CHECK(osr_view_point(PointI{330, 170}, client_device, one, false) ==
          osr_view_point(PointI{330, 170}, client_device, one, true));
}

int main()
{
    test_osr_view_point_is_the_inverse_of_the_screen_mapping();
    test_osr_popup_dest_rect_scales_the_origin_and_takes_the_texture_size();
    test_osr_popup_dest_rect_size_is_not_the_rounded_dip_size();
    test_osr_popup_dest_rect_origin_rounds_to_nearest_like_the_screen_mapping();
    test_osr_popup_dest_rect_with_no_texture_is_empty();
    test_osr_screen_point_converts_view_dip_to_the_platform_convention();
    test_osr_screen_point_handles_a_window_left_of_the_primary_monitor();
    test_osr_root_screen_rect_is_dip_on_every_platform_convention();
    test_the_screen_mapping_lands_on_the_client_origin_not_the_window_rect();
    test_scale_is_derived_from_dpi();
    test_clamp_rejects_os_nonsense();
    test_extent_round_trips();
    test_rounding_is_nearest_not_truncating();
    test_a_non_empty_extent_never_collapses();
    test_points_convert_symmetrically_across_zero();
    test_scale_equality_is_by_dpi();
    SHELL_TEST_MAIN_END();
}
