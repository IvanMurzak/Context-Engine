// The portable half of the window seam: the headless backend, the pure Win32 and X11 event
// decoders, and the platform CHOICE (make_window_backend). See window.h for why the decoding lives
// here rather than inside win32_window.cpp / x11_window.cpp.
//
// make_window_backend moved here in e12a. It used to live in win32_window.cpp, whose `#else` branch
// was the honest "no backend on this platform yet" report for BOTH other OSes. With a second real
// backend that shape stops working: the choice belongs to neither platform file, and leaving it in
// one of them would mean the Linux build's selection logic sat inside a file that is entirely
// `#if defined(_WIN32)`.

#include "context/editor/shell/window.h"

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

} // namespace

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
    {
        ShellEvent event;
        event.kind = ShellEventKind::pointer;
        event.pointer.action = PointerAction::leave;
        event.pointer.modifiers = make_key_modifiers(keys);
        return event;
    }
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
#else
    (void)desc;
    (void)error;
    selection.diagnostic =
        "no native window backend on this platform: the macOS NSWindow/NSView backend lands with "
        "e12b (design 03 §1). The Shell runs headless here; use HeadlessWindowBackend explicitly to "
        "make that choice visible.";
#endif
    return selection;
}

} // namespace context::editor::shell
