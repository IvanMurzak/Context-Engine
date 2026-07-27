// The smoke-tier window seam (M9 e12a-x11-legs, issue #408) — see smoke_window.h for the contract
// and for why real mode injects through the X server rather than through a queue.

#include "context/editor/shell/smoke/smoke_window.h"

// Named HERE, not in the seam header: `PresentSetup` reports the blitter by NAME and hands back no
// handle, so the header has no `render::present` type left to declare.
#include "context/render/present/present_blit.h"

// The Cocoa injection arm (M9 e12c-3). An INTERNAL header, so no smoke can call the platform half
// directly and bypass the anti-degrade guards `inject_event` applies first. It is a TU pair rather
// than an `#if` in this file because CMake picks a compiler by file extension — see its own header.
#include "smoke_inject_cocoa.h"

#include <cmath>
#include <cstring>
#include <string>
#include <utility>

#if defined(CONTEXT_SHELL_SMOKE_HAS_X11)
// Xlib pollutes the global namespace with macros this file must not leak (`None`, `Status`,
// `Success`, and the event-type names). It is confined to this TU on purpose: nothing in
// smoke_window.h names an X type, so every consumer — including the nine CEF smokes — stays
// X11-free.
#include <X11/Xlib.h>
#include <X11/keysym.h>
#endif

