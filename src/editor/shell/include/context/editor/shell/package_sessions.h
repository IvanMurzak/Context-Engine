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
//  1. THE SCOPE IS DERIVED FROM THE PERSISTED GRANT, AT ONE SITE. ⚠ **CHANGED BY M9 e13c-4** — this
//     control used to read "a HARDCODED CONSTANT, NOT A DERIVED VALUE", and the derivation it named
//     as e13c-4's job has now landed. `AttachOptions::scope` is client-CHOSEN (`client.h`; the daemon
//     clamps it to the launch-time ceiling at `dispatcher.cpp`'s `ceiling_.intersect(requested)`), so
//     what decides a package's authority is the string this class asks for — and it now asks for
//     `scope_resolver_(package_id)`, whose production value comes from the operator's recorded
//     consent (`package_grants.h`) clamped to the package's own manifest declaration.
//     WHAT DID NOT CHANGE, and is why this is still ONE control rather than a new surface: the
//     resolver is consulted HERE, at the single attach site, and its DEFAULT is still
//     `kPackageSessionScope` — so a wiring that forgets to supply one denies rather than grants, and
//     no code path lets a PACKAGE influence its own answer (the grant document is a Shell document
//     the store scan can never reach — package_grants.h decision 1).
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
//     Sessions are POOLED PER PACKAGE, not per package per window, so a package with panels in three
//     windows holds one connection. Pooling by package is strictly finer than the "pool by
//     granted-scope-set" option the e13c-1 brief names, and today identical to it (every package
//     session holds the same baseline) — but it keeps the daemon's `clients` topic attributable per
//     package, which a scope-set pool loses.
//     ⚠ THE POOL IS PER HOST INSTANCE, AND TODAY THAT IS THE PRIMARY WINDOW'S ROUTER ONLY. Each
//     factory-created window builds its OWN `BridgeRouter` (editor_main.cpp) and
//     `SecondaryWindowSurfaces::install` does NOT install this method, so a package panel torn out
//     into a secondary window gets `unknown_method` — fail-closed, and a KNOWN GAP rather than a
//     design. Do not read the per-package pooling above as a cross-window guarantee it has not yet
//     had to earn: installing this ONE host on each secondary router is what would make it one, and
//     giving each window its own host would multiply the sub-cap by window count and break S3.
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
#include "context/editor/shell/package_events.h" // e13c-2: the BOUNDED fan-out buffer

#include <cstddef>
#include <cstdint>
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
/**
 * `unsubscribe` / `ack` named a `subId` THIS PACKAGE DID NOT MINT — control 5 (S8).
 *
 * ⚠ DELIBERATELY THE SAME CODE whether the subId belongs to another client or does not exist at all.
 * Differentiating them would rebuild the enumeration oracle this control exists to close, exactly as
 * `kErrPackageMethodNotAllowed` refuses identically for a real and an imaginary method.
 */
inline constexpr const char* kErrPackageUnknownSubscription = "panel.daemon.unknown_subscription";
/** The per-package SUBSCRIPTION cap is full — control 5. Distinct from the SESSION cap above. */
inline constexpr const char* kErrPackageSubscriptionCapacity = "panel.daemon.subscription_capacity";
/**
 * `subscribe` named ANOTHER package's fact topic without the operator's consent — control 6 (D4).
 *
 * ⚠ DELIBERATELY DISTINCT FROM `kErrPackageMethodNotAllowed`, and the reasoning is the mirror image
 * of the one that COLLAPSED the two subscription refusals. There, differentiating would have built
 * an enumeration oracle over a global id namespace nobody may probe. Here the topic is ALREADY the
 * caller's own string and the answer reveals nothing it did not supply — while the two faults are
 * genuinely different repairs: a method refusal is a contract statement ("panels do not call that"),
 * and this is a CONSENT state a human can change. One code would send a package author to argue
 * with the wrong control.
 */
inline constexpr const char* kErrPackageTopicNotGranted = "panel.daemon.topic_not_granted";

// ------------------------------------------------------------------------------------ the policy

// The scope a package session attaches with when NOTHING was consented to — the `read_query`
// baseline, and nothing else.
//
// ⚠ THIS CONSTANT WAS THE WHOLE CAPABILITY MODEL OF e13c-1, AND IS NOW THE FLOOR OF e13c-4's. It is
// what `PackageSessionHost`'s DEFAULT resolver answers for every package, so a build that wires no
// grant source is byte-for-byte the deny-all build e13c-1 shipped. Widening it is not a configuration
// change; it is the difference between a panel that can read and a panel that can rewrite authored
// files. Spelled `"read"` rather than left to `AttachOptions`' own default so the choice is VISIBLE
// and greppable here; a default is not a decision anyone can find.
inline constexpr const char* kPackageSessionScope = "read";

