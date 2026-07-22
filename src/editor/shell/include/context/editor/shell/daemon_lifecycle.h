// The daemon lifecycle SPINE (M9 e14a, design 07 §4 / 02 / 03 §7 / 05 §2): the process model the rest
// of e14 rides on. Four coupled facets of ONE seam:
//
//   1. Launch -> resolve -> attach-or-spawn. Resolve the project; if a live daemon is discoverable,
//      ATTACH to it as an ordinary D10 client (never owned); otherwise SPAWN `context daemon` as a
//      CHILD and read the D20 attach token off the child's STDOUT (never argv/env — 05 §2 / 08 threat
//      model) via the client SDK's spawn handshake.
//   2. Exit policy. The daemon survives editor exit ONLY if other clients hold attachments (observed
//      over the `clients` topic); otherwise a clean `shutdown` verb. A pre-existing EXTERNAL daemon is
//      attached-to, NEVER owned or shut down.
//   3. Reconnect. Daemon lost -> read-only STATE + bounded backoff until reattached; on reattach the
//      e02 subscription consumer re-snapshots (a fresh subscribe on the new incarnation). (The banner
//      UI is e14d; the lifecycle/backoff/read-only STATE is here.)
//   4. Ownership. Whether THIS process spawned + owns the daemon, or merely attached to an external
//      one — the pivot for the exit policy.
//
// D10 CLEANLINESS. The spawn primitive lives in the boundary-clean `context_common` (it links nothing
// kernel-internal), and the daemon is launched as a SUBPROCESS — a runtime dependency, not a link one
// — so the Shell's link closure gains nothing forbidden and `context_assert_shell_boundary` stays
// non-vacuous with its FORBIDDEN list byte-identical.
//
// This header exposes the three PURE decision pieces (exit action, client census, reconnect
// controller) so the policy is unit-tested without a real daemon, plus the DaemonLifecycle coordinator
// that glues them to the real Client + child process (proven end to end by the T2 integration drill).

#pragma once

