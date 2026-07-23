// The Shell's WINDOW REGISTRY vocabulary (design 03 §1, §7) — M9 e10a.
//
// 03 §1 says "`WindowManager` owns N `EditorWindow`s ... window 0 hosts the app menu + welcome
// screen; windows are docking peers otherwise". Until e10a that N was structurally 1: the app built
// one window inline and adopted it, and nothing could make a second one at RUNTIME. This header is
// the vocabulary that makes N real — the spec of a window, what ONE window owns, how a window is
// created and destroyed on demand, and what happens LOUDLY when creation fails.
//
// It deliberately holds NO panel concept. e10a is the pure group-B foundation: a second native
// window that exists, boots its own editor-core instance, and tears down cleanly. Tear-out, rehome
// and cross-window drag (e10b–e10d) target the registry declared here; none of them is implemented.
//
// ─────────────────────────────────────────────────────────────────────────────────────────────────
// THE LIFETIME RULE, which is the whole reason a "session" exists as a named thing (CE #319).
//
// A window's CEF browser holds a raw `BridgeRouter*` inside its message-router handler for the
// browser's ENTIRE life — and "the browser's life" does not end when `CloseBrowser` returns. CEF
// finishes tearing browser/frame state down inside `CefShutdown()`, still dispatching frame work to
// the client. CE #319 was exactly that: the router died first and `CefShutdown` faulted with an
// ACCESS_VIOLATION on the Session-0 Windows runner.
//
// With N windows there are now N such routers, and — new in e10a — a window can be destroyed
// MID-PROCESS, long before `CefShutdown`. So the objects a window's browser can reach are grouped
// into ONE session (this file's `WindowSessionParts`), and the registry never destroys a retired
// session eagerly: it holds it until the WindowManager itself is destroyed, which the app must
// sequence AFTER `shell::cef::shutdown()`. Closing the browser is not proof CEF is done with the
// client; outliving `CefShutdown` is.
//
// Declaration order inside a session IS its destruction order reversed, and it is load-bearing:
// browser → backend → daemon client → bridge → the surfaces the bridge's handlers captured.
// ─────────────────────────────────────────────────────────────────────────────────────────────────

#pragma once