namespace context::editor::shell::smoke
{

namespace
{

// The MemoryBlitter reports itself as "memory" (present_blit.h). Real mode refuses it BY NAME
// rather than by RTTI: the CEF-linking smoke targets compile under CEF's `-fno-rtti` dialect, so a
// dynamic_cast here would not compile where it matters most.
constexpr const char* kMemoryBlitterName = "memory";

// The EXACT dpi factor, shared by every smoke-side conversion below that needs one. `dpi.dpi` is the
// source of truth and `DpiScale::factor()` the derived value (dpi.h) — the smoke side needs the
// double rather than that float so its inverses round-trip at 1.25x / 1.5x. `make_dpi_scale` clamps
// into kMinDpi..kMaxDpi, so the only value worth guarding is a HAND-CONSTRUCTED 0, which would
// otherwise divide the whole location away; a unit test pins that refuse-to-1x behaviour. No
// isfinite check: an integer dpi cannot be NaN. Kept in ONE place deliberately — `window.cpp`'s own
// header records the hazard for exactly this shape, two copies of one conversion feeding one
// observable, where the copy no CI leg executes is the one free to drift.
[[nodiscard]] double ns_exact_dpi_factor(DpiScale dpi)
{
    return dpi.dpi == 0u ? 1.0 : static_cast<double>(dpi.dpi) / static_cast<double>(kReferenceDpi);
}

#if defined(CONTEXT_SHELL_SMOKE_HAS_X11)

struct X11Target
{
    ::Display* display = nullptr;
    ::Window window = 0;
};

// The Display* + Window a real-mode injection needs, or a zeroed target when this backend does not
// carry an Xlib window (which is itself the anti-degrade check: a headless backend reports
// NativeWindowKind::None and therefore cannot be injected into in real mode).
[[nodiscard]] X11Target x11_target(const IWindowBackend& backend)
{
    const render::NativeWindowDesc native = backend.native_window();
    if (native.kind != render::NativeWindowKind::XlibWindow || native.display == nullptr ||
        native.handle == nullptr)
    {
        return X11Target{};
    }
    X11Target target;
    target.display = static_cast<::Display*>(native.display);
    // rhi.h's contract: an XID widened to a pointer.
    target.window = static_cast<::Window>(reinterpret_cast<std::uintptr_t>(native.handle));
    return target;
}

// The X modifier/button mask for `modifiers`. X reports the mask BEFORE the event is applied, which
// is what the decoder's press/release bookkeeping assumes — so the caller composes the mask for the
// state the event is leaving, not the one it produces.
[[nodiscard]] unsigned int x11_state_mask(const Modifiers& modifiers)
{
    unsigned int state = 0;
    if (modifiers.shift)
    {
        state |= static_cast<unsigned int>(kX11ShiftMask);
    }
    if (modifiers.control)
    {
        state |= static_cast<unsigned int>(kX11ControlMask);
    }
    if (modifiers.alt)
    {
        state |= static_cast<unsigned int>(kX11Mod1Mask);
    }
    if (modifiers.meta)
    {
        state |= static_cast<unsigned int>(kX11Mod4Mask);
    }
    if (modifiers.left_button_down)
    {
        state |= static_cast<unsigned int>(kX11Button1Mask);
    }
    if (modifiers.middle_button_down)
    {
        state |= static_cast<unsigned int>(kX11Button2Mask);
    }
    if (modifiers.right_button_down)
    {
        state |= static_cast<unsigned int>(kX11Button3Mask);
    }
    return state;
}

[[nodiscard]] unsigned int x11_button_number(MouseButton button)
{
    switch (button)
    {
    case MouseButton::left:
        return kX11ButtonLeft;
    case MouseButton::middle:
        return kX11ButtonMiddle;
    case MouseButton::right:
        return kX11ButtonRight;
    case MouseButton::none:
        break;
    }
    return 0;
}

// Send one composed event to the smoke's own window.
//
// `propagate` false + an EMPTY event mask is the load-bearing pair: the X protocol delivers such an
// event to the window's CREATING CLIENT regardless of what that client selected for, and regardless
// of where the pointer or the input focus happens to be. That is what makes this deterministic
// under xvfb, which has no window manager to give anything focus.
[[nodiscard]] bool send(::Display* display, ::Window window, ::XEvent& event)
{
    event.xany.display = display;
    event.xany.window = window;
    event.xany.send_event = True;
    if (::XSendEvent(display, window, False, 0 /* empty event mask */, &event) == 0)
    {
        return false;
    }
    // Without the flush the request sits in the output buffer until something else forces it out;
    // the owner loop's XPending would then see an empty queue and the caller would time out waiting
    // for an event that was never sent.
    ::XFlush(display);
    return true;
}

[[nodiscard]] bool inject_pointer_x11(const X11Target& target, const PointerEvent& pointer)
{
    ::XEvent event;
    std::memset(&event, 0, sizeof(event));
    const unsigned int state = x11_state_mask(pointer.modifiers);
    const ::Window root = DefaultRootWindow(target.display);

    switch (pointer.action)
    {
    case PointerAction::move:
        event.type = MotionNotify;
        event.xmotion.root = root;
        event.xmotion.time = CurrentTime;
        event.xmotion.x = pointer.position.x;
        event.xmotion.y = pointer.position.y;
        event.xmotion.x_root = pointer.position.x;
        event.xmotion.y_root = pointer.position.y;
        event.xmotion.state = state;
        event.xmotion.same_screen = True;
        break;
    case PointerAction::down:
    case PointerAction::up:
    {
        const unsigned int button = x11_button_number(pointer.button);
        if (button == 0)
        {
            return false; // a press/release with no button is not an event X can carry
        }
        event.type = pointer.action == PointerAction::down ? ButtonPress : ButtonRelease;
        event.xbutton.root = root;
        event.xbutton.time = CurrentTime;
        event.xbutton.x = pointer.position.x;
        event.xbutton.y = pointer.position.y;
        event.xbutton.x_root = pointer.position.x;
        event.xbutton.y_root = pointer.position.y;
        event.xbutton.state = state;
        event.xbutton.button = button;
        event.xbutton.same_screen = True;
        break;
    }
    case PointerAction::wheel:
    case PointerAction::leave:
        // A wheel notch is a ButtonPress/ButtonRelease PAIR on button 4-7 and a leave is a
        // LeaveNotify — both are decodable, and both would need their own round-trip contract with
        // the caller (two events for one sample; a crossing mode). No smoke injects either today,
        // so this refuses LOUDLY rather than shipping an untested encoder.
        return false;
    }
    return send(target.display, target.window, event);
}

[[nodiscard]] bool inject_key_x11(const X11Target& target, const KeyEvent& key)
{
    const std::uint32_t keysym = x11_keysym_for_windows_key_code(key.windows_key_code);
    if (keysym == 0)
    {
        return false;
    }
    const ::KeyCode keycode = ::XKeysymToKeycode(target.display, static_cast<::KeySym>(keysym));
    if (keycode == 0)
    {
        return false; // this server's keymap cannot produce the key at all
    }

    ::XEvent event;
    std::memset(&event, 0, sizeof(event));
    switch (key.action)
    {
    case KeyAction::raw_key_down:
    case KeyAction::key_down:
        event.type = KeyPress;
        break;
    case KeyAction::key_up:
        event.type = KeyRelease;
        break;
    case KeyAction::character:
        // X carries no separate character event: the decoder SYNTHESIZES the character alongside
        // the press from what the input method produced. Injecting one directly would be inventing
        // a native event shape that does not exist.
        return false;
    }
    event.xkey.root = DefaultRootWindow(target.display);
    event.xkey.time = CurrentTime;
    event.xkey.state = x11_state_mask(key.modifiers);
    event.xkey.keycode = keycode;
    event.xkey.same_screen = True;
    return send(target.display, target.window, event);
}

#endif // CONTEXT_SHELL_SMOKE_HAS_X11

} // namespace

const char* to_string(WindowMode mode)
{
    return mode == WindowMode::real ? "real" : "headless";
}

WindowMode window_mode_from_args(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        if (argv[i] != nullptr && std::strcmp(argv[i], "--real-window") == 0)
        {
            return WindowMode::real;
        }
    }
    return WindowMode::headless;
}

