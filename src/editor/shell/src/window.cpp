// The portable half of the window seam: the headless backend, the pure Win32, X11 and Cocoa event
// decoders, and the platform CHOICE (make_window_backend). See window.h for why the decoding lives
// here rather than inside win32_window.cpp / x11_window.cpp / cocoa_window.mm.
//
// make_window_backend moved here in e12a. It used to live in win32_window.cpp, whose `#else` branch
// was the honest "no backend on this platform yet" report for BOTH other OSes. With a second real
// backend that shape stops working: the choice belongs to neither platform file, and leaving it in
// one of them would mean the Linux build's selection logic sat inside a file that is entirely
// `#if defined(_WIN32)`. e12b added the third arm, and with it the last of that `#else`.

#include "context/editor/shell/window.h"

#include <algorithm>
#include <cmath>

namespace context::editor::shell
{
namespace
{

// The low/high halves of LPARAM as SIGNED 16-bit values. Mouse coordinates go negative the moment a
// captured drag leaves the client area on the left or top edge, and reading them unsigned turns -36
// into 65500 — a position that lands outside every region and silently re-routes the drag.
[[nodiscard]] std::int32_t signed_low_word(std::int64_t lparam)
{
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(lparam & 0xFFFF));
}

[[nodiscard]] std::int32_t signed_high_word(std::int64_t lparam)
{
    return static_cast<std::int16_t>(static_cast<std::uint16_t>((lparam >> 16) & 0xFFFF));
}

[[nodiscard]] std::uint32_t low_word(std::uint64_t value)
{
    return static_cast<std::uint32_t>(value & 0xFFFF);
}

// WM_SIZE / WM_MOVE pack two UNSIGNED extents; WM_MOVE's are signed screen coordinates.
[[nodiscard]] render::Extent2D extent_from_lparam(std::int64_t lparam)
{
    return render::Extent2D{static_cast<std::uint32_t>(lparam & 0xFFFF),
                            static_cast<std::uint32_t>((lparam >> 16) & 0xFFFF)};
}

// The MK_* button bits WM_MOUSE* messages carry in wParam.
inline constexpr std::uint64_t kMkLButton = 0x0001;
inline constexpr std::uint64_t kMkRButton = 0x0002;
inline constexpr std::uint64_t kMkMButton = 0x0010;

[[nodiscard]] Modifiers make_modifiers(std::uint64_t wparam, const Win32ModifierState& keys)
{
    Modifiers modifiers;
    modifiers.shift = keys.shift;
    modifiers.control = keys.control;
    modifiers.alt = keys.alt;
    modifiers.meta = keys.meta;
    modifiers.left_button_down = (wparam & kMkLButton) != 0;
    modifiers.right_button_down = (wparam & kMkRButton) != 0;
    modifiers.middle_button_down = (wparam & kMkMButton) != 0;
    return modifiers;
}

// The keyboard messages carry no button bits, so a button state is not inferable from wParam there.
// Kept as a named function rather than inlined at each call: the NAME is the reason a zero wparam is
// correct here, and it stops a reader wondering whether the button bits were simply forgotten.
[[nodiscard]] Modifiers make_key_modifiers(const Win32ModifierState& keys)
{
    return make_modifiers(0, keys);
}

// The messages that carry NOTHING but their kind — no pointer position, no key, no DPI. Named for
// the same reason as the make_* helpers below, so the decoder's switch reads uniformly instead of
// alternating between one-line arms and four-line blocks that all say the same thing.
[[nodiscard]] ShellEvent simple_event(ShellEventKind kind)
{
    ShellEvent event;
    event.kind = kind;
    return event;
}

[[nodiscard]] ShellEvent make_key_event(KeyAction action, const Win32Message& message,
                                        const Win32ModifierState& keys, bool is_system_key)
{
    ShellEvent event;
    event.kind = ShellEventKind::key;
    event.key.action = action;
    event.key.windows_key_code = static_cast<std::int32_t>(low_word(message.wparam));
    event.key.native_key_code = static_cast<std::int32_t>(message.lparam & 0xFFFFFFFF);
    event.key.modifiers = make_key_modifiers(keys);
    event.key.is_system_key = is_system_key;
    return event;
}

[[nodiscard]] ShellEvent make_pointer_event(PointerAction action, MouseButton button,
                                            const Win32Message& message,
                                            const Win32ModifierState& keys, int click_count = 1)
{
    ShellEvent event;
    event.kind = ShellEventKind::pointer;
    event.pointer.action = action;
    event.pointer.button = button;
    event.pointer.click_count = click_count;
    event.pointer.position =
        PointI{signed_low_word(message.lparam), signed_high_word(message.lparam)};
    event.pointer.modifiers = make_modifiers(message.wparam, keys);
    return event;
}

// The pointer-leave shape, shared by the kWmMouseLeave decode and the b1 NC-synthesized leave —
// ONE constructor, so the browser cannot tell a "left the NC controls" from a "left the client
// area" by construction rather than by hand-synced copies.
[[nodiscard]] ShellEvent make_leave_event(const Win32ModifierState& keys)
{
    ShellEvent event;
    event.kind = ShellEventKind::pointer;
    event.pointer.action = PointerAction::leave;
    event.pointer.modifiers = make_key_modifiers(keys);
    return event;
}

} // namespace

// ------------------------------------------------------------------------------- IWindowBackend

// The b1 chrome-fact defaults: ignoring a pushed-down fact is the truthful behaviour for a backend
// whose platform does not consume it (window.h § the two chrome FACTS) — X11 permanently (D6: the
// WM owns the frame), cocoa until c1. win32 and headless override both.
void IWindowBackend::set_chrome_regions(const std::vector<ShellRegion>& /*regions*/) {}
void IWindowBackend::set_appearance(bool /*dark*/) {}

// ------------------------------------------------------------------- HeadlessWindowBackend

HeadlessWindowBackend::HeadlessWindowBackend(const WindowDesc& desc) : title_(desc.title)
{
    dpi_ = DpiScale{};
    if (desc.placement.has_value())
    {
        placement_ = *desc.placement;
        size_ = placement_.size();
    }
    else
    {
        size_ = to_physical(desc.logical_size, dpi_);
        placement_.width = size_.width;
        placement_.height = size_.height;
    }
}

bool HeadlessWindowBackend::pump(std::vector<ShellEvent>& out)
{
    for (const ShellEvent& event : queued_)
    {
        // The backend applies the state-changing events to ITSELF before handing them on, exactly
        // as a real OS window does: by the time a resize is observed, client_size() already reports
        // the new size. A backend that reported the old size until the caller wrote it back would
        // make every consumer responsible for a synchronization the OS performs for free.
        if (event.kind == ShellEventKind::resize)
        {
            size_ = event.size;
            placement_.width = event.size.width;
            placement_.height = event.size.height;
        }
        else if (event.kind == ShellEventKind::dpi_changed)
        {
            dpi_ = event.dpi;
        }
        else if (event.kind == ShellEventKind::moved)
        {
            placement_.x = event.position.x;
            placement_.y = event.position.y;
        }
        else if (event.kind == ShellEventKind::close_requested)
        {
            alive_ = false;
        }
        out.push_back(event);
    }
    queued_.clear();
    return alive_;
}

void HeadlessWindowBackend::request_redraw()
{
    ++redraw_requests_;
}

void HeadlessWindowBackend::apply_placement(const WindowPlacement& placement)
{
    placement_ = placement;
    size_ = placement.size();
}

void HeadlessWindowBackend::post(const ShellEvent& event)
{
    queued_.push_back(event);
}

// ------------------------------------------------------------------ translate_win32_message