#include "context/common/child_process.h"
#include "context/editor/client/client.h"
#include "context/editor/client/instance.h"
#include "context/editor/client/subscription.h"
#include "context/editor/contract/json.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace context::editor::shell
{

// ------------------------------------------------------------------------------- pure: exit policy

// What to do with the daemon when the editor exits. The daemon has NO refcount of its own (the
// `shutdown` verb tears it down unconditionally), so the Shell must not send it while other clients
// are attached.
enum class DaemonExitAction
{
    leave_running,  // an external daemon, or an owned one other clients still hold
    shutdown_owned, // an owned daemon this process is the last client of -> clean `shutdown`
};

// Owned + sole client => shutdown; anything else => leave it running. Pure so the policy is asserted
// without a daemon.
[[nodiscard]] DaemonExitAction decide_daemon_exit_action(bool owns_daemon,
                                                         int other_clients_attached) noexcept;

// ---------------------------------------------------------------------------- pure: client census

// How the Shell relates to the daemon it is talking to.
enum class DaemonOwnership
{
    none,              // not attached
    attached_external, // attached to a pre-existing daemon — NEVER owned
    spawned_owned,     // this process spawned it as a child — owned
};

// Folds the `clients` topic (D19) into a live count of attached clients. The topic's snapshot does not
// enumerate the attached set (it is `{incarnationId, generation, lastSeq}`), so the count is built from
// DELTA events off a baseline of 1 — this process's OWN attachment, established the instant it attaches
// to the daemon it spawned. That baseline is EXACT for the only case the exit policy consults it (an
// owned daemon, of which the Shell is by construction the first client); `others()` is what "is anyone
// else using this daemon" resolves to.
class ClientCensus
{
public:
    // Reset to "only this client attached" (count == 1). Called on every (re)attach.
    void reset_to_self() noexcept { count_ = 1; }
    // Fold one `clients` event payload: {event:"attached"} => +1, {event:"detached"} => -1 (floored at
    // 0). Any other shape is ignored.
    void on_clients_event(const contract::Json& payload) noexcept;

    [[nodiscard]] int attached() const noexcept { return count_; }
    [[nodiscard]] int others() const noexcept { return count_ > 1 ? count_ - 1 : 0; }

private:
    int count_ = 0;
};

// ------------------------------------------------------------------- pure: reconnect / read-only

enum class DaemonLinkState
{
    attached,  // live, read-write
    read_only, // the daemon is lost; reconnecting with backoff
};

// Bounded exponential backoff for the reattach ladder (a sibling of the SDK's BackoffPolicy, kept here
// so the read-only STATE machine is testable end to end without waiting).
struct ReconnectPolicy
{
    int initial_ms = 200;
    int max_ms = 5000;
    int multiplier = 2;
};

// The read-only STATE machine (03 §7). Drives the timing of reattach attempts off an injected clock so
// the whole attached -> lost -> read-only -> reattached cycle is a deterministic ctest.
class ReconnectController
{
public:
    ReconnectController() = default;
    explicit ReconnectController(ReconnectPolicy policy) : policy_(policy) {}

    [[nodiscard]] DaemonLinkState state() const noexcept { return state_; }
    [[nodiscard]] int attempts() const noexcept { return attempts_; }

    // The link just went live (first attach OR a successful reattach): back to `attached`, ladder reset.
    void note_attached(std::int64_t now_ms) noexcept;
    // The link dropped: enter `read_only` and arm the FIRST reattach for now + initial backoff.
    void note_lost(std::int64_t now_ms) noexcept;
    // In `read_only`, is a reattach attempt due at `now_ms`?
    [[nodiscard]] bool attempt_due(std::int64_t now_ms) const noexcept;
    // Record a reattach attempt made at `now_ms`. Success => `attached` (ladder reset via note_attached
    // by the caller is not required — this does it). Failure => arm the next attempt on a grown delay.
    void note_attempt_result(bool succeeded, std::int64_t now_ms) noexcept;

    // The delay before the 0-based `attempt`: initial * multiplier^attempt, clamped to max_ms. Pure.
    [[nodiscard]] int delay_for_attempt(int attempt) const noexcept;

private:
    ReconnectPolicy policy_;
    DaemonLinkState state_ = DaemonLinkState::attached;
    std::int64_t next_attempt_ms_ = 0;
    int attempts_ = 0;
};

// Best-effort resolution of the `context` daemon binary to spawn, relative to the editor executable.
// Checks the install-layout sibling and the dev build-tree layout (`<build>/editor/shell` ->
// `<build>/cli`); returns the first that exists, else a sibling best-guess so spawn reports a clear
// error. A caller (or a test) that knows the path passes it directly to spawn_or_attach instead.
[[nodiscard]] std::filesystem::path
locate_context_binary(const std::filesystem::path& editor_executable);

// ---------------------------------------------------------------------------- the coordinator

class DaemonLifecycle
{
public:
    using SnapshotHandler =
        std::function<void(const std::string& sub_id, const contract::Json& snapshot)>;
    using EventHandler =
        std::function<void(const std::string& sub_id, const client::ClientEvent& event)>;
    // Invoked on every (re)attach with the now-live client, so the Shell re-protects the fresh D20
    // token in its egress guard (a reattach after a daemon restart mints a NEW token).
    using AttachedHandler = std::function<void(client::Client& client)>;

    DaemonLifecycle() = default;
    ~DaemonLifecycle();
    DaemonLifecycle(const DaemonLifecycle&) = delete;
    DaemonLifecycle& operator=(const DaemonLifecycle&) = delete;

    // --- configuration (before spawn_or_attach) ---
    // The daemon topics to subscribe to on every attach (the live feed the Shell renders). `clients` is
    // ALWAYS added on top for the exit-policy census, whether or not it is listed here.
    void set_subscription_topics(std::vector<std::string> topics) { topics_ = std::move(topics); }
    void set_reconnect_policy(ReconnectPolicy policy) { reconnect_policy_ = policy; }
    void on_snapshot(SnapshotHandler handler) { on_snapshot_ = std::move(handler); }
    void on_event(EventHandler handler) { on_event_ = std::move(handler); }
    void on_attached(AttachedHandler handler) { on_attached_ = std::move(handler); }

    // --- the spine ---
    // Resolve `project`: attach to a live daemon if one is discoverable, else spawn `daemon_binary`
    // (`context`) as a child and attach over the token it prints on stdout. true when attached; false
    // (+ last_error()) otherwise (the caller opens read-only and pump() keeps retrying). NEVER throws.
    [[nodiscard]] bool spawn_or_attach(const std::filesystem::path& project,
                                       const std::filesystem::path& daemon_binary,
                                       std::string& error);

    // Drive the link once (call from the owner loop): pump the live subscription; on a lost wire enter
    // read-only and, on the backoff schedule, re-establish (discover an external recovery daemon, else
    // respawn an owned one) and re-subscribe (the e02 re-snapshot). `now_ms` is a steady clock in ms.
    void pump(std::int64_t now_ms);

    // The exit policy (idempotent): an owned daemon this process is the last client of gets a clean
    // in-band `shutdown`; an owned daemon other clients still hold is DETACHED (left running); an
    // external daemon is never touched.
    void shutdown_at_exit();

    // --- observation ---
    [[nodiscard]] client::Client* client() noexcept { return client_.get(); }
    [[nodiscard]] DaemonOwnership ownership() const noexcept { return ownership_; }
    [[nodiscard]] bool owns_daemon() const noexcept
    {
        return ownership_ == DaemonOwnership::spawned_owned;
    }
    [[nodiscard]] bool attached() const noexcept
    {
        return client_ != nullptr && link_.state() == DaemonLinkState::attached;
    }
    // The read-only STATE (03 §7): true whenever the Shell is NOT live read-write — no daemon yet, or a
    // lost daemon mid-reconnect. This is the state the e14d banner will render.
    [[nodiscard]] bool read_only() const noexcept { return !attached(); }
    // Bumps on every successful (re)attach. A caller polls it to notice a NEW live client cheaply.
    [[nodiscard]] std::uint64_t attach_generation() const noexcept { return attach_generation_; }
    [[nodiscard]] const ClientCensus& census() const noexcept { return census_; }
    // The endpoint + D20 token of the daemon currently attached. For the OWNED path this token arrived
    // over stdio; the caller protects it from the egress guard from HERE (the directly-built Client's
    // own instance() is empty on the spawn path).
    [[nodiscard]] const client::InstanceInfo& instance() const noexcept { return instance_; }
    // True when the CURRENT daemon's token was read off the spawn stdio pipe (an owned daemon) rather
    // than discovered from instance.json (an external one) — the DoD "token via stdio" assertion.
    [[nodiscard]] bool token_via_stdio() const noexcept { return token_via_stdio_; }
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }

private:
    [[nodiscard]] bool establish(std::string& error);       // one full attach-or-spawn cycle
    [[nodiscard]] bool attach_existing(std::string& error); // discover + attach (external)
    // Build a client over `info.endpoint` and attach with `info.token`; sets client_ + instance_.
    [[nodiscard]] bool attach_at(const client::InstanceInfo& info, std::string& error);
    // Spawn the daemon child, read the token off its stdout, attach (owned). `deferred_to_existing` is
    // set when the child hit the single-instance lock race and exited — the caller attaches externally.
    [[nodiscard]] bool spawn_and_attach(std::string& error, bool& deferred_to_existing);
    void start_subscription(); // (re)subscribe topics + clients census
    void tear_down_link();     // drop the client/subscription on a loss

    std::filesystem::path project_;
    std::filesystem::path daemon_binary_;
    std::vector<std::string> topics_ = {"diagnostics", "derivation"};
    ReconnectPolicy reconnect_policy_;
    SnapshotHandler on_snapshot_;
    EventHandler on_event_;
    AttachedHandler on_attached_;

    std::unique_ptr<client::Client> client_;
    std::unique_ptr<client::SubscriptionConsumer> subscription_;
    common::process::ChildProcess child_; // valid only for an owned daemon
    client::InstanceInfo instance_;
    DaemonOwnership ownership_ = DaemonOwnership::none;
    ReconnectController link_{reconnect_policy_};
    ClientCensus census_;
    std::uint64_t attach_generation_ = 0;
    bool token_via_stdio_ = false;
    bool closed_ = false;
    std::string last_error_;
};

} // namespace context::editor::shell
