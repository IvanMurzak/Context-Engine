// The window seam: the headless backend's behaviour, and the PURE Win32 and X11 event decoders.
//
// The decoders are the point of this file. The local dev gate defines _WIN32 and CI's Windows leg is
// the only thing that ever runs a real WndProc — and the X11 branch is preprocessed out entirely on
// both — so event decoding written inside either OS backend would have exactly one place it could be
// exercised, and the Linux one would have NONE locally. Written as pure functions over plain
// integers, every branch of both runs HERE — on ubuntu, macOS and Windows alike.

#include "context/editor/shell/window.h"

#include "shell_test.h"

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <optional>
#include <vector>

using namespace context::editor::shell;
namespace render = context::render;

namespace
{

// Pack two 16-bit halves into an LPARAM the way Windows does.
std::int64_t lparam(std::int32_t low, std::int32_t high)
{
    return static_cast<std::int64_t>((static_cast<std::uint32_t>(high & 0xFFFF) << 16) |
                                     static_cast<std::uint32_t>(low & 0xFFFF));
}

std::optional<ShellEvent> decode(std::uint32_t message, std::uint64_t wparam, std::int64_t lp,
                                 Win32ModifierState keys = {})
{
    return translate_win32_message(Win32Message{message, wparam, lp}, keys);
}

void test_resize_decoding_and_the_minimize_carve_out()
{
    const std::optional<ShellEvent> resized = decode(kWmSize, 0, lparam(1600, 900));
    CHECK(resized.has_value());
    CHECK(resized->kind == ShellEventKind::resize);
    CHECK(shelltest::extent_eq(resized->size, render::Extent2D{1600, 900}));

    // A MINIMIZED window reports a 0x0 client size every frame. Forwarding that as a resize would
    // ask the swapchain to reconfigure to nothing on every one of them.
    CHECK(!decode(kWmSize, kSizeMinimized, lparam(0, 0)).has_value());
    // ...and a 0x0 size is dropped even without the minimize flag.
    CHECK(!decode(kWmSize, 0, lparam(0, 0)).has_value());
    CHECK(!decode(kWmSize, 0, lparam(1600, 0)).has_value());
}

void test_mouse_coordinates_are_signed()
{
    // THE TRAP: LPARAM's halves are SIGNED 16-bit. A captured drag left of the client area reports
    // -36, which read unsigned becomes 65500 — a position outside every region that silently
    // re-routes the drag.
    const std::optional<ShellEvent> event = decode(kWmMouseMove, 0, lparam(-36, -12));
    CHECK(event.has_value());
    CHECK(event->kind == ShellEventKind::pointer);
    CHECK(event->pointer.action == PointerAction::move);
    CHECK(event->pointer.position == (PointI{-36, -12}));

    const std::optional<ShellEvent> positive = decode(kWmMouseMove, 0, lparam(400, 300));
    CHECK(positive->pointer.position == (PointI{400, 300}));
}

void test_button_messages_map_to_actions_and_buttons()
{
    struct Case
    {
        std::uint32_t message;
        PointerAction action;
        MouseButton button;
    };
    const Case cases[] = {
        {kWmLButtonDown, PointerAction::down, MouseButton::left},
        {kWmLButtonUp, PointerAction::up, MouseButton::left},
        {kWmRButtonDown, PointerAction::down, MouseButton::right},
        {kWmRButtonUp, PointerAction::up, MouseButton::right},
        {kWmMButtonDown, PointerAction::down, MouseButton::middle},
        {kWmMButtonUp, PointerAction::up, MouseButton::middle},
    };
    for (const Case& c : cases)
    {
        const std::optional<ShellEvent> event = decode(c.message, 0, lparam(20, 30));
        CHECK(event.has_value());
        CHECK(event->pointer.action == c.action);
        CHECK(event->pointer.button == c.button);
        CHECK(event->pointer.position == (PointI{20, 30}));
    }
}

void test_mouse_button_state_comes_from_wparam_and_modifier_keys_from_the_os()
{
    Win32ModifierState keys;
    keys.control = true;
    keys.shift = true;
    // MK_LBUTTON | MK_RBUTTON
    const std::optional<ShellEvent> event = decode(kWmMouseMove, 0x0001 | 0x0002, lparam(5, 5), keys);
    CHECK(event->pointer.modifiers.left_button_down);
    CHECK(event->pointer.modifiers.right_button_down);
    CHECK(!event->pointer.modifiers.middle_button_down);
    // The keyboard modifiers cannot be read out of a mouse message's parameters — they are supplied
    // by the backend, which keeps the decoder pure.
    CHECK(event->pointer.modifiers.control);
    CHECK(event->pointer.modifiers.shift);
    CHECK(!event->pointer.modifiers.alt);
}

void test_wheel_carries_a_signed_delta_and_deliberately_no_position()
{
    // The delta is in the HIGH word of wParam and is signed: a scroll toward the user is negative.
    const std::uint64_t down_notch = static_cast<std::uint64_t>(
        (static_cast<std::uint32_t>(static_cast<std::uint16_t>(-kWheelDelta)) << 16));
    const std::optional<ShellEvent> down = decode(kWmMouseWheel, down_notch, lparam(900, 900));
    CHECK(down.has_value());
    CHECK(down->pointer.action == PointerAction::wheel);
    CHECK(down->pointer.wheel_delta_y == -kWheelDelta);

    const std::uint64_t up_notch =
        static_cast<std::uint64_t>(static_cast<std::uint32_t>(kWheelDelta) << 16);
    const std::optional<ShellEvent> up = decode(kWmMouseWheel, up_notch, lparam(900, 900));
    CHECK(up->pointer.wheel_delta_y == kWheelDelta);

    // WM_MOUSEWHEEL's lParam is SCREEN-relative, unlike every other mouse message. The decoder
    // therefore reports NO position and the backend supplies the last known CLIENT position — using
    // the screen coordinate would arbitrate against the wrong region whenever the window is not at
    // the desktop origin.
    CHECK(up->pointer.position == (PointI{0, 0}));
}

// The HORIZONTAL wheel (tilt wheel / precision touchpad). Same wParam shape, the other axis.
// Without this case wheel_delta_x is forwarded to CEF as a permanent 0 and horizontal scrolling is
// silently dead, while the field's presence advertises that it works.
void test_horizontal_wheel_decodes_onto_the_x_axis()
{
    const std::uint64_t right_notch =
        static_cast<std::uint64_t>(static_cast<std::uint32_t>(kWheelDelta) << 16);
    const std::optional<ShellEvent> right = decode(kWmMouseHWheel, right_notch, lparam(900, 900));
    CHECK(right.has_value());
    CHECK(right->pointer.action == PointerAction::wheel);
    CHECK(right->pointer.wheel_delta_x == kWheelDelta);
    CHECK(right->pointer.wheel_delta_y == 0); // the axes must not bleed into each other

    const std::uint64_t left_notch = static_cast<std::uint64_t>(
        (static_cast<std::uint32_t>(static_cast<std::uint16_t>(-kWheelDelta)) << 16));
    const std::optional<ShellEvent> left = decode(kWmMouseHWheel, left_notch, lparam(900, 900));
    CHECK(left->pointer.wheel_delta_x == -kWheelDelta);
    CHECK(left->pointer.wheel_delta_y == 0);
    // Screen-relative lParam here too, so the decoder reports no position.
    CHECK(left->pointer.position == (PointI{0, 0}));
}

// A double click arrives as WM_*BUTTONDBLCLK, not a second WM_*BUTTONDOWN, and must reach CEF as a
// press carrying click_count == 2 — Chromium derives double-click-to-select-word from that field
// alone, so a shell that always reports 1 disables it everywhere.
void test_double_click_messages_decode_as_a_press_with_click_count_two()
{
    const std::optional<ShellEvent> left = decode(kWmLButtonDblClk, 0, lparam(40, 50));
    CHECK(left.has_value());
    CHECK(left->pointer.action == PointerAction::down);
    CHECK(left->pointer.button == MouseButton::left);
    CHECK(left->pointer.click_count == 2);
    CHECK(left->pointer.position == (PointI{40, 50})); // client-relative, unlike the wheel

    const std::optional<ShellEvent> right = decode(kWmRButtonDblClk, 0, lparam(1, 2));
    CHECK(right->pointer.button == MouseButton::right);
    CHECK(right->pointer.click_count == 2);

    const std::optional<ShellEvent> middle = decode(kWmMButtonDblClk, 0, lparam(3, 4));
    CHECK(middle->pointer.button == MouseButton::middle);
    CHECK(middle->pointer.click_count == 2);

    // A plain press is still a SINGLE click — the default must not drift to 2.
    const std::optional<ShellEvent> single = decode(kWmLButtonDown, 0, lparam(40, 50));
    CHECK(single->pointer.click_count == 1);
}

void test_key_and_char_decoding()
{
    const std::optional<ShellEvent> down = decode(kWmKeyDown, 'S', 0x001F0001);
    CHECK(down.has_value());
    CHECK(down->kind == ShellEventKind::key);
    CHECK(down->key.action == KeyAction::raw_key_down);
    CHECK(down->key.windows_key_code == 'S');
    CHECK(down->key.native_key_code == 0x001F0001);
    CHECK(!down->key.is_system_key);

    const std::optional<ShellEvent> up = decode(kWmKeyUp, 'S', 0);
    CHECK(up->key.action == KeyAction::key_up);

    const std::optional<ShellEvent> character = decode(kWmChar, 0x00E9, 0); // 'é'
    CHECK(character->key.action == KeyAction::character);
    CHECK(character->key.character == 0x00E9);

    // WM_SYSKEYDOWN / WM_SYSCHAR (Alt-combinations) decode to the same actions but are FLAGGED, so
    // the browser can tell an Alt-accelerator from a plain keystroke.
    const std::optional<ShellEvent> sys = decode(kWmSysKeyDown, 'F', 0);
    CHECK(sys->key.action == KeyAction::raw_key_down);
    CHECK(sys->key.is_system_key);
    CHECK(decode(kWmSysChar, 'f', 0)->key.is_system_key);
}

void test_dpi_change_reads_the_low_word()
{
    // WM_DPICHANGED packs the X dpi in the LOW word and the Y dpi in the high word; they are always
    // equal on Windows, and reading the wrong half yields a scale that is off by 65536.
    const std::uint64_t both_144 = (144ull << 16) | 144ull;
    const std::optional<ShellEvent> event = decode(kWmDpiChanged, both_144, 0);
    CHECK(event.has_value());
    CHECK(event->kind == ShellEventKind::dpi_changed);
    CHECK(event->dpi.dpi == 144u);

    // A nonsense value is CLAMPED rather than trusted.
    CHECK(decode(kWmDpiChanged, 0, 0)->dpi.dpi == kMinDpi);
}

void test_lifecycle_and_focus_messages()
{
    CHECK(decode(kWmPaint, 0, 0)->kind == ShellEventKind::paint_requested);
    CHECK(decode(kWmSetFocus, 0, 0)->kind == ShellEventKind::focus_gained);
    CHECK(decode(kWmKillFocus, 0, 0)->kind == ShellEventKind::focus_lost);
    CHECK(decode(kWmClose, 0, 0)->kind == ShellEventKind::close_requested);
    CHECK(decode(kWmDestroy, 0, 0)->kind == ShellEventKind::close_requested);
    CHECK(decode(kWmMouseLeave, 0, 0)->pointer.action == PointerAction::leave);

    const std::optional<ShellEvent> moved = decode(kWmMove, 0, lparam(-1900, 40));
    CHECK(moved->kind == ShellEventKind::moved);
    CHECK(moved->position == (PointI{-1900, 40})); // a monitor left of the primary

    // Every other message is not the Shell's — which is most of them. WM_NCHITTEST is deliberately
    // STILL absent from this decoder: since b1 it is the frameless frame's, answered OS-side
    // through the pure hit_test_frame (the b1 tests below) and returned to Windows as an HT code —
    // never decoded into a ShellEvent the pump would forward.
    CHECK(!decode(kWmNcHitTest, 0, 0).has_value());
    CHECK(!decode(0x0113 /* WM_TIMER */, 0, 0).has_value());
}

// ------------------------------------------------------------ the b1 frameless frame (pure)

void test_resize_border_and_corner_scale_with_dpi()
{
    // SM_CXSIZEFRAME (4) + SM_CXPADDEDBORDER (4) at 96 dpi, round-to-nearest at every scale — the
    // one number the NCCALCSIZE insets AND the hit-test bands share by construction.
    CHECK(win32_resize_border_thickness(DpiScale{96}) == 8);
    CHECK(win32_resize_border_thickness(DpiScale{120}) == 10);
    CHECK(win32_resize_border_thickness(DpiScale{144}) == 12); // 150%
    CHECK(win32_resize_border_thickness(DpiScale{192}) == 16);
    CHECK(win32_resize_corner_extent(DpiScale{96}) == 16);
    CHECK(win32_resize_corner_extent(DpiScale{144}) == 24);
}

void test_frameless_client_insets_restored_and_maximized()
{
    // Restored: l/r/b keep the system border (the NC strips the resize bands live in); TOP IS ZERO
    // — the client reaches the window top, which is where the web titlebar draws.
    const Win32FrameInsets restored_96 = win32_frameless_client_insets(false, DpiScale{96});
    CHECK(restored_96.left == 8 && restored_96.top == 0 && restored_96.right == 8 &&
          restored_96.bottom == 8);
    // Maximized: ALL sides inset (ROADMAP risk 2's branch), at both pinned scales.
    const Win32FrameInsets max_96 = win32_frameless_client_insets(true, DpiScale{96});
    CHECK(max_96.left == 8 && max_96.top == 8 && max_96.right == 8 && max_96.bottom == 8);
    const Win32FrameInsets restored_144 = win32_frameless_client_insets(false, DpiScale{144});
    CHECK(restored_144.left == 12 && restored_144.top == 0 && restored_144.right == 12 &&
          restored_144.bottom == 12);
    const Win32FrameInsets max_144 = win32_frameless_client_insets(true, DpiScale{144});
    CHECK(max_144.left == 12 && max_144.top == 12 && max_144.right == 12 && max_144.bottom == 12);
}

void test_maximized_client_lands_exactly_on_the_work_area_at_96_and_150_percent()
{
    // THE no-8px-overhang pin (ROADMAP risk 2): WM_GETMINMAXINFO's geometry and the maximized
    // insets COMPOSE to client == work area EXACTLY — the frame Windows hangs off-monitor is
    // cancelled by the all-sides inset, at both pinned scales and with a taskbar offset in play.
    struct Case
    {
        std::uint32_t dpi;
        PointI work_origin; // work-area top-left relative to the monitor (the taskbar offset)
        render::Extent2D work;
    };
    const Case cases[] = {
        {96, PointI{0, 0}, render::Extent2D{1920, 1032}},
        {96, PointI{64, 0}, render::Extent2D{1856, 1080}}, // taskbar docked left
        {144, PointI{0, 0}, render::Extent2D{2560, 1352}}, // 150%
    };
    for (const Case& c : cases)
    {
        const DpiScale dpi{c.dpi};
        const Win32MaxGeometry geometry = win32_frameless_max_geometry(c.work_origin, c.work, dpi);
        const Win32FrameInsets insets = win32_frameless_client_insets(true, dpi);
        // The client rect the two produce together, in monitor-relative coordinates.
        const std::int32_t client_left = geometry.position.x + insets.left;
        const std::int32_t client_top = geometry.position.y + insets.top;
        const std::int32_t client_right = geometry.position.x +
                                          static_cast<std::int32_t>(geometry.size.width) -
                                          insets.right;
        const std::int32_t client_bottom = geometry.position.y +
                                           static_cast<std::int32_t>(geometry.size.height) -
                                           insets.bottom;
        CHECK(client_left == c.work_origin.x);
        CHECK(client_top == c.work_origin.y);
        CHECK(client_right == c.work_origin.x + static_cast<std::int32_t>(c.work.width));
        CHECK(client_bottom == c.work_origin.y + static_cast<std::int32_t>(c.work.height));
    }
}

// The a2-shaped chrome map the hit-test consumes: caption FIRST, controls after (last-match-wins),
// plus a viewport rect to prove non-caption kinds stay client.
RegionMap make_chrome_regions()
{
    RegionMap map;
    map.publish(
        {ShellRegion{"chrome.caption", shelltest::rect(0, 0, 1096, 38), RegionKind::caption},
         ShellRegion{"chrome.caption-min", shelltest::rect(1096, 0, 46, 38),
                     RegionKind::caption_min},
         ShellRegion{"chrome.caption-max", shelltest::rect(1142, 0, 46, 38),
                     RegionKind::caption_max},
         ShellRegion{"chrome.caption-close", shelltest::rect(1188, 0, 46, 38),
                     RegionKind::caption_close},
         ShellRegion{"viewport", shelltest::rect(100, 100, 600, 400), RegionKind::viewport}});
    return map;
}

void test_hit_test_frame_resize_bands_and_corners()
{
    const RegionMap regions = make_chrome_regions();
    const render::Extent2D client{1280, 800};
    const DpiScale dpi{96}; // border 8, corner 16
    const auto hit = [&](std::int32_t x, std::int32_t y)
    { return hit_test_frame(PointI{x, y}, client, dpi, false, regions); };

    // The l/r/b NC strips (outside the client rect the insets carved).
    CHECK(hit(-1, 400) == kHtLeft);
    CHECK(hit(1280, 400) == kHtRight);
    CHECK(hit(640, 800) == kHtBottom);
    // The top band: the first `border` rows INSIDE the client.
    CHECK(hit(640, 4) == kHtTop);
    CHECK(hit(640, 7) == kHtTop);
    // Corners, via the corner extent along each edge.
    CHECK(hit(-1, 5) == kHtTopLeft);
    CHECK(hit(4, 4) == kHtTopLeft);
    CHECK(hit(1276, 2) == kHtTopRight);
    CHECK(hit(1285, 10) == kHtTopRight);
    CHECK(hit(-1, 790) == kHtBottomLeft);
    CHECK(hit(5, 805) == kHtBottomLeft);
    CHECK(hit(1280, 795) == kHtBottomRight);
    CHECK(hit(1275, 801) == kHtBottomRight);
    // Bands come FIRST: the top rows win over the caption AND over a control under them — the same
    // priority a stock titlebar's top edge has.
    CHECK(hit(1200, 4) == kHtTop);      // over the close button's rect
    CHECK(hit(1270, 4) == kHtTopRight); // over the close button, within the corner extent
}

void test_hit_test_frame_regions_and_precedence()
{
    const RegionMap regions = make_chrome_regions();
    const render::Extent2D client{1280, 800};
    const DpiScale dpi{96};
    const auto hit = [&](std::int32_t x, std::int32_t y)
    { return hit_test_frame(PointI{x, y}, client, dpi, false, regions); };

    // Below the band: the published regions decide.
    CHECK(hit(640, 20) == kHtCaption);
    CHECK(hit(1119, 20) == kHtMinButton);
    CHECK(hit(1165, 20) == kHtMaxButton); // what lights Snap Layouts on Win11
    CHECK(hit(1211, 20) == kHtClose);
    // Non-caption kinds are CLIENT content — the frame never claims a viewport.
    CHECK(hit(150, 150) == kHtClient);
    // No region at all: client.
    CHECK(hit(640, 400) == kHtClient);

    // Control-over-caption by last-match-wins: a caption spanning the WHOLE width with the close
    // control published after it still resolves the control (no carve-out token — input.h).
    RegionMap stacked;
    stacked.publish(
        {ShellRegion{"chrome.caption", shelltest::rect(0, 0, 1280, 38), RegionKind::caption},
         ShellRegion{"chrome.caption-close", shelltest::rect(1188, 0, 46, 38),
                     RegionKind::caption_close}});
    CHECK(hit_test_frame(PointI{1211, 20}, client, dpi, false, stacked) == kHtClose);
    CHECK(hit_test_frame(PointI{640, 20}, client, dpi, false, stacked) == kHtCaption);

    // An EMPTY map (nothing published yet — the pre-boot window): everything in the client is
    // client, and the bands still work.
    const RegionMap empty;
    CHECK(hit_test_frame(PointI{640, 20}, client, dpi, false, empty) == kHtClient);
    CHECK(hit_test_frame(PointI{-1, 400}, client, dpi, false, empty) == kHtLeft);
}

void test_hit_test_frame_scales_bands_with_dpi()
{
    const RegionMap regions = make_chrome_regions();
    const render::Extent2D client{1280, 800};
    // y == 10: below the 8px band at 96 dpi (the caption's), inside the 12px band at 150%.
    CHECK(hit_test_frame(PointI{640, 10}, client, DpiScale{96}, false, regions) == kHtCaption);
    CHECK(hit_test_frame(PointI{640, 10}, client, DpiScale{144}, false, regions) == kHtTop);
    // The corner extent scales too: 20 px down the left strip is past the 16px corner at 96 dpi,
    // inside the 24px one at 150%.
    CHECK(hit_test_frame(PointI{-1, 20}, client, DpiScale{96}, false, regions) == kHtLeft);
    CHECK(hit_test_frame(PointI{-1, 20}, client, DpiScale{144}, false, regions) == kHtTopLeft);
}

void test_hit_test_frame_maximized_has_no_resize_bands()
{
    const RegionMap regions = make_chrome_regions();
    const render::Extent2D client{1280, 800};
    const DpiScale dpi{96};
    // Maximized, the top rows belong to the caption (nothing to resize)...
    CHECK(hit_test_frame(PointI{640, 4}, client, dpi, true, regions) == kHtCaption);
    // ...and a would-be band point with no region under it is plain client.
    CHECK(hit_test_frame(PointI{640, 799}, client, dpi, true, regions) == kHtClient);
    CHECK(hit_test_frame(PointI{-1, 400}, client, dpi, true, regions) == kHtClient);
}

// --- the g1 SWEEP CORPUS over hit_test_frame ----------------------------------------------------
//
// editor-window-chrome g1 (verification closeout). The four tests above pin hand-picked points,
// and a hand-picked point cannot catch a boundary that slipped by one pixel SOMEWHERE ELSE in the
// 2-D domain — which is exactly the kind of function the frame decision is: four NC strips, eight
// corner zones, a top band INSIDE the client, four DPIs, two frame states, an overlapping region
// map. So
// this sweeps EVERY point of the window rect — the client plus the l/r/b strips the insets carved,
// i.e. the whole domain WM_NCHITTEST can ask about — at four DPIs, in both frame states, over three
// region maps, and judges every answer two ways:
//
//   1. against an INDEPENDENT ORACLE written from the spec (window.h § hit_test_frame) in ZONE
//      terms — which strip, which corner zone, which rect contains the point — with its own DPI
//      arithmetic, rather than by re-running the implementation's control flow;
//   2. against oracle-FREE invariants of the spec that must hold whatever the oracle says: the
//      answer set is CLOSED; a maximized window has NO band answer anywhere; the band answers do
//      not depend on the region map (bands come first); an empty map is left/right
//      MIRROR-symmetric; the band
//      thickness and corner extent MEASURED along the edges equal the DPI-scaled metrics; and a
//      higher DPI's band set CONTAINS a lower DPI's.
//
// A mismatch is REPORTED with its coordinates, DPI, frame state and map, so a red names a point and
// not a count. The domain (a 640x400 client) keeps the sweep at ~6.5M evaluations — sub-second in
// the dev gate and comfortably inside the sanitizer legs' budget.

struct SweepMetrics
{
    std::int32_t border; // the resize border, in px at this DPI
    std::int32_t corner; // the corner grip's extent along each edge
};

// The spec's numbers, computed HERE from the 96-dpi reference (8 px border, 16 px corner) with the
// spec's own round-to-nearest rule — deliberately not read back from the functions under test, so
// a metric regression is visible to the oracle too.
SweepMetrics sweep_metrics(std::uint32_t dpi)
{
    const auto scaled = [dpi](std::int32_t at_96)
    { return static_cast<std::int32_t>((static_cast<std::uint32_t>(at_96) * dpi + 48u) / 96u); };
    return SweepMetrics{scaled(8), scaled(16)};
}

enum class SweepZone
{
    client,
    top,
    top_left,
    top_right,
    left,
    right,
    bottom,
    bottom_left,
    bottom_right,
};

// Which resize zone, if any, the spec places `point` in: the l/r/b NC strips outside the client
// rect, the first `border` rows INSIDE it, and within any of those, the corner zones within
// `corner` of a client edge. A maximized window has no zones at all.
SweepZone sweep_zone(PointI point, render::Extent2D client, SweepMetrics metrics, bool maximized)
{
    if (maximized)
    {
        return SweepZone::client;
    }
    const std::int32_t width = static_cast<std::int32_t>(client.width);
    const std::int32_t height = static_cast<std::int32_t>(client.height);
    const bool left_strip = point.x < 0;
    const bool right_strip = point.x >= width;
    const bool bottom_strip = point.y >= height;
    const bool top_band = point.y < metrics.border;
    if (!left_strip && !right_strip && !bottom_strip && !top_band)
    {
        return SweepZone::client;
    }
    const bool near_left = point.x < metrics.corner;
    const bool near_right = point.x >= width - metrics.corner;
    if (point.y < metrics.corner)
    {
        return near_left ? SweepZone::top_left : near_right ? SweepZone::top_right : SweepZone::top;
    }
    if (point.y >= height - metrics.corner)
    {
        return near_left    ? SweepZone::bottom_left
               : near_right ? SweepZone::bottom_right
                            : SweepZone::bottom;
    }
    return near_left ? SweepZone::left : SweepZone::right;
}

std::int32_t sweep_zone_code(SweepZone zone)
{
    switch (zone)
    {
    case SweepZone::top:
        return kHtTop;
    case SweepZone::top_left:
        return kHtTopLeft;
    case SweepZone::top_right:
        return kHtTopRight;
    case SweepZone::left:
        return kHtLeft;
    case SweepZone::right:
        return kHtRight;
    case SweepZone::bottom:
        return kHtBottom;
    case SweepZone::bottom_left:
        return kHtBottomLeft;
    case SweepZone::bottom_right:
        return kHtBottomRight;
    case SweepZone::client:
        break;
    }
    return kHtClient;
}

// The oracle's own region lookup: half-open rects, LAST match wins, the spec's kind -> code table.
std::int32_t sweep_region_code(PointI point, const std::vector<ShellRegion>& regions)
{
    for (std::size_t i = regions.size(); i > 0; --i)
    {
        const ShellRegion& region = regions[i - 1];
        const std::int64_t x0 = region.rect.origin.x;
        const std::int64_t y0 = region.rect.origin.y;
        const std::int64_t x1 = x0 + region.rect.size.width;
        const std::int64_t y1 = y0 + region.rect.size.height;
        if (point.x < x0 || point.x >= x1 || point.y < y0 || point.y >= y1)
        {
            continue;
        }
        switch (region.kind)
        {
        case RegionKind::caption:
            return kHtCaption;
        case RegionKind::caption_min:
            return kHtMinButton;
        case RegionKind::caption_max:
            return kHtMaxButton;
        case RegionKind::caption_close:
            return kHtClose;
        case RegionKind::viewport:
        case RegionKind::native:
            return kHtClient;
        }
        return kHtClient;
    }
    return kHtClient;
}

std::int32_t sweep_oracle(PointI point, render::Extent2D client, SweepMetrics metrics,
                          bool maximized, const std::vector<ShellRegion>& regions)
{
    const SweepZone zone = sweep_zone(point, client, metrics, maximized);
    return zone == SweepZone::client ? sweep_region_code(point, regions) : sweep_zone_code(zone);
}

bool sweep_is_band_code(std::int32_t code)
{
    return code == kHtLeft || code == kHtRight || code == kHtTop || code == kHtTopLeft ||
           code == kHtTopRight || code == kHtBottom || code == kHtBottomLeft ||
           code == kHtBottomRight;
}

bool sweep_is_known_code(std::int32_t code)
{
    return code == kHtClient || code == kHtCaption || code == kHtMinButton ||
           code == kHtMaxButton || code == kHtClose || sweep_is_band_code(code);
}

// The left/right mirror of a code, for the symmetry invariant.
std::int32_t sweep_mirror_code(std::int32_t code)
{
    switch (code)
    {
    case kHtLeft:
        return kHtRight;
    case kHtRight:
        return kHtLeft;
    case kHtTopLeft:
        return kHtTopRight;
    case kHtTopRight:
        return kHtTopLeft;
    case kHtBottomLeft:
        return kHtBottomRight;
    case kHtBottomRight:
        return kHtBottomLeft;
    default:
        return code;
    }
}

// Mismatch accounting: every failed claim counts, the FIRST is described with its coordinates.
struct SweepReport
{
    int failures = 0;
    bool described = false;