WindowSetup make_smoke_window(WindowDesc desc, WindowMode mode)
{
    WindowSetup setup;
    if (mode == WindowMode::headless)
    {
        desc.visible = false;
        setup.backend = std::make_unique<HeadlessWindowBackend>(desc);
        return setup;
    }

    desc.visible = true;
    WindowBackendSelection selection = make_window_backend(desc);
    if (selection.backend == nullptr)
    {
        setup.diagnostic = selection.diagnostic.empty()
                               ? std::string("no native window backend could be created")
                               : selection.diagnostic;
        return setup;
    }
    // The anti-degrade guard, and the reason this is not just a call to make_window_backend: a
    // real-window smoke that quietly ran on the headless backend would assert every one of its
    // claims against no OS at all and stay green forever. Like the MemoryBlitter guard in
    // `attach_smoke_present` this is DEFENCE IN DEPTH — today `make_window_backend` returns a
    // platform backend or nullptr on every arm, never the headless one — so it is the FUTURE
    // regression it forbids, and `test_real_mode_refuses_the_headless_backend_rather_than_degrading`
    // reaches it by handing the seam a headless backend the factory would not have produced.
    if (std::strcmp(selection.backend->name(), "headless") == 0)
    {
        setup.diagnostic = "the platform window factory resolved to the HEADLESS backend, which is "
                           "the degrade --real-window exists to forbid";
        return setup;
    }
    setup.backend = std::move(selection.backend);
    return setup;
}

WindowMode mode_of(const IWindowBackend& backend)
{
    return std::strcmp(backend.name(), "headless") == 0 ? WindowMode::headless : WindowMode::real;
}

BrowserGeometry browser_geometry(const IWindowBackend& backend)
{
    BrowserGeometry geometry;
    geometry.dpi = backend.dpi();
    geometry.logical_size = to_logical(backend.client_size(), geometry.dpi);
    return geometry;
}

PresentSetup attach_smoke_present(EditorWindow& window, WindowMode mode)
{
    PresentSetup setup;
    if (mode == WindowMode::headless)
    {
        window.compositor().attach_cpu(std::make_unique<render::present::MemoryBlitter>(),
                                       window.backend().client_size());
        setup.blitter_name = kMemoryBlitterName;
        setup.ok = true;
        return setup;
    }

    // The REAL OS blitter, selected by the SHIPPING code path from the REAL native handle — not a
    // blitter this seam picked. That is the whole point: `attach_cpu_present()` is what
    // `context_editor` itself calls on a GPU-less boot, so a break in it breaks the product too.
    window.attach_cpu_present();
    render::present::IPresentBlitter* blitter = window.compositor().blitter();
    if (blitter == nullptr)
    {
        // The WINDOW's diagnostic first, and it is the one that matters: `attach_cpu_present()`
        // records what `make_present_blitter` actually refused (no X display connection / no X11
        // headers in this build / a Wayland surface / no presentable native window), whereas the
        // compositor's is one fixed string naming every platform's failure at once. On nine
        // blocking gates the difference is between a log that says WHICH cause fired and one that
        // makes the reader guess.
        if (!window.diagnostic().empty())
        {
            setup.diagnostic = window.diagnostic();
        }
        else if (!window.compositor().diagnostic().empty())
        {
            setup.diagnostic = window.compositor().diagnostic();
        }
        else
        {
            setup.diagnostic = "the CPU present path attached no OS blitter";
        }
        return setup;
    }
    setup.blitter_name = blitter->name();
    // DEFENCE IN DEPTH, not a branch the shipping factories can reach today: `make_present_blitter`
    // returns a win32/X11/Cocoa blitter or nullptr, never a MemoryBlitter. It is kept because the
    // claim it enforces ("real mode never presents into memory") must survive someone giving that
    // selection a memory fallback; the null case above is the one a real refusal takes.
    if (setup.blitter_name == kMemoryBlitterName)
    {
        setup.diagnostic = "the CPU present path resolved to the in-memory blitter, which is the "
                           "degrade --real-window exists to forbid";
        return setup;
    }
    if (!window.compositor().diagnostic().empty())
    {
        setup.diagnostic = window.compositor().diagnostic();
        return setup;
    }
    setup.ok = true;
    return setup;
}

