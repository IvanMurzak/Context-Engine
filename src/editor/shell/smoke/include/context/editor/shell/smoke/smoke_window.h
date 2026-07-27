// The SMOKE-TIER window seam (M9 e12a-x11-legs, issue #408) — how a LIVE shell smoke chooses
// between the honest offscreen shell and a REAL OS window, in one place instead of nine.
//
// WHAT IT REPLACES, and why that mattered. Every `editor-cef-smoke-shell*` smoke hard-coded
// `std::make_unique<HeadlessWindowBackend>(desc)` plus
// `compositor().attach_cpu(std::make_unique<MemoryBlitter>(), size)`, and the two that drive input
// reached the queue by keeping a `HeadlessWindowBackend*` (or `static_cast`ing the base reference
// back down to it). So the whole scenario family — restart/restore, the palette, settings,
// multi-window, tear-out, cross-window drag, the ui mirror, the package iframe — proved its claims
// against a window with no OS behind it, and NOTHING said whether those scenarios survive a real
// window's event source, a real present target, or a real server-granted geometry. e12a shipped the
// X11 backend and a single dedicated smoke for it (`editor-shell-x11-window`); this seam is what
// lets the REAL scenarios ride it too.
//
// THE TWO RULES THAT KEEP IT FROM BEING VACUOUS:
//
//   1. `WindowMode::real` NEVER degrades to headless. `make_smoke_window` reports a null backend
//      plus a diagnostic when the platform window could not be created, and `attach_smoke_present`
//      reports `ok == false` when the OS blitter did not resolve (or resolved to the in-memory
//      one). A smoke asked for a real window either gets one or fails — a silent fallback is
//      exactly how a compiled-out X11 path sails through a blocking gate green forever.
//   2. `inject_event` in `WindowMode::real` does NOT post into a queue. Pointer + key events are
//      sent to the smoke's own window through the X SERVER (XSendEvent with an empty event-mask,
//      which the protocol delivers back to the window's creating client), so they re-enter the
//      Shell through the same XNextEvent + `translate_x11_event` path a hardware event takes; a
//      resize is REQUESTED with `apply_placement()` so the size change arrives as the server's own
//      ConfigureNotify. A `post()`-shaped seam on the real backend would have bypassed the server,
//      the decoder and the whole window path — passing just as happily with all three broken,
//      which is what issue #408 asks NOT to duplicate.
//
// WHY XSendEvent RATHER THAN XTEST. XTEST injects at the server's input pipeline, one step closer
// to real hardware — but it needs `libXtst` (a new CI package), it delivers to whatever holds
// pointer/keyboard focus, and under `xvfb` there is no window manager to give a window focus at
// all. That is a flaky blocking gate waiting to happen, and e12a already shipped one. XSendEvent
// needs no new dependency, is deterministic regardless of focus, and still makes the full
// client -> server -> client round trip through the real decoder; the one thing it cannot claim is
// `send_event == False`, which nothing in the Shell inspects.
//
// WHY -[NSApplication postEvent:atStart:] RATHER THAN CGEventPost ON macOS (M9 e12c-3). The X11
// choice above has a direct macOS analogue and the same reasoning decides it, but there the stakes
// are higher: `CGEventPost` / `CGEventTapCreate` cross the HID/system boundary, so macOS gates them
// behind a TCC **Accessibility** grant that a human must give in System Settings — un-grantable on a
// GitHub-hosted runner, which would make a CGEventPost-based design worthless as a CI gate.
// `postEvent:` is IN-PROCESS: the event goes onto the app's own queue and comes back out of
// `-[NSApplication nextEventMatchingMask:]`, exactly the pump `CocoaWindowBackend::pump()` runs, so
// nothing crosses that boundary. MEASURED rather than assumed, and measured in the direction that
// makes it decisive: on the reproduction host `CGPreflightPostEventAccess()` and
// `AXIsProcessTrusted()` are BOTH false — no grant is held — and a synthesized move + press +
// release + key still round-tripped 5/5 through `nextEventMatchingMask` with `[event window]`
// resolving to the smoke's own window. It is also the closer analogue of the X11 path, which sends
// to the smoke's OWN window rather than driving the server globally.
//
// ⚠ TWO MEASURED COCOA LIMITS THE X11 ARM DOES NOT HAVE, both documented at the injection site:
//   1. The delivered `locationInWindow` is NOT the requested one. AppKit round-trips a posted mouse
//      location through the window server and returns it scaled about the window's centre — MEASURED
//      ~1.4% on the reproduction host (a 640-point-wide window's edges came back 4.5 points out,
//      linear in the offset from centre, on an exact-2x non-scaled display). So a real-mode smoke
//      must NOT assert an exact position round trip on macOS; assert ORDER and SEPARATION (which no
//      such scaling can invert) or region membership with margin.
//   2. The pressed-BUTTON mask cannot be injected at all. `CocoaWindowBackend::handle` reads it from
//      `+[NSEvent pressedMouseButtons]`, a live HID query, not from the event — so an injected press
//      arrives with `Modifiers::left_button_down` false. The modifier FLAGS (shift/control/alt/meta)
//      DO travel, because those come off the event.
//
// NOTHING HERE LINKS CEF. It is compiled + unit-tested on all three default `build` legs
// (`editor-shell-test_smoke_window`), which is the same layering the rest of the Shell follows: the
// CEF smokes are the only place this seam is exercised in `real` mode, so everything ABOUT the seam
// that can be decided without a browser is decided where every leg can see it.