std::optional<ShellEvent> translate_win32_message(const Win32Message& message,
                                                  const Win32ModifierState& keys)
{
    switch (message.message)
    {
    case kWmSize:
    {
        if (message.wparam == kSizeMinimized)
        {
            // See kSizeMinimized: a minimized window's 0x0 client size is not a resize.
            return std::nullopt;
        }
        ShellEvent event;
        event.kind = ShellEventKind::resize;
        event.size = extent_from_lparam(message.lparam);
        if (render::is_empty(event.size))
        {
            return std::nullopt;
        }
        return event;
    }
    case kWmMove:
    {
        ShellEvent event;
        event.kind = ShellEventKind::moved;
        event.position = PointI{signed_low_word(message.lparam), signed_high_word(message.lparam)};
        return event;
    }
    case kWmDpiChanged:
    {
        ShellEvent event;
        event.kind = ShellEventKind::dpi_changed;
        // The DPI is in the LOW word of wParam (X and Y are reported separately and are always
        // equal on Windows); make_dpi_scale clamps a nonsense value rather than trusting it.
        event.dpi = make_dpi_scale(low_word(message.wparam));
        return event;
    }
    case kWmPaint:
        return simple_event(ShellEventKind::paint_requested);
    case kWmSetFocus:
        return simple_event(ShellEventKind::focus_gained);
    case kWmKillFocus:
        return simple_event(ShellEventKind::focus_lost);
    case kWmClose:
    case kWmDestroy:
        return simple_event(ShellEventKind::close_requested);
    case kWmMouseMove:
        return make_pointer_event(PointerAction::move, MouseButton::none, message, keys);
    case kWmLButtonDown:
        return make_pointer_event(PointerAction::down, MouseButton::left, message, keys);
    case kWmLButtonUp:
        return make_pointer_event(PointerAction::up, MouseButton::left, message, keys);
    // A double click arrives as its own message (the window class sets CS_DBLCLKS), NOT as a second
    // WM_*BUTTONDOWN. Decoding it as a press carrying click_count=2 is what makes Chromium's
    // double-click-to-select-word work: CEF derives that behaviour SOLELY from click_count, so a
    // shell that never reports 2 leaves it dead in every text field.
    case kWmLButtonDblClk:
        return make_pointer_event(PointerAction::down, MouseButton::left, message, keys, 2);
    case kWmRButtonDown:
        return make_pointer_event(PointerAction::down, MouseButton::right, message, keys);
    case kWmRButtonUp:
        return make_pointer_event(PointerAction::up, MouseButton::right, message, keys);
    case kWmRButtonDblClk:
        return make_pointer_event(PointerAction::down, MouseButton::right, message, keys, 2);
    case kWmMButtonDown:
        return make_pointer_event(PointerAction::down, MouseButton::middle, message, keys);
    case kWmMButtonUp:
        return make_pointer_event(PointerAction::up, MouseButton::middle, message, keys);
    case kWmMButtonDblClk:
        return make_pointer_event(PointerAction::down, MouseButton::middle, message, keys, 2);
    case kWmMouseLeave:
        return make_leave_event(keys);
    case kWmMouseWheel:
    case kWmMouseHWheel:
    {
        ShellEvent event;
        event.kind = ShellEventKind::pointer;
        event.pointer.action = PointerAction::wheel;
        // NO position: WM_MOUSEWHEEL's lParam is SCREEN-relative (see the header note). The backend
        // fills in the last known CLIENT position, which is the coordinate space every other
        // pointer message — and the region map — is expressed in. WM_MOUSEHWHEEL is the same shape
        // on the other axis; without it a horizontal wheel or a tilt-wheel scrolls nothing, while
        // wheel_delta_x is forwarded to CEF as a permanent 0.
        const std::int32_t delta = signed_high_word(static_cast<std::int64_t>(message.wparam));
        if (message.message == kWmMouseHWheel)
        {
            event.pointer.wheel_delta_x = delta;
        }
        else
        {
            event.pointer.wheel_delta_y = delta;
        }
        event.pointer.modifiers = make_modifiers(message.wparam, keys);
        return event;
    }
    case kWmKeyDown:
    case kWmSysKeyDown:
        return make_key_event(KeyAction::raw_key_down, message, keys,
                              message.message == kWmSysKeyDown);
    case kWmKeyUp:
    case kWmSysKeyUp:
        return make_key_event(KeyAction::key_up, message, keys, message.message == kWmSysKeyUp);
    case kWmChar:
    case kWmSysChar:
    {
        ShellEvent event =
            make_key_event(KeyAction::character, message, keys, message.message == kWmSysChar);
        // The one field the other arms have no analogue for: WM_CHAR's wParam IS the character (a
        // UTF-16 code unit on Windows, which is what CEF wants), not a virtual-key code.
        event.key.character = static_cast<char32_t>(low_word(message.wparam));
        return event;
    }
    default:
        return std::nullopt;
    }
}

// ------------------------------------------------------------- Win32 frameless frame (b1, pure)

namespace
{

// A 96-dpi metric scaled to `dpi` through the Shell's ONE rounding policy (dpi.cpp's
// round-to-nearest, via to_physical_point) — so a frame border can never round differently from
// any other DPI-scaled quantity in the Shell, pointer conversion included. Exact by construction
// for these metrics: every halfway case of 8/16 over the clamped DPI range is exactly
// representable in the float path, so this equals the plain-integer form digit for digit.
[[nodiscard]] std::int32_t scale_frame_metric(std::int32_t at_96, DpiScale dpi)
{
    return to_physical_point(PointI{at_96, 0}, dpi).x;
}

// SM_CXSIZEFRAME + SM_CXPADDEDBORDER at the 96-dpi reference (4 + 4). One constant, deliberately,
// because both addends scale identically and every consumer wants their SUM.
inline constexpr std::int32_t kResizeBorder96 = 8;
inline constexpr std::int32_t kResizeCorner96 = 16;

[[nodiscard]] bool is_caption_control_hit(std::int32_t hit_code)
{
    return hit_code == kHtMinButton || hit_code == kHtMaxButton || hit_code == kHtClose;
}

// A forwarded NC pointer event. Mirrors make_pointer_event, except the position arrives already
// client-converted (an NC lParam is SCREEN-relative) and the button state cannot be read from a
// wParam that carries a hit-test code instead of MK_* bits — so it is stated explicitly.
[[nodiscard]] ShellEvent make_nc_pointer_event(PointerAction action, PointI position,
                                               const Win32ModifierState& keys, bool left_down,
                                               int click_count = 1)
{
    ShellEvent event;
    event.kind = ShellEventKind::pointer;
    event.pointer.action = action;
    event.pointer.button =
        action == PointerAction::move ? MouseButton::none : MouseButton::left;
    event.pointer.click_count = click_count;
    event.pointer.position = position;
    event.pointer.modifiers = make_key_modifiers(keys);
    event.pointer.modifiers.left_button_down = left_down;
    return event;
}

} // namespace

std::int32_t win32_resize_border_thickness(DpiScale dpi)
{
    return scale_frame_metric(kResizeBorder96, dpi);
}

std::int32_t win32_resize_corner_extent(DpiScale dpi)
{
    return scale_frame_metric(kResizeCorner96, dpi);
}

Win32FrameInsets win32_frameless_client_insets(bool maximized, DpiScale dpi)
{
    const std::int32_t border = win32_resize_border_thickness(dpi);
    Win32FrameInsets insets;
    insets.left = border;
    // Restored: top stays 0 — the client reaches the window top and the web titlebar draws there.
    // Maximized: the window rect is the work area inflated by `border` on ALL sides (see
    // win32_frameless_max_geometry), so insetting all sides is what lands the client exactly on
    // the work area instead of hanging the classic 8px off every monitor edge (ROADMAP risk 2).
    insets.top = maximized ? border : 0;
    insets.right = border;
    insets.bottom = border;
    return insets;
}

Win32MaxGeometry win32_frameless_max_geometry(PointI work_origin_in_monitor,
                                              render::Extent2D work_size, DpiScale dpi)
{
    const std::int32_t border = win32_resize_border_thickness(dpi);
    Win32MaxGeometry geometry;
    geometry.position =
        PointI{work_origin_in_monitor.x - border, work_origin_in_monitor.y - border};
    geometry.size =
        render::Extent2D{work_size.width + 2u * static_cast<std::uint32_t>(border),
                         work_size.height + 2u * static_cast<std::uint32_t>(border)};
    return geometry;
}

