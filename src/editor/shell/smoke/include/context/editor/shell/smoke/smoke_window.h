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
//   2. `inject_event` in `WindowMode::real` NEVER posts into a queue THE SMOKE OWNS. On Linux,
//      pointer + key events are sent to the smoke's own window through the X SERVER (XSendEvent
//      with an empty event-mask, which the protocol delivers back to the window's creating client),
//      so they re-enter the Shell through the same XNextEvent + `translate_x11_event` path a
//      hardware event takes. On macOS they go onto the APPLICATION's own queue with
//      `-[NSApplication postEvent:atStart:]` and come back out of the same `nextEventMatchingMask`
//      pump + `translate_ns_event` decoder — a queue AppKit owns, never one this seam wrote. Either
//      way a resize is REQUESTED with `apply_placement()` so the size change arrives as the window
//      system's own configure/geometry notification. A `post()`-shaped seam on the real backend
//      would have bypassed the window system, the decoder and the whole window path — passing just
//      as happily with all three broken, which is what issue #408 asks NOT to duplicate.
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
    // `EditorWindow::attach_cpu_present()` selects from the REAL native handle. TWO arms run this
    // in CI, both inside the `editor-cef-smoke` job: Linux/X11 (the job already carries xvfb +
    // libx11-dev + libxext-dev) and, since M9 e12c-3, macOS/Cocoa — the hosted `macos-latest`
    // runner has a real window-server session (MEASURED in CI: `launchctl managername` prints
    // `Aqua` and `/dev/console` is owned by `runner`; run 30238034460, job 89889609193).
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

// The centre of a published region's rect, in the PHYSICAL client pixels `hit_test_frame` consumes —
// the point a smoke probes to ask what the frame answers over a LIVE rect. Named and shared for the
// same reason as every conversion in this header: two CEF smokes (window 0's boot proof and the
// tear-out's factory-window proof) probe identically, and inlined copies of the arithmetic can only
// drift.
[[nodiscard]] PointI region_mid(const ShellRegion& region);

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
    // The blitter that actually attached ("memory", "x11-shm", "x11-putimage", "cocoa-calayer",
    // "win32-gdi", ...). Recorded so a caller asserts what it GOT rather than what it asked for.
    std::string blitter_name;
};

// Attach the C-F2 CPU present path for `mode`, at the window's CURRENT client extent.
//
//   * headless — e03's MemoryBlitter, the honest present target for an offscreen shell.
//   * real — `EditorWindow::attach_cpu_present()`, which selects the REAL OS blitter from the REAL
//     native window (X11 MIT-SHM on Linux, a `CALayer.contents` blit on macOS). A missing blitter,
//     a compositor diagnostic, or a blitter that resolved to the in-memory one are all reported as
//     `ok == false`.
[[nodiscard]] PresentSetup attach_smoke_present(EditorWindow& window, WindowMode mode);

// Deliver `event` to `backend` the way `mode` demands. Returns false when it could not be
// delivered — an unmapped key, a backend that is not the one the mode requires, an empty resize
// extent, or a build with NEITHER X11 development headers NOR AppKit (a Windows real-mode build, or
// a Linux one configured without X11: both route to the Cocoa arm's honest refusal). The macOS arm
// adds its own refusals, all of them event SHAPES no public `NSEvent` factory can express: a
// `MouseButton::middle` or `MouseButton::none` press, a wheel or leave sample, a
// `KeyAction::character`, a nil `NSApp`, and a window not findable from the layer the backend
// publishes. A false is ALWAYS a smoke failure; nothing here degrades quietly.
//
//   * headless — `HeadlessWindowBackend::post()`, delivered by the next `pump()`.
//   * real, Linux — a genuine X server round trip:
//       - pointer + key: XSendEvent to the smoke's own window, decoded by the real
//         `translate_x11_event` on the way back in;
//   * real, macOS — a genuine AppKit queue round trip: a synthesized NSEvent posted with
//     `-[NSApplication postEvent:atStart:]`, dequeued by the backend's own
//     `nextEventMatchingMask` pump and decoded by the real `translate_ns_event` (see the header
//     preamble for why this needs no TCC grant, and for the THREE fidelity limits it carries).
//   * real, either — resize: `apply_placement()`, so the new geometry is the window system's own
//     configure/geometry change.
//     ⚠ That makes a real-mode resize ASYNCHRONOUS: the caller must pump until
//     `backend.client_size()` changes rather than asserting straight after the call.
//     ⚠ `event.size` is in PHYSICAL PIXELS — the unit `client_size()` and every other `ShellEvent`
//     speak — and this seam converts it to the units `placement()` uses with
//     `placement_extent_for_physical` below. Callers pass physical pixels on every platform and do
//     NOT pre-convert.
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
// not-in-the-table sentinel the way X11's `NoSymbol` legitimately does. A `covered` flag is the
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

