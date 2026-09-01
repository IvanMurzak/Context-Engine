// Per-monitor DPI arithmetic (03 §1): scale derivation, the clamp, round-to-nearest, the
// never-collapse rule, the signed point conversions, and — a1 — the OSR screen mapping the two
// `CefRenderHandler` geometry members answer with.

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

// Both conventions of the OSR screen rect, on every leg. The macOS branch of this choice is the one
// piece of the CEF binding that no CI job EXECUTES (the live smoke is Windows/Linux only) and that
// the local gate cannot even compile, so the arithmetic lives in dpi.h to be pinned here instead.
void test_osr_screen_extent_follows_the_platform_convention()
{
    const render::Extent2D logical{800, 500};
    const DpiScale two_x = DpiScale{192};

    // Windows/Linux: CEF wants DEVICE pixels, so the view size scales by the monitor factor.
    const render::Extent2D device = osr_screen_extent(logical, two_x, /*screen_rect_is_dip*/ false);
    CHECK(device.width == 1600u);
    CHECK(device.height == 1000u);
    CHECK(shelltest::extent_eq(device, to_physical(logical, two_x)));

    // macOS: CEF wants DIP, so the logical size passes through UNSCALED. Returning the device size
    // here would report a screen twice the real one and mis-place every popup and screen-point.
    const render::Extent2D dip = osr_screen_extent(logical, two_x, /*screen_rect_is_dip*/ true);
    CHECK(shelltest::extent_eq(dip, logical));

    // At 1x the two conventions coincide — which is exactly why a wrong choice stays invisible until
    // someone runs the editor on a scaled monitor.
    const DpiScale one_x = DpiScale{96};
    CHECK(shelltest::extent_eq(osr_screen_extent(logical, one_x, false),
                               osr_screen_extent(logical, one_x, true)));
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
    const PointI screen = osr_screen_point(view, client_device, one_five, false);
    CHECK(screen == (PointI{360, 180})); // 300 + 40*1.5, 150 + 20*1.5

    // The three ways this has been or could be wrong, each distinguishable HERE and at no other
    // scale/origin combination:
    CHECK(screen != view);               // the un-overridden CEF default: view AS screen
    CHECK(screen != (PointI{60, 30}));   // the offset dropped: scaled, but from the screen origin
    CHECK(screen != (PointI{340, 170})); // the split dropped: DIP added to a device-pixel origin

    // macOS: screen DIP, so the view offset is added UNSCALED to an origin already in DIP.
    const PointI client_dip{200, 100}; // the same window, in that platform's units
    const PointI screen_mac = osr_screen_point(view, client_dip, one_five, true);
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
// platform, with no per-platform split (pinned cef_render_handler.h:70-78). Giving it
// `osr_screen_extent`'s treatment multiplies the rect by the scale factor on Windows/Linux, which is
// the same class of mistake a2 fixes and is likewise invisible at 1.0.
void test_osr_root_screen_rect_is_dip_on_every_platform_convention()
{
    const DpiScale one_five{144};
    const render::Extent2D logical{800, 500};

    // ONE physical window, described in each platform's own convention: 800x500 DIP whose client
    // starts 300x150 device pixels (== 200x100 DIP) from the screen origin.
    const ScreenRect from_device = osr_root_screen_rect(PointI{300, 150}, logical, one_five, false);
    const ScreenRect from_dip = osr_root_screen_rect(PointI{200, 100}, logical, one_five, true);

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

int main()
{
    test_osr_screen_extent_follows_the_platform_convention();
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