std::int32_t hit_test_frame(PointI point, render::Extent2D client_size, DpiScale dpi,
                            bool maximized, const RegionMap& regions)
{
    const std::int32_t width = static_cast<std::int32_t>(client_size.width);
    const std::int32_t height = static_cast<std::int32_t>(client_size.height);

    // Resize bands FIRST — but only restored: a maximized window cannot be edge-resized, and
    // keeping bands there would steal the top rows of the caption for a grip that does nothing.
    // The bands are the l/r/b NC strips OUTSIDE the client rect the insets carved, plus the top
    // band: the first `border` rows INSIDE the client (there is no top NC strip to put it in —
    // the top inset is zero), which win over the caption AND the controls, the same priority a
    // stock titlebar's top edge has. Within a band, a point within `corner` of either end
    // resolves to the diagonal — ONE statement of the corner rule for all four strips, sound
    // because band membership already implies the matching zone (x < 0 lands in the left `corner`,
    // y >= height in the bottom one, ...) for any client taller and wider than `corner`, which
    // the OS minimum track size guarantees.
    if (!maximized && (point.y >= height || point.x < 0 || point.x >= width ||
                       point.y < win32_resize_border_thickness(dpi)))
    {
        const std::int32_t corner = win32_resize_corner_extent(dpi);
        const bool near_left = point.x < corner;
        const bool near_right = point.x >= width - corner;
        if (point.y < corner)
        {
            return near_left ? kHtTopLeft : near_right ? kHtTopRight : kHtTop;
        }
        if (point.y >= height - corner)
        {
            return near_left ? kHtBottomLeft : near_right ? kHtBottomRight : kHtBottom;
        }
        return near_left ? kHtLeft : kHtRight;
    }

    // Then the published chrome regions, by the map's own back-to-front last-match-wins — a control
    // published after the caption wins over it with no carve-out token (input.h). Exhaustive over
    // RegionKind with no default arm, so -Wswitch makes a future kind's frame answer a deliberate
    // choice here (its InputArbiter routing gets the same treatment in input.cpp's target_for).
    if (const ShellRegion* hit = regions.hit_test(point))
    {
        switch (hit->kind)
        {
        case RegionKind::caption:
            return kHtCaption;
        case RegionKind::caption_min:
            return kHtMinButton;
        case RegionKind::caption_max:
            return kHtMaxButton; // Snap Layouts' trigger on Windows 11
        case RegionKind::caption_close:
            return kHtClose;
        case RegionKind::viewport:
        case RegionKind::native:
            // Client content: its routing is the InputArbiter's concern, not the OS frame's.
            break;
        }
    }
    return kHtClient;
}

Win32NcMouseDecision translate_win32_nc_mouse(std::uint32_t message, std::int32_t hit_code,
                                              PointI client_position,
                                              const Win32ModifierState& keys, bool hover_live,
                                              bool pressed_live)
{
    Win32NcMouseDecision decision;
    decision.hover = hover_live;
    decision.pressed = pressed_live;
    if (message != kWmNcMouseMove && message != kWmNcLButtonDown &&
        message != kWmNcLButtonDblClk && message != kWmNcLButtonUp && message != kWmNcMouseLeave)
    {
        return decision; // not the NC mouse family's: nothing forwarded, nothing consumed
    }

    // The hover/leave choreography, stated ONCE for the whole family: after any family message the
    // forwarded-hover state is simply "is the pointer over a control NOW" (kWmNcMouseLeave's
    // caller pins its hit to kHtNowhere, so a leave always clears it), and sliding off the
    // controls — onto the caption, the bands, or off the window — synthesizes the leave a
    // client-area exit would have produced, or the last hovered button stays lit (ROADMAP risk
    // 3). A case below only ever ADDS its own forwarded event on top; the up overwrites the leave
    // slot deliberately — "release wherever it lands" supersedes the goodbye.
    const bool control = is_caption_control_hit(hit_code);
    decision.hover = control;
    if (hover_live && !control)
    {
        decision.event = make_leave_event(keys);
    }

    switch (message)
    {
    case kWmNcMouseMove:
        if (control)
        {
            // Forward so CSS hover lights; never consumed — DefWindowProc's own NC move handling
            // (the Snap Layouts flyout timing rides it) keeps running.
            decision.event =
                make_nc_pointer_event(PointerAction::move, client_position, keys, pressed_live);
        }
        break;
    case kWmNcLButtonDown:
    case kWmNcLButtonDblClk:
        if (control)
        {
            // A double NC click on a control is a second PRESS (click_count 2, the CS_DBLCLKS
            // decode's convention) — consumed either way, so DefWindowProc's classic caption-button
            // tracking can never fire the system command a web button is about to dispatch.
            decision.event = make_nc_pointer_event(PointerAction::down, client_position, keys, true,
                                                   message == kWmNcLButtonDblClk ? 2 : 1);
            decision.pressed = true;
            decision.consume = true;
        }
        break;
    case kWmNcLButtonUp:
        if (pressed_live)
        {
            // Release a forwarded press WHEREVER it lands — a browser holding a phantom pressed
            // button would drag-select from the next hover. Consumed: the press never reached
            // DefWindowProc, so the release is not its to interpret either. (Without a live
            // forwarded press there is nothing to close — a release whose press predates the
            // region publish, or was the OS's to track, forwards nothing; the hoisted rule above
            // still records where the pointer is.)
            decision.event = make_nc_pointer_event(PointerAction::up, client_position, keys, false);
            decision.pressed = false;
            decision.consume = true;
        }
        break;
    default: // kWmNcMouseLeave: the hoisted choreography above is the whole behaviour
        break;
    }
    return decision;
}

// ---------------------------------------------------------------------------- X11 event decoding

