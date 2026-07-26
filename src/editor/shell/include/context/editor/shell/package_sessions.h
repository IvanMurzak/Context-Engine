// PER-PACKAGE BASELINE DAEMON SESSIONS + the `panel.daemon.call` fan-in route (M9 e13c-1,
// design 04 §5 / 08 §1-§2).
//
// WHAT THIS IS. Until now editor-core could not call a single daemon contract verb: `boot.ts`'s
// `contractDispatch` answered a hardcoded `daemon RPC fan-in not wired yet (D19)` for every projected
// verb, and a package panel's `bridge.call` was a deny-all lookup miss in `panelverbs.ts`. This is
// that route — and it is deliberately the SMALLEST version of it that makes `scope.denied` REACHABLE
// FROM A PANEL, which is what closes the e13 DoD line "un-granted `file_write` / `build_install`
// rejected IN THE DISPATCHER" with a real negative test instead of an argument.
//
// ⚠ THE ENFORCEMENT IS NOT HERE, AND THAT IS THE POINT (08 §2). `Dispatcher::dispatch` runs
// `authorize(method, session.scopes)` on EVERY method, ahead of both `backend_->invoke` and
// `serve_subscription` — the adapter-is-bypassable rule of R-SEC-007. So this class does not decide
// what a package may DO; it decides only WHAT SESSION the request travels on, and the session it
// opens holds the `read_query` BASELINE and nothing else (`kPackageSessionScope`). A panel asking for
// `set` is refused by the daemon, in the dispatcher, with the catalog's own `scope.denied` — the
// refusal is real because the scope is real, not because this file checked a list.
//
// ================================ THE FOUR CONTROLS ================================
//
//  1. THE SCOPE IS A HARDCODED CONSTANT, NOT A DERIVED VALUE. `AttachOptions::scope` is
//     client-CHOSEN (`client.h`; the daemon clamps it to the launch-time ceiling at
//     `dispatcher.cpp`'s `ceiling_.intersect(requested)`), so "a per-package session at the baseline"
//     needs no consent surface, no grant store and no manifest plumbing — it needs one string that
//     nothing computes. Deriving that string from a manifest, a grant or a consent answer is e13c-4's
//     job and is deliberately absent: today there is no code path by which a package can influence it.
//
//  2. THE METHOD SET IS AN ALLOWLIST (S4), and it is checked BEFORE a session is opened. The Shell's
//     privileged router already refuses the credential-bearing methods BY NAME
//     (`forbidden_bridge_methods()`), but that control is a DENYLIST over the ROUTER's own method
//     namespace — and a forwarder makes the *payload* the method name, which routes straight around
//     it. A denylist over an open namespace is not a control; `panel_callable_daemon_methods()` is
//     therefore closed, small, and the only way in.
//
//  3. …WHICH IS ALSO WHAT CLOSES S7. `required_scope_for` DEFAULTS AN UNKNOWN METHOD TO
//     `read_query` (scope.cpp — deliberately, so the baseline is least-privilege for CLIENT-chosen
//     verbs). That default is safe while the method set is the registry's own; it is NOT safe once a
//     *panel* chooses the string, because an unrecognised-but-backend-served method would then run at
//     the baseline. The allowlist is what keeps a panel from ever choosing an unclassified name.
//
//  4. CONNECTION EXHAUSTION IS BOUNDED, TWICE (S3). The daemon serves `max_connections_` clients
//     (DEFAULT 16, `kernel_server.h`) and refuses the (N+1)th attach with `daemon.busy`, so N packages
//     x M windows of eager sessions can lock out the Shell itself, the CLI and every AI client. Two
//     mechanisms, both here:
//       (a) LAZY ATTACH — a session is opened on a package's FIRST forwarded call, never at mount. A
//           mounted package that never calls the daemon costs zero connections, which is the common
//           case for a panel that only draws.
//       (b) A SUB-CAP — `kMaxPackageSessions` (4) of the daemon's 16, so >=12 slots always remain for
//           the Shell's own attach, the CLI and AI clients. Over the cap the call is refused HERE
//           (`kErrPackageCapacity`) rather than attempted: an attach that would answer `daemon.busy`
//           still costs a connect + handshake round trip, and a Shell-side refusal is the one that
//           cannot starve anyone.
//     Sessions are POOLED PER PACKAGE, not per package per window: the router outlives every browser
//     and is shared by the windows (ipc_bridge.h), so a package with panels in three windows holds one
//     connection. Pooling by package is strictly finer than the "pool by granted-scope-set" option the
//     e13c-1 brief names, and today identical to it (every package session holds the same baseline) —
//     but it keeps the daemon's `clients` topic attributable per package, which a scope-set pool loses.
//
// ⚠ NAMED RESIDUAL, NOT CLOSED HERE (S2, design 09 §3). The package -> port -> session chain of custody
// is entirely TRUSTED-SIDE. `panelport.ts` binds a port to the FIRST DOCUMENT loaded into a frame and
// says so explicitly ("WHAT IS NOT CLAIMED"): it does not prove WHICH PACKAGE that document belonged
// to, because the host chose the URL and the Shell served exactly that URL — a Shell/editor-core
// agreement, not a browser-verified fact, and no sandboxed frame can do better (every one reports
// `event.origin === "null"`). So the `packageId` reaching this router method is editor-core's word.
// That was already true when the consequence was a wrongly-themed panel; from e13c-1 the consequence
// is which daemon session a call rides. It is bounded — every package session holds the SAME baseline
// scope, so mis-attribution cannot ESCALATE, only mis-attribute — and it is recorded rather than
// papered over. Real provenance is e13c-3's package store.