bool inject_event(IWindowBackend& backend, WindowMode mode, const ShellEvent& event)
{
    if (mode == WindowMode::headless)
    {
        // The one place the concrete headless backend is still named — and the reason it is safe:
        // headless mode CONSTRUCTED that backend, so this cast cannot be wrong. Real mode never
        // reaches it, which is what removes the `static_cast<HeadlessWindowBackend&>` that used to
        // sit in the smokes themselves (issue #408).
        if (std::strcmp(backend.name(), "headless") != 0)
        {
            return false;
        }
        static_cast<HeadlessWindowBackend&>(backend).post(event);
        return true;
    }

    // THE ANTI-DEGRADE GUARD, and it is checked before ANY injection path — including the resize
    // one below, which would otherwise "work" against the headless backend's own placement
    // bookkeeping and hand a real-mode smoke a size change no window system ever granted.
    if (std::strcmp(backend.name(), "headless") == 0)
    {
        return false;
    }

    // A resize is a REQUEST to the window system in real mode, never a synthesized event: the size
    // the Shell must react to is the one the SERVER grants, which arrives later as a real
    // ConfigureNotify. Handled before the X11 guard below because it needs no Xlib at all — a Win32
    // or Cocoa real window answers `apply_placement` with its own configure notification too.
    //
    // ⚠ The read-modify-write carries the CURRENT position and maximized state back in, which is
    // exactly right under xvfb (no window manager, the CI configuration) but is a known limitation
    // on a WM'd desktop such as WSLg: `placement()` reports the ROOT-relative origin, and a
    // reparenting WM reads a move request as the FRAME origin, so a resize also nudges the window
    // by the frame offset — and a MAXIMIZED window would be un-maximized and re-maximized around
    // the request (see X11WindowBackend::apply_placement). Neither affects the smokes, which never
    // maximize and never assert a position; both are why this seam asserts the size the server
    // GRANTS rather than the one it asked for.
    if (event.kind == ShellEventKind::resize)
    {
        if (render::is_empty(event.size))
        {
            return false;
        }
        WindowPlacement placement = backend.placement();
        // ⚠ UNIT CONVERSION, NOT A NO-OP. `event.size` is PHYSICAL PIXELS (see the header) while
        // `placement()` is physical pixels on X11/Win32 and COCOA POINTS on macOS. Assigning
        // `event.size` straight through — which this seam did until M9 e12c-3's review — asked a
        // Retina window for twice the size the caller meant, and asked for exactly the right size
        // at 1x, so no assertion on a 1x runner could see it.
        const render::Extent2D requested = placement_extent_for_physical(backend, event.size);
        placement.width = requested.width;
        placement.height = requested.height;
        backend.apply_placement(placement);
        return true;
    }

#if defined(CONTEXT_SHELL_SMOKE_HAS_X11)
    const X11Target target = x11_target(backend);
    if (target.display == nullptr || target.window == 0)
    {
        return false;
    }
    switch (event.kind)
    {
    case ShellEventKind::pointer:
        return inject_pointer_x11(target, event.pointer);
    case ShellEventKind::key:
        return inject_key_x11(target, event.key);
    default:
        return false;
    }
#else
    // Every non-X11 build routes to the Cocoa arm, which is a REAL AppKit injection on macOS
    // (smoke_inject_cocoa.mm) and an honest refusal everywhere else (smoke_inject_cocoa.cpp) — the
    // always-linkable-symbol shape make_cocoa_window_backend uses. So on Windows, and on a Linux
    // build configured without the X11 development headers, this still answers the same honest
    // "real-mode injection has no implementation here" it always did, and every caller treats that
    // false as a failure.
    return inject_cocoa_event(backend, event);
#endif
}