namespace
{

// X11 keysym values (X11/keysymdef.h). Declared locally for the same reason the WM_* values are:
// the decoder must compile on a host with no X headers at all.
inline constexpr std::uint32_t kXkBackSpace = 0xff08;
inline constexpr std::uint32_t kXkTab = 0xff09;
inline constexpr std::uint32_t kXkReturn = 0xff0d;
inline constexpr std::uint32_t kXkPause = 0xff13;
inline constexpr std::uint32_t kXkScrollLock = 0xff14;
inline constexpr std::uint32_t kXkEscape = 0xff1b;
inline constexpr std::uint32_t kXkHome = 0xff50;
inline constexpr std::uint32_t kXkLeft = 0xff51;
inline constexpr std::uint32_t kXkUp = 0xff52;
inline constexpr std::uint32_t kXkRight = 0xff53;
inline constexpr std::uint32_t kXkDown = 0xff54;
inline constexpr std::uint32_t kXkPageUp = 0xff55;
inline constexpr std::uint32_t kXkPageDown = 0xff56;
inline constexpr std::uint32_t kXkEnd = 0xff57;
inline constexpr std::uint32_t kXkPrint = 0xff61;
inline constexpr std::uint32_t kXkInsert = 0xff63;
inline constexpr std::uint32_t kXkMenu = 0xff67;
inline constexpr std::uint32_t kXkNumLock = 0xff7f;
inline constexpr std::uint32_t kXkKpEnter = 0xff8d;
inline constexpr std::uint32_t kXkKpMultiply = 0xffaa;
inline constexpr std::uint32_t kXkKpAdd = 0xffab;
inline constexpr std::uint32_t kXkKpSubtract = 0xffad;
inline constexpr std::uint32_t kXkKpDecimal = 0xffae;
inline constexpr std::uint32_t kXkKpDivide = 0xffaf;
inline constexpr std::uint32_t kXkKp0 = 0xffb0;
inline constexpr std::uint32_t kXkKp9 = 0xffb9;
inline constexpr std::uint32_t kXkF1 = 0xffbe;
inline constexpr std::uint32_t kXkF24 = 0xffd5;
inline constexpr std::uint32_t kXkShiftL = 0xffe1;
inline constexpr std::uint32_t kXkShiftR = 0xffe2;
inline constexpr std::uint32_t kXkControlL = 0xffe3;
inline constexpr std::uint32_t kXkControlR = 0xffe4;
inline constexpr std::uint32_t kXkCapsLock = 0xffe5;
inline constexpr std::uint32_t kXkAltL = 0xffe9;
inline constexpr std::uint32_t kXkAltR = 0xffea;
inline constexpr std::uint32_t kXkSuperL = 0xffeb;
inline constexpr std::uint32_t kXkSuperR = 0xffec;
inline constexpr std::uint32_t kXkDelete = 0xffff;

// The Windows virtual-key codes the map above resolves to. Named rather than spelled numerically at
// each use, because a bare 0xBA in a punctuation table is unreviewable.
inline constexpr std::int32_t kVkBack = 0x08;
inline constexpr std::int32_t kVkTab = 0x09;
inline constexpr std::int32_t kVkReturn = 0x0D;
inline constexpr std::int32_t kVkShift = 0x10;
inline constexpr std::int32_t kVkControl = 0x11;
inline constexpr std::int32_t kVkMenu = 0x12; // Alt
inline constexpr std::int32_t kVkPause = 0x13;
inline constexpr std::int32_t kVkCapital = 0x14;
inline constexpr std::int32_t kVkEscape = 0x1B;
inline constexpr std::int32_t kVkSpace = 0x20;
inline constexpr std::int32_t kVkPageUp = 0x21;
inline constexpr std::int32_t kVkPageDown = 0x22;
inline constexpr std::int32_t kVkEnd = 0x23;
inline constexpr std::int32_t kVkHome = 0x24;
inline constexpr std::int32_t kVkLeft = 0x25;
inline constexpr std::int32_t kVkUp = 0x26;
inline constexpr std::int32_t kVkRight = 0x27;
inline constexpr std::int32_t kVkDown = 0x28;
inline constexpr std::int32_t kVkSnapshot = 0x2C;
inline constexpr std::int32_t kVkInsert = 0x2D;
inline constexpr std::int32_t kVkDelete = 0x2E;
inline constexpr std::int32_t kVkLwin = 0x5B;
inline constexpr std::int32_t kVkRwin = 0x5C;
inline constexpr std::int32_t kVkApps = 0x5D;
inline constexpr std::int32_t kVkNumpad0 = 0x60;
inline constexpr std::int32_t kVkMultiply = 0x6A;
inline constexpr std::int32_t kVkAdd = 0x6B;
inline constexpr std::int32_t kVkSubtract = 0x6D;
inline constexpr std::int32_t kVkDecimal = 0x6E;
inline constexpr std::int32_t kVkDivide = 0x6F;
inline constexpr std::int32_t kVkF1 = 0x70;
inline constexpr std::int32_t kVkNumLock = 0x90;
inline constexpr std::int32_t kVkScroll = 0x91;
inline constexpr std::int32_t kVkOem1 = 0xBA;      // ; :
inline constexpr std::int32_t kVkOemPlus = 0xBB;   // = +
inline constexpr std::int32_t kVkOemComma = 0xBC;  // , <
inline constexpr std::int32_t kVkOemMinus = 0xBD;  // - _
inline constexpr std::int32_t kVkOemPeriod = 0xBE; // . >
inline constexpr std::int32_t kVkOem2 = 0xBF;      // / ?
inline constexpr std::int32_t kVkOem3 = 0xC0;      // ` ~
inline constexpr std::int32_t kVkOem4 = 0xDB;      // [ {
inline constexpr std::int32_t kVkOem5 = 0xDC;      // \ |
inline constexpr std::int32_t kVkOem6 = 0xDD;      // ] }
inline constexpr std::int32_t kVkOem7 = 0xDE;      // ' "

// The X11 button mask a core button number occupies in `state`. Zero for the wheel pseudo-buttons
// (4-7) and the thumb buttons (8/9), which have no mask the Shell tracks.
[[nodiscard]] std::uint32_t x11_button_mask(std::uint32_t button)
{
    switch (button)
    {
    case kX11ButtonLeft:
        return kX11Button1Mask;
    case kX11ButtonMiddle:
        return kX11Button2Mask;
    case kX11ButtonRight:
        return kX11Button3Mask;
    default:
        return 0;
    }
}

// X reports `state` as the mask BEFORE the event. A press must therefore ADD its own button and a
// release must CLEAR it, or the button flags handed to the browser lag one event behind: the very
// first mousedown of a drag would arrive with no button held, which is how a drag never starts.
[[nodiscard]] Modifiers make_x11_modifiers(std::uint32_t state)
{
    Modifiers modifiers;
    modifiers.shift = (state & kX11ShiftMask) != 0;
    modifiers.control = (state & kX11ControlMask) != 0;
    modifiers.alt = (state & kX11Mod1Mask) != 0;
    modifiers.meta = (state & kX11Mod4Mask) != 0;
    modifiers.left_button_down = (state & kX11Button1Mask) != 0;
    modifiers.middle_button_down = (state & kX11Button2Mask) != 0;
    modifiers.right_button_down = (state & kX11Button3Mask) != 0;
    return modifiers;
}

[[nodiscard]] MouseButton x11_mouse_button(std::uint32_t button)
{
    switch (button)
    {
    case kX11ButtonLeft:
        return MouseButton::left;
    case kX11ButtonMiddle:
        return MouseButton::middle;
    case kX11ButtonRight:
        return MouseButton::right;
    default:
        return MouseButton::none;
    }
}

} // namespace

std::int32_t x11_keysym_to_windows_key_code(std::uint32_t keysym)
{
    // Latin letters. A LOWERCASE keysym maps to the UPPERCASE VK code: `a` is keysym 0x61 but VK
    // 0x41, and forwarding 0x61 makes every unshifted letter an unrecognised key in Chromium.
    if (keysym >= 'a' && keysym <= 'z')
    {
        return static_cast<std::int32_t>(keysym - 'a' + 'A');
    }
    if ((keysym >= 'A' && keysym <= 'Z') || (keysym >= '0' && keysym <= '9'))
    {
        return static_cast<std::int32_t>(keysym);
    }
    // The keypad digits are their OWN VK codes; mapping them onto the row digits breaks any
    // keypad-specific binding and, worse, makes them indistinguishable in a keymap.
    if (keysym >= kXkKp0 && keysym <= kXkKp9)
    {
        return kVkNumpad0 + static_cast<std::int32_t>(keysym - kXkKp0);
    }
    if (keysym >= kXkF1 && keysym <= kXkF24)
    {
        return kVkF1 + static_cast<std::int32_t>(keysym - kXkF1);
    }

    switch (keysym)
    {
    case ' ':
        return kVkSpace;
    case kXkBackSpace:
        return kVkBack;
    case kXkTab:
        return kVkTab;
    case kXkReturn:
    case kXkKpEnter:
        return kVkReturn;
    case kXkPause:
        return kVkPause;
    case kXkScrollLock:
        return kVkScroll;
    case kXkEscape:
        return kVkEscape;
    case kXkHome:
        return kVkHome;
    case kXkLeft:
        return kVkLeft;
    case kXkUp:
        return kVkUp;
    case kXkRight:
        return kVkRight;
    case kXkDown:
        return kVkDown;
    case kXkPageUp:
        return kVkPageUp;
    case kXkPageDown:
        return kVkPageDown;
    case kXkEnd:
        return kVkEnd;
    case kXkPrint:
        return kVkSnapshot;
    case kXkInsert:
        return kVkInsert;
    case kXkMenu:
        return kVkApps;
    case kXkNumLock:
        return kVkNumLock;
    case kXkKpMultiply:
        return kVkMultiply;
    case kXkKpAdd:
        return kVkAdd;
    case kXkKpSubtract:
        return kVkSubtract;
    case kXkKpDecimal:
        return kVkDecimal;
    case kXkKpDivide:
        return kVkDivide;
    case kXkShiftL:
    case kXkShiftR:
        return kVkShift;
    case kXkControlL:
    case kXkControlR:
        return kVkControl;
    case kXkCapsLock:
        return kVkCapital;
    case kXkAltL:
    case kXkAltR:
        return kVkMenu;
    case kXkSuperL:
        return kVkLwin;
    case kXkSuperR:
        return kVkRwin;
    case kXkDelete:
        return kVkDelete;
    // Punctuation goes to the VK_OEM_* codes, which are NOT the ASCII values — passing ';' (0x3B)
    // through unchanged collides with VK_F12's neighbourhood rather than naming the key.
    case ';':
        return kVkOem1;
    case '=':
        return kVkOemPlus;
    case ',':
        return kVkOemComma;
    case '-':
        return kVkOemMinus;
    case '.':
        return kVkOemPeriod;
    case '/':
        return kVkOem2;
    case '`':
        return kVkOem3;
    case '[':
        return kVkOem4;
    case '\\':
        return kVkOem5;
    case ']':
        return kVkOem6;
    case '\'':
        return kVkOem7;
    default:
        // No VK equivalent. Honest 0 rather than a guess: a wrong code is a key that fires the
        // WRONG command, which is worse than one that fires none.
        return 0;
    }
}