// ⚠ THE TWO UNIT SYSTEMS THIS SEAM STRADDLES, and the reason the next two functions exist at all.
//
// `client_size()` is PHYSICAL PIXELS on every backend, and so is every `ShellEvent` position and
// extent the Shell handles. `placement()` is physical pixels on Win32 and X11 — but DELIBERATELY
// COCOA POINTS on macOS: `CocoaWindowBackend::placement()` carries the "⚠ IN COCOA POINTS" note and
// the reason (the document is per-machine session state that backend alone writes and reads, so
// points round-trip exactly through `apply_placement`, while a conversion would need a screen
// height and still not make a macOS placement meaningful on another OS).
//
// So writing a `client_size()`-derived number straight into a `WindowPlacement` asks a Retina
// window for TWICE the size intended — and asks for exactly the right size at 1x, which is what
// makes it a latent bug rather than an obvious one. It is a NAMED, UNIT-TESTED conversion rather
// than arithmetic inlined at a call site for the same reason `ns_extent_to_physical` is exported
// rather than file-local (see its comment in `window.cpp`): two copies of one conversion feeding
// one observable, with only the copy no CI leg executes able to drift.

// PHYSICAL pixels -> COCOA POINTS: the exact inverse of the shipping `ns_extent_to_physical`
// (`window.h`), round-to-nearest. A non-empty input never becomes empty — it floors at 1, the same
// rule `to_logical` states for the forward direction, because a 0 in a `WindowPlacement` is refused
// by `apply_placement` as an empty rect.
[[nodiscard]] std::uint32_t ns_extent_to_points(std::uint32_t physical, DpiScale dpi);

// The extent a `WindowPlacement` must carry to request `physical` PHYSICAL pixels of CLIENT area
// from `backend`.
//
// Identity on every backend whose placement is already physical pixels; `ns_extent_to_points` on
// the Cocoa one. The Cocoa backend is identified by the native window KIND it publishes
// (`MetalLayer`) — never by a compile-time `__APPLE__`, so a headless or X11 backend built on macOS
// still gets the identity, which is what keeps this correct for `mode_of`-selected windows.
[[nodiscard]] render::Extent2D placement_extent_for_physical(const IWindowBackend& backend,
                                                             render::Extent2D physical);

// A window-space Cocoa location, in POINTS with Cocoa's BOTTOM-left origin.
struct NsViewPointPoints
{
    double x_points = 0.0;
    double y_points = 0.0;
};

// The view-space Cocoa POINT that delivers `position` (Shell-space PHYSICAL pixels, TOP-left
// origin) — the exact inverse of the shipping `ns_view_point_to_physical` (`window.h`), given the
// view's height in points.
//
// ⚠ WHY THIS IS HERE AND NOT INLINE IN THE OBJECTIVE-C++ ARM, which is where it started. It needs
// no AppKit at all — only a height, a scale and a `PointI` — so in the `.mm` it compiled on ONE leg
// of three and, being file-local there, was reachable by no test. Two things follow from that, and
// the second is why this hoist happened during review rather than being left as tidying:
//
//   * `window.h` writes the hazard down for the FORWARD direction: "On a 1x display the two are
//     equal, which is exactly why a missing scale ships looking correct and breaks on Retina only."
//     The inverse has the identical property. * MEASURED, not assumed: with the arithmetic inline,
//     mutating it to divide AFTER the flip instead of before — the exact error
//     `ns_view_point_to_physical`'s own comment warns about — left `editor-shell-cocoa-window`
//     GREEN on a 2x host, because that smoke can only assert the flip DIRECTION, the separation and
//     a half-plane (the delivered location is scaled about the window centre, so no equality is
//     available to it). A wrong scale preserves all three. The round-trip sweep in
//     `editor-shell-test_smoke_window` is what actually catches it, on every leg.
//
// The AppKit half — reading `[view bounds]` and the view->window conversion — deliberately stays in
// the `.mm`, which is the same split `ns_extent_to_physical` and the key tables already use.
[[nodiscard]] NsViewPointPoints ns_view_point_for_physical(PointI position, double height_points,
                                                           DpiScale dpi);

