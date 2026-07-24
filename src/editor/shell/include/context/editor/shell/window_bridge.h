// The WINDOW-MANAGEMENT bridge surface for editor-core (M9 e10b, design 03 §1 / §7, 04 §2).
//
// WHAT THIS IS, AND WHY IT HAS TO EXIST. e10a made a second native window a runtime primitive
// (`WindowManager::create_window` / `destroy_window`, window_registry.h) but added NO bridge method:
// nothing editor-core could call to ASK for one. This surface is that ask, and its answer. It is the
// ONE new boot-time bridge surface e10b introduces (window_registry.h anticipated "e10b ... can add
// exactly one method then"), so — like every sibling bridge — it MUST be installed on EVERY live CEF
// smoke's router or the deny-by-default router turns editor-core's boot-time `window.*` calls into
// `unknown_method` refusals that trip each smoke's `bridge.refused() == 0` invariant (the e06d
// regression, standing lesson #6).
//
// THE ONE MECHANISM (D6). Tear-out, "move to window N", and rehome-on-close are the SAME move: the
// SOURCE window's editor-core SERIALIZES a panel (`panel.state.get`), the Shell RELAYS that seed to a
// TARGET window, and the TARGET window's editor-core RECREATES the panel and restores its state
// (`PanelHost.open` + `panel.state.set`). There is no second recreate path — the divergence D6 exists
// to prevent. The only difference between the moves is WHEN the target reads its seed:
//   * TEAR-OUT   → a NEW window, created here, reads its seed at boot (`window.seed`).
//   * MOVE-TO-N  → an EXISTING window reads it on its cheap `window.rehomed` poll.
//   * REHOME     → a closing window moves EVERY open panel to window 0, delivered the same way as
//                  move-to-N (window 0 is just the target). "Never silently lost" is that relay.
//
// LOUD DEGRADATION (03 §7). `window.tear-out` NEVER throws and NEVER silently succeeds: a failed
// create answers `created:false` with the `WindowCreateOutcome` token and the reason, so the source
// window's editor-core can degrade to a floating Dockview group IN THE SOURCE WINDOW and say so
// loudly. The `WindowManager`'s C++ `on_window_create_failed` sink still fires (for the log); this
// surface is the bridge-shaped report editor-core degrades on.
//
// CEF-FREE and D10 BOUNDARY-CLEAN, exactly like ipc_bridge.h / session_bridge.h and for the same
// reasons: the handler runs nowhere the local dev gate can reach, so the routing + parsing logic
// lives here where tests/test_window_bridge.cpp drives the SAME code the renderer reaches on all
// three default `build` legs, and nothing here touches a kernel-internal module (the panel seed's
// state blob is an OPAQUE `contract::Json`, never interpreted).

#pragma once