X11EventBatch translate_x11_event(const X11Event& event, const X11WindowGeometry& previous)
{
    X11EventBatch batch;
    switch (event.type)
    {
    case kX11ConfigureNotify:
    {
        // ONE X event, up to TWO facts. A maximize changes size and position together, and X sends a
        // ConfigureNotify for a pure move too — so both are compared against the previous geometry
        // rather than reported unconditionally.
        const render::Extent2D size{static_cast<std::uint32_t>(event.width < 0 ? 0 : event.width),
                                    static_cast<std::uint32_t>(event.height < 0 ? 0 : event.height)};
        if (!render::is_empty(size) &&
            (size.width != previous.width || size.height != previous.height))
        {
            ShellEvent resize;
            resize.kind = ShellEventKind::resize;
            resize.size = size;
            batch.push(resize);
        }
        if (event.x != previous.x || event.y != previous.y)
        {
            ShellEvent moved;
            moved.kind = ShellEventKind::moved;
            moved.position = PointI{event.x, event.y};
            batch.push(moved);
        }
        return batch;
    }
    case kX11Expose:
    {
        // X sends one Expose per damaged rectangle and counts down; only the LAST one is worth a
        // frame. Repainting per rectangle is a full composite + present for every sliver of a
        // window drag across another window.
        if (event.count == 0)
        {
            batch.push(simple_event(ShellEventKind::paint_requested));
        }
        return batch;
    }
    case kX11FocusIn:
        if (event.mode == kX11NotifyNormal)
        {
            batch.push(simple_event(ShellEventKind::focus_gained));
        }
        return batch;
    case kX11FocusOut:
        // A grab-induced FocusOut is not the window losing focus — it is a menu opening. Reporting
        // it tells the browser to blur, which visibly drops the caret mid-interaction.
        if (event.mode == kX11NotifyNormal)
        {
            batch.push(simple_event(ShellEventKind::focus_lost));
        }
        return batch;
    case kX11ClientMessage:
        if (event.is_delete_window)
        {
            batch.push(simple_event(ShellEventKind::close_requested));
        }
        return batch;
    case kX11MotionNotify:
    {
        ShellEvent move;
        move.kind = ShellEventKind::pointer;
        move.pointer.action = PointerAction::move;
        move.pointer.position = PointI{event.x, event.y};
        move.pointer.modifiers = make_x11_modifiers(event.state);
        batch.push(move);
        return batch;
    }
    case kX11LeaveNotify:
    {
        // Same grab rule as FocusOut: X synthesizes a LeaveNotify around every pointer grab, and
        // forwarding it makes the browser drop its hover state in the middle of a drag.
        if (event.mode != kX11NotifyNormal)
        {
            return batch;
        }
        ShellEvent leave;
        leave.kind = ShellEventKind::pointer;
        leave.pointer.action = PointerAction::leave;
        leave.pointer.modifiers = make_x11_modifiers(event.state);
        batch.push(leave);
        return batch;
    }
    case kX11ButtonPress:
    case kX11ButtonRelease:
    {
        const bool pressed = event.type == kX11ButtonPress;
        if (event.detail >= kX11ButtonWheelUp && event.detail <= kX11ButtonWheelRight)
        {
            // A wheel notch is a PRESS/RELEASE PAIR in the core protocol. Decoding the release too
            // scrolls exactly twice as far as the user asked — the single easiest X11 input bug to
            // ship, because it looks like an over-sensitive mouse rather than a defect.
            if (!pressed)
            {
                return batch;
            }
            ShellEvent wheel;
            wheel.kind = ShellEventKind::pointer;
            wheel.pointer.action = PointerAction::wheel;
            wheel.pointer.position = PointI{event.x, event.y};
            wheel.pointer.modifiers = make_x11_modifiers(event.state);
            // Sign convention matches Win32: +Y is away from the user, +X is to the right.
            if (event.detail == kX11ButtonWheelUp)
            {
                wheel.pointer.wheel_delta_y = kWheelDelta;
            }
            else if (event.detail == kX11ButtonWheelDown)
            {
                wheel.pointer.wheel_delta_y = -kWheelDelta;
            }
            else if (event.detail == kX11ButtonWheelRight)
            {
                wheel.pointer.wheel_delta_x = kWheelDelta;
            }
            else
            {
                wheel.pointer.wheel_delta_x = -kWheelDelta;
            }
            batch.push(wheel);
            return batch;
        }

        const MouseButton button = x11_mouse_button(event.detail);
        if (button == MouseButton::none)
        {
            // Buttons 8/9 (back/forward) and anything else the server invents. Dropped rather than
            // routed as a nameless click.
            return batch;
        }
        ShellEvent pointer;
        pointer.kind = ShellEventKind::pointer;
        pointer.pointer.action = pressed ? PointerAction::down : PointerAction::up;
        pointer.pointer.button = button;
        pointer.pointer.position = PointI{event.x, event.y};
        // Apply this event's OWN button to the pre-event mask — see make_x11_modifiers.
        const std::uint32_t mask = x11_button_mask(event.detail);
        pointer.pointer.modifiers =
            make_x11_modifiers(pressed ? (event.state | mask) : (event.state & ~mask));
        // X11 has no double-click message: Chromium derives it from timing, and the Shell reports
        // every press as a single click rather than inventing a threshold the desktop already owns.
        pointer.pointer.click_count = 1;
        batch.push(pointer);
        return batch;
    }
    case kX11KeyPress:
    case kX11KeyRelease:
    {
        ShellEvent key;
        key.kind = ShellEventKind::key;
        key.key.action =
            event.type == kX11KeyPress ? KeyAction::raw_key_down : KeyAction::key_up;
        key.key.windows_key_code = x11_keysym_to_windows_key_code(event.keysym);
        // CEF's Linux native_key_code IS the X11 hardware keycode.
        key.key.native_key_code = static_cast<std::int32_t>(event.detail);
        key.key.modifiers = make_x11_modifiers(event.state);
        // is_system_key is documented by CEF as Windows-only (the WM_SYSKEY* distinction), so it
        // stays false here rather than being guessed from the Alt modifier.
        key.key.is_system_key = false;
        batch.push(key);

        // Windows splits the character into its own WM_CHAR; X does not, so the SECOND event is
        // synthesized here. Only on a press, and only when the input method actually produced text:
        // a bare Shift or an arrow key must not emit a character event carrying 0.
        if (event.type == kX11KeyPress && event.text != 0)
        {
            ShellEvent character = key;
            character.key.action = KeyAction::character;
            character.key.character = event.text;
            batch.push(character);
        }
        return batch;
    }
    default:
        return batch;
    }
}

// ------------------------------------------------------------------------------------- X11 DPI

