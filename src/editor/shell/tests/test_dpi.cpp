// Per-monitor DPI arithmetic (03 §1): scale derivation, the clamp, round-to-nearest, the
// never-collapse rule, and the signed point conversions.

#include "context/editor/shell/dpi.h"

#include "shell_test.h"

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

int main()
{
    test_osr_screen_extent_follows_the_platform_convention();
    test_scale_is_derived_from_dpi();
    test_clamp_rejects_os_nonsense();
    test_extent_round_trips();
    test_rounding_is_nearest_not_truncating();
    test_a_non_empty_extent_never_collapses();
    test_points_convert_symmetrically_across_zero();
    test_scale_equality_is_by_dpi();
    SHELL_TEST_MAIN_END();
}