    void note(bool ok, const char* claim, const char* map_name, std::uint32_t dpi, bool maximized,
              PointI point, std::int32_t expected, std::int32_t got)
    {
        if (ok)
        {
            return;
        }
        ++failures;
        if (!described)
        {
            described = true;
            std::fprintf(stderr,
                         "hit_test_frame sweep: %s -- map=%s dpi=%u %s point=(%d,%d) expected=%d "
                         "got=%d\n",
                         claim, map_name, dpi, maximized ? "maximized" : "restored", point.x,
                         point.y, expected, got);
        }
    }
};

struct SweepMap
{
    const char* name;
    std::vector<ShellRegion> regions;
};

void test_hit_test_frame_sweep_corpus_matches_the_spec_oracle_at_every_point()
{
    const render::Extent2D client{640, 400};
    const std::uint32_t dpis[] = {96, 120, 144, 192};

    // Three maps: nothing published (the pre-boot window), the a2 strip shape scaled to this client
    // (caption FIRST, the three controls after, a viewport below), and an OVERLAPPING shape — a
    // full-width caption with two controls published over it in a non-geometric order plus a native
    // rect under a viewport — so last-match-wins is judged over whole overlap AREAS, not one point.
    const SweepMap maps[] = {
        SweepMap{"empty", {}},
        SweepMap{"chrome",
                 {ShellRegion{"chrome.caption", shelltest::rect(0, 0, 502, 38),
                              RegionKind::caption},
                  ShellRegion{"chrome.caption-min", shelltest::rect(502, 0, 46, 38),
                              RegionKind::caption_min},
                  ShellRegion{"chrome.caption-max", shelltest::rect(548, 0, 46, 38),
                              RegionKind::caption_max},
                  ShellRegion{"chrome.caption-close", shelltest::rect(594, 0, 46, 38),
                              RegionKind::caption_close},
                  ShellRegion{"viewport", shelltest::rect(60, 60, 300, 200),
                              RegionKind::viewport}}},
        SweepMap{"stacked",
                 {ShellRegion{"chrome.caption", shelltest::rect(0, 0, 640, 38),
                              RegionKind::caption},
                  ShellRegion{"chrome.caption-close", shelltest::rect(594, 0, 46, 38),
                              RegionKind::caption_close},
                  ShellRegion{"chrome.caption-max", shelltest::rect(548, 0, 46, 38),
                              RegionKind::caption_max},
                  ShellRegion{"native", shelltest::rect(100, 100, 200, 100), RegionKind::native},
                  ShellRegion{"viewport", shelltest::rect(150, 150, 200, 100),
                              RegionKind::viewport}}},
    };

    SweepReport report;
    const std::int32_t width = static_cast<std::int32_t>(client.width);
    const std::int32_t height = static_cast<std::int32_t>(client.height);
    const RegionMap bare;

    for (const std::uint32_t dpi_value : dpis)
    {
        const DpiScale dpi{dpi_value};
        const SweepMetrics metrics = sweep_metrics(dpi_value);
        // The window rect: the client plus the l/r/b strips of `border` px. Nothing above y == 0 —
        // the top inset is zero, so there is no NC strip there to ask about.
        const std::int32_t x_begin = -metrics.border;
        const std::int32_t x_end = width + metrics.border;
        const std::int32_t y_end = height + metrics.border;

        for (const SweepMap& map : maps)
        {
            RegionMap regions;
            regions.publish(map.regions);
            for (const bool maximized : {false, true})
            {
                for (std::int32_t y = 0; y < y_end; ++y)
                {
                    for (std::int32_t x = x_begin; x < x_end; ++x)
                    {
                        const PointI point{x, y};
                        const std::int32_t got =
                            hit_test_frame(point, client, dpi, maximized, regions);
                        const std::int32_t expected =
                            sweep_oracle(point, client, metrics, maximized, map.regions);
                        report.note(got == expected, "the answer matches the spec oracle",
                                    map.name, dpi_value, maximized, point, expected, got);
                        report.note(sweep_is_known_code(got), "the answer is a known HT code",
                                    map.name, dpi_value, maximized, point, expected, got);
                        report.note(!maximized || !sweep_is_band_code(got),
                                    "a maximized window answers no resize band anywhere",
                                    map.name, dpi_value, maximized, point, expected, got);
                        if (!maximized)
                        {
                            // Bands come FIRST: the empty map's band answer is every map's answer,
                            // and where the empty map says client, no map may say band.
                            const std::int32_t plain =
                                hit_test_frame(point, client, dpi, false, bare);
                            const bool band_agrees = sweep_is_band_code(plain)
                                                         ? got == plain
                                                         : !sweep_is_band_code(got);
                            report.note(band_agrees,
                                        "the band answers are independent of the region map",
                                        map.name, dpi_value, maximized, point, plain, got);
                        }
                    }
                }
            }
        }

        // Mirror symmetry of the bare frame: x and (width - 1 - x) answer mirrored codes, on every
        // row of the domain (the strips included: -border mirrors onto width + border - 1).
        for (std::int32_t y = 0; y < y_end; ++y)
        {
            for (std::int32_t x = x_begin; x < x_end; ++x)
            {
                const std::int32_t got = hit_test_frame(PointI{x, y}, client, dpi, false, bare);
                const std::int32_t mirrored =
                    hit_test_frame(PointI{width - 1 - x, y}, client, dpi, false, bare);
                report.note(got == sweep_mirror_code(mirrored),
                            "the bare frame is left/right mirror-symmetric", "empty", dpi_value,
                            false, PointI{x, y}, sweep_mirror_code(mirrored), got);
            }
        }

        // The metrics, MEASURED along the edges: the top band is exactly `border` rows deep at the
        // mid-column, the left strip is `border` columns wide at mid-height (its whole extent in
        // the domain), and the top-left corner zone runs exactly `corner` rows down the left strip.
        std::int32_t top_rows = 0;
        while (top_rows < height &&
               hit_test_frame(PointI{width / 2, top_rows}, client, dpi, false, bare) == kHtTop)
        {
            ++top_rows;
        }
        report.note(top_rows == metrics.border, "the top band measures `border` rows", "empty",
                    dpi_value, false, PointI{width / 2, top_rows}, metrics.border, top_rows);
        std::int32_t left_columns = 0;
        while (left_columns < metrics.border &&
               hit_test_frame(PointI{-1 - left_columns, height / 2}, client, dpi, false, bare) ==
                   kHtLeft)
        {
            ++left_columns;
        }
        report.note(left_columns == metrics.border, "the left strip measures `border` columns",
                    "empty", dpi_value, false, PointI{-1 - left_columns, height / 2},
                    metrics.border, left_columns);
        std::int32_t corner_rows = 0;
        while (corner_rows < height &&
               hit_test_frame(PointI{-1, corner_rows}, client, dpi, false, bare) == kHtTopLeft)
        {
            ++corner_rows;
        }
        report.note(corner_rows == metrics.corner, "the corner zone measures `corner` rows",
                    "empty", dpi_value, false, PointI{-1, corner_rows}, metrics.corner,
                    corner_rows);
        // And the spec's numbers agree with the shipped metric functions.
        report.note(win32_resize_border_thickness(dpi) == metrics.border,
                    "win32_resize_border_thickness matches the spec's scaling", "empty",
                    dpi_value, false, PointI{0, 0}, metrics.border,
                    win32_resize_border_thickness(dpi));
        report.note(win32_resize_corner_extent(dpi) == metrics.corner,
                    "win32_resize_corner_extent matches the spec's scaling", "empty", dpi_value,
                    false, PointI{0, 0}, metrics.corner, win32_resize_corner_extent(dpi));
    }

    // DPI monotonicity: every point that is a band at a lower DPI is a band at every higher one,
    // over the lower DPI's domain (which the higher one's contains).
    for (std::size_t i = 1; i < 4; ++i)
    {
        const SweepMetrics low = sweep_metrics(dpis[i - 1]);
        for (std::int32_t y = 0; y < height + low.border; ++y)
        {
            for (std::int32_t x = -low.border; x < width + low.border; ++x)
            {
                const std::int32_t at_low =
                    hit_test_frame(PointI{x, y}, client, DpiScale{dpis[i - 1]}, false, bare);
                const std::int32_t at_high =
                    hit_test_frame(PointI{x, y}, client, DpiScale{dpis[i]}, false, bare);
                report.note(!sweep_is_band_code(at_low) || sweep_is_band_code(at_high),
                            "a higher DPI's band set contains a lower DPI's", "empty", dpis[i],
                            false, PointI{x, y}, at_low, at_high);
            }
        }
    }

    CHECK(report.failures == 0);
}

void test_nc_mouse_forwards_controls_and_leaves_the_caption_to_the_os()
{
    const PointI at{1211, 20};
    // A move over a control is FORWARDED (CSS hover lights) and never consumed.
    const Win32NcMouseDecision move =
        translate_win32_nc_mouse(kWmNcMouseMove, kHtClose, at, {}, false, false);
    CHECK(move.event.has_value());
    CHECK(move.event->pointer.action == PointerAction::move);
    CHECK(move.event->pointer.position == at);
    CHECK(!move.event->pointer.modifiers.left_button_down);
    CHECK(!move.consume);
    CHECK(move.hover);
    CHECK(!move.pressed);

    // A press over a control is forwarded AND consumed — DefWindowProc's classic caption-button
    // tracking must never fire the system command a web button is about to dispatch.
    const Win32NcMouseDecision down =
        translate_win32_nc_mouse(kWmNcLButtonDown, kHtMaxButton, at, {}, true, false);
    CHECK(down.event.has_value());
    CHECK(down.event->pointer.action == PointerAction::down);
    CHECK(down.event->pointer.button == MouseButton::left);
    CHECK(down.event->pointer.click_count == 1);
    CHECK(down.event->pointer.modifiers.left_button_down);
    CHECK(down.consume);
    CHECK(down.pressed);
    // ...and an NC double-click on a control is a second press with click_count 2, not a maximize.
    const Win32NcMouseDecision dbl =
        translate_win32_nc_mouse(kWmNcLButtonDblClk, kHtMinButton, at, {}, true, false);
    CHECK(dbl.event.has_value() && dbl.event->pointer.click_count == 2 && dbl.consume);

    // The caption and the bands are the OS's: nothing forwarded, nothing consumed — DefWindowProc
    // owns drag/snap/double-click-maximize/system-menu through HTCAPTION.
    const Win32NcMouseDecision caption_press =
        translate_win32_nc_mouse(kWmNcLButtonDown, kHtCaption, PointI{400, 20}, {}, false, false);
    CHECK(!caption_press.event.has_value());
    CHECK(!caption_press.consume);
    const Win32NcMouseDecision band_move =
        translate_win32_nc_mouse(kWmNcMouseMove, kHtTop, PointI{400, 4}, {}, false, false);
    CHECK(!band_move.event.has_value());
    CHECK(!band_move.consume);
}

void test_nc_mouse_synthesizes_the_leave_that_prevents_a_stuck_hover()
{
    // Sliding off a hovered control onto the caption synthesizes the leave a client-area exit
    // would have produced (ROADMAP risk 3): without it the close button stays lit while the OS
    // drags the window.
    const Win32NcMouseDecision off =
        translate_win32_nc_mouse(kWmNcMouseMove, kHtCaption, PointI{800, 20}, {}, true, false);
    CHECK(off.event.has_value());
    CHECK(off.event->pointer.action == PointerAction::leave);
    CHECK(!off.hover);
    CHECK(!off.consume);
    // Same on a caption PRESS while a control was hovered, and on WM_NCMOUSELEAVE.
    const Win32NcMouseDecision press =
        translate_win32_nc_mouse(kWmNcLButtonDown, kHtCaption, PointI{800, 20}, {}, true, false);
    CHECK(press.event.has_value() && press.event->pointer.action == PointerAction::leave);
    CHECK(!press.consume); // the drag itself stays DefWindowProc's
    const Win32NcMouseDecision gone =
        translate_win32_nc_mouse(kWmNcMouseLeave, kHtNowhere, PointI{}, {}, true, false);
    CHECK(gone.event.has_value() && gone.event->pointer.action == PointerAction::leave);
    CHECK(!gone.hover);
    // Without a live hover, none of those synthesize anything.
    CHECK(!translate_win32_nc_mouse(kWmNcMouseMove, kHtCaption, PointI{800, 20}, {}, false, false)
               .event.has_value());
    CHECK(!translate_win32_nc_mouse(kWmNcMouseLeave, kHtNowhere, PointI{}, {}, false, false)
               .event.has_value());
}

void test_nc_mouse_releases_a_forwarded_press_wherever_it_lands()
{
    // The release completes ON the control: forwarded, consumed, still hovered.
    const Win32NcMouseDecision up_on =
        translate_win32_nc_mouse(kWmNcLButtonUp, kHtClose, PointI{1211, 20}, {}, true, true);
    CHECK(up_on.event.has_value());
    CHECK(up_on.event->pointer.action == PointerAction::up);
    CHECK(!up_on.event->pointer.modifiers.left_button_down);
    CHECK(up_on.consume);
    CHECK(!up_on.pressed);
    CHECK(up_on.hover);
    // The user dragged off and released on the caption: the release is STILL forwarded — a browser
    // left holding a phantom pressed button drag-selects from the next hover.
    const Win32NcMouseDecision up_off =
        translate_win32_nc_mouse(kWmNcLButtonUp, kHtCaption, PointI{800, 20}, {}, true, true);
    CHECK(up_off.event.has_value() && up_off.event->pointer.action == PointerAction::up);
    CHECK(up_off.consume);
    CHECK(!up_off.pressed);
    CHECK(!up_off.hover);
    // A release with no forwarded press outstanding forwards nothing (there is nothing to close),
    // but the pointer IS over a control now.
    const Win32NcMouseDecision stray =
        translate_win32_nc_mouse(kWmNcLButtonUp, kHtClose, PointI{1211, 20}, {}, false, false);
    CHECK(!stray.event.has_value());
    CHECK(stray.hover);
    // A forwarded move while pressed carries the button state, so CEF sees a drag, not a hover.
    const Win32NcMouseDecision drag =
        translate_win32_nc_mouse(kWmNcMouseMove, kHtClose, PointI{1211, 22}, {}, true, true);
    CHECK(drag.event.has_value() && drag.event->pointer.modifiers.left_button_down);
    CHECK(drag.pressed);
}

void test_headless_backend_reports_no_native_window_by_default()
{
    WindowDesc desc;
    desc.logical_size = render::Extent2D{800, 600};
    HeadlessWindowBackend backend(desc);
    // kind == None is the HONEST report of "there is no presentable native window here", which is
    // what routes the compositor to the CPU present fallback rather than failing.
    CHECK(backend.native_window().kind == render::NativeWindowKind::None);
    CHECK(shelltest::extent_eq(backend.client_size(), render::Extent2D{800, 600}));
    CHECK(backend.dpi().dpi == kReferenceDpi);
    CHECK(backend.alive());
}

void test_headless_backend_applies_state_before_delivering_events()
{
    WindowDesc desc;
    desc.logical_size = render::Extent2D{800, 600};
    HeadlessWindowBackend backend(desc);

    ShellEvent resize;
    resize.kind = ShellEventKind::resize;
    resize.size = render::Extent2D{1024, 768};
    backend.post(resize);

    ShellEvent dpi;
    dpi.kind = ShellEventKind::dpi_changed;
    dpi.dpi = DpiScale{192};
    backend.post(dpi);

    std::vector<ShellEvent> events;
    CHECK(backend.pump(events));
    CHECK(events.size() == 2u);
    // A real OS window already reports the new size by the time the resize is observed; a backend
    // that reported the old one would make every consumer responsible for a synchronization the OS
    // performs for free.
    CHECK(shelltest::extent_eq(backend.client_size(), render::Extent2D{1024, 768}));
    CHECK(backend.dpi().dpi == 192u);

    // Drained: a second pump delivers nothing.
    events.clear();
    CHECK(backend.pump(events));
    CHECK(events.empty());
}

void test_headless_backend_close_ends_the_pump()
{
    WindowDesc desc;
    HeadlessWindowBackend backend(desc);
    ShellEvent close;
    close.kind = ShellEventKind::close_requested;
    backend.post(close);
    std::vector<ShellEvent> events;
    CHECK(!backend.pump(events)); // false is the owner loop's termination condition
    CHECK(!backend.alive());
    CHECK(events.size() == 1u);
}

void test_headless_backend_records_placement_and_redraws()
{
    WindowDesc desc;
    HeadlessWindowBackend backend(desc);
    backend.apply_placement(WindowPlacement{"\\\\.\\DISPLAY1", 40, 50, 640, 480, false});
    CHECK(backend.placement().x == 40);
    CHECK(shelltest::extent_eq(backend.client_size(), render::Extent2D{640, 480}));

    backend.request_redraw();
    backend.request_redraw();
    CHECK(backend.redraw_requests() == 2);

    backend.set_title("Context Editor — demo");
    CHECK(backend.title() == "Context Editor — demo");
}

void test_headless_backend_chrome_verbs_are_honest_state_only()
{
    // a1 (editor-window-chrome): the two chrome verbs on the honest offscreen shell. There is no OS
    // window, so `minimize` RECORDS the ask and `set_maximized` flips the placement bit — which is
    // exactly the lever the WindowManager placement-poll -> chrome-fact test in test_shell.cpp
    // flips, so the two suites exercise the same seam from both sides.
    WindowDesc desc;
    HeadlessWindowBackend backend(desc);
    CHECK(!backend.minimized());
    CHECK(!backend.placement().maximized);

    backend.minimize();
    CHECK(backend.minimized());

    backend.set_maximized(true);
    CHECK(backend.placement().maximized);
    // The rest of the placement is UNTOUCHED — set_maximized is the state bit, not a resize (the
    // restored rect survives, per WindowPlacement's own contract).
    backend.apply_placement(WindowPlacement{"", 40, 50, 640, 480, true});
    backend.set_maximized(false);
    CHECK(!backend.placement().maximized);
    CHECK(backend.placement().x == 40);
    CHECK(backend.placement().width == 640u);
}

void test_headless_backend_records_the_pushed_down_chrome_facts()
{
    // b1: the two chrome FACTS on the honest offscreen shell. Recording is the whole behaviour —
    // there is no OS frame — and it is the observable test_shell.cpp's push-down wiring test reads,
    // which is what gives the EditorWindow -> backend push a ctest on every leg (the real consumer,
    // the win32 WM_NCHITTEST, only ever runs on an interactive Windows desktop).
    WindowDesc desc;
    HeadlessWindowBackend backend(desc);
    CHECK(backend.chrome_regions().empty());
    CHECK(backend.chrome_region_pushes() == 0);
    CHECK(!backend.appearance_dark().has_value()); // "nothing reported" != "light"

    backend.set_chrome_regions(
        {ShellRegion{"chrome.caption", shelltest::rect(0, 0, 1096, 38), RegionKind::caption}});
    CHECK(backend.chrome_region_pushes() == 1);
    CHECK(backend.chrome_regions().size() == 1u);
    CHECK(backend.chrome_regions().front().id == "chrome.caption");

    // Wholesale replace, like RegionMap::publish — an empty push CLEARS (mode changed to system).
    backend.set_chrome_regions({});
    CHECK(backend.chrome_region_pushes() == 2);
    CHECK(backend.chrome_regions().empty());

    backend.set_appearance(true);
    CHECK(backend.appearance_dark().has_value() && *backend.appearance_dark());
    backend.set_appearance(false);
    CHECK(backend.appearance_dark().has_value() && !*backend.appearance_dark());
}

void test_platform_backend_selection_is_never_silent()
{
    WindowDesc desc;
    desc.visible = false;
    WindowBackendSelection selection = make_window_backend(desc);
#if defined(_WIN32)
    // On Windows a real window is created. (This runs on the CI Windows leg and the local dev gate;
    // it is also the only automated exercise of RegisterClassExW/CreateWindowExW, and it is
    // Session-0-safe because the window is never shown.)
    CHECK(selection.backend != nullptr);
    if (selection.backend != nullptr)
    {
        CHECK(selection.diagnostic.empty());
        CHECK(selection.backend->native_window().kind == render::NativeWindowKind::Win32Hwnd);
        CHECK(selection.backend->native_window().handle != nullptr);
        selection.backend->close();
    }
#elif defined(__linux__)
    // On Linux e12a landed the X11 backend, but a CI leg has no display — so BOTH outcomes are
    // legitimate here and each must be honest about itself. What is asserted is the property that
    // actually matters: the selection is never silent, and it never again claims the backend is
    // owed by a future task.
    if (selection.backend != nullptr)
    {
        CHECK(selection.diagnostic.empty());
        CHECK(selection.backend->native_window().kind == render::NativeWindowKind::XlibWindow);
        CHECK(selection.backend->native_window().handle != nullptr);
        CHECK(selection.backend->native_window().display != nullptr);
        selection.backend->close();
    }
    else
    {
        // PIN THE SANCTIONED REASONS, for the argument spelled out in the macOS arm below: a bare
        // non-emptiness check cannot tell an environment fact from a regression, so an XCreateWindow
        // that started returning nothing would keep this arm green on every runner forever.
        //
        // TWO causes are legitimate here, not one, and both must be allowed or this assertion reds a
        // healthy leg: no reachable X server (the `build` leg — it installs libx11-dev but has no
        // display), and a build configured without the X11 development headers (the `sanitize` legs,
        // which run this suite because their ctest step passes no -E, and which do NOT install them).
        // What is excluded is precisely the regression: "XCreateWindow returned no window".
        CHECK(shelltest::mentions(selection.diagnostic, "X server")
              || shelltest::mentions(selection.diagnostic, "development headers"));
        CHECK(!shelltest::mentions(selection.diagnostic, "e12"));
    }
#elif defined(__APPLE__)
    // On macOS e12b landed the Cocoa backend, but a CI leg commonly has no GUI (Aqua) session — so
    // BOTH outcomes are legitimate here and each must be honest about itself, exactly as on Linux.
    // What is asserted is the property that actually matters: the selection is never silent, and it
    // never again claims the backend is owed by a future task.
    //
    // ⚠ THIS IS THE ONLY AUTOMATED EXERCISE OF THE COCOA BACKEND ANYWHERE. No CI job runs a windowed
    // macOS test, so if the runner does have a session, this is the one place
    // -[NSWindow initWithContentRect:] is ever called; if it does not, the CGSession probe's refusal
    // is the one place that path is ever taken. It is safe either way because the window is never
    // shown (`visible = false`) and is closed immediately.
    //
    // ⚠ NOT for the same reason the Windows arm above is Session-0-safe, and the difference is worth
    // knowing: on a runner that DOES have a session, create() also calls
    // -[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular] + -finishLaunching, which
    // registers THIS CTEST PROCESS as a foreground application. The Win32 arm sets no process-wide
    // state at all. Harmless for a test that exits promptly; it is the reason a future windowed macOS
    // assertion belongs in its own smoke binary rather than growing here.
    if (selection.backend != nullptr)
    {
        CHECK(selection.diagnostic.empty());
        CHECK(selection.backend->native_window().kind == render::NativeWindowKind::MetalLayer);
        CHECK(selection.backend->native_window().handle != nullptr);
        selection.backend->close();
    }
    else
    {
        // PIN THE SANCTIONED REASON, because "both outcomes are legitimate" must not decay into
        // "any outcome is legitimate". create() refuses for exactly two reasons, and only ONE of
        // them is an environment fact: no Aqua session. The other —
        // "[NSWindow initWithContentRect:] returned nil" — is a genuine regression, and a bare
        // non-emptiness check cannot tell them apart, so a break that killed window creation on
        // every macOS runner would keep this, the ONLY automated exercise of the backend, green
        // forever. Matching on "session" makes any other refusal red.
        CHECK(shelltest::mentions(selection.diagnostic, "session"));
        CHECK(!shelltest::mentions(selection.diagnostic, "e12"));
    }
#else
    // A FOURTH platform — a BSD, or a future Wayland-only target. All three v1 platforms have a
    // backend since e12b, so the remaining refusal names the shipped set rather than a task.
    CHECK(selection.backend == nullptr);
    CHECK(!selection.diagnostic.empty());
    CHECK(!shelltest::mentions(selection.diagnostic, "e12"));
#endif
}

void test_platform_window_factories_refuse_off_their_platform()
{
    WindowDesc desc;
    desc.visible = false;
    std::string error;

#if !defined(_WIN32)
    // Compiled everywhere, real only on Windows — mirroring make_win32_gdi_blitter, so the refusal
    // is a VALUE this suite asserts on every leg rather than a symbol that is simply absent.
    CHECK(make_win32_window_backend(desc, error) == nullptr);
    CHECK(!error.empty());
#endif

#if !defined(__linux__)
    error.clear();
    CHECK(make_x11_window_backend(desc, error) == nullptr);
    CHECK(!error.empty());
#endif

#if !defined(__APPLE__)
    // Same shape again for the Cocoa backend (e12b). Its off-platform half lives in a separate .cpp
    // rather than the .mm's `#else` — CMake picks a compiler by file extension, so a single .mm
    // would have to be compiled as Objective-C++ on legs that enable no OBJCXX compiler at all — but
    // the OBSERVABLE contract is identical to its two siblings', which is what this asserts.
    error.clear();
    CHECK(make_cocoa_window_backend(desc, error) == nullptr);
    CHECK(!error.empty());
#endif
}

// ------------------------------------------------------------------------- the X11 event decoder

X11Event x11_event(std::int32_t type)
{
    X11Event event;
    event.type = type;
    return event;
}

void test_configure_notify_reports_only_what_actually_changed()
{
    const X11WindowGeometry previous{100, 50, 640, 480};

    // A pure MOVE: X sends a ConfigureNotify for every drag step, and reporting an unchanged size
    // as a resize would reconfigure the swapchain on each one.
    X11Event moved = x11_event(kX11ConfigureNotify);
    moved.x = 140;
    moved.y = 70;
    moved.width = 640;
    moved.height = 480;
    X11EventBatch batch = translate_x11_event(moved, previous);
    CHECK(batch.count == 1u);
    CHECK(batch.events[0].kind == ShellEventKind::moved);
    CHECK(batch.events[0].position == (PointI{140, 70}));

    // A pure RESIZE.
    X11Event resized = x11_event(kX11ConfigureNotify);
    resized.x = 100;
    resized.y = 50;
    resized.width = 800;
    resized.height = 600;
    batch = translate_x11_event(resized, previous);
    CHECK(batch.count == 1u);
    CHECK(batch.events[0].kind == ShellEventKind::resize);
    CHECK(shelltest::extent_eq(batch.events[0].size, render::Extent2D{800, 600}));

    // BOTH at once — what a maximize actually produces. ONE X event, TWO facts.
    X11Event maximized = x11_event(kX11ConfigureNotify);
    maximized.x = 0;
    maximized.y = 0;
    maximized.width = 1920;
    maximized.height = 1080;
    batch = translate_x11_event(maximized, previous);
    CHECK(batch.count == 2u);
    CHECK(batch.events[0].kind == ShellEventKind::resize);
    CHECK(batch.events[1].kind == ShellEventKind::moved);

    // Nothing changed at all: X still sends the event; the Shell must not act on it.
    X11Event unchanged = x11_event(kX11ConfigureNotify);
    unchanged.x = 100;
    unchanged.y = 50;
    unchanged.width = 640;
    unchanged.height = 480;
    CHECK(translate_x11_event(unchanged, previous).count == 0u);

    // A zero-sized configure (an unmapped/withdrawn window) is not a resize, exactly as WM_SIZE's
    // minimize carve-out is not.
    X11Event collapsed = x11_event(kX11ConfigureNotify);
    collapsed.x = 100;
    collapsed.y = 50;
    collapsed.width = 0;
    collapsed.height = 0;
    CHECK(translate_x11_event(collapsed, previous).count == 0u);
}

void test_wheel_is_a_button_pair_and_only_the_press_counts()
{
    const X11WindowGeometry geometry{0, 0, 640, 480};

    X11Event up = x11_event(kX11ButtonPress);
    up.detail = kX11ButtonWheelUp;
    up.x = 12;
    up.y = 34;
    X11EventBatch batch = translate_x11_event(up, geometry);
    CHECK(batch.count == 1u);
    CHECK(batch.events[0].pointer.action == PointerAction::wheel);
    CHECK(batch.events[0].pointer.wheel_delta_y == kWheelDelta);
    CHECK(batch.events[0].pointer.wheel_delta_x == 0);
    CHECK(batch.events[0].pointer.position == (PointI{12, 34}));

    // THE TRAP: the core protocol sends a ButtonRelease for the same pseudo-button. Decoding it too
    // scrolls exactly twice as far as the user asked — which reads as an over-sensitive mouse
    // rather than as a defect, and is the single easiest X11 input bug to ship.
    X11Event release = x11_event(kX11ButtonRelease);
    release.detail = kX11ButtonWheelUp;
    CHECK(translate_x11_event(release, geometry).count == 0u);

    X11Event down = x11_event(kX11ButtonPress);
    down.detail = kX11ButtonWheelDown;
    CHECK(translate_x11_event(down, geometry).events[0].pointer.wheel_delta_y == -kWheelDelta);

    // The horizontal pair lands on the OTHER axis, right positive — matching Win32's convention so
    // the browser sees one sign rule regardless of platform.
    X11Event right = x11_event(kX11ButtonPress);
    right.detail = kX11ButtonWheelRight;
    batch = translate_x11_event(right, geometry);
    CHECK(batch.events[0].pointer.wheel_delta_x == kWheelDelta);
    CHECK(batch.events[0].pointer.wheel_delta_y == 0);

    X11Event left = x11_event(kX11ButtonPress);
    left.detail = kX11ButtonWheelLeft;
    CHECK(translate_x11_event(left, geometry).events[0].pointer.wheel_delta_x == -kWheelDelta);
}

void test_x11_button_numbers_are_left_middle_right()
{
    const X11WindowGeometry geometry{0, 0, 640, 480};

    // THE TRAP: X's button 2 is MIDDLE and 3 is RIGHT, whereas Win32's MK_MBUTTON is the third bit.
    // An index-based port swaps middle and right on every three-button mouse.
    X11Event middle = x11_event(kX11ButtonPress);
    middle.detail = kX11ButtonMiddle;
    X11EventBatch batch = translate_x11_event(middle, geometry);
    CHECK(batch.count == 1u);
    CHECK(batch.events[0].pointer.button == MouseButton::middle);
    CHECK(batch.events[0].pointer.action == PointerAction::down);
    CHECK(batch.events[0].pointer.click_count == 1);

    X11Event right = x11_event(kX11ButtonRelease);
    right.detail = kX11ButtonRight;
    batch = translate_x11_event(right, geometry);
    CHECK(batch.events[0].pointer.button == MouseButton::right);
    CHECK(batch.events[0].pointer.action == PointerAction::up);

    // Buttons 8/9 are the thumb back/forward keys. Routing them as a nameless click would fire
    // whatever sits under the cursor; they are dropped instead.
    X11Event thumb = x11_event(kX11ButtonPress);
    thumb.detail = 8;
    CHECK(translate_x11_event(thumb, geometry).count == 0u);
}

void test_button_state_is_the_mask_before_the_event()
{
    const X11WindowGeometry geometry{0, 0, 640, 480};

    // THE TRAP: X reports `state` as it was BEFORE the event. A press whose own button is not added
    // hands the browser a mousedown with no button held — which is how a drag never starts.
    X11Event press = x11_event(kX11ButtonPress);
    press.detail = kX11ButtonLeft;
    press.state = kX11ShiftMask; // no Button1Mask yet, exactly as X sends it
    X11EventBatch batch = translate_x11_event(press, geometry);
    CHECK(batch.events[0].pointer.modifiers.left_button_down);
    CHECK(batch.events[0].pointer.modifiers.shift);
    CHECK(!batch.events[0].pointer.modifiers.control);

    // ...and symmetrically, a release must CLEAR its own button, which X still has set.
    X11Event release = x11_event(kX11ButtonRelease);
    release.detail = kX11ButtonLeft;
    release.state = kX11Button1Mask | kX11Button3Mask;
    batch = translate_x11_event(release, geometry);
    CHECK(!batch.events[0].pointer.modifiers.left_button_down);
    CHECK(batch.events[0].pointer.modifiers.right_button_down);

    // Mod1 is Alt and Mod4 is Super — X names its modifier slots by number, not by meaning.
    X11Event motion = x11_event(kX11MotionNotify);
    motion.state = kX11ControlMask | kX11Mod1Mask | kX11Mod4Mask | kX11Button2Mask;
    motion.x = 7;
    motion.y = 9;
    batch = translate_x11_event(motion, geometry);
    CHECK(batch.count == 1u);
    CHECK(batch.events[0].pointer.action == PointerAction::move);
    CHECK(batch.events[0].pointer.position == (PointI{7, 9}));
    CHECK(batch.events[0].pointer.modifiers.control);
    CHECK(batch.events[0].pointer.modifiers.alt);
    CHECK(batch.events[0].pointer.modifiers.meta);
    CHECK(batch.events[0].pointer.modifiers.middle_button_down);
}

void test_grab_synthesized_crossing_and_focus_events_are_ignored()
{
    const X11WindowGeometry geometry{0, 0, 640, 480};

    X11Event leave = x11_event(kX11LeaveNotify);
    leave.mode = kX11NotifyNormal;
    CHECK(translate_x11_event(leave, geometry).count == 1u);
    CHECK(translate_x11_event(leave, geometry).events[0].pointer.action == PointerAction::leave);

    // THE TRAP: X synthesizes Leave/Focus transitions around EVERY grab (opening a menu, starting a
    // drag). Forwarding them makes the browser drop its hover state and blur the caret mid-gesture.
    leave.mode = kX11NotifyGrab;
    CHECK(translate_x11_event(leave, geometry).count == 0u);
    leave.mode = kX11NotifyUngrab;
    CHECK(translate_x11_event(leave, geometry).count == 0u);

    X11Event focus_in = x11_event(kX11FocusIn);
    CHECK(translate_x11_event(focus_in, geometry).events[0].kind == ShellEventKind::focus_gained);
    focus_in.mode = kX11NotifyGrab;
    CHECK(translate_x11_event(focus_in, geometry).count == 0u);

    X11Event focus_out = x11_event(kX11FocusOut);
    CHECK(translate_x11_event(focus_out, geometry).events[0].kind == ShellEventKind::focus_lost);
    focus_out.mode = kX11NotifyUngrab;
    CHECK(translate_x11_event(focus_out, geometry).count == 0u);

    // EnterNotify carries no fact the Shell acts on — the following MotionNotify already reports
    // the position — so it is deliberately not decoded.
    CHECK(translate_x11_event(x11_event(kX11EnterNotify), geometry).count == 0u);
}

void test_expose_repaints_once_per_run_and_delete_closes()
{
    const X11WindowGeometry geometry{0, 0, 640, 480};

    // X sends one Expose per damaged rectangle and counts DOWN; only the last is worth a frame.
    X11Event mid_run = x11_event(kX11Expose);
    mid_run.count = 3;
    CHECK(translate_x11_event(mid_run, geometry).count == 0u);

    X11Event last = x11_event(kX11Expose);
    last.count = 0;
    X11EventBatch batch = translate_x11_event(last, geometry);
    CHECK(batch.count == 1u);
    CHECK(batch.events[0].kind == ShellEventKind::paint_requested);

    // A ClientMessage is only a close when it carries WM_DELETE_WINDOW — every other one (a task-bar
    // command, an XDND handshake) must not tear the editor down.
    X11Event other = x11_event(kX11ClientMessage);
    CHECK(translate_x11_event(other, geometry).count == 0u);
    X11Event close = x11_event(kX11ClientMessage);
    close.is_delete_window = true;
    batch = translate_x11_event(close, geometry);
    CHECK(batch.count == 1u);
    CHECK(batch.events[0].kind == ShellEventKind::close_requested);
}

void test_key_press_yields_the_raw_key_and_only_real_text()
{
    const X11WindowGeometry geometry{0, 0, 640, 480};

    X11Event press = x11_event(kX11KeyPress);
    press.detail = 38;   // the hardware keycode for `a` on a typical layout
    press.keysym = 'a';  // the UNSHIFTED keysym, which is what a VK code is derived from
    press.text = U'a';
    X11EventBatch batch = translate_x11_event(press, geometry);
    // Windows splits the character into its own WM_CHAR; X does not, so the decoder synthesizes the
    // second event rather than making every consumer special-case Linux.
    CHECK(batch.count == 2u);
    CHECK(batch.events[0].key.action == KeyAction::raw_key_down);
    CHECK(batch.events[0].key.windows_key_code == 'A');
    CHECK(batch.events[0].key.native_key_code == 38);
    CHECK(batch.events[0].key.character == 0);
    CHECK(batch.events[1].key.action == KeyAction::character);
    CHECK(batch.events[1].key.character == U'a');
    CHECK(batch.events[1].key.windows_key_code == 'A');
    // CEF documents is_system_key as the Windows WM_SYSKEY* distinction, so it stays false rather
    // than being guessed from the Alt modifier.
    CHECK(!batch.events[0].key.is_system_key);

    // A key that produces NO text (an arrow, a bare modifier) must not emit a character event
    // carrying 0 — the browser would insert a NUL.
    X11Event arrow = x11_event(kX11KeyPress);
    arrow.detail = 111;
    arrow.keysym = 0xff52; // XK_Up
    arrow.text = 0;
    batch = translate_x11_event(arrow, geometry);
    CHECK(batch.count == 1u);
    CHECK(batch.events[0].key.windows_key_code == 0x26); // VK_UP

    // A release never carries text, even when the press did.
    X11Event release = x11_event(kX11KeyRelease);
    release.detail = 38;
    release.keysym = 'a';
    release.text = U'a';
    batch = translate_x11_event(release, geometry);
    CHECK(batch.count == 1u);
    CHECK(batch.events[0].key.action == KeyAction::key_up);
}

void test_keysym_to_windows_key_code()
{
    // THE TRAP: a lowercase latin keysym is 0x61..0x7a but the VK code is the UPPERCASE letter.
    // Passing the keysym through makes every unshifted letter an unrecognised key in Chromium.
    CHECK(x11_keysym_to_windows_key_code('a') == 'A');
    CHECK(x11_keysym_to_windows_key_code('z') == 'Z');
    CHECK(x11_keysym_to_windows_key_code('A') == 'A');
    CHECK(x11_keysym_to_windows_key_code('7') == '7');

    // The keypad digits are VK_NUMPAD0..9, NOT the row digits — mapping them together makes a
    // keypad binding indistinguishable from its row twin.
    CHECK(x11_keysym_to_windows_key_code(0xffb0) == 0x60); // XK_KP_0  -> VK_NUMPAD0
    CHECK(x11_keysym_to_windows_key_code(0xffb9) == 0x69); // XK_KP_9  -> VK_NUMPAD9
    CHECK(x11_keysym_to_windows_key_code(0xff8d) == 0x0D); // XK_KP_Enter -> VK_RETURN

    CHECK(x11_keysym_to_windows_key_code(0xffbe) == 0x70); // XK_F1  -> VK_F1
    CHECK(x11_keysym_to_windows_key_code(0xffc9) == 0x7B); // XK_F12 -> VK_F12

    CHECK(x11_keysym_to_windows_key_code(0xff08) == 0x08); // XK_BackSpace
    CHECK(x11_keysym_to_windows_key_code(0xff0d) == 0x0D); // XK_Return
    CHECK(x11_keysym_to_windows_key_code(0xff1b) == 0x1B); // XK_Escape
    CHECK(x11_keysym_to_windows_key_code(0xffff) == 0x2E); // XK_Delete
    CHECK(x11_keysym_to_windows_key_code(0xffe1) == 0x10); // XK_Shift_L   -> VK_SHIFT
    CHECK(x11_keysym_to_windows_key_code(0xffe4) == 0x11); // XK_Control_R -> VK_CONTROL
    CHECK(x11_keysym_to_windows_key_code(0xffea) == 0x12); // XK_Alt_R     -> VK_MENU
    CHECK(x11_keysym_to_windows_key_code(0xffeb) == 0x5B); // XK_Super_L   -> VK_LWIN
    CHECK(x11_keysym_to_windows_key_code(' ') == 0x20);

    // Punctuation goes to the VK_OEM_* codes, which are NOT the ASCII values.
    CHECK(x11_keysym_to_windows_key_code(';') == 0xBA);
    CHECK(x11_keysym_to_windows_key_code('=') == 0xBB);
    CHECK(x11_keysym_to_windows_key_code(',') == 0xBC);
    CHECK(x11_keysym_to_windows_key_code('-') == 0xBD);
    CHECK(x11_keysym_to_windows_key_code('.') == 0xBE);
    CHECK(x11_keysym_to_windows_key_code('/') == 0xBF);
    CHECK(x11_keysym_to_windows_key_code('`') == 0xC0);
    CHECK(x11_keysym_to_windows_key_code('[') == 0xDB);
    CHECK(x11_keysym_to_windows_key_code('\\') == 0xDC);
    CHECK(x11_keysym_to_windows_key_code(']') == 0xDD);
    CHECK(x11_keysym_to_windows_key_code('\'') == 0xDE);

    // An honest 0 for a keysym with no VK equivalent. A guess here fires the WRONG command, which
    // is strictly worse than firing none.
    CHECK(x11_keysym_to_windows_key_code(0x20ac) == 0); // the euro sign
    CHECK(x11_keysym_to_windows_key_code(0) == 0);
}

void test_x11_dpi_sources()
{
    // Xft.dpi is what the desktop's own scaling setting writes, so it wins when it parses.
    CHECK(x11_parse_xft_dpi("144") == std::optional<std::uint32_t>{144});
    CHECK(x11_parse_xft_dpi("144.0") == std::optional<std::uint32_t>{144});
    CHECK(x11_parse_xft_dpi("  96\t") == std::optional<std::uint32_t>{96});
    CHECK(!x11_parse_xft_dpi("").has_value());
    CHECK(!x11_parse_xft_dpi("auto").has_value());
    CHECK(!x11_parse_xft_dpi("96dpi").has_value());
    CHECK(!x11_parse_xft_dpi("0").has_value());
    CHECK(!x11_parse_xft_dpi("-96").has_value());

    // The screen derivation, and the two shapes it must REFUSE. A 1-metre-wide screen is what a
    // server invents when it has no EDID (~33 dpi, which would shrink the UI to unreadable), and a
    // multi-head X screen sums BOTH axes so its per-axis ratio is plausible but wrong per monitor.
    CHECK(x11_screen_dpi(1920, 509).dpi == 96);  // a real 23" 1080p panel
    CHECK(x11_screen_dpi(3840, 600).dpi == 163); // a real 27" 4K panel
    CHECK(x11_screen_dpi(1920, 1000).dpi == kReferenceDpi); // ~49 dpi: refused
    CHECK(x11_screen_dpi(1920, 100).dpi == kReferenceDpi);  // ~488 dpi: refused
    CHECK(x11_screen_dpi(0, 509).dpi == kReferenceDpi);
    CHECK(x11_screen_dpi(1920, 0).dpi == kReferenceDpi);
    CHECK(x11_screen_dpi(1920, -1).dpi == kReferenceDpi);
}

// ----------------------------------------------------------------------- the Cocoa event decoder
//
// macOS is the harshest case this file exists for. The local dev gate defines _WIN32 so no
// `__APPLE__` branch compiles here at all, and NO CI job runs a windowed macOS test — the
// `build (macos-latest)` leg compiles cocoa_window.mm and then executes exactly this suite. So every
// decision below is either asserted here or asserted nowhere.

NsEvent ns_event(std::int32_t type)
{
    NsEvent event;
    event.type = type;
    return event;
}

// A 1280x800-point view on a 2x display: the shape that makes a missing flip AND a missing scale
// both visible, because 800 points is 1600 pixels and the two errors do not cancel.
NsViewGeometry retina_view()
{
    NsViewGeometry view;
    view.height_points = 800.0;
    view.dpi = ns_dpi_from_backing_scale(2.0);
    return view;
}

void test_cocoa_y_axis_is_flipped_and_points_are_scaled()
{
    // THE TRAP, TWICE OVER. Cocoa's origin is the BOTTOM-left of the view and its coordinates are
    // POINTS. A decoder that forgot the flip mirrors the whole UI vertically; one that forgot the
    // scale is pixel-perfect on a 1x display and half-scale on every Retina one — which is why both
    // are asserted on a 2x view, where a single mistake cannot hide behind the other.
    NsViewGeometry view = retina_view();

    NsEvent moved = ns_event(kNsMouseMoved);
    moved.location_x = 100.0;
    moved.location_y = 700.0; // 100 points below the TOP edge, in Cocoa's upside-down space
    ShellEventBatch batch = translate_ns_event(moved, view);
    CHECK(batch.count == 1u);
    CHECK(batch.events[0].kind == ShellEventKind::pointer);
    CHECK(batch.events[0].pointer.action == PointerAction::move);
    CHECK(batch.events[0].pointer.position == (PointI{200, 200}));

    // The view's ORIGIN in Cocoa (bottom-left) is the BOTTOM-left in the Shell's space too, i.e. the
    // maximum y — the assertion that fails loudly if the flip is dropped.
    NsEvent bottom_left = ns_event(kNsMouseMoved);
    bottom_left.location_x = 0.0;
    bottom_left.location_y = 0.0;
    CHECK(translate_ns_event(bottom_left, view).events[0].pointer.position == (PointI{0, 1600}));

    // ...and Cocoa's TOP-left is the Shell's origin.
    NsEvent top_left = ns_event(kNsMouseMoved);
    top_left.location_x = 0.0;
    top_left.location_y = 800.0;
    CHECK(translate_ns_event(top_left, view).events[0].pointer.position == (PointI{0, 0}));

    // A captured drag legitimately leaves the view. Above the top edge is a NEGATIVE y after the
    // flip, and rounding must carry the sign rather than flooring it away from zero.
    NsEvent above = ns_event(kNsLeftMouseDragged);
    above.location_x = -18.0;
    above.location_y = 806.0;
    CHECK(translate_ns_event(above, view).events[0].pointer.position == (PointI{-36, -12}));

    // At 1x the two spaces coincide — which is exactly why a missing scale ships looking correct.
    NsViewGeometry standard;
    standard.height_points = 800.0;
    NsEvent same = ns_event(kNsMouseMoved);
    same.location_x = 100.0;
    same.location_y = 700.0;
    CHECK(translate_ns_event(same, standard).events[0].pointer.position == (PointI{100, 100}));
}

void test_cocoa_buttons_and_click_counts()
{
    NsViewGeometry view;
    view.height_points = 100.0;

    struct Case
    {
        std::int32_t type;
        PointerAction action;
        MouseButton button;
    };
    const Case cases[] = {
        {kNsLeftMouseDown, PointerAction::down, MouseButton::left},
        {kNsLeftMouseUp, PointerAction::up, MouseButton::left},
        {kNsRightMouseDown, PointerAction::down, MouseButton::right},
        {kNsRightMouseUp, PointerAction::up, MouseButton::right},
    };
    for (const Case& c : cases)
    {
        NsEvent event = ns_event(c.type);
        ShellEventBatch batch = translate_ns_event(event, view);
        CHECK(batch.count == 1u);
        CHECK(batch.events[0].pointer.action == c.action);
        CHECK(batch.events[0].pointer.button == c.button);
    }

    // THE TRAP: the middle button is NSEventTypeOtherMouse* with buttonNumber 2 — left and right
    // have their own event types. X11 numbers its buttons 1/2/3 = left/MIDDLE/right, so a port that
    // carried that ordering across would answer `right` here.
    NsEvent middle = ns_event(kNsOtherMouseDown);
    middle.button_number = kNsOtherButtonMiddle;
    ShellEventBatch batch = translate_ns_event(middle, view);
    CHECK(batch.count == 1u);
    CHECK(batch.events[0].pointer.button == MouseButton::middle);
    CHECK(batch.events[0].pointer.action == PointerAction::down);

    // A thumb (back/forward) button is DROPPED rather than routed as a nameless click — the same
    // choice the X11 decoder makes for buttons 8/9.
    NsEvent thumb = ns_event(kNsOtherMouseDown);
    thumb.button_number = 3;
    CHECK(translate_ns_event(thumb, view).count == 0u);

    // Cocoa COUNTS clicks; there is no separate double-click event type as on Windows. CEF derives
    // double-click-to-select-word SOLELY from click_count, so a shell that never reported 2 would
    // leave it dead in every text field.
    NsEvent double_click = ns_event(kNsLeftMouseDown);
    double_click.click_count = 2;
    CHECK(translate_ns_event(double_click, view).events[0].pointer.click_count == 2);

    // A drag that outlived its press reports clickCount 0; forwarding it tells CEF "zero clicks".
    NsEvent stale = ns_event(kNsLeftMouseDragged);
    stale.click_count = 0;
    CHECK(translate_ns_event(stale, view).events[0].pointer.click_count == 1);
}

void test_cocoa_modifier_mapping_and_pressed_buttons()
{
    NsViewGeometry view;
    view.height_points = 100.0;

    // THE TRAP: Option is ALT and Command is META. Mapping Command onto `control` — which the keycap
    // POSITIONS invite, since Cmd sits where Ctrl does on the other two platforms — turns every
    // Cmd-shortcut in the editor into a Ctrl-shortcut at the browser.
    NsEvent event = ns_event(kNsMouseMoved);
    event.modifier_flags = kNsModifierCommand | kNsModifierOption;
    ShellEventBatch batch = translate_ns_event(event, view);
    CHECK(batch.events[0].pointer.modifiers.meta);
    CHECK(batch.events[0].pointer.modifiers.alt);
    CHECK(!batch.events[0].pointer.modifiers.control);
    CHECK(!batch.events[0].pointer.modifiers.shift);

    NsEvent both = ns_event(kNsMouseMoved);
    both.modifier_flags = kNsModifierControl | kNsModifierShift;
    batch = translate_ns_event(both, view);
    CHECK(batch.events[0].pointer.modifiers.control);
    CHECK(batch.events[0].pointer.modifiers.shift);
    CHECK(!batch.events[0].pointer.modifiers.meta);

    // The button state comes from +[NSEvent pressedMouseButtons], not from the event — a CLASS
    // property, so the backend reads it once per event exactly as the Win32 backend reads the
    // keyboard modifier state. ⚠ Bit 1 is the RIGHT button and bit 2 the MIDDLE one, the inverse of
    // X11's 1/2/3 ordering.
    NsEvent buttons = ns_event(kNsMouseMoved);
    buttons.pressed_mouse_buttons = kNsPressedButtonLeft | kNsPressedButtonRight;
    batch = translate_ns_event(buttons, view);
    CHECK(batch.events[0].pointer.modifiers.left_button_down);
    CHECK(batch.events[0].pointer.modifiers.right_button_down);
    CHECK(!batch.events[0].pointer.modifiers.middle_button_down);

    NsEvent middle_held = ns_event(kNsMouseMoved);
    middle_held.pressed_mouse_buttons = kNsPressedButtonMiddle;
    batch = translate_ns_event(middle_held, view);
    CHECK(batch.events[0].pointer.modifiers.middle_button_down);
    CHECK(!batch.events[0].pointer.modifiers.right_button_down);
}

void test_cocoa_scroll_wheel_units()
{
    NsViewGeometry view = retina_view();

    // A LINE delta is a notch count: one notch becomes kWheelDelta, the ONE convention the Win32 and
    // X11 decoders already hand the browser.
    NsEvent notch = ns_event(kNsScrollWheel);
    notch.scrolling_delta_y = 1.0;
    ShellEventBatch batch = translate_ns_event(notch, view);
    CHECK(batch.count == 1u);
    CHECK(batch.events[0].pointer.action == PointerAction::wheel);
    CHECK(batch.events[0].pointer.wheel_delta_y == kWheelDelta);
    CHECK(batch.events[0].pointer.wheel_delta_x == 0);

    // Sign convention matches both siblings: +Y is away from the user, +X is to the right.
    NsEvent down_notch = ns_event(kNsScrollWheel);
    down_notch.scrolling_delta_y = -1.0;
    CHECK(translate_ns_event(down_notch, view).events[0].pointer.wheel_delta_y == -kWheelDelta);

    NsEvent right_notch = ns_event(kNsScrollWheel);
    right_notch.scrolling_delta_x = 1.0;
    batch = translate_ns_event(right_notch, view);
    CHECK(batch.events[0].pointer.wheel_delta_x == kWheelDelta);
    CHECK(batch.events[0].pointer.wheel_delta_y == 0);

    // THE TRAP: a PRECISE delta (trackpad, high-resolution wheel) is already a distance in POINTS.
    // Multiplying it by 120 as if it were a notch count makes a gentle two-finger swipe jump a
    // hundred screens.
    //
    // AND THE TRAP ON THE OTHER SIDE, which is why this fixture is deliberately at 2x: the delta is
    // NOT scaled to physical pixels either. A wheel delta is the one field on this seam that is not a
    // physical-pixel quantity — both siblings emit a scale-free value and the consumer hands it to
    // the browser unconverted — so scaling it here would silently double every trackpad scroll on a
    // Retina display while a 1x fixture stayed green.
    NsEvent swipe = ns_event(kNsScrollWheel);
    swipe.has_precise_scrolling_deltas = true;
    swipe.scrolling_delta_y = 12.0;
    CHECK(view.dpi.factor() > 1.0f); // the assertion below is only meaningful at a non-unit scale
    CHECK(translate_ns_event(swipe, view).events[0].pointer.wheel_delta_y == 12); // points, as-is

    // Unlike WM_MOUSEWHEEL — whose lParam is SCREEN-relative, so the Win32 decoder reports no
    // position at all — a Cocoa scroll carries an ordinary view-relative location, so the wheel is
    // arbitrated against the region under the pointer with no backend fix-up.
    NsEvent positioned = ns_event(kNsScrollWheel);
    positioned.scrolling_delta_y = 1.0;
    positioned.location_x = 50.0;
    positioned.location_y = 700.0;
    CHECK(translate_ns_event(positioned, view).events[0].pointer.position == (PointI{100, 200}));

    // A momentum tail decays to sub-pixel deltas. Forwarding them is one browser wheel event per
    // frame that scrolls nothing, for as long as the tail runs.
    NsEvent tail = ns_event(kNsScrollWheel);
    tail.has_precise_scrolling_deltas = true;
    tail.scrolling_delta_y = 0.001;
    CHECK(translate_ns_event(tail, view).count == 0u);
}

void test_cocoa_key_press_yields_the_raw_key_and_the_character()
{
    NsViewGeometry view;
    view.height_points = 100.0;

    NsEvent key = ns_event(kNsKeyDown);
    key.key_code = kNsVkAnsiA;
    key.text = U'a';
    ShellEventBatch batch = translate_ns_event(key, view);
    // TWO events from one: Windows splits the character into its own WM_CHAR and Cocoa does not, so
    // the second is synthesized here — the same shape the X11 decoder takes.
    CHECK(batch.count == 2u);
    CHECK(batch.events[0].kind == ShellEventKind::key);
    CHECK(batch.events[0].key.action == KeyAction::raw_key_down);
    CHECK(batch.events[0].key.windows_key_code == 'A');
    CHECK(batch.events[0].key.native_key_code == static_cast<std::int32_t>(kNsVkAnsiA));
    CHECK(batch.events[1].key.action == KeyAction::character);
    CHECK(batch.events[1].key.character == U'a');

    // is_system_key is documented by CEF as Windows-only, so it is never guessed from Command.
    CHECK(!batch.events[0].key.is_system_key);

    NsEvent release = ns_event(kNsKeyUp);
    release.key_code = kNsVkAnsiA;
    release.text = U'a';
    batch = translate_ns_event(release, view);
    CHECK(batch.count == 1u); // a RELEASE never produces a character
    CHECK(batch.events[0].key.action == KeyAction::key_up);

    // A key that produced no text (an arrow, a bare function key) must not emit a character
    // carrying 0.
    NsEvent arrow = ns_event(kNsKeyDown);
    arrow.key_code = kNsVkLeftArrow;
    batch = translate_ns_event(arrow, view);
    CHECK(batch.count == 1u);
    CHECK(batch.events[0].key.windows_key_code == 0x25); // VK_LEFT

    // THE TRAP, AND ITS EXACT COUNTERPART. -[NSEvent characters] answers "s" for Cmd+S, so
    // forwarding it unconditionally TYPES an `s` into the focused field every time the user saves.
    // Windows never had this problem: Ctrl+S produces a WM_CHAR carrying control code 0x13, which no
    // field inserts.
    NsEvent command_s = ns_event(kNsKeyDown);
    command_s.key_code = kNsVkAnsiS;
    command_s.text = U's';
    command_s.modifier_flags = kNsModifierCommand;
    batch = translate_ns_event(command_s, view);
    CHECK(batch.count == 1u);
    CHECK(batch.events[0].key.action == KeyAction::raw_key_down);
    CHECK(batch.events[0].key.modifiers.meta);

    NsEvent control_c = ns_event(kNsKeyDown);
    control_c.key_code = 0x08; // kVK_ANSI_C
    control_c.text = U'c';
    control_c.modifier_flags = kNsModifierControl;
    CHECK(translate_ns_event(control_c, view).count == 1u);

    // ...and OPTION IS THE OPPOSITE CASE. On macOS it is a TEXT modifier — Option+2 types a trademark
    // sign, Option+e begins an accent — so suppressing it would silently disable dead keys and half
    // the symbol set. This is the assertion that stops the fix above from over-reaching.
    NsEvent option_2 = ns_event(kNsKeyDown);
    option_2.key_code = 0x13; // kVK_ANSI_2
    // Spelled as an ESCAPE, never as a literal '™'. This file carries no BOM and the MSVC leg is
    // given no /utf-8, so MSVC reads the source in the system code page and the three UTF-8 bytes of
    // a pasted ™ become three characters — `error C2015: too many characters in constant`, which is
    // a HARD error, not a warning to be waived. Non-ASCII in COMMENTS is fine (the bytes are
    // skipped, as the rest of this file and win32_window.cpp already rely on); only a character
    // literal has to be an escape.
    option_2.text = U'\u2122'; // TRADE MARK SIGN
    option_2.modifier_flags = kNsModifierOption;
    batch = translate_ns_event(option_2, view);
    CHECK(batch.count == 2u);
    CHECK(batch.events[1].key.action == KeyAction::character);
    CHECK(batch.events[1].key.character == U'\u2122');
    CHECK(batch.events[1].key.modifiers.alt);
}

void test_cocoa_flags_changed_recovers_the_direction_by_diffing()
{
    NsViewGeometry view;
    view.height_points = 100.0;

    // THE TRAP: a modifier key produces NO KeyDown/KeyUp on macOS at all. It arrives as
    // NSEventTypeFlagsChanged, which says WHICH key moved but not which WAY — the direction exists
    // only in the difference against the previous mask.
    NsEvent press = ns_event(kNsFlagsChanged);
    press.key_code = kNsVkShift;
    press.modifier_flags = kNsModifierShift;
    view.previous_modifier_flags = 0;
    ShellEventBatch batch = translate_ns_event(press, view);
    CHECK(batch.count == 1u);
    CHECK(batch.events[0].kind == ShellEventKind::key);
    CHECK(batch.events[0].key.action == KeyAction::raw_key_down);
    CHECK(batch.events[0].key.windows_key_code == 0x10); // VK_SHIFT
    CHECK(batch.events[0].key.modifiers.shift);

    NsEvent release = ns_event(kNsFlagsChanged);
    release.key_code = kNsVkShift;
    release.modifier_flags = 0;
    view.previous_modifier_flags = kNsModifierShift;
    batch = translate_ns_event(release, view);
    CHECK(batch.count == 1u);
    CHECK(batch.events[0].key.action == KeyAction::key_up);
    CHECK(!batch.events[0].key.modifiers.shift);

    // No transition of the PAIR: pressing the RIGHT shift while the left is already held leaves the
    // mask unchanged, and Cocoa's plain mask carries no side. Emitting anyway would hand the browser
    // a keydown with no matching keyup — worse than the missing event, because a stuck modifier
    // re-interprets every subsequent keystroke.
    NsEvent right_shift = ns_event(kNsFlagsChanged);
    right_shift.key_code = kNsVkRightShift;
    right_shift.modifier_flags = kNsModifierShift;
    view.previous_modifier_flags = kNsModifierShift;
    CHECK(translate_ns_event(right_shift, view).count == 0u);

    // The right-hand keys still map to the same VK codes when they DO transition.
    NsEvent right_command = ns_event(kNsFlagsChanged);
    right_command.key_code = kNsVkRightCommand;
    right_command.modifier_flags = kNsModifierCommand;
    view.previous_modifier_flags = 0;
    batch = translate_ns_event(right_command, view);
    CHECK(batch.count == 1u);
    CHECK(batch.events[0].key.windows_key_code == 0x5C); // VK_RWIN

    // A key whose flag the Shell does not track is dropped rather than reported as an unnamed key.
    NsEvent unknown = ns_event(kNsFlagsChanged);
    unknown.key_code = kNsVkAnsiA;
    unknown.modifier_flags = kNsModifierShift;
    view.previous_modifier_flags = 0;
    CHECK(translate_ns_event(unknown, view).count == 0u);
}

void test_cocoa_key_code_to_windows_key_code()
{
    // THE TRAP: macOS virtual key codes are POSITIONAL, following the physical ASDF row of the
    // original Apple keyboard. `key_code + 'A'` is wrong for every letter but `A`, and there is no
    // contiguous run to index into — this is a table, and these are the witnesses.
    CHECK(ns_key_code_to_windows_key_code(kNsVkAnsiA) == 'A'); // 0x00
    CHECK(ns_key_code_to_windows_key_code(kNsVkAnsiS) == 'S'); // 0x01, NOT 'B'
    CHECK(ns_key_code_to_windows_key_code(kNsVkAnsiZ) == 'Z'); // 0x06, NOT 'G'
    CHECK(ns_key_code_to_windows_key_code(0x2E) == 'M');
    CHECK(ns_key_code_to_windows_key_code(0x0C) == 'Q');

    // The digit row is TRANSPOSED at 5/6 — 6 is 0x16 and 5 is 0x17.
    CHECK(ns_key_code_to_windows_key_code(kNsVkAnsi6) == '6');
    CHECK(ns_key_code_to_windows_key_code(kNsVkAnsi5) == '5');
    CHECK(ns_key_code_to_windows_key_code(0x12) == '1');
    CHECK(ns_key_code_to_windows_key_code(0x1D) == '0');

    // THE SECOND TRAP: the big key above Return is engraved "delete" and BACKSPACES. Mapping it to
    // VK_DELETE — the reading its NAME invites — makes Backspace delete forwards in every field.
    CHECK(ns_key_code_to_windows_key_code(kNsVkDelete) == 0x08);        // VK_BACK
    CHECK(ns_key_code_to_windows_key_code(kNsVkForwardDelete) == 0x2E); // VK_DELETE

    // Command is META and Option is ALT; neither is VK_CONTROL, however much the keycap positions
    // suggest it.
    CHECK(ns_key_code_to_windows_key_code(kNsVkCommand) == 0x5B);      // VK_LWIN
    CHECK(ns_key_code_to_windows_key_code(kNsVkRightCommand) == 0x5C); // VK_RWIN
    CHECK(ns_key_code_to_windows_key_code(kNsVkOption) == 0x12);       // VK_MENU
    CHECK(ns_key_code_to_windows_key_code(kNsVkRightOption) == 0x12);
    CHECK(ns_key_code_to_windows_key_code(kNsVkControl) == 0x11); // VK_CONTROL
    CHECK(ns_key_code_to_windows_key_code(kNsVkShift) == 0x10);
    CHECK(ns_key_code_to_windows_key_code(kNsVkCapsLock) == 0x14);

    // The function keys are SCATTERED across two ranges — F1 is 0x7A while F5 is 0x60 — so no
    // arithmetic covers them and every one is its own table row.
    CHECK(ns_key_code_to_windows_key_code(kNsVkF1) == 0x70); // VK_F1
    CHECK(ns_key_code_to_windows_key_code(0x78) == 0x71);    // F2
    CHECK(ns_key_code_to_windows_key_code(kNsVkF5) == 0x74); // F5
    CHECK(ns_key_code_to_windows_key_code(0x6F) == 0x7B);    // F12

    // The keypad digits are their OWN VK codes — folding them onto the row digits makes a
    // keypad-specific binding indistinguishable — and they are themselves out of order: 8 is 0x5B
    // and 9 is 0x5C, past F20 at 0x5A.
    CHECK(ns_key_code_to_windows_key_code(kNsVkKeypad0) == 0x60); // VK_NUMPAD0
    CHECK(ns_key_code_to_windows_key_code(0x59) == 0x67);         // VK_NUMPAD7
    CHECK(ns_key_code_to_windows_key_code(0x5B) == 0x68);         // VK_NUMPAD8
    CHECK(ns_key_code_to_windows_key_code(0x5C) == 0x69);         // VK_NUMPAD9
    CHECK(ns_key_code_to_windows_key_code(0x4C) == 0x0D);      // the keypad Enter folds to VK_RETURN

    // Punctuation goes to the VK_OEM_* codes, which are NOT the ASCII values.
    CHECK(ns_key_code_to_windows_key_code(0x29) == 0xBA); // ; -> VK_OEM_1
    CHECK(ns_key_code_to_windows_key_code(0x2B) == 0xBC); // , -> VK_OEM_COMMA
    CHECK(ns_key_code_to_windows_key_code(0x32) == 0xC0); // ` -> VK_OEM_3

    CHECK(ns_key_code_to_windows_key_code(kNsVkReturn) == 0x0D);
    CHECK(ns_key_code_to_windows_key_code(kNsVkTab) == 0x09);
    CHECK(ns_key_code_to_windows_key_code(kNsVkSpace) == 0x20);
    CHECK(ns_key_code_to_windows_key_code(kNsVkEscape) == 0x1B);
    CHECK(ns_key_code_to_windows_key_code(kNsVkUpArrow) == 0x26);
    CHECK(ns_key_code_to_windows_key_code(kNsVkDownArrow) == 0x28);
    CHECK(ns_key_code_to_windows_key_code(kNsVkRightArrow) == 0x27);

    // An honest 0 for a key with no VK equivalent. A guess here fires the WRONG command, which is
    // strictly worse than firing none.
    CHECK(ns_key_code_to_windows_key_code(0x5D) == 0); // kVK_JIS_Yen
    CHECK(ns_key_code_to_windows_key_code(0x4A) == 0); // kVK_Mute
    CHECK(ns_key_code_to_windows_key_code(kNsVkFunction) == 0);
    CHECK(ns_key_code_to_windows_key_code(0xFFFF) == 0);
}

void test_cocoa_pointer_leave_and_undecoded_events()
{
    NsViewGeometry view;
    view.height_points = 100.0;

    // CEF needs the explicit mouse-leave or it keeps a control hover-highlighted forever.
    NsEvent exited = ns_event(kNsMouseExited);
    exited.modifier_flags = kNsModifierShift;
    ShellEventBatch batch = translate_ns_event(exited, view);
    CHECK(batch.count == 1u);
    CHECK(batch.events[0].pointer.action == PointerAction::leave);
    CHECK(batch.events[0].pointer.modifiers.shift);

    // MouseEntered carries no fact the Shell acts on — the pointer's arrival is already described by
    // the move that follows it. Mirrors the X11 decoder's treatment of EnterNotify.
    CHECK(translate_ns_event(ns_event(kNsMouseEntered), view).count == 0u);
    // Periodic / app-defined / tablet families.
    CHECK(translate_ns_event(ns_event(16), view).count == 0u);
    CHECK(translate_ns_event(ns_event(0), view).count == 0u);
}

void test_cocoa_backing_scale_to_dpi()
{
    CHECK(ns_dpi_from_backing_scale(1.0).dpi == kReferenceDpi);
    CHECK(ns_dpi_from_backing_scale(2.0).dpi == 192u);
    CHECK(ns_dpi_from_backing_scale(3.0).dpi == 288u);
    CHECK(shelltest::near_eq(ns_dpi_from_backing_scale(2.0).factor(), 2.0f));

    // A window not yet on a screen reports 0 and a torn-down one can report NaN. Neither is a scale,
    // and multiplying the whole layout by either is how a UI collapses to nothing.
    CHECK(ns_dpi_from_backing_scale(0.0).dpi == kReferenceDpi);
    CHECK(ns_dpi_from_backing_scale(-2.0).dpi == kReferenceDpi);
    CHECK(ns_dpi_from_backing_scale(std::numeric_limits<double>::quiet_NaN()).dpi == kReferenceDpi);
    CHECK(ns_dpi_from_backing_scale(std::numeric_limits<double>::infinity()).dpi == kReferenceDpi);

    // TWO REGIMES, and this pair is the boundary between them — found by writing the assertion the
    // other way round first and watching it fail. A scale that is merely IMPLAUSIBLE still names a
    // DPI, so make_dpi_scale's clamp applies to it...
    CHECK(ns_dpi_from_backing_scale(100.0).dpi == kMaxDpi);
    CHECK(ns_dpi_from_backing_scale(0.4).dpi == kMinDpi);
    // ...whereas a scale that rounds to ZERO dpi names no display at all. That is the same class as
    // the NaN and the 0 above and is REFUSED to 1x, not clamped: clamping would report a 0.5x
    // display that does not exist, where refusing reports "this is not a scale".
    CHECK(ns_dpi_from_backing_scale(0.001).dpi == kReferenceDpi);
}

void test_cocoa_window_geometry_reports_only_what_changed()
{
    NsWindowGeometry previous;
    previous.width_points = 1280.0;
    previous.height_points = 800.0;
    previous.x = 100;
    previous.y = 50;
    previous.dpi = ns_dpi_from_backing_scale(1.0);

    // A pure MOVE. AppKit re-delivers geometry on notifications that changed nothing else, and
    // reporting an unchanged size as a resize would reconfigure the swapchain on each one.
    NsWindowGeometry moved = previous;
    moved.x = 140;
    moved.y = 70;
    ShellEventBatch batch = translate_ns_window_geometry(previous, moved);
    CHECK(batch.count == 1u);
    CHECK(batch.events[0].kind == ShellEventKind::moved);
    CHECK(batch.events[0].position == (PointI{140, 70}));

    // A pure RESIZE, reported in PHYSICAL pixels — the space the swapchain is configured in.
    NsWindowGeometry resized = previous;
    resized.width_points = 1600.0;
    batch = translate_ns_window_geometry(previous, resized);
    CHECK(batch.count == 1u);
    CHECK(batch.events[0].kind == ShellEventKind::resize);
    CHECK(shelltest::extent_eq(batch.events[0].size, render::Extent2D{1600, 800}));

    // Nothing changed at all: the notification still arrives; the Shell must not act on it.
    CHECK(translate_ns_window_geometry(previous, previous).count == 0u);

    // THE TRAP THIS COMPARISON EXISTS FOR: a window DRAGGED ONTO A RETINA DISPLAY keeps its POINT
    // size and DOUBLES its backbuffer. A points-only comparison reports no resize on exactly the
    // transition that needs one most, leaving the swapchain half the size of the window.
    NsWindowGeometry retina = previous;
    retina.dpi = ns_dpi_from_backing_scale(2.0);
    batch = translate_ns_window_geometry(previous, retina);
    CHECK(batch.count == 2u);
    CHECK(batch.events[0].kind == ShellEventKind::resize);
    CHECK(shelltest::extent_eq(batch.events[0].size, render::Extent2D{2560, 1600}));
    CHECK(batch.events[1].kind == ShellEventKind::dpi_changed);
    CHECK(batch.events[1].dpi == ns_dpi_from_backing_scale(2.0));

    // All THREE at once — the window dragged to a differently-scaled monitor, which is what the
    // batch's three-slot capacity is sized for.
    NsWindowGeometry everything = retina;
    everything.x = 0;
    everything.y = 0;
    batch = translate_ns_window_geometry(previous, everything);
    CHECK(batch.count == 3u);
    CHECK(batch.events[0].kind == ShellEventKind::resize);
    CHECK(batch.events[1].kind == ShellEventKind::moved);
    CHECK(batch.events[2].kind == ShellEventKind::dpi_changed);
    // THE CAPACITY IS NOW EXACTLY SATURATED, so this is the assertion that keeps the NEXT decoder
    // fact from silently vanishing. `push` drops past kCapacity, which is the right behaviour on the
    // pump's hot path, but it must never happen unobserved — and this is the one input in the whole
    // suite that fills the batch.
    CHECK(!batch.overflowed);

    // A miniaturized window reports a 0x0 content size, exactly as WM_SIZE's minimize carve-out
    // does; the resize is dropped while a move still comes through.
    NsWindowGeometry collapsed = previous;
    collapsed.width_points = 0.0;
    collapsed.height_points = 0.0;
    CHECK(translate_ns_window_geometry(previous, collapsed).count == 0u);
}

// The `!overflowed` assertion above is only worth anything if the flag can actually be set — a flag
// that never fires is the vacuous-gate failure in miniature. This is the control that proves it.
void test_shell_event_batch_reports_overflow_rather_than_dropping_silently()
{
    ShellEventBatch batch;
    ShellEvent event;
    event.kind = ShellEventKind::resize;

    for (std::size_t i = 0; i < ShellEventBatch::kCapacity; ++i)
    {
        batch.push(event);
    }
    CHECK(batch.count == ShellEventBatch::kCapacity);
    CHECK(!batch.overflowed); // filling it exactly is not an overflow

    batch.push(event);
    CHECK(batch.overflowed);
    CHECK(batch.count == ShellEventBatch::kCapacity); // the tail is dropped, never a torn batch
}

} // namespace

