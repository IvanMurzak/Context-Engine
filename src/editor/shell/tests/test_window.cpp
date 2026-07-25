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
        CHECK(!selection.diagnostic.empty());
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
    // shown (`visible = false`), the same reason the Windows arm above is Session-0-safe.
    if (selection.backend != nullptr)
    {
        CHECK(selection.diagnostic.empty());
        CHECK(selection.backend->native_window().kind == render::NativeWindowKind::MetalLayer);
        CHECK(selection.backend->native_window().handle != nullptr);
        selection.backend->close();
    }
    else
    {
        CHECK(!selection.diagnostic.empty());
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
    // hundred screens; it is scaled to physical pixels like every other coordinate on this seam.
    NsEvent swipe = ns_event(kNsScrollWheel);
    swipe.has_precise_scrolling_deltas = true;
    swipe.scrolling_delta_y = 12.0;
    CHECK(translate_ns_event(swipe, view).events[0].pointer.wheel_delta_y == 24); // 12 pt at 2x

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
    option_2.text = U'™'; // TRADE MARK SIGN
    option_2.modifier_flags = kNsModifierOption;
    batch = translate_ns_event(option_2, view);
    CHECK(batch.count == 2u);
    CHECK(batch.events[1].key.action == KeyAction::character);
    CHECK(batch.events[1].key.character == U'™');
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
    CHECK(ns_key_code_to_windows_key_code(0x4C) == 0x0D);         // the keypad Enter folds to VK_RETURN

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

    // A miniaturized window reports a 0x0 content size, exactly as WM_SIZE's minimize carve-out
    // does; the resize is dropped while a move still comes through.
    NsWindowGeometry collapsed = previous;
    collapsed.width_points = 0.0;
    collapsed.height_points = 0.0;
    CHECK(translate_ns_window_geometry(previous, collapsed).count == 0u);
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
    test_headless_backend_reports_no_native_window_by_default();
    test_headless_backend_applies_state_before_delivering_events();
    test_headless_backend_close_ends_the_pump();
    test_headless_backend_records_placement_and_redraws();
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
    SHELL_TEST_MAIN_END();
}