// ⚠ CONTROL 5 (S8) — A PACKAGE MAY ONLY ADDRESS SUBSCRIPTIONS IT MINTED ITSELF.
//
// WHY THIS EXISTS, and why control 2 does not already cover it. The allowlist is closed over METHOD
// NAMES; `unsubscribe` and `ack` are on it, and both address a subscription by a `subId` that travels
// in the PANEL's own params. That id namespace is daemon-GLOBAL, sequential (`"sub-" + (++counter)`,
// bridge/event_stream.cpp) and — crucially — UNAUTHENTICATED: `Dispatcher::dispatch` routes the three
// subscription verbs to `serve_subscription(*stream_, method, params)` WITHOUT the connection's
// `Session`, and `EventStream::unsubscribe` / `::ack` then match on `id` alone, over a
// `struct Subscription` that carries no owner field at all. One daemon holds ONE stream shared by the
// Shell's own subscription, every CLI client and every AI client.
//
// So control 2's own argument — "a denylist over an OPEN namespace is not a control" — applies one
// level down, to the ID namespace, and was not made there. Without this control a sandboxed package
// could walk `sub-1…sub-N` and:
//   * `unsubscribe` the SHELL's own {diagnostics, derivation, session} subscription, leaving Problems
//     / Scene tree / Inspector rendering their last state forever with nothing reporting it — the
//     precise silent-stale failure `package_events.h` § "a drop is loud" exists to prevent, reached
//     from untrusted code; and
//   * `ack` a THIRD PARTY's cursor to a huge seq, which advances the retention floor and prunes ring
//     history that victim has genuinely not read. `EventStream::ack` is monotonic, so it cannot be
//     undone.
// The daemon's answer also differs for a live vs an unknown subId, which is an existence ORACLE over
// every subscription in the process.
//
// WHAT THIS CONTROL IS, AND WHAT IT IS NOT. It is a SHELL-side ownership check: the host records the
// `subId` out of each package's OWN `subscribe` reply, and refuses `unsubscribe` / `ack` for any id
// that package did not mint — with ONE code for "not yours" and "does not exist", so the oracle stays
// closed. It is deliberately NOT the whole answer: a Shell-side filter cannot bind a client that does
// not go through this Shell, so the DURABLE fix is daemon-side (put the owning client id on
// `Subscription` and hand `serve_subscription` the `Session`), and this control must be RETIRED when
// that lands rather than kept as a second copy to drift — the hazard control 2 names. It is here
// because an authorization hole reachable from third-party code is not survivable at the distance of
// the grant store that will own scopes.
inline constexpr std::size_t kMaxSubscriptionsPerPackage = 8;

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
//   * `subscribe` /        — the R-CLI-015 subscription protocol (M9 e13c-2). Baseline-scoped, so
//     `unsubscribe` /        the dispatcher would always have accepted them; e13c-1 held them out
//     `ack`                  anyway because "a subscription with no bounded buffer is an unbounded
//                            allocation driven by untrusted code" — and they are here NOW, and only
//                            now, because `package_events.h` supplies that bound. The three travel
//                            together on purpose: `subscribe` with no `ack` pins the daemon's ring
//                            retention at the slowest cursor (client/subscription.h § 2), and
//                            `subscribe` with no `unsubscribe` leaks a subId per re-subscribe.
//
// WHAT IS DELIBERATELY ABSENT, and why the absences are load-bearing rather than an oversight:
//   * `set` / `edit` / `build` / `package.add` are not here AND would be refused by the dispatcher
//     anyway. Both controls, independently — that redundancy is the S4 point: the allowlist must hold
//     even for a method the scope table would have let through.
//   * `resource.read`, `snapshot`, `validate`, `reconcile`, `doctor` are simply not needed yet. Adding
//     one is a reviewed one-line change to this list, which is the shape a capability surface should
//     have.
//
// ⚠ M9 e13c-4 CHANGED WHAT THE ABSENCES ARE WORTH - READ THIS BEFORE ADDING A NAME.
// Two things that were true when the list above was written are no longer true, and BOTH of them
// concern exactly the write verbs:
//   1. THE "REFUSED BY THE DISPATCHER ANYWAY" REDUNDANCY IS GONE FOR A GRANTED PACKAGE. It held while
//      every package session was the `read_query` baseline. Since e13c-4 a consented package attaches
//      with a DERIVED scope (`package_grants.h` § attach_scope_spec_for), so a session really can
//      carry `file_write` / `session_control` / `build_install` - and the dispatcher would then ADMIT
//      `set` / `edit` / `build` / `package.add`. For those methods this allowlist is now the ONLY
//      control, not the redundant second one.
//   2. ADDING A WRITE VERB HERE ALSO ARMS `package.grants.decide`. That route writes the consent
//      DOCUMENT and is installed on the privileged bridge, whose own threat model (`ipc_bridge.h`)
//      treats every inbound message as untrusted renderer content. It is harmless today only because
//      nothing a renderer can reach through THIS list can act on a grant it wrote. A write verb here
//      closes that loop: renderer -> `decide` -> a grant it chose -> this list -> the authored file.
// So a name added here is no longer "a reviewed one-line change"; it is a change to the sandbox
// boundary, and it needs the consent-write route re-examined in the same review.
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

    // THE DERIVED SCOPE (M9 e13c-4, control 1). Answers the `AttachOptions::scope` spec for ONE
    // package — in production `attach_scope_spec(granted_scope_set(<the consented, manifest-clamped
    // grant>))` (package_grants.h), which is a pure function of a document no package can write.
    //
    // ⚠ A `std::function` RATHER THAN A `PackageGrantStore&`, deliberately: this class must not
    // acquire a dependency on the consent subsystem to attach a session, the resolver is what the
    // suite substitutes to drive a WIDENED scope through the REAL dispatcher, and a value-returning
    // seam cannot be tricked into returning a scope for a DIFFERENT package the way a shared lookup
    // table could. An EMPTY answer is treated as the baseline, so a resolver that goes wrong fails
    // CLOSED.
    using ScopeResolver = std::function<std::string(const std::string& package_id)>;

    // ⚠ CONTROL 6 (editor-UX d2, D4) — THE PACKAGE FACT BUS'S TWO SHELL-SIDE ANSWERS.
    //
    // WHY THIS CLASS AT ALL. Both questions are about a MANIFEST, and the daemon has never read one:
    // `events.publish` can check its own grammar and its own registry, but "did THAT package declare
    // THIS topic" and "was that package consented to another's" are answerable only where the store
    // scan and the grant document are. That is the Shell, and specifically this class, because it is
    // what owns a package's session and therefore the only place the two facts can be applied to a
    // wire call. `package_facts.h` supplies the values; this file applies them.
    //
    //  * `publishes` — the topics a package DECLARED in `events.publishes[]`. Declared on the
    //    package's OWN session the moment it opens, which is D4's "topics registered at
    //    install/load": a package that never opens a session declares nothing, and a topic nobody
    //    declared cannot be published on by anyone.
    //  * `may_subscribe` — may `package_id` RECEIVE facts on `topic`? True for its own topics; true
    //    for another package's only when the manifest declared it in `events.subscribes[]` AND the
    //    operator consented. It is consulted on all THREE paths a fact can reach a package by,
    //    deliberately: the `subscribe` REQUEST (`forward`, the good diagnostic), the `subscribe`
    //    REPLY (`forward` again — the D5 snapshot and the replayed `catchup`, neither of which the
    //    pump ever sees), and every delivered event (`pump`).
    //
    // std::function rather than a `PackageFactHost&`, for control 1's stated reason: this class must
    // not acquire a dependency on the consent subsystem to open a session, and a value-returning
    // seam cannot be tricked into answering for a DIFFERENT package the way a shared table could.
    // ABSENT is the DENY-ALL build — no topic is declared and no package-namespaced fact is
    // delivered — so a wiring that forgets to supply one is byte-for-byte the pre-d2 editor.
    using TopicResolver = std::function<std::vector<std::string>(const std::string& package_id)>;
    using SubscribeGate =
        std::function<bool(const std::string& package_id, const std::string& topic)>;

    // Supply control 6's two answers. Called once by the composition root, after the grant host and
    // the store scan exist. Idempotent — the last call wins.
    void set_fact_policy(TopicResolver publishes, SubscribeGate may_subscribe);

    // Publish ONE package fact on `package_id`'s own baseline session (D4).
    //
    // ⚠ DELIBERATELY **NOT** REACHABLE THROUGH `panel.daemon.call`, and that is the whole reason it
    // is a method rather than an allowlist entry. Adding `events.publish` to
    // `panel_callable_daemon_methods()` would let a panel send it with any topic it liked —
    // `bridge.call` forwards the method VERBATIM — and the Shell-side "did that package declare that
    // topic" check would be an adapter a caller routes around, which is exactly the S4 failure the
    // allowlist exists to prevent. So the allowlist stays closed at seven names and `panel.facts.publish`
    // (package_facts.h) is the ONE door, with the declaration check standing on it.
    [[nodiscard]] BridgeResult publish_fact(const std::string& package_id, const std::string& topic,
                                            const contract::Json& payload);

    /** May `package_id` receive a fact on `topic`? Control 6's predicate, exposed for the suite. */
    [[nodiscard]] bool may_receive_fact(const std::string& package_id,
                                        const std::string& topic) const;

    /** `subscribe` calls refused because a named topic was another package's — control 6. */
    [[nodiscard]] std::size_t refused_topics() const { return refused_topics_; }
    /** Delivered events DROPPED by the pump filter because the topic was not this package's — control 6. */
    [[nodiscard]] std::uint64_t events_filtered() const { return events_filtered_; }

    explicit PackageSessionHost(ClientFactory factory, std::size_t max_sessions = kMaxPackageSessions);

    // As above, plus the grant-derived scope source. The two-argument form above keeps the deny-all
    // default (`kPackageSessionScope` for every package), which is what makes an un-wired build
    // identical to the e13c-1 one rather than merely similar to it.
    PackageSessionHost(ClientFactory factory, ScopeResolver scope_resolver,
                       std::size_t max_sessions = kMaxPackageSessions);

    /** The scope spec this host WOULD attach `package_id` with. The decision path, exposed. */
    [[nodiscard]] std::string attach_scope_for(const std::string& package_id) const;

    PackageSessionHost(const PackageSessionHost&) = delete;
    PackageSessionHost& operator=(const PackageSessionHost&) = delete;

    // Bind `panel.daemon.call` AND `panel.events.poll`. False when either binding was refused (a name
    // collision, or the name landing on `forbidden_bridge_methods()`), which the caller must treat as
    // a wiring bug.
    [[nodiscard]] bool install(BridgeRouter& router);

    // Forward one call onto `package_id`'s baseline session, opening it on first use. The whole
    // decision path, exposed so the suite drives it without a router in the way.
    [[nodiscard]] BridgeResult forward(const std::string& package_id, const std::string& method,
                                       const contract::Json& params);

    // Drop every session AND everything buffered for it (a daemon that went away, or shutdown).
    // Idempotent.
    //
    // ⚠ THE BUFFER IS CLEARED WITH THE SESSIONS, DELIBERATELY. The events in it were minted by a
    // subscription on a connection that no longer exists, and their seqs belong to an incarnation
    // that may not survive (client/subscription.h § 5). Delivering them after a reset would hand a
    // panel a cursor into a dead lifetime — precisely the corruption `Client::reconnect` clears
    // `pending_events_` to avoid.
    void reset();

    // Drain every live package session's inbound event frames into the BOUNDED buffer (e13c-2).
    //
    // ⚠ THIS MUST RUN ON THE OWNER LOOP, NOT ONLY WHEN editor-core POLLS. Between two polls the frames
    // otherwise accumulate in `Client::pending_events_` — a plain `std::deque` with NO bound — so a
    // buffer that filled only on demand would leave the real accumulation point unbounded and the
    // whole control cosmetic. Pumping every frame is what makes `kMaxBufferedEventsPerPackage` the
    // actual ceiling.
    //
    // NON-BLOCKING (a 0 ms poll per session) and bounded at `kMaxDrainedFramesPerPump` frames per
    // session per call, so one chatty topic cannot hold the frame. Returns how many events it
    // buffered — 0 on a quiet pump, which is the common case.
    std::size_t pump();

    // Read + CLEAR `package_id`'s buffered events, with the LOUD `dropped` / `gapped` pair. The whole
    // decision path, exposed so the suite drives it without a router in the way.
    [[nodiscard]] PackageEventDrain poll_events(const std::string& package_id);

    // --- what it did ------------------------------------------------------------------------------
    /** Live package sessions. THE lazy-attach observable: 0 until a package actually calls. */
    [[nodiscard]] std::size_t sessions_open() const { return sessions_.size(); }
    /** Calls handed to a daemon session (refusals never reach one). */
    [[nodiscard]] std::size_t calls_forwarded() const { return calls_forwarded_; }
    /** Calls refused by the ALLOWLIST — control 2. NON-ZERO IS A PANEL MISBEHAVING, not a metric. */
    [[nodiscard]] std::size_t refused_methods() const { return refused_methods_; }
    /** Calls refused by the sub-cap — control 4(b). */
    [[nodiscard]] std::size_t refused_capacity() const { return refused_capacity_; }
    /** Daemon events buffered by `pump()` (e13c-2). */
    [[nodiscard]] std::uint64_t events_buffered() const { return events_buffered_; }
    /**
     * Events DISCARDED because a package fell behind its bound (e13c-2). NON-ZERO IS A PANEL FALLING
     * BEHIND, not a metric — and it is the Shell-side half of "a drop is observable, never silent".
     */
    [[nodiscard]] std::uint64_t events_dropped() const { return events_.dropped_total(); }
    /** How many subscriptions `package_id` currently holds — control 5's live state. */
    [[nodiscard]] std::size_t subscriptions_open(const std::string& package_id) const;
    /** `unsubscribe`/`ack` calls refused because the subId was not this package's — control 5. */
    [[nodiscard]] std::size_t refused_subscriptions() const { return refused_subscriptions_; }
    /** Is `package_id` currently holding a session? */
    [[nodiscard]] bool has_session(const std::string& package_id) const;

