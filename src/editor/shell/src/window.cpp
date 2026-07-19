// The portable half of the window seam: the headless backend and the pure Win32 message decoder.
// See window.h for why the decoding lives here rather than inside win32_window.cpp.

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
    {
        ShellEvent event;
        event.kind = ShellEventKind::paint_requested;
        return event;
    }
    case kWmSetFocus:
    {
        ShellEvent event;
        event.kind = ShellEventKind::focus_gained;
        return event;
    }
    case kWmKillFocus:
    {
        ShellEvent event;
        event.kind = ShellEventKind::focus_lost;
        return event;
    }
    case kWmClose:
    case kWmDestroy:
    {
        ShellEvent event;
        event.kind = ShellEventKind::close_requested;
        return event;
    }
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

} // namespace context::editor::shell