// ⚠ THE THIRD COCOA FIDELITY LIMIT, and the one that cost a red `main`: A POSTED LOCATION IS
// RESOLVED AGAINST THE WINDOW'S FRAME ORIGIN AT DEQUEUE TIME, NOT AT POST TIME.
//
// `+[NSEvent mouseEventWithType:location:...windowNumber:]` stores its `location` GLOBALLY (screen
// space) — it adds the window's frame origin as it is when the event is CREATED — while
// `-locationInWindow` subtracts the frame origin as it is when the event is READ. So if anything
// moves the window between `-[NSApplication postEvent:atStart:]` and the
// `-nextEventMatchingMask:` that dequeues it, EVERY already-queued sample arrives displaced by
// exactly the negation of that move, and the shipping decoder — correctly — reports the displaced
// location it was handed.
//
// MEASURED on the macOS host with a standalone AppKit probe (a 380x240-point window, 2x, posting the
// smoke's own two samples and then moving the window before draining).
//
// ⚠ EVERY FIGURE IN THIS TABLE IS IN COCOA POINTS — including the derived "Shell-space y" column,
// which is taken against the 240-POINT height (240 - 292.9 = -52.9). The probe worked in the window's
// own point space. Shell space in the SHIPPING code is PHYSICAL PIXELS, and so is what this function
// returns, so at the probe's 2x the corresponding physical figure is TWICE the one shown here
// (-52.9 points = -105.8 physical px). Do not mix the two units when re-deriving any of this; the
// unit test below pins the function in physical pixels, which is why it must carry the dpi factor.
//
//   * no move       -> delivered y 211.4 / 28.0 for posted 210 / 30, i.e. faithful to ~2 points;
//   * moved DOWN 80 -> delivered y 292.9 / 109.5, so the TOP sample flips to a NEGATIVE Shell-space
//                      y (-52.9) while the order, the separation and the half-plane all still hold —
//                      exactly ONE failing assertion, which is bit-for-bit the CI signature;
//   * moved UP 80   -> delivered y 129.9 / -53.5, i.e. a sample BELOW the window that a one-sided
//                      `y >= 0` range check passes in silence.
//
// The clean, no-move distortion was measured in the same probe and is a ~1.9% scale about (roughly)
// the view centre — about 2 points at these sample positions, and INDEPENDENT of where the window
// sits on screen. It therefore cannot produce the ~35-point displacement the failure needs, which is
// what rules the conversion arithmetic out as the cause and rules a window move in.
//
// This function is the correction: the PHYSICAL-PIXEL, Shell-space (TOP-left) displacement that a
// frame-origin change from `origin_at_post` to `origin_at_delivery` imposes on every sample posted
// before the move. Both origins are COCOA POINTS with Cocoa's BOTTOM-left screen origin — i.e.
// exactly what `CocoaWindowBackend::placement()` reports (see its "⚠ IN COCOA POINTS" note) — so a
// caller reads them straight off `placement()` and needs no AppKit and no screen height.
//
// ⚠ THE TWO AXES CARRY OPPOSITE SIGNS, and that asymmetry is the y-flip itself rather than a typo:
// a delivered `locationInWindow` gains `origin_at_post - origin_at_delivery` on BOTH axes, but x
// passes through the decoder unflipped while y is subtracted from the view height, so the Shell-space
// y displacement comes out as `origin_at_delivery.y - origin_at_post.y`. Subtracting a value with
// the wrong sign would DOUBLE the error instead of cancelling it, and at a zero delta — the ordinary
// case — both spellings are a no-op, so only a test with a non-zero origin delta can tell them
// apart. That is why this is a named, unit-tested function and not arithmetic inlined at its one
// call site.
// A DISPLACEMENT, which is why this is not the otherwise identically-shaped `PointI`: every claim
// about it is about a difference (it is zero when nothing moved, and it is SUBTRACTED from a delivered
// coordinate), and naming it as a position invites exactly the "corrected by adding an origin" misread
// the opposite-sign paragraph above exists to prevent. Same reasoning as the sibling
// `NsViewPointPoints`, which is likewise a bespoke pair rather than a reused point type.
struct NsDeliveredShift
{
    std::int32_t dx = 0;
    std::int32_t dy = 0;
};

[[nodiscard]] NsDeliveredShift ns_delivered_shift_for_window_move(PointI origin_at_post_points,
                                                                  PointI origin_at_delivery_points,
                                                                  DpiScale dpi);

} // namespace context::editor::shell::smoke