#pragma once

#include "context/editor/shell/dpi.h"
#include "context/editor/shell/shell.h"
#include "context/editor/shell/window.h"

#include <cstdint>
#include <memory>
#include <string>

namespace context::editor::shell::smoke
{

// Which window a smoke runs over.
enum class WindowMode
{
    // The honest offscreen shell: HeadlessWindowBackend + e03's MemoryBlitter. No OS window, no
    // display required — the ONLY mode that is safe on the Session-0 self-hosted Windows runner,
    // and therefore the default everywhere.
    headless,
    // A REAL OS window through `make_window_backend`, presenting through the REAL OS blitter that
    // `EditorWindow::attach_cpu_present()` selects from the REAL native handle. Linux/X11 in CI
    // (the `editor-cef-smoke` job already carries xvfb + libx11-dev + libxext-dev).
    real,
};

[[nodiscard]] const char* to_string(WindowMode mode);

// `--real-window` on the command line selects `WindowMode::real`; its absence is `headless`.
//
// A FLAG, not an environment variable, and chosen at CMAKE time rather than by probing for a
// display: the ctest registration passes it on the platforms where a real window is required, so a
// build that quietly lost its X11 backend fails LOUDLY instead of skipping. (An env var would also
// have meant a `std::getenv` in every smoke, which MSVC rejects as C4996 under /W4 /WX.)
//
// Safe to call before CEF's `execute_subprocess` has run and safe to call after: it only reads
// argv, and a CEF subprocess never reaches the smoke's own body.
[[nodiscard]] WindowMode window_mode_from_args(int argc, char** argv);

// What `make_smoke_window` produced. `backend` is null exactly when creation failed, and then
// `diagnostic` says why — there is no third state and no silent degrade.
struct WindowSetup
{
    std::unique_ptr<IWindowBackend> backend;
    std::string diagnostic;
};

// Build the smoke's window backend for `mode`.
//
//   * headless — `desc.visible` is forced false and a HeadlessWindowBackend is returned. Cannot
//     fail.
//   * real — `desc.visible` is forced true and `make_window_backend(desc)` decides. A null backend
//     (no display, no GUI session, a build with no X11 development headers, a refused window) is
//     returned AS a failure with the platform's own diagnostic. A backend that reports itself as
//     "headless" is ALSO a failure here, because that is the degrade this mode exists to forbid.
[[nodiscard]] WindowSetup make_smoke_window(WindowDesc desc, WindowMode mode);

// The mode that matches an ALREADY-CREATED backend: `headless` for the offscreen one, `real` for
// any native one.
//
// For windows the smoke did NOT build itself — the ones a `WindowManager` factory produced, which
// honour `WindowSpec::headless` per window. Their present path must match the window that was
// ACTUALLY created rather than the one the run asked for, or a deliberately-offscreen Nth window
// would be handed the real-mode present attach and fail. Never use it for a window this seam built:
// there the run's own mode is the stronger claim, because it is what makes a degrade detectable.
[[nodiscard]] WindowMode mode_of(const IWindowBackend& backend);

// The LOGICAL size + DPI the browser must be told about for `backend`'s window.
//
// A smoke cannot keep using the extent it ASKED for once a real window is involved: the X11 backend
// derives physical pixels from the screen's DPI, so a 640x480 logical request on a 120-dpi display
// is an 800x600 client area, and a CEF view rect (which is DIP) sized from the request would lay the
// document out at the wrong size. Reading both back from the backend is correct in BOTH modes — the
// headless backend answers exactly what it was constructed with.
struct BrowserGeometry
{
    render::Extent2D logical_size;
    DpiScale dpi;
};

[[nodiscard]] BrowserGeometry browser_geometry(const IWindowBackend& backend);

// What `attach_smoke_present` resolved to. `ok` is the whole verdict.
//
// Deliberately NO blitter HANDLE: the compositor owns the blitter and destroys it at `detach()`, so
// handing one back would re-create exactly the non-owning pointer this task removed from the nine
// smokes. Callers assert presenting through `compositor().stats().frames_presented`, which advances
// only when the ATTACHED blitter's `blit()` returned true — the same claim in both modes — and read
// pixels through `compositor().cpu_surface()`.
struct PresentSetup
{
    bool ok = false;
    // Empty on success; otherwise why the CPU present path is not usable in this mode.
    std::string diagnostic;
    // The blitter that actually attached ("memory", "x11-shm", "x11-put-image", "win32-gdi", ...).
    // Recorded so a caller asserts what it GOT rather than what it asked for.
    std::string blitter_name;
};

// Attach the C-F2 CPU present path for `mode`, at the window's CURRENT client extent.
//
//   * headless — e03's MemoryBlitter, the honest present target for an offscreen shell.
//   * real — `EditorWindow::attach_cpu_present()`, which selects the REAL OS blitter from the REAL
//     native window (X11 MIT-SHM on Linux). A missing blitter, a compositor diagnostic, or a
//     blitter that resolved to the in-memory one are all reported as `ok == false`.
[[nodiscard]] PresentSetup attach_smoke_present(EditorWindow& window, WindowMode mode);

// Deliver `event` to `backend` the way `mode` demands. Returns false when it could not be
// delivered — an unmapped key, a backend that is not the one the mode requires, or a build with no
// X11 development headers. A false is ALWAYS a smoke failure; nothing here degrades quietly.
//
//   * headless — `HeadlessWindowBackend::post()`, delivered by the next `pump()`.
//   * real, Linux — a genuine X server round trip:
//       - pointer + key: XSendEvent to the smoke's own window, decoded by the real
//         `translate_x11_event` on the way back in;
//   * real, macOS — a genuine AppKit queue round trip: a synthesized NSEvent posted with
//     `-[NSApplication postEvent:atStart:]`, dequeued by the backend's own
//     `nextEventMatchingMask` pump and decoded by the real `translate_ns_event` (see the header
//     preamble for why this needs no TCC grant, and for the two fidelity limits it carries).
//   * real, either — resize: `apply_placement()`, so the new geometry is the window system's own
//     configure/geometry change.
//     ⚠ That makes a real-mode resize ASYNCHRONOUS: the caller must pump until
//     `backend.client_size()` changes rather than asserting straight after the call.
//
// ⚠ A real-mode pointer PRESS must be followed by its RELEASE. The Shell's pump forwards every
// dequeued event to AppKit (`cocoa_window.mm` shape 4), and a burst of unpaired synthesized
// LeftMouseDowns drove AppKit into a nested mouse-tracking loop that never returned (MEASURED: a
// 60 s hang in an e12c-3 probe). Injecting down/up in pairs — which every smoke already does — is
// the whole discipline.
bool inject_event(IWindowBackend& backend, WindowMode mode, const ShellEvent& event);

// The X keysym `inject_event` sends for a CEF `windows_key_code`, or 0 when this table does not
// cover it. The inverse of `x11_keysym_to_windows_key_code` over the keys the smokes actually
// inject — deliberately NARROW and deliberately loud: an uncovered code makes `inject_event` return
// false, so a future smoke that injects a new key is told to extend the table instead of silently
// injecting nothing.
[[nodiscard]] std::uint32_t x11_keysym_for_windows_key_code(std::int32_t windows_key_code);

// The macOS side of the same idea (M9 e12c-3): the virtual key code AND the `-[NSEvent characters]`
// code unit `inject_event` synthesizes for a CEF `windows_key_code`. The inverse of
// `ns_key_code_to_windows_key_code` over the keys the smokes actually inject — deliberately NARROW
// and deliberately loud, exactly like the keysym table above.
//
// ⚠ WHY THIS IS A STRUCT WHERE THE X11 SIDE RETURNS A BARE VALUE, and it is not stylistic: macOS
// virtual key code **0x00 is a real key** (`kVK_ANSI_A`), so zero cannot double as the
// not-in-the-table sentinel the way X11's `NoSymbol` legitimately does. An `covered` flag is the
// only honest encoding. The struct also carries `text` because a synthesized NSEvent must be handed
// its `characters:` string — macOS puts the character IN the key event rather than in a separate one
// (`translate_ns_event` reads it from there), so the character is part of the injection contract and
// not an afterthought a caller could be expected to supply.
struct NsVirtualKey
{
    // kVK_* — meaningless unless `covered`.
    std::uint32_t key_code = 0;
    // The FIRST UTF-16 code unit `-[NSEvent characters]` must carry, or 0 for a key that produces no
    // text. NOTE macOS's own spellings, which are NOT the Windows VK intuition: Backspace produces
    // U+007F (DELETE, not U+0008), Return produces U+000D, and an arrow key produces its AppKit
    // private-use `NS*ArrowFunctionKey` code point.
    char32_t text = 0;
    bool covered = false;
};

[[nodiscard]] NsVirtualKey ns_virtual_key_for_windows_key_code(std::int32_t windows_key_code);

} // namespace context::editor::shell::smoke