std::uint32_t x11_keysym_for_windows_key_code(std::int32_t windows_key_code)
{
    // Kept NARROW on purpose (see the header): each entry is a key some smoke actually injects, and
    // an uncovered code returns 0 so `inject_event` fails loudly instead of injecting nothing. The
    // values are spelled as the numeric keysyms rather than the X11 macros so this table compiles —
    // and is unit-tested — on every leg, including the ones with no Xlib at all.
    switch (windows_key_code)
    {
    case 0x08: // VK_BACK
        return 0xFF08u; // XK_BackSpace
    case 0x09: // VK_TAB
        return 0xFF09u; // XK_Tab
    case 0x0D: // VK_RETURN
        return 0xFF0Du; // XK_Return
    case 0x1B: // VK_ESCAPE
        return 0xFF1Bu; // XK_Escape
    case 0x20: // VK_SPACE
        return 0x0020u; // XK_space
    case 0x25: // VK_LEFT
        return 0xFF51u; // XK_Left
    case 0x26: // VK_UP
        return 0xFF52u; // XK_Up
    case 0x27: // VK_RIGHT
        return 0xFF53u; // XK_Right
    case 0x28: // VK_DOWN
        return 0xFF54u; // XK_Down
    default:
        break;
    }
    // The letter row: VK 0x41..0x5A are the UPPERCASE codes, and the keysym that produces them
    // unshifted is the LOWERCASE latin one (`a` == 0x61) — the exact inverse of the uppercasing
    // x11_keysym_to_windows_key_code documents.
    if (windows_key_code >= 0x41 && windows_key_code <= 0x5A)
    {
        return static_cast<std::uint32_t>(windows_key_code - 0x41 + 0x61);
    }
    // The digit row: VK 0x30..0x39 are the ASCII digits, and so are their keysyms.
    if (windows_key_code >= 0x30 && windows_key_code <= 0x39)
    {
        return static_cast<std::uint32_t>(windows_key_code);
    }
    return 0;
}