std::optional<std::uint32_t> x11_parse_xft_dpi(std::string_view value)
{
    std::size_t start = 0;
    while (start < value.size() && (value[start] == ' ' || value[start] == '\t'))
    {
        ++start;
    }
    std::uint64_t whole = 0;
    std::size_t digits = 0;
    std::size_t i = start;
    for (; i < value.size() && value[i] >= '0' && value[i] <= '9'; ++i)
    {
        whole = whole * 10u + static_cast<std::uint64_t>(value[i] - '0');
        ++digits;
        if (whole > 100000u)
        {
            return std::nullopt; // absurd; treat as unparseable rather than clamping a typo
        }
    }
    if (digits == 0)
    {
        return std::nullopt;
    }
    // A fractional part is accepted and TRUNCATED — "144.0" is the common spelling and must not be
    // rejected — but anything else trailing is a malformed resource, not a DPI.
    if (i < value.size() && value[i] == '.')
    {
        ++i;
        while (i < value.size() && value[i] >= '0' && value[i] <= '9')
        {
            ++i;
        }
    }
    while (i < value.size() && (value[i] == ' ' || value[i] == '\t' || value[i] == '\n'))
    {
        ++i;
    }
    if (i != value.size() || whole == 0)
    {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(whole);
}

DpiScale x11_screen_dpi(std::int32_t pixels, std::int32_t millimetres)
{
    // The plausible band. Deliberately much tighter than make_dpi_scale's 48..960 clamp: that clamp
    // exists to keep a hostile value from allocating an impossible backbuffer, whereas this one is
    // asking "is this number believable as a desktop monitor's DPI at all". A value outside it is
    // far more likely to be a server default than a real screen.
    constexpr std::int64_t kPlausibleMin = 72;
    constexpr std::int64_t kPlausibleMax = 288;
    if (pixels <= 0 || millimetres <= 0)
    {
        return DpiScale{kReferenceDpi};
    }
    // 25.4 mm per inch, in integer arithmetic with round-to-nearest: (px * 254 + 5*mm) / (10*mm).
    const std::int64_t numerator = static_cast<std::int64_t>(pixels) * 254 +
                                   static_cast<std::int64_t>(millimetres) * 5;
    const std::int64_t derived = numerator / (static_cast<std::int64_t>(millimetres) * 10);
    if (derived < kPlausibleMin || derived > kPlausibleMax)
    {
        return DpiScale{kReferenceDpi};
    }
    return make_dpi_scale(static_cast<std::uint32_t>(derived));
}

// ------------------------------------------------------------------- Cocoa/NSEvent event decoding

namespace
{

// Round-half-AWAY-from-zero, and NaN/inf safe. `static_cast<int>` truncates toward zero, so a
// pointer at -0.6 physical pixels and one at +0.6 BOTH land on 0: the bucket straddling the origin is
// twice as wide as every other one, so the coordinate space compresses by half a pixel on each side
// of it and a slow drag stalls as it crosses zero. The clamp is not
// paranoia: a Cocoa geometry read during a window teardown can legitimately be inf, and
// `static_cast<std::int32_t>` of an out-of-range double is UNDEFINED behaviour (a real UBSan finding
// on the sanitize legs, not a theoretical one).
[[nodiscard]] std::int32_t ns_round_to_int(double value)
{
    if (!std::isfinite(value))
    {
        return 0;
    }
    const double clamped = std::min(std::max(value, -1.0e9), 1.0e9);
    return static_cast<std::int32_t>(clamped < 0.0 ? -std::floor(-clamped + 0.5)
                                                   : std::floor(clamped + 0.5));
}

[[nodiscard]] Modifiers make_ns_modifiers(std::uint64_t flags, std::uint32_t pressed_buttons)
{
    Modifiers modifiers;
    modifiers.shift = (flags & kNsModifierShift) != 0;
    modifiers.control = (flags & kNsModifierControl) != 0;
    // Option IS Alt and Command IS Meta. Mapping Command onto `control` — the shape a Windows-first
    // port reaches for, because Cmd is where Ctrl sits on the other two platforms — makes every
    // Cmd-shortcut in the editor arrive at the browser as a Ctrl-shortcut.
    modifiers.alt = (flags & kNsModifierOption) != 0;
    modifiers.meta = (flags & kNsModifierCommand) != 0;
    modifiers.left_button_down = (pressed_buttons & kNsPressedButtonLeft) != 0;
    modifiers.middle_button_down = (pressed_buttons & kNsPressedButtonMiddle) != 0;
    modifiers.right_button_down = (pressed_buttons & kNsPressedButtonRight) != 0;
    return modifiers;
}

// The modifier flag a FlagsChanged key code toggles, or 0 for a key whose flag the Shell does not
// track. Both the left and right key of a pair map to the SAME flag — Cocoa's plain mask has no
// side, which is what the "no transition" rule in translate_ns_event is about.
[[nodiscard]] std::uint64_t ns_modifier_flag_for_key_code(std::uint32_t key_code)
{
    switch (key_code)
    {
    case kNsVkShift:
    case kNsVkRightShift:
        return kNsModifierShift;
    case kNsVkControl:
    case kNsVkRightControl:
        return kNsModifierControl;
    case kNsVkOption:
    case kNsVkRightOption:
        return kNsModifierOption;
    case kNsVkCommand:
    case kNsVkRightCommand:
        return kNsModifierCommand;
    case kNsVkCapsLock:
        return kNsModifierCapsLock;
    default:
        // fn IS DELIBERATELY ABSENT, and that is not an oversight. Unlike every flag above,
        // NSEventModifierFlagFunction is not a modifier-key signal: AppKit sets it on the
        // modifierFlags of ordinary key events for the F-keys AND the whole navigation set — the
        // arrows, Home/End, Page Up/Down, Forward Delete. So an arrow-key press alone would latch
        // the bit in the backend's running mask, and the diff rule in translate_ns_event would then
        // see "no transition" for the real fn key's own FlagsChanged and DROP its key-down, then
        // emit an UNPAIRED key-up on release — precisely the stuck-modifier failure that rule exists
        // to prevent, and intermittently, since the next mouse-move clears the bit again. Returning
        // 0 means the fn key toggles nothing; it has no Windows virtual-key code either
        // (ns_key_code_to_windows_key_code answers 0 for it), so nothing downstream loses a signal.
        return 0;
    }
}

// One normalized pointer sample from an NSEvent, with the flip + scale already applied.
[[nodiscard]] ShellEvent make_ns_pointer(PointerAction action, MouseButton button,
                                          const NsEvent& event, const NsViewGeometry& view)
{
    ShellEvent shell_event;
    shell_event.kind = ShellEventKind::pointer;
    shell_event.pointer.action = action;
    shell_event.pointer.button = button;
    shell_event.pointer.position =
        ns_view_point_to_physical(event.location_x, event.location_y, view);
    shell_event.pointer.modifiers =
        make_ns_modifiers(event.modifier_flags, event.pressed_mouse_buttons);
    // Cocoa COUNTS clicks for us — unlike Windows, which sends a distinct WM_*BUTTONDBLCLK message,
    // and unlike X11, which counts nothing at all. A clickCount of 0 arrives on a drag that outlived
    // its press, and forwarding it would tell CEF "zero clicks", so it floors at 1.
    shell_event.pointer.click_count = event.click_count > 0 ? event.click_count : 1;
    return shell_event;
}

} // namespace

// EXPORTED (declared in window.h) rather than file-local, so the Cocoa backend derives its initial
// client size from the SAME function translate_ns_window_geometry uses for every later resize. It was
// file-local until the backend was found re-deriving this arithmetic inline: two copies of one
// conversion feeding one observable (client_size()), with only the copy no CI leg executes able to
// drift. Same reason ns_dpi_from_backing_scale and ns_view_point_to_physical below are exported.
std::uint32_t ns_extent_to_physical(double points, DpiScale dpi)
{
    if (!(points > 0.0) || !std::isfinite(points))
    {
        return 0u;
    }
    const std::int32_t scaled = ns_round_to_int(points * static_cast<double>(dpi.factor()));
    return scaled <= 0 ? 0u : static_cast<std::uint32_t>(scaled);
}

DpiScale ns_dpi_from_backing_scale(double backing_scale)
{
    if (!std::isfinite(backing_scale) || !(backing_scale > 0.0))
    {
        // A window not yet on a screen reports 0; a torn-down one can report NaN. Neither is a
        // scale, and multiplying the whole layout by either is how a UI collapses to nothing.
        return DpiScale{kReferenceDpi};
    }
    const std::int32_t dpi =
        ns_round_to_int(backing_scale * static_cast<double>(kReferenceDpi));
    if (dpi <= 0)
    {
        // TWO REGIMES, deliberately, and the boundary is "did this round to a DPI at all". A scale
        // that is merely implausible (0.4) still names a DPI, so it is CLAMPED into the supported
        // band by make_dpi_scale below — that clamp exists precisely to keep a hostile value from
        // allocating an impossible backbuffer. A scale that rounds to ZERO dpi (1e-3) names no
        // display at all, which is the same class as the NaN and the 0 handled above, and is
        // REFUSED to 1x rather than clamped: clamping it would report a 0.5x display that is not
        // there, where refusing reports the honest "this is not a scale".
        return DpiScale{kReferenceDpi};
    }
    // make_dpi_scale clamps whatever survives into kMinDpi..kMaxDpi.
    return make_dpi_scale(static_cast<std::uint32_t>(dpi));
}

PointI ns_view_point_to_physical(double x_points, double y_points, const NsViewGeometry& view)
{
    const double factor = static_cast<double>(view.dpi.factor());
    // THE FLIP. Cocoa's origin is the BOTTOM-left of the view; the region map, CEF and both sibling
    // backends speak TOP-left. Done BEFORE the scale, against the height in POINTS, because that is
    // the space the height is expressed in — flipping a scaled y against an unscaled height is the
    // shape that looks right at 1x and mirrors the pointer on a Retina display.
    const double flipped_y = view.height_points - y_points;
    return PointI{ns_round_to_int(x_points * factor), ns_round_to_int(flipped_y * factor)};
}

std::int32_t ns_key_code_to_windows_key_code(std::uint32_t key_code)
{
    // A TABLE, not arithmetic. macOS key codes are POSITIONAL (0x00 is `A`, 0x01 is `S`, 0x06 is
    // `Z` — the physical ASDF row), so there is no run to index into for the letters, the digits are
    // transposed at 5/6, and the function keys are scattered across two ranges.
    switch (key_code)
    {
    // --- letters, in macOS key-code order (see above: this order is the KEYBOARD's, not the
    // alphabet's) -----------------------------------------------------------------------------
    case kNsVkAnsiA: return 'A';
    case kNsVkAnsiS: return 'S';
    case 0x02: return 'D';
    case 0x03: return 'F';
    case 0x04: return 'H';
    case 0x05: return 'G';
    case kNsVkAnsiZ: return 'Z';
    case 0x07: return 'X';
    case 0x08: return 'C';
    case 0x09: return 'V';
    case 0x0B: return 'B';
    case 0x0C: return 'Q';
    case 0x0D: return 'W';
    case 0x0E: return 'E';
    case 0x0F: return 'R';
    case 0x10: return 'Y';
    case 0x11: return 'T';
    case 0x1F: return 'O';
    case 0x20: return 'U';
    case 0x22: return 'I';
    case 0x23: return 'P';
    case 0x25: return 'L';
    case 0x26: return 'J';
    case 0x28: return 'K';
    case 0x2D: return 'N';
    case 0x2E: return 'M';
    // --- digit row. NOT monotonic: 6 (0x16) comes BEFORE 5 (0x17). ------------------------------
    case 0x12: return '1';
    case 0x13: return '2';
    case 0x14: return '3';
    case 0x15: return '4';
    case kNsVkAnsi6: return '6';
    case kNsVkAnsi5: return '5';
    case 0x19: return '9';
    case 0x1A: return '7';
    case 0x1C: return '8';
    case 0x1D: return '0';
    // --- punctuation -> the VK_OEM_* codes, which are NOT the ASCII values ------------------------
    case 0x18: return kVkOemPlus;   // = +
    case 0x1B: return kVkOemMinus;  // - _
    case 0x1E: return kVkOem6;      // ] }
    case 0x21: return kVkOem4;      // [ {
    case 0x27: return kVkOem7;      // ' "
    case 0x29: return kVkOem1;      // ; :
    case 0x2A: return kVkOem5;      // backslash |
    case 0x2B: return kVkOemComma;  // , <
    case 0x2C: return kVkOem2;      // / ?
    case 0x2F: return kVkOemPeriod; // . >
    case 0x32: return kVkOem3;      // ` ~
    // --- editing + navigation --------------------------------------------------------------------
    case kNsVkReturn: return kVkReturn;
    case kNsVkTab: return kVkTab;
    case kNsVkSpace: return kVkSpace;
    // kVK_Delete is the key engraved "delete" ABOVE Return, and it BACKSPACES. VK_DELETE is the
    // navigation-cluster key, kVK_ForwardDelete. Reading the names the other way round makes
    // Backspace delete forwards in every text field of the editor.
    case kNsVkDelete: return kVkBack;
    case kNsVkForwardDelete: return kVkDelete;
    case kNsVkEscape: return kVkEscape;
    case 0x72: return kVkInsert; // kVK_Help occupies the Insert position on an Apple keyboard
    case 0x73: return kVkHome;
    case 0x74: return kVkPageUp;
    case 0x77: return kVkEnd;
    case 0x79: return kVkPageDown;
    case kNsVkLeftArrow: return kVkLeft;
    case kNsVkRightArrow: return kVkRight;
    case kNsVkDownArrow: return kVkDown;
    case kNsVkUpArrow: return kVkUp;
    // --- modifiers. Command is META (VK_LWIN/VK_RWIN) and Option is ALT (VK_MENU); neither is
    // VK_CONTROL, however much the keycap positions suggest otherwise. --------------------------
    case kNsVkShift:
    case kNsVkRightShift: return kVkShift;
    case kNsVkControl:
    case kNsVkRightControl: return kVkControl;
    case kNsVkOption:
    case kNsVkRightOption: return kVkMenu;
    case kNsVkCommand: return kVkLwin;
    case kNsVkRightCommand: return kVkRwin;
    case kNsVkCapsLock: return kVkCapital;
    // --- keypad. Its digits are their OWN VK codes; folding them onto the row digits makes a
    // keypad-specific binding indistinguishable from the row one. AND the keypad codes are
    // themselves out of order — 8 is 0x5B and 9 is 0x5C, past F20 at 0x5A. -----------------------
    case kNsVkKeypad0: return kVkNumpad0 + 0;
    case 0x53: return kVkNumpad0 + 1;
    case 0x54: return kVkNumpad0 + 2;
    case 0x55: return kVkNumpad0 + 3;
    case 0x56: return kVkNumpad0 + 4;
    case 0x57: return kVkNumpad0 + 5;
    case 0x58: return kVkNumpad0 + 6;
    case 0x59: return kVkNumpad0 + 7;
    case 0x5B: return kVkNumpad0 + 8;
    case 0x5C: return kVkNumpad0 + 9;
    case 0x41: return kVkDecimal;
    case 0x43: return kVkMultiply;
    case 0x45: return kVkAdd;
    case 0x4B: return kVkDivide;
    case 0x4E: return kVkSubtract;
    case 0x4C: return kVkReturn; // the keypad Enter, exactly as X11 folds XK_KP_Enter
    // --- function keys. Two scattered ranges, deliberately spelled one per line: F1 is 0x7A while
    // F5 is 0x60, so no arithmetic covers them. ---------------------------------------------------
    case kNsVkF1: return kVkF1 + 0;
    case 0x78: return kVkF1 + 1;
    case 0x63: return kVkF1 + 2;
    case 0x76: return kVkF1 + 3;
    case kNsVkF5: return kVkF1 + 4;
    case 0x61: return kVkF1 + 5;
    case 0x62: return kVkF1 + 6;
    case 0x64: return kVkF1 + 7;
    case 0x65: return kVkF1 + 8;
    case 0x6D: return kVkF1 + 9;
    case 0x67: return kVkF1 + 10;
    case 0x6F: return kVkF1 + 11;
    case 0x69: return kVkF1 + 12;
    case 0x6B: return kVkF1 + 13;
    case 0x71: return kVkF1 + 14;
    case 0x6A: return kVkF1 + 15;
    case 0x40: return kVkF1 + 16;
    case 0x4F: return kVkF1 + 17;
    case 0x50: return kVkF1 + 18;
    case 0x5A: return kVkF1 + 19;
    default:
        // No VK equivalent (the JIS-only keys, the media keys, kVK_Function). Honest 0 rather than a
        // guess: a wrong code is a key that fires the WRONG command, which is worse than none.
        return 0;
    }
}

ShellEventBatch translate_ns_event(const NsEvent& event, const NsViewGeometry& view)
{
    ShellEventBatch batch;
    switch (event.type)
    {
    case kNsMouseMoved:
    case kNsLeftMouseDragged:
    case kNsRightMouseDragged:
    case kNsOtherMouseDragged:
        batch.push(make_ns_pointer(PointerAction::move, MouseButton::none, event, view));
        return batch;
    case kNsLeftMouseDown:
        batch.push(make_ns_pointer(PointerAction::down, MouseButton::left, event, view));
        return batch;
    case kNsLeftMouseUp:
        batch.push(make_ns_pointer(PointerAction::up, MouseButton::left, event, view));
        return batch;
    case kNsRightMouseDown:
        batch.push(make_ns_pointer(PointerAction::down, MouseButton::right, event, view));
        return batch;
    case kNsRightMouseUp:
        batch.push(make_ns_pointer(PointerAction::up, MouseButton::right, event, view));
        return batch;
    case kNsOtherMouseDown:
    case kNsOtherMouseUp:
    {
        if (event.button_number != kNsOtherButtonMiddle)
        {
            // A thumb (back/forward) or higher button. Dropped rather than routed as a nameless
            // click — the same choice the X11 decoder makes for buttons 8/9.
            return batch;
        }
        batch.push(make_ns_pointer(event.type == kNsOtherMouseDown ? PointerAction::down
                                                                   : PointerAction::up,
                                   MouseButton::middle, event, view));
        return batch;
    }
    case kNsMouseExited:
    {
        ShellEvent leave;
        leave.kind = ShellEventKind::pointer;
        leave.pointer.action = PointerAction::leave;
        leave.pointer.modifiers =
            make_ns_modifiers(event.modifier_flags, event.pressed_mouse_buttons);
        batch.push(leave);
        return batch;
    }
    case kNsScrollWheel:
    {
        ShellEvent wheel = make_ns_pointer(PointerAction::wheel, MouseButton::none, event, view);
        // A PRECISE delta (trackpad, or a high-resolution wheel) is already a distance in POINTS and
        // is forwarded AS-IS. A LINE delta is a notch count, and one notch is kWheelDelta in the
        // convention the Win32 and X11 decoders already hand the browser. Multiplying a precise delta
        // by 120 instead is the mistake that makes a gentle two-finger swipe jump a hundred screens.
        //
        // ⚠ AND IT IS NOT SCALED BY THE DPI FACTOR EITHER — a wheel delta is the ONE field on this
        // seam that is not a physical-pixel quantity. Both siblings emit a scale-free value (Win32
        // forwards WM_MOUSEWHEEL's ±120 verbatim, X11 emits ±kWheelDelta), and the consumer forwards
        // it to the browser UNCONVERTED while converting the position separately
        // (cef_shell.cpp's SendMouseWheelEvent), so the browser reads the delta in the same
        // point/DIP space it reads that position in. Scaling here would double every trackpad scroll
        // on a Retina display and triple it at 3x — invisible at 1x, which is why the assertion in
        // test_window.cpp uses a 2x fixture.
        const double scale = event.has_precise_scrolling_deltas
                                 ? 1.0
                                 : static_cast<double>(kNsLinesToWheelDelta);
        wheel.pointer.wheel_delta_x = ns_round_to_int(event.scrolling_delta_x * scale);
        wheel.pointer.wheel_delta_y = ns_round_to_int(event.scrolling_delta_y * scale);
        if (wheel.pointer.wheel_delta_x == 0 && wheel.pointer.wheel_delta_y == 0)
        {
            // A momentum tail decays to sub-pixel deltas that round to nothing. Forwarding them is a
            // browser wheel event per frame that scrolls zero pixels, for as long as the tail runs.
            return batch;
        }
        batch.push(wheel);
        return batch;
    }
    case kNsKeyDown:
    case kNsKeyUp:
    {
        ShellEvent key;
        key.kind = ShellEventKind::key;
        key.key.action =
            event.type == kNsKeyDown ? KeyAction::raw_key_down : KeyAction::key_up;
        key.key.windows_key_code = ns_key_code_to_windows_key_code(event.key_code);
        // CEF's macOS native_key_code IS the NSEvent keyCode.
        key.key.native_key_code = static_cast<std::int32_t>(event.key_code);
        key.key.modifiers = make_ns_modifiers(event.modifier_flags, event.pressed_mouse_buttons);
        // is_system_key is documented by CEF as Windows-only (the WM_SYSKEY* distinction), so it
        // stays false here rather than being guessed from the Command modifier.
        key.key.is_system_key = false;
        batch.push(key);

        // Windows splits the character into its own WM_CHAR; Cocoa does not, so the SECOND event is
        // synthesized here — the same shape the X11 decoder takes.
        //
        // ⚠ COMMAND AND CONTROL SUPPRESS IT, OPTION DOES NOT. -[NSEvent characters] answers "s" for
        // Cmd+S, so forwarding it unconditionally TYPES an `s` into the focused text field every
        // time the user saves. Windows never had this problem: Ctrl+S produces a WM_CHAR carrying
        // the control code 0x13, which no field inserts. Option is the opposite case and must NOT be
        // suppressed — on macOS it is a TEXT modifier (Option+e begins an accent, Option+2 types a
        // trademark sign), so suppressing it silently disables dead keys and half the symbol set.
        const bool command_or_control =
            (event.modifier_flags & (kNsModifierCommand | kNsModifierControl)) != 0;
        if (event.type == kNsKeyDown && event.text != 0 && !command_or_control)
        {
            ShellEvent character = key;
            character.key.action = KeyAction::character;
            character.key.character = event.text;
            batch.push(character);
        }
        return batch;
    }
    case kNsFlagsChanged:
    {
        // A modifier key produces NO KeyDown/KeyUp on macOS — it arrives here, saying WHICH key
        // moved but not which WAY. The direction is recovered by diffing the mask.
        const std::uint64_t flag = ns_modifier_flag_for_key_code(event.key_code);
        if (flag == 0)
        {
            return batch;
        }
        const bool now_down = (event.modifier_flags & flag) != 0;
        const bool was_down = (view.previous_modifier_flags & flag) != 0;
        if (now_down == was_down)
        {
            // No transition of the PAIR. Cocoa's plain modifier mask has no side, so pressing the
            // right Shift while the left one is already held is genuinely not observable here —
            // distinguishing them needs the device-dependent bits, which the Shell does not read.
            // Reporting an event anyway would hand the browser a keydown with no matching keyup.
            // CAPS LOCK rides the same rule and is a LOCK: its flag follows the toggle state, so a
            // press reports down, the release reports nothing, and the NEXT press reports up.
            return batch;
        }
        ShellEvent key;
        key.kind = ShellEventKind::key;
        key.key.action = now_down ? KeyAction::raw_key_down : KeyAction::key_up;
        key.key.windows_key_code = ns_key_code_to_windows_key_code(event.key_code);
        key.key.native_key_code = static_cast<std::int32_t>(event.key_code);
        key.key.modifiers = make_ns_modifiers(event.modifier_flags, event.pressed_mouse_buttons);
        batch.push(key);
        return batch;
    }
    default:
        return batch;
    }
}

ShellEventBatch translate_ns_window_geometry(const NsWindowGeometry& previous,
                                              const NsWindowGeometry& current)
{
    ShellEventBatch batch;

    // Compared in PHYSICAL PIXELS, not in points: a window dragged from a 1x to a 2x display keeps
    // its point size and doubles its backbuffer, so a points-only comparison reports no resize on
    // exactly the transition that needs one most. Integers, so no float equality is involved.
    const render::Extent2D previous_size{
        ns_extent_to_physical(previous.width_points, previous.dpi),
        ns_extent_to_physical(previous.height_points, previous.dpi)};
    const render::Extent2D size{ns_extent_to_physical(current.width_points, current.dpi),
                                ns_extent_to_physical(current.height_points, current.dpi)};

    if (!render::is_empty(size) &&
        (size.width != previous_size.width || size.height != previous_size.height))
    {
        ShellEvent resize;
        resize.kind = ShellEventKind::resize;
        resize.size = size;
        batch.push(resize);
    }
    if (current.x != previous.x || current.y != previous.y)
    {
        ShellEvent moved;
        moved.kind = ShellEventKind::moved;
        moved.position = PointI{current.x, current.y};
        batch.push(moved);
    }
    if (current.dpi != previous.dpi)
    {
        ShellEvent dpi_changed;
        dpi_changed.kind = ShellEventKind::dpi_changed;
        dpi_changed.dpi = current.dpi;
        batch.push(dpi_changed);
    }
    // A minimized/zero-sized window reports 0x0 every step, exactly as WM_SIZE's minimize carve-out
    // does; the guard above drops the resize while still letting a move or a scale change through.
    return batch;
}

// ------------------------------------------------------------------------------ backend selection

WindowBackendSelection make_window_backend(const WindowDesc& desc)
{
    WindowBackendSelection selection;
    std::string error;

#if defined(_WIN32)
    selection.backend = make_win32_window_backend(desc, error);
    if (selection.backend == nullptr)
    {
        selection.diagnostic = "the Windows window backend could not create a window: " + error;
    }
#elif defined(__linux__)
    selection.backend = make_x11_window_backend(desc, error);
    if (selection.backend == nullptr)
    {
        selection.diagnostic = "the Linux X11 window backend could not create a window: " + error;
    }
#elif defined(__APPLE__)
    selection.backend = make_cocoa_window_backend(desc, error);
    if (selection.backend == nullptr)
    {
        selection.diagnostic = "the macOS Cocoa window backend could not create a window: " + error;
    }
#else
    // Not "nobody has written this one yet" — all three v1 platforms have a backend since e12b.
    // This arm is what a FOURTH platform (a BSD, a future Wayland-only target) would land on, and it
    // says so rather than naming a task that has shipped.
    (void)desc;
    (void)error;
    selection.diagnostic =
        "no native window backend for this platform: the Shell ships Windows, Linux (X11/XWayland, "
        "D21) and macOS backends. The Shell runs headless here; use HeadlessWindowBackend explicitly "
        "to make that choice visible.";
#endif
    return selection;
}

} // namespace context::editor::shell