#include "context/editor/client/client.h"
#include "context/editor/shell/browser.h"
#include "context/editor/shell/editor_state.h"
#include "context/editor/shell/ipc_bridge.h"
#include "context/editor/shell/window.h"
#include "context/render/rhi.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace context::editor::shell
{

// Windows are PEERS addressable by id. Ids are minted in creation order, never reused, and
// **window 0 is primary** — it hosts the app menu + welcome screen (D13), so it is the rehome
// target e10b serialises panels back to and the one window `destroy_window` refuses to take away.
using WindowId = std::uint32_t;

inline constexpr WindowId kPrimaryWindowId = 0;
inline constexpr WindowId kInvalidWindowId = static_cast<WindowId>(-1);

// A hard ceiling on LIVE windows. Not a UI limit — a containment one: `window.open` is suppressed
// (03 §1) precisely so a renderer cannot mint windows, and once e10b lets editor-core ASK for one,
// an unbounded ask is a denial-of-service with N CEF browsers behind it. 16 is far past any real
// multi-monitor layout and far short of a resource problem.
inline constexpr std::size_t kMaxEditorWindows = 16;

// Why a create attempt did not produce a window. Every value is REPORTED, never swallowed: the
// degradation seam of 03 §7 ("secondary-window create fails → popout degrades to a floating
// Dockview group inside the source window, LOUDLY") can only be honest if the Shell says which of
// these happened.
enum class WindowCreateOutcome
{
    created,
    no_factory,       // nothing bound a window factory — a build with no way to make a window
    factory_failed,   // the factory reported a failure (no native window, browser did not start …)
    incomplete_parts, // the factory reported success but produced no backend and/or no browser
    limit_reached,    // kMaxEditorWindows live windows already
};

enum class WindowDestroyOutcome
{
    destroyed,
    unknown_window,  // no live window carries that id
    primary_refused, // window 0 hosts the menu/welcome screen; it is closed by app shutdown only
};

[[nodiscard]] const char* to_string(WindowCreateOutcome outcome);
[[nodiscard]] const char* to_string(WindowDestroyOutcome outcome);

// What to create. `state_index` indexes `EditorState::windows` for placement persistence — the same
// meaning `EditorWindowConfig::state_index` has, threaded here so a created window remembers where
// it was, exactly like window 0.
struct WindowSpec
{
    std::string title = "Context Editor";
    render::Extent2D logical_size{1280, 800};
    // Never open an OS window (CI / Session 0 / a remote box). The registry does not decide this —
    // the factory does — but it travels with the request so the factory need not consult globals.
    bool headless = false;
    std::size_t state_index = 0;
    std::optional<WindowPlacement> placement;
};

// Everything ONE window owns. See the LIFETIME RULE in the file header: member order here is
// destruction order REVERSED, so the browser dies first and the surfaces its bridge captured die
// last. Do not reorder these without reading that block.
struct WindowSessionParts
{
    // Whatever the per-window bridge surfaces are (a handshake, later a panel host). Type-erased on
    // purpose: the registry must OUTLIVE them correctly without knowing what they are — knowing
    // would drag panels into e10a, which is precisely what this task is not.
    std::vector<std::shared_ptr<void>> surfaces;

    // THIS window's privileged native↔JS channel. Each window gets its OWN router: a shared one
    // would make every window's editor-core reach the same handlers, and a handler could not tell
    // which window asked — the exact ambiguity e10b's tear-out has to resolve. May be null in a
    // build with no browser.
    std::unique_ptr<BridgeRouter> bridge;

    // THIS window's own wire connection to the daemon, and therefore its own `origin` (e08a: ids
    // are minted per WIRE CONNECTION, so N windows are genuinely N origins). Optional: a window may
    // run detached, in which case its origin is 0 — which e08a also spells "not attached", so a
    // consumer must never read 0 as an identity.
    std::unique_ptr<client::Client> daemon_client;

    std::unique_ptr<IWindowBackend> backend;
    std::unique_ptr<IBrowserHost> browser;

    // Non-fatal notes from creation (e.g. "no native window backend on this platform; running
    // offscreen"). Reported by the caller; never a reason to fail.
    std::string diagnostic;
};

// Build one window's session. Returns false + a non-empty `error` on failure — a factory that
// returns false with an empty error is itself reported (the Shell would otherwise say a window
// failed for no stated reason). Bound by the app, because only the app knows how to make a browser.
using WindowFactory =
    std::function<bool(const WindowSpec& spec, WindowSessionParts& parts, std::string& error)>;

struct WindowCreateResult
{
    WindowCreateOutcome outcome = WindowCreateOutcome::no_factory;
    WindowId id = kInvalidWindowId;
    // Empty exactly when `ok()`.
    std::string error;

    [[nodiscard]] bool ok() const { return outcome == WindowCreateOutcome::created; }
};

struct WindowDestroyResult
{
    WindowDestroyOutcome outcome = WindowDestroyOutcome::unknown_window;
    std::string error;

    [[nodiscard]] bool ok() const { return outcome == WindowDestroyOutcome::destroyed; }
};

// THE DEGRADATION SEAM (03 §7). A failed secondary-window creation is reported through this, once
// per attempt, with everything the consumer needs to degrade: which window asked, what was asked
// for, which failure class, and why.
//
// e10a builds the seam and the loud report; **e10b owns the behaviour** (fall back to a floating
// Dockview group inside the source window). Deliberately a C++ callback rather than a new bridge
// method: a new boot-time bridge surface must be installed in EVERY smoke or the router's
// deny-unknown-methods default reddens the ones that were not updated (the e06d regression), and
// e10b — which needs a bridge-shaped report because editor-core is what degrades — can add exactly
// one method then, wired to this sink.
struct WindowCreateFailure
{
    WindowCreateOutcome outcome = WindowCreateOutcome::factory_failed;
    // The window the request came from — where e10b's floating group belongs.
    WindowId source = kPrimaryWindowId;
    std::string title;
    std::string error;
};

using WindowCreateFailureSink = std::function<void(const WindowCreateFailure&)>;

// One human-readable line for a failure. Pure, so the wording is asserted by a test rather than
// eyeballed in a log — a degradation report that reads as a shrug is how a real failure gets
// mistaken for a design choice.
[[nodiscard]] std::string describe(const WindowCreateFailure& failure);

// What a factory MUST have produced for the parts to be usable: a backend and a browser. The bridge
// and the daemon client are optional (a CEF-free build has no browser bridge; a detached window has
// no connection). false + `error` names the missing piece.
[[nodiscard]] bool validate_window_parts(const WindowSessionParts& parts, std::string& error);

} // namespace context::editor::shell