NsVirtualKey ns_virtual_key_for_windows_key_code(std::int32_t windows_key_code)
{
    // Kept NARROW on purpose, and covering exactly the same VK set as the keysym table above so the
    // two real-mode arms accept the same injections: an uncovered code leaves `covered` false and
    // `inject_event` fails loudly instead of injecting nothing.
    //
    // ⚠ A TABLE ALL THE WAY DOWN — no arithmetic anywhere, unlike the X11 inverse, whose letters and
    // digits ARE runs. macOS virtual key codes are POSITIONAL (0x00 is `A`, 0x01 is `S`, 0x06 is
    // `Z` — the physical ASDF row) and the digit row is TRANSPOSED at 5/6 (`6` is 0x16, `5` is
    // 0x17). The
    // values are spelled numerically rather than via <Carbon/HIToolbox/Events.h> so this table
    // compiles — and is unit-tested — on every leg, including the two with no Carbon at all; the
    // window.h `kNsVk*` constants are used wherever one already exists.
    //
    // ⚠ THE `text` COLUMN IS macOS's SPELLING, NOT THE WINDOWS VK INTUITION, because
    // `translate_ns_event` forwards it verbatim to CEF: Backspace produces U+007F (NSDeleteCharacter,
    // where Windows would say U+0008), Return U+000D, Escape U+001B, and an arrow key its AppKit
    // private-use NS*ArrowFunctionKey code point. The letters inject the UNSHIFTED (lowercase)
    // character, exactly as an unmodified key press produces.
    switch (windows_key_code)
    {
    // WARNING every non-printable / non-ASCII `text` below is a UNIVERSAL-CHARACTER-NAME ESCAPE with
    // the character's NAME in a trailing comment, never the raw glyph. This repo ships no UTF-8 BOM
    // and passes no `/utf-8`, so MSVC decodes the source in the system code page and ONE non-ASCII
    // code point inside a `U'...'` constant arrives as several chars — C2015 "too many characters in
    // constant", a HARD error on `build (windows-latest)` that GCC and Apple Clang both accept
    // silently, so neither local host can see it. The landed precedent is
    // `src/editor/shell/tests/test_window.cpp`. (The rule binds CHARACTER literals only; non-ASCII in
    // a comment or a string literal is established house style here.)
    case 0x08: // VK_BACK -> the key engraved "delete" ABOVE Return, which backspaces
        return NsVirtualKey{kNsVkDelete, U'\u007F', true}; // DELETE (NSDeleteCharacter)
    case 0x09: // VK_TAB
        return NsVirtualKey{kNsVkTab, U'\t', true}; // CHARACTER TABULATION
    case 0x0D: // VK_RETURN
        // CARRIAGE RETURN (NSCarriageReturnCharacter) — not U+000A
        return NsVirtualKey{kNsVkReturn, U'\r', true};
    case 0x1B: // VK_ESCAPE
        return NsVirtualKey{kNsVkEscape, U'\u001B', true}; // ESCAPE
    case 0x20: // VK_SPACE
        return NsVirtualKey{kNsVkSpace, U' ', true};
    // The arrows carry AppKit's private-use function-key code points (NSLeftArrowFunctionKey and
    // friends), which is what a real arrow press puts in -characters.
    case 0x25: // VK_LEFT
        return NsVirtualKey{kNsVkLeftArrow, U'\uF702', true}; // NSLeftArrowFunctionKey
    case 0x26: // VK_UP
        return NsVirtualKey{kNsVkUpArrow, U'\uF700', true}; // NSUpArrowFunctionKey
    case 0x27: // VK_RIGHT
        return NsVirtualKey{kNsVkRightArrow, U'\uF703', true}; // NSRightArrowFunctionKey
    case 0x28: // VK_DOWN
        return NsVirtualKey{kNsVkDownArrow, U'\uF701', true}; // NSDownArrowFunctionKey
    // --- the letter row, in VK order; the macOS codes are the KEYBOARD's order, not the alphabet's
    case 'A': return NsVirtualKey{kNsVkAnsiA, U'a', true};
    case 'B': return NsVirtualKey{0x0B, U'b', true};
    case 'C': return NsVirtualKey{0x08, U'c', true};
    case 'D': return NsVirtualKey{0x02, U'd', true};
    case 'E': return NsVirtualKey{0x0E, U'e', true};
    case 'F': return NsVirtualKey{0x03, U'f', true};
    case 'G': return NsVirtualKey{0x05, U'g', true};
    case 'H': return NsVirtualKey{0x04, U'h', true};
    case 'I': return NsVirtualKey{0x22, U'i', true};
    case 'J': return NsVirtualKey{0x26, U'j', true};
    case 'K': return NsVirtualKey{0x28, U'k', true};
    case 'L': return NsVirtualKey{0x25, U'l', true};
    case 'M': return NsVirtualKey{0x2E, U'm', true};
    case 'N': return NsVirtualKey{0x2D, U'n', true};
    case 'O': return NsVirtualKey{0x1F, U'o', true};
    case 'P': return NsVirtualKey{0x23, U'p', true};
    case 'Q': return NsVirtualKey{0x0C, U'q', true};
    case 'R': return NsVirtualKey{0x0F, U'r', true};
    case 'S': return NsVirtualKey{kNsVkAnsiS, U's', true};
    case 'T': return NsVirtualKey{0x11, U't', true};
    case 'U': return NsVirtualKey{0x20, U'u', true};
    case 'V': return NsVirtualKey{0x09, U'v', true};
    case 'W': return NsVirtualKey{0x0D, U'w', true};
    case 'X': return NsVirtualKey{0x07, U'x', true};
    case 'Y': return NsVirtualKey{0x10, U'y', true};
    case 'Z': return NsVirtualKey{kNsVkAnsiZ, U'z', true};
    // --- the digit row. NOT monotonic: `6` (0x16) comes BEFORE `5` (0x17).
    case '0': return NsVirtualKey{0x1D, U'0', true};
    case '1': return NsVirtualKey{0x12, U'1', true};
    case '2': return NsVirtualKey{0x13, U'2', true};
    case '3': return NsVirtualKey{0x14, U'3', true};
    case '4': return NsVirtualKey{0x15, U'4', true};
    case '5': return NsVirtualKey{kNsVkAnsi5, U'5', true};
    case '6': return NsVirtualKey{kNsVkAnsi6, U'6', true};
    case '7': return NsVirtualKey{0x1A, U'7', true};
    case '8': return NsVirtualKey{0x1C, U'8', true};
    case '9': return NsVirtualKey{0x19, U'9', true};
    default:
        break;
    }
    return NsVirtualKey{};
}