#pragma once

#include "context/editor/client/client.h"
#include "context/editor/shell/ipc_bridge.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace context::editor::shell
{

// ------------------------------------------------------------------------------- the router method

// The fan-in route editor-core calls on behalf of ONE package panel's `bridge.call`.
//
// Params: `{packageId, method, params?}`. `packageId` comes from editor-core's own per-panel binding
// (closed over in `makePanelBridgeVerbs`, never read from the panel's request) — see the S2 residual.
inline constexpr const char* kPanelDaemonCallMethod = "panel.daemon.call";

// --------------------------------------------------------------------------------- the refusals
//
// Distinct codes because they are distinct FAULTS and a package author must be able to tell them
// apart: a bad request is theirs to fix, a non-allowlisted method is a contract statement, capacity is
// transient, and an unavailable daemon is the editor's state. A single code would make all four read
// as "it did not work".

/** `packageId` / `method` missing, not a string, or not a syntactically valid package id. */
inline constexpr const char* kErrPackageBadParams = "panel.daemon.bad_params";
/** The method is not on `panel_callable_daemon_methods()` — control 2 (S4). NOT a scope refusal. */
inline constexpr const char* kErrPackageMethodNotAllowed = "panel.daemon.method_not_allowed";
/** The per-package session sub-cap is full — control 4(b) (S3). Transient, like `daemon.busy`. */
inline constexpr const char* kErrPackageCapacity = "panel.daemon.capacity";
/** No daemon to attach to, or the attach was refused. The editor is read-only / detached. */
inline constexpr const char* kErrPackageNoSession = "panel.daemon.unavailable";

// ------------------------------------------------------------------------------------ the policy

// The scope EVERY package session attaches with — the `read_query` baseline, and nothing else.
//
// ⚠ THIS CONSTANT IS THE WHOLE CAPABILITY MODEL OF e13c-1. Widening it is not a configuration change;
// it is the difference between a panel that can read and a panel that can rewrite authored files.
// e13c-4 replaces the VALUE with one derived from an install-consent grant — at this one site, exactly
// as `DENY_ALL_CAPABILITY_GRANTS` is replaced at one site on the editor-core side. Spelled `"read"`
// rather than left to `AttachOptions`' own default so the choice is VISIBLE and greppable here; a
// default is not a decision anyone can find.
inline constexpr const char* kPackageSessionScope = "read";

// How many package sessions this Shell process may hold at once — control 4(b).
//
// FOUR OF THE DAEMON'S DEFAULT SIXTEEN (`kernel_server.h`), leaving >=12 for the Shell's own attach,
// the CLI, AI clients and a second editor process. Chosen to be OBVIOUSLY sufficient for the panels a
// human actually keeps calling the daemon and OBVIOUSLY unable to starve the rest of the fleet — an
// unbounded count is the failure this exists to prevent, so the number errs small.
inline constexpr std::size_t kMaxPackageSessions = 4;

// THE PANEL-CALLABLE METHOD ALLOWLIST (S4/S7) — closed, small, and the ONLY way a package reaches the
// daemon.
//
// EVERY ENTRY IS A READ, and each one earns its place by being something a panel cannot draw without:
//
//   * `describe`           — the R-CLI-010 contract self-description. Carries no project data at all
//                            (it is what `--help` prints), and without it a package must hardcode the
//                            surface it calls.
//   * `query`              — the ONE authored-data read (R-CLI-012, docs/query-language.md).
//   * `editor.scene-tree`  — the e05d3 composed scene projection: what a hierarchy panel renders.
//   * `editor.inspect`     — the e05d3 composed entity projection: what a property panel renders.
//
// WHAT IS DELIBERATELY ABSENT, and why the absences are load-bearing rather than an oversight:
//   * `subscribe` / `unsubscribe` / `ack` are baseline-scoped and would therefore be ACCEPTED by the
//     dispatcher — they are held out because the fan-OUT buffer that makes them safe to expose (a
//     bound, a drop policy, an ack cursor) is e13c-2's, and a subscription with no bounded buffer is
//     an unbounded allocation driven by untrusted code.
//   * `set` / `edit` / `build` / `package.add` are not here AND would be refused by the dispatcher
//     anyway. Both controls, independently — that redundancy is the S4 point: the allowlist must hold
//     even for a method the scope table would have let through.
//   * `resource.read`, `snapshot`, `validate`, `reconcile`, `doctor` are simply not needed yet. Adding
//     one is a reviewed one-line change to this list, which is the shape a capability surface should
//     have.
[[nodiscard]] const std::vector<std::string>& panel_callable_daemon_methods();

/** Is `method` on the allowlist? EXACT match only — no prefix rule, no normalization. */
[[nodiscard]] bool is_panel_callable_daemon_method(const std::string& method);

// ------------------------------------------------------------------------------------ the host

// Owns one baseline `client::Client` per package that has actually called the daemon, and serves
// `panel.daemon.call` over the privileged router.
class PackageSessionHost
{
public:
    // Mints an UNATTACHED client for a package session, or nullptr + `error`.
    //
    // ⚠ THE FACTORY DOES NOT ATTACH, DELIBERATELY. If it did, the SCOPE would be the factory's choice
    // — i.e. the caller's — and control 1 would be a convention rather than a mechanism. This class
    // performs the attach itself with `kPackageSessionScope`, so there is exactly one place the scope
    // is decided and a production wiring cannot widen it by supplying a different factory.
    //
    // In production it is `client::Client::connect_to_project`, whose discovered D20 token
    // `attach()` falls back to. In tests it is a `MockChannel`-backed client, which is what lets the
    // scope this class REQUESTS be asserted on the wire.
    using ClientFactory = std::function<std::unique_ptr<client::Client>(std::string& error)>;

    explicit PackageSessionHost(ClientFactory factory, std::size_t max_sessions = kMaxPackageSessions);

    PackageSessionHost(const PackageSessionHost&) = delete;
    PackageSessionHost& operator=(const PackageSessionHost&) = delete;

    // Bind `panel.daemon.call`. False when the binding was refused (a name collision, or the name
    // landing on `forbidden_bridge_methods()`), which the caller must treat as a wiring bug.
    [[nodiscard]] bool install(BridgeRouter& router);

    // Forward one call onto `package_id`'s baseline session, opening it on first use. The whole
    // decision path, exposed so the suite drives it without a router in the way.
    [[nodiscard]] BridgeResult forward(const std::string& package_id, const std::string& method,
                                       const contract::Json& params);

    // Drop every session (a daemon that went away, or shutdown). Idempotent.
    void reset();

    // --- what it did ------------------------------------------------------------------------------
    /** Live package sessions. THE lazy-attach observable: 0 until a package actually calls. */
    [[nodiscard]] std::size_t sessions_open() const { return sessions_.size(); }
    /** Calls handed to a daemon session (refusals never reach one). */
    [[nodiscard]] std::size_t calls_forwarded() const { return calls_forwarded_; }
    /** Calls refused by the ALLOWLIST — control 2. NON-ZERO IS A PANEL MISBEHAVING, not a metric. */
    [[nodiscard]] std::size_t refused_methods() const { return refused_methods_; }
    /** Calls refused by the sub-cap — control 4(b). */
    [[nodiscard]] std::size_t refused_capacity() const { return refused_capacity_; }
    /** Is `package_id` currently holding a session? */
    [[nodiscard]] bool has_session(const std::string& package_id) const;

private:
    struct Session
    {
        std::string package_id;
        std::unique_ptr<client::Client> client;
    };

    // The live session for `package_id`, opening one if the cap allows. nullptr + `error_code` /
    // `message` otherwise.
    [[nodiscard]] client::Client* session_for(const std::string& package_id, std::string& error_code,
                                              std::string& message);

    ClientFactory factory_;
    std::size_t max_sessions_;
    std::vector<Session> sessions_;
    std::size_t calls_forwarded_ = 0;
    std::size_t refused_methods_ = 0;
    std::size_t refused_capacity_ = 0;
};

} // namespace context::editor::shell