int main()
{
    test_resize_decoding_and_the_minimize_carve_out();
    test_mouse_coordinates_are_signed();
    test_button_messages_map_to_actions_and_buttons();
    test_mouse_button_state_comes_from_wparam_and_modifier_keys_from_the_os();
    test_wheel_carries_a_signed_delta_and_deliberately_no_position();
    test_horizontal_wheel_decodes_onto_the_x_axis();
    test_double_click_messages_decode_as_a_press_with_click_count_two();
    test_key_and_char_decoding();
    test_dpi_change_reads_the_low_word();
    test_lifecycle_and_focus_messages();
    test_resize_border_and_corner_scale_with_dpi();
    test_frameless_client_insets_restored_and_maximized();
    test_maximized_client_lands_exactly_on_the_work_area_at_96_and_150_percent();
    test_hit_test_frame_resize_bands_and_corners();
    test_hit_test_frame_regions_and_precedence();
    test_hit_test_frame_scales_bands_with_dpi();
    test_hit_test_frame_maximized_has_no_resize_bands();
    test_hit_test_frame_sweep_corpus_matches_the_spec_oracle_at_every_point();
    test_nc_mouse_forwards_controls_and_leaves_the_caption_to_the_os();
    test_nc_mouse_synthesizes_the_leave_that_prevents_a_stuck_hover();
    test_nc_mouse_releases_a_forwarded_press_wherever_it_lands();
    test_headless_backend_reports_no_native_window_by_default();
    test_headless_backend_applies_state_before_delivering_events();
    test_headless_backend_close_ends_the_pump();
    test_headless_backend_records_placement_and_redraws();
    test_headless_backend_chrome_verbs_are_honest_state_only();
    test_headless_backend_records_the_pushed_down_chrome_facts();
    test_platform_backend_selection_is_never_silent();
    test_platform_window_factories_refuse_off_their_platform();
    test_configure_notify_reports_only_what_actually_changed();
    test_wheel_is_a_button_pair_and_only_the_press_counts();
    test_x11_button_numbers_are_left_middle_right();
    test_button_state_is_the_mask_before_the_event();
    test_grab_synthesized_crossing_and_focus_events_are_ignored();
    test_expose_repaints_once_per_run_and_delete_closes();
    test_key_press_yields_the_raw_key_and_only_real_text();
    test_keysym_to_windows_key_code();
    test_x11_dpi_sources();
    test_cocoa_y_axis_is_flipped_and_points_are_scaled();
    test_cocoa_buttons_and_click_counts();
    test_cocoa_modifier_mapping_and_pressed_buttons();
    test_cocoa_scroll_wheel_units();
    test_cocoa_key_press_yields_the_raw_key_and_the_character();
    test_cocoa_flags_changed_recovers_the_direction_by_diffing();
    test_cocoa_key_code_to_windows_key_code();
    test_cocoa_pointer_leave_and_undecoded_events();
    test_cocoa_backing_scale_to_dpi();
    test_cocoa_window_geometry_reports_only_what_changed();
    test_shell_event_batch_reports_overflow_rather_than_dropping_silently();
    SHELL_TEST_MAIN_END();
}