#include "context/editor/contract/json.h"
#include "context/editor/shell/ipc_bridge.h"
#include "context/editor/shell/window_registry.h"

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace context::editor::shell
{

// The cross-window drag relay (cross_window_drag.h). Forward-declared: `drag.probe` / `drag.report-zone`
// are served by this surface (they are window management), but the store's full type lives with the
// drag SESSION, so only window_bridge.cpp needs it. A null store makes both methods INERT (the honest
// state of a build/smoke that installed no drag session), which is why the five sibling CEF smokes need
// no change to keep `bridge.refused() == 0` — they get the methods, wired to nothing.
class CrossWindowDragStore;

// --------------------------------------------------------------------------- the wire vocabulary
//
// Grep-stable, and MIRRORED by the TS side (src/editor/webui/core/src/window.ts, src/.../drag.ts). The
// `webui-panel-contract` gate re-reads these values out of the BUILT bundle and compares them to the
// C++ constants here, the same cross-language discipline e05c/e08d apply to their vocabularies — so a
// rename on either side reds a ctest instead of silently unbinding the window surface at runtime.

inline constexpr const char* kWindowListMethod = "window.list";
inline constexpr const char* kWindowTearOutMethod = "window.tear-out";
inline constexpr const char* kWindowMoveToMethod = "window.move-to";
inline constexpr const char* kWindowSeedMethod = "window.seed";
inline constexpr const char* kWindowRehomedMethod = "window.rehomed";
inline constexpr const char* kWindowCloseMethod = "window.close";

// The CROSS-WINDOW DRAG surface (M9 e10c, 04 §2). `drag.probe`: the TARGET window's editor-core asks
// "is a cross-window drag over me, and where?" and gets the cursor in its OWN client pixels. `drag.
// report-zone`: it answers back which drop zone the cursor is over. Both ride this window-management
// surface so they are installed on EVERY window that installs `window.*` — which is every live smoke —
// with no per-smoke change, exactly the property window_registry.h wanted from a new boot-time method.
inline constexpr const char* kDragProbeMethod = "drag.probe";
inline constexpr const char* kDragReportZoneMethod = "drag.report-zone";

// Refusal codes a window method answers with. LOCAL codes (not R-CLI-008 catalog codes), the same
// rationale panel_host.h states: they classify a HOST-side caller/wiring error, not a daemon-contract
// failure, so minting catalog codes for them would pollute the published surface.
inline constexpr const char* kErrWindowBadParams = "window.bad_params";
inline constexpr const char* kErrWindowUnknownTarget = "window.unknown_target";

// --------------------------------------------------------------------------- the panel seed (D6)
//
// One panel's identity plus its OPAQUE D6 state blob, in transit between two windows' editor-core
// instances. The blob is exactly what `panel.state.get` returned (`{schemaVersion, data}`) and is
// handed back verbatim to `panel.state.set` in the target window — the Shell never interprets it.
struct PanelSeed
{
    std::string panel_id;
    contract::Json state; // opaque; may be null when the panel persists no state
};

// -------------------------------------------------------------- the cross-window relay store
//
// The shared home of every in-transit seed, keyed by TARGET window id. ONE store for the whole app
// (referenced by every window's `WindowBridge`), because a seed produced by window 1 must be readable
// by window 0 — a per-window store could not express a MOVE. CEF-free and fully unit-testable: it is
// pure data movement with no browser, no window, no bridge.
//
// TWO delivery shapes, matching the two moments a target reads its seed (see the file header):
//   * a single BOOT seed  — a tear-out's new window reads it exactly once at boot (`take_seed`);
//   * a REHOME queue      — move-to-N and rehome-on-close append here, drained on the target's poll.
class WindowMoveStore
{
public:
    // TEAR-OUT: the new window opens exactly this one panel at boot. Overwrites any prior boot seed
    // for `target` (a window is seeded once, before it boots).
    void set_boot_seed(WindowId target, PanelSeed seed);
    // Read + CLEAR the boot seed for `target`. nullopt when there is none (an ordinary window).
    [[nodiscard]] std::optional<PanelSeed> take_boot_seed(WindowId target);
    [[nodiscard]] bool has_boot_seed(WindowId target) const;

    // MOVE-TO-N / REHOME: append a panel to `target`'s rehome queue, delivered on its next poll.
    void enqueue_rehome(WindowId target, PanelSeed seed);
    // Read + CLEAR every queued rehome panel for `target`, in enqueue order (empty when none).
    [[nodiscard]] std::vector<PanelSeed> take_rehomed(WindowId target);

    // Drop everything queued for a window that is going away for good, so a destroyed-and-never-read
    // target does not leak its seeds until app exit. Called by the app on `destroy_window`.
    void forget(WindowId target);

    [[nodiscard]] std::size_t pending_boot_seeds() const { return boot_seeds_.size(); }
    [[nodiscard]] std::size_t pending_rehomes(WindowId target) const;

private:
    std::map<WindowId, PanelSeed> boot_seeds_;
    std::map<WindowId, std::vector<PanelSeed>> rehome_queues_;
};

// --------------------------------------------------------------------------------- the move result
//
// What the app's move handler produced. `window.tear-out` projects it to editor-core so a failed
// create is a LOUD, structured answer (03 §7), never a silent success.
struct WindowMoveResult
{
    bool ok = false;
    WindowId window_id = kInvalidWindowId; // the target/created window on success
    // The `WindowCreateOutcome` token on a tear-out (created / factory_failed / limit_reached …), so
    // editor-core can say WHY it degraded rather than only THAT it did. Empty for a move-to.
    std::string outcome;
    std::string error; // empty exactly when `ok`
};

// ----------------------------------------------------------------------------------- the bridge

class WindowBridge
{
public:
    // A tear-out request the app fulfils by CREATING a window and seeding it. `title` is the new
    // window's title; `source` is the window the request came from (the floating-group home on
    // failure, window_registry.h `WindowCreateFailure::source`).
    struct TearOut
    {
        WindowId source = kPrimaryWindowId;
        std::string title;
        PanelSeed seed;
    };
    // A move to an EXISTING window (its editor-core is already up; it reads the seed on its poll).
    struct MoveTo
    {
        WindowId source = kPrimaryWindowId;
        WindowId target = kPrimaryWindowId;
        PanelSeed seed;
    };

    // The app binds how a tear-out / a move / a close is actually carried out. Kept as callbacks so
    // the create-a-window machinery (which needs the `WindowManager` + the CEF factory) stays in the
    // composition root and this surface stays CEF-free and unit-testable.
    using TearOutHandler = std::function<WindowMoveResult(const TearOut&)>;
    using MoveToHandler = std::function<WindowMoveResult(const MoveTo&)>;
    using CloseHandler = std::function<WindowMoveResult(WindowId self)>;
    using WindowsProvider = std::function<std::vector<WindowId>()>;

    // `self_id` is THIS window's id — the source of every request from this window's editor-core, and
    // the key its `window.seed` / `window.rehomed` read. `store` outlives every window (it is owned by
    // the app); a reference is held, so the store must not be relocated (it is not, being an app
    // local).
    WindowBridge(WindowId self_id, WindowMoveStore& store);

    // Non-copyable and non-movable, like every sibling bridge: `install` binds handlers capturing
    // `this`, and a router outlives nothing that could be relocated out from under them.
    WindowBridge(const WindowBridge&) = delete;
    WindowBridge& operator=(const WindowBridge&) = delete;
    WindowBridge(WindowBridge&&) = delete;
    WindowBridge& operator=(WindowBridge&&) = delete;

    void bind_tear_out(TearOutHandler handler);
    void bind_move_to(MoveToHandler handler);
    void bind_close(CloseHandler handler);
    void bind_windows(WindowsProvider provider);

    // Bind the shared cross-window drag relay (e10c). NULL — the default — leaves `drag.probe` /
    // `drag.report-zone` INERT: probe answers `{active:false}` and report-zone is an accepted no-op, so
    // a smoke that installed no drag session still routes them (never `unknown_method`) while doing
    // nothing. The app (and the drag smoke) bind a real store; the five sibling smokes do not, and need
    // no edit for it.
    void bind_drag_store(CrossWindowDragStore* store);

    // --- the method bodies, exposed for direct testing -------------------------------------------
    // Each is total over renderer-controlled `params`, and each is what the corresponding `window.*`
    // handler calls, so the T1 suite exercises the SAME code the renderer reaches.

    [[nodiscard]] contract::Json list() const;
    // Fails closed with `error_code` set (kErrWindowBadParams) on a missing/empty `panelId`.
    [[nodiscard]] contract::Json tear_out(const contract::Json& params, std::string& error_code);
    [[nodiscard]] contract::Json move_to(const contract::Json& params, std::string& error_code);
    // The boot seed for THIS window, consumed once. `{seeded:false}` for an ordinary window.
    [[nodiscard]] contract::Json seed();
    // Every panel queued to rehome INTO this window, consumed. `{panels:[]}` when none pending.
    [[nodiscard]] contract::Json rehomed();
    [[nodiscard]] contract::Json close();

    // e10c — the cross-window drag probe/answer. `drag_probe` reports the hover THIS window (`self_id_`)
    // should act on: `{active, panelId, x, y, generation}`, with x/y in this window's client pixels, or
    // `{active:false}` when no drag is over it (or no store is bound). `drag_report_zone` records this
    // window's answer (`{valid, zoneId, generation}`) into the store; `error_code` is set (kErrWindow
    // BadParams) on a malformed report. Both are total over renderer-controlled `params`.
    [[nodiscard]] contract::Json drag_probe();
    [[nodiscard]] contract::Json drag_report_zone(const contract::Json& params,
                                                  std::string& error_code);

    // Bind every `window.*` + `drag.*` method on `router`. False when ANY binding was refused (a name
    // collision), which is a wiring bug the caller must not ignore.
    [[nodiscard]] bool install(BridgeRouter& router);

    // --- what it saw (the live-smoke / unit assertion surface) -----------------------------------
    [[nodiscard]] std::size_t tear_outs() const { return tear_outs_; }
    [[nodiscard]] std::size_t moves() const { return moves_; }
    [[nodiscard]] std::size_t seeds_served() const { return seeds_served_; }
    // How many `drag.probe` calls saw an ACTIVE hover for this window, and how many `drag.report-zone`
    // answers it forwarded to the store — the live smoke asserts an editor-core round trip from these.
    [[nodiscard]] std::size_t drag_probes_active() const { return drag_probes_active_; }
    [[nodiscard]] std::size_t drag_zones_reported() const { return drag_zones_reported_; }
    [[nodiscard]] WindowId self_id() const { return self_id_; }

private:
    WindowId self_id_;
    WindowMoveStore& store_;
    CrossWindowDragStore* drag_store_ = nullptr;
    TearOutHandler tear_out_;
    MoveToHandler move_to_;
    CloseHandler close_;
    WindowsProvider windows_;
    std::size_t tear_outs_ = 0;
    std::size_t moves_ = 0;
    std::size_t seeds_served_ = 0;
    std::size_t drag_probes_active_ = 0;
    std::size_t drag_zones_reported_ = 0;
};

} // namespace context::editor::shell
