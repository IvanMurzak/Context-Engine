// The window seam: the headless backend's behaviour, and the PURE Win32 message decoder.
//
// The decoder is the point of this file. The local dev gate defines _WIN32 and CI's Windows leg is
// the only thing that ever runs a real WndProc, so message decoding written inside the OS backend
// would have exactly one place it could be exercised. Written as a pure function over plain
// integers, every branch of it runs HERE — on ubuntu, macOS and Windows alike.

#include "context/editor/shell/window.h"

#include "shell_test.h"

#include <cstdint>
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

    // Every other message is not the Shell's — which is most of them.
    CHECK(!decode(0x0084 /* WM_NCHITTEST */, 0, 0).has_value());
    CHECK(!decode(0x0113 /* WM_TIMER */, 0, 0).has_value());
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
#else
    // Elsewhere the backend does not exist YET, and that gap is REPORTED rather than silent: a
    // shell that quietly opened no window looks identical to one that opened an invisible one.
    CHECK(selection.backend == nullptr);
    CHECK(!selection.diagnostic.empty());
    CHECK(shelltest::mentions(selection.diagnostic, "e12"));
#endif
}

} // namespace

int main()
{
    test_resize_decoding_and_the_minimize_carve_out();
    test_mouse_coordinates_are_signed();
    test_button_messages_map_to_actions_and_buttons();
    test_mouse_button_state_comes_from_wparam_and_modifier_keys_from_the_os();
    test_wheel_carries_a_signed_delta_and_deliberately_no_position();
    test_key_and_char_decoding();
    test_dpi_change_reads_the_low_word();
    test_lifecycle_and_focus_messages();
    test_headless_backend_reports_no_native_window_by_default();
    test_headless_backend_applies_state_before_delivering_events();
    test_headless_backend_close_ends_the_pump();
    test_headless_backend_records_placement_and_redraws();
    test_platform_backend_selection_is_never_silent();
    SHELL_TEST_MAIN_END();
}