std::uint32_t ns_extent_to_points(std::uint32_t physical, DpiScale dpi)
{
    if (physical == 0u || dpi.dpi == 0u)
    {
        return 0u;
    }
    // INTEGER round-to-nearest, deliberately: `ns_extent_to_physical` multiplies by the float
    // `factor()`, and inverting through the same float would make the round trip lossy at scales
    // whose factor is not exactly representable (1.25x, 1.5x). `dpi` IS the source of truth —
    // `factor()` is the derived value (dpi.h) — so dividing by it recovers the points exactly
    // wherever the forward direction was exact. u64 intermediate so a large extent cannot overflow
    // the *96.
    const std::uint64_t scaled = static_cast<std::uint64_t>(physical) * kReferenceDpi;
    const std::uint64_t rounded = (scaled + dpi.dpi / 2u) / dpi.dpi;
    // A non-empty extent never becomes empty: `apply_placement` refuses an empty rect outright, so
    // a 1px-tall window at 10x must still ask for 1 point rather than silently asking for nothing.
    return rounded == 0u ? 1u : static_cast<std::uint32_t>(rounded);
}

NsViewPointPoints ns_view_point_for_physical(PointI position, double height_points, DpiScale dpi)
{
    const double factor = ns_exact_dpi_factor(dpi);
    // ⚠ THE SCALE COMES OFF FIRST, AND THE FLIP IS AGAINST THE UNSCALED HEIGHT — the mirror image
    // of the ordering `ns_view_point_to_physical` documents ("Done BEFORE the scale, against the
    // height in POINTS, because that is the space the height is expressed in"). Flipping against a
    // height in one space while the coordinate is in another is the shape that reads correct at 1x
    // and mirrors the pointer on a Retina display, so the two orderings must stay mirror images.
    const double y_in_points = static_cast<double>(position.y) / factor;
    return NsViewPointPoints{static_cast<double>(position.x) / factor, height_points - y_in_points};
}

NsDeliveredShift ns_delivered_shift_for_window_move(PointI origin_at_post_points,
                                                    PointI origin_at_delivery_points, DpiScale dpi)
{
    const double factor = ns_exact_dpi_factor(dpi);
    // A delivered locationInWindow gains (post - delivery) on BOTH axes. x reaches Shell space
    // unflipped, so it keeps that sign; y is subtracted from the view height by the decoder, so its
    // Shell-space displacement is the NEGATION — (delivery - post). See the header for why only a
    // non-zero delta can distinguish the two spellings.
    const double dx_points = static_cast<double>(origin_at_post_points.x) -
                             static_cast<double>(origin_at_delivery_points.x);
    const double dy_points = static_cast<double>(origin_at_delivery_points.y) -
                             static_cast<double>(origin_at_post_points.y);
    // Round-half-AWAY-from-zero, matching the shipping decoder's own `ns_round_to_int` (window.cpp)
    // rather than `static_cast`'s truncate-toward-zero: a displacement of -0.6 and one of +0.6 must
    // not both land on 0, or the correction's bucket straddling the origin is twice as wide as every
    // other one. Both inputs are integer points and the factor is bounded, so no isfinite guard is
    // needed here — unlike in `ns_round_to_int`, which also sees live Cocoa geometry reads.
    return NsDeliveredShift{static_cast<std::int32_t>(std::lround(dx_points * factor)),
                            static_cast<std::int32_t>(std::lround(dy_points * factor))};
}

render::Extent2D placement_extent_for_physical(const IWindowBackend& backend,
                                               render::Extent2D physical)
{
    // Identified by the published native window KIND, the same discriminator every other Cocoa arm
    // of this seam uses — never `#if defined(__APPLE__)`, which would wrongly convert for a
    // headless or X11 backend that happens to have been built on macOS.
    if (backend.native_window().kind != render::NativeWindowKind::MetalLayer)
    {
        return physical;
    }
    const DpiScale dpi = backend.dpi();
    return render::Extent2D{ns_extent_to_points(physical.width, dpi),
                            ns_extent_to_points(physical.height, dpi)};
}

} // namespace context::editor::shell::smoke