private:
    struct Session
    {
        std::string package_id;
        std::unique_ptr<client::Client> client;
        // CONTROL 5 (S8) — the subIds THIS package minted, in mint order. Small and linear on
        // purpose: it is capped at `kMaxSubscriptionsPerPackage`, so a set would cost more than it
        // saves, and the order is what makes a capacity refusal reproducible.
        std::vector<std::string> sub_ids;
    };

    // The live session for `package_id`, opening one if the cap allows. nullptr + `error_code` /
    // `message` otherwise.
    [[nodiscard]] client::Client* session_for(const std::string& package_id, std::string& error_code,
                                              std::string& message);

    // What an entry whose `topic` this Shell cannot READ should do — the ONE axis on which the two
    // arms of the `subscribe`-reply filter differ, spelled as an argument rather than as an inverted
    // conditional in a second copy of the loop.
    //
    // `drop` is the SNAPSHOT's: a `packageFacts` entry is a shape this Shell defines, so one it
    // cannot read is one it cannot police either, and the deny direction is the only one that stays
    // a control if that shape ever moves. `keep` is the CATCHUP's: those are the daemon's own WIRE
    // envelopes, and `may_receive_fact` is about package topics and nothing else — so a pathless
    // frame passes exactly as it does through `pump()`.
    enum class UnreadableEntry
    {
        drop,
        keep,
    };

    // `in` (an array) minus every entry `package_id` may not receive, counting each removal in
    // `events_filtered_`. Control 6's reply-side filter, using the SAME `may_receive_fact` predicate
    // the request and delivery sides use, so there is one answer to "may this package see this
    // topic" rather than three that can drift.
    [[nodiscard]] contract::Json filter_topic_array(const contract::Json& in,
                                                    const std::string& package_id,
                                                    UnreadableEntry unreadable);

    ClientFactory factory_;
    // EMPTY in the two-argument construction = the e13c-1 deny-all build (see `attach_scope_for`).
    ScopeResolver scope_resolver_;
    // EMPTY = the pre-d2 deny-all fact bus: nothing declared, nothing package-namespaced delivered.
    TopicResolver fact_topics_;
    SubscribeGate fact_gate_;
    std::size_t max_sessions_;
    std::vector<Session> sessions_;
    // The e13c-2 bound. OWNED here rather than beside this class because its lifetime is exactly the
    // sessions' — an event only exists while the connection that delivered it does (see reset()).
    PackageEventBuffer events_;
    std::size_t calls_forwarded_ = 0;
    std::size_t refused_methods_ = 0;
    std::size_t refused_capacity_ = 0;
    std::size_t refused_subscriptions_ = 0;
    std::size_t refused_topics_ = 0;
    std::uint64_t events_buffered_ = 0;
    std::uint64_t events_filtered_ = 0;
};

} // namespace context::editor::shell
