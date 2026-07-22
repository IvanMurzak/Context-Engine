// The daemon lifecycle spine implementation (see daemon_lifecycle.h).

#include "context/editor/shell/daemon_lifecycle.h"

#include "context/editor/shell/shell.h" // guard_shell_attach / make_shell_attach_options (D10 attach)

#include <algorithm>
#include <chrono>
#include <optional>
#include <utility>

namespace context::editor::shell
{

namespace
{
// The spawned `context daemon` exits with THIS code when it finds a live daemon already holding the
// single-instance lock (R-BRIDGE-001) — a launch race. Mirrors cli::kDaemonAttachSignalExit; the Shell
// cannot link the CLI, so the process-protocol constant is duplicated with this note.
constexpr int kDaemonAttachSignalExit = 3;
constexpr int kDiscoverTimeoutMs = 400;     // "is a daemon already up?" — short so no-daemon -> spawn is fast
constexpr int kConnectTimeoutMs = 5000;
constexpr int kSpawnReadyTimeoutMs = 20000; // wait for the child's ready line (widened for slow CI legs)
constexpr int kShutdownWaitMs = 5000;

int now_remaining_ms(std::chrono::steady_clock::time_point deadline)
{
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             deadline - std::chrono::steady_clock::now())
                             .count();
    if (ms <= 0)
        return 0;
    if (ms > 0x7fffffffLL)
        return 0x7fffffff;
    return static_cast<int>(ms);
}
} // namespace

// --------------------------------------------------------------------------- pure: exit policy

DaemonExitAction decide_daemon_exit_action(bool owns_daemon, int other_clients_attached) noexcept
{
    if (owns_daemon && other_clients_attached <= 0)
        return DaemonExitAction::shutdown_owned;
    return DaemonExitAction::leave_running;
}

// --------------------------------------------------------------------------- pure: client census

void ClientCensus::on_clients_event(const contract::Json& payload) noexcept
{
    if (!payload.is_object() || !payload.contains("event") || !payload.at("event").is_string())
        return;
    const std::string& event = payload.at("event").as_string();
    if (event == "attached")
        ++count_;
    else if (event == "detached" && count_ > 0)
        --count_;
}

// ------------------------------------------------------------------ pure: reconnect controller

void ReconnectController::note_attached(std::int64_t /*now_ms*/) noexcept
{
    state_ = DaemonLinkState::attached;
    attempts_ = 0;
    next_attempt_ms_ = 0;
}

void ReconnectController::note_lost(std::int64_t now_ms) noexcept
{
    state_ = DaemonLinkState::read_only;
    attempts_ = 0;
    next_attempt_ms_ = now_ms + delay_for_attempt(0);
}

bool ReconnectController::attempt_due(std::int64_t now_ms) const noexcept
{
    return state_ == DaemonLinkState::read_only && now_ms >= next_attempt_ms_;
}

void ReconnectController::note_attempt_result(bool succeeded, std::int64_t now_ms) noexcept
{
    if (succeeded)
    {
        note_attached(now_ms);
        return;
    }
    ++attempts_;
    next_attempt_ms_ = now_ms + delay_for_attempt(attempts_);
}

int ReconnectController::delay_for_attempt(int attempt) const noexcept
{
    if (attempt <= 0 || policy_.multiplier <= 1)
        return std::min(policy_.initial_ms, policy_.max_ms);
    long long delay = policy_.initial_ms;
    for (int i = 0; i < attempt; ++i)
    {
        delay *= policy_.multiplier;
        if (delay >= policy_.max_ms)
            return policy_.max_ms;
    }
    return static_cast<int>(delay);
}

std::filesystem::path locate_context_binary(const std::filesystem::path& editor_executable)
{
    namespace fs = std::filesystem;
    const fs::path dir =
        editor_executable.has_parent_path() ? editor_executable.parent_path() : fs::current_path();
#if defined(_WIN32)
    const char* exe = "context.exe";
#else
    const char* exe = "context";
#endif
    const fs::path candidates[] = {
        dir / exe,                             // install layout: the CLI ships beside the editor
        dir / ".." / ".." / "cli" / exe,       // dev build tree: <build>/editor/shell -> <build>/cli
        dir / ".." / "cli" / exe,
    };
    for (const fs::path& candidate : candidates)
    {
        std::error_code ec;
        if (fs::exists(candidate, ec))
            return candidate.lexically_normal();
    }
    return (dir / exe).lexically_normal(); // best guess; spawn reports a clear error if it is absent
}

// ------------------------------------------------------------------------------ the coordinator

DaemonLifecycle::~DaemonLifecycle()
{
    // Members destruct in reverse order: subscription_ (references client_) first, then client_, then
    // child_. ChildProcess's destructor DETACHES (leaves a still-running daemon alive without killing
    // it) — the safe default when a caller forgot shutdown_at_exit(). A caller that wants the owned
    // daemon gone calls shutdown_at_exit() (clean) or terminate via that path.
}

bool DaemonLifecycle::attach_existing(std::string& error)
{
    const std::optional<client::InstanceInfo> info =
        client::discover_instance(project_, kDiscoverTimeoutMs);
    if (!info.has_value())
    {
        error = "no discoverable daemon for '" + project_.string() + "'";
        return false;
    }
    return attach_at(*info, error);
}

bool DaemonLifecycle::attach_at(const client::InstanceInfo& info, std::string& error)
{
    std::unique_ptr<client::WireChannel> channel =
        client::make_transport_channel(info.endpoint, kConnectTimeoutMs);
    if (channel == nullptr)
    {
        error = "could not connect to the daemon endpoint '" + info.endpoint + "'";
        return false;
    }
    auto candidate = std::make_unique<client::Client>(std::move(channel));

    // The Shell has NO unauthenticated path (token enforcement on since e02): refuse rather than let
    // the daemon return an opaque attach.denied. options.token IS info.token (from stdio or discovery).
    const client::AttachOptions options = make_shell_attach_options(info.token);
    std::string reason;
    if (!guard_shell_attach(options, info.token, reason))
    {
        error = reason;
        return false;
    }
    std::string attach_err;
    bool rejected = false;
    if (!candidate->attach(options, attach_err, &rejected))
    {
        error = attach_err;
        return false;
    }
    client_ = std::move(candidate);
    instance_ = info;
    return true;
}

bool DaemonLifecycle::spawn_and_attach(std::string& error, bool& deferred_to_existing)
{
    deferred_to_existing = false;
    if (daemon_binary_.empty())
    {
        error = "no daemon binary configured to spawn";
        return false;
    }

    common::process::SpawnOptions options;
    options.executable = daemon_binary_;
    options.args = {"daemon", "--project", project_.string()}; // token is NEVER an arg — it comes on stdout
    options.capture_stdout = true;

    std::string spawn_err;
    common::process::ChildProcess child = common::process::ChildProcess::spawn(options, spawn_err);
    if (!child.valid())
    {
        error = "could not spawn '" + daemon_binary_.string() + "': " + spawn_err;
        return false;
    }

    // Read the D20 attach token off the child's STDOUT (never argv/env). Drain non-marker lines (the
    // daemon's own pretty "listening" envelope, log lines) until the ready marker, EOF, or the timeout.
    client::InstanceInfo ready;
    bool got_ready = false;
    std::string line;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kSpawnReadyTimeoutMs);
    while (now_remaining_ms(deadline) > 0)
    {
        if (!child.read_line(line, now_remaining_ms(deadline)))
            break; // timeout, or the child closed stdout / exited
        if (std::optional<client::InstanceInfo> parsed = client::parse_daemon_ready_line(line))
        {
            ready = *parsed;
            got_ready = true;
            break;
        }
    }

    if (!got_ready)
    {
        // A launch race: the daemon found a live one holding the lock and exited kDaemonAttachSignalExit
        // — tell the caller to attach to the winner instead.
        int code = -1;
        const bool exited = child.wait(1000, code);
        child.terminate();
        if (exited && code == kDaemonAttachSignalExit)
        {
            deferred_to_existing = true;
            error = "the spawned daemon deferred to an existing one (single-instance lock)";
            return false;
        }
        error = "the spawned daemon did not announce readiness on stdout";
        return false;
    }

    std::string attach_err;
    if (!attach_at(ready, attach_err))
    {
        child.terminate();
        error = attach_err;
        return false;
    }
    child_ = std::move(child); // own the daemon
    return true;
}

bool DaemonLifecycle::establish(std::string& error)
{
    // 1. A wire blip to a still-live OWNED daemon: reattach to it, keeping ownership + the child. (A
    //    real daemon death makes child_.running() false, so this is skipped and we respawn below.)
    if (ownership_ == DaemonOwnership::spawned_owned && child_.valid() && child_.running() &&
        !instance_.endpoint.empty())
    {
        std::string reattach_err;
        if (attach_at(instance_, reattach_err))
        {
            token_via_stdio_ = true;
            return true;
        }
        // The owned process lingers but is unreachable — fall through to discover/respawn.
    }

    // 2. Attach to a discoverable daemon (an external one, or a recovery daemon that came up).
    std::string attach_err;
    if (attach_existing(attach_err))
    {
        ownership_ = DaemonOwnership::attached_external;
        token_via_stdio_ = false;
        child_.terminate(); // drop any stale child; the daemon we attached to is not ours
        return true;
    }

    // 3. Spawn a fresh daemon (owned).
    std::string spawn_err;
    bool deferred = false;
    if (spawn_and_attach(spawn_err, deferred))
    {
        ownership_ = DaemonOwnership::spawned_owned;
        token_via_stdio_ = true;
        return true;
    }
    if (deferred)
    {
        // The winner of the lock race is now discoverable — attach to it as external.
        std::string ae;
        if (attach_existing(ae))
        {
            ownership_ = DaemonOwnership::attached_external;
            token_via_stdio_ = false;
            return true;
        }
    }
    error = "attach failed (" + attach_err + "); spawn failed (" + spawn_err + ")";
    return false;
}

void DaemonLifecycle::start_subscription()
{
    census_.reset_to_self(); // this process's own attachment is the baseline
    if (client_ == nullptr)
        return;

    client::SubscriptionOptions options;
    options.poll_timeout_ms = 0;        // non-blocking — driven from the owner loop
    options.reconnect_timeout_ms = 250; // the consumer's OWN reconnect covers a transient blip; a real
    options.backoff.initial_ms = 50;    // daemon death (new token) fails it fast, and pump() then
    options.backoff.max_ms = 250;       // re-establishes at the lifecycle level (fresh token)
    options.backoff.max_attempts = 2;

    subscription_ = std::make_unique<client::SubscriptionConsumer>(
        *client_, make_shell_attach_options(instance_.token), options);
    subscription_->on_snapshot(
        [this](const std::string& sub_id, const contract::Json& snapshot)
        {
            if (on_snapshot_)
                on_snapshot_(sub_id, snapshot);
        });
    subscription_->on_event(
        [this](const std::string& sub_id, const client::ClientEvent& event)
        {
            if (event.topic == "clients")
                census_.on_clients_event(event.payload); // the exit-policy census (D19)
            if (on_event_)
                on_event_(sub_id, event);
        });

    client::SubscriptionSpec spec;
    spec.topics = topics_;
    if (std::find(spec.topics.begin(), spec.topics.end(), std::string("clients")) ==
        spec.topics.end())
        spec.topics.push_back("clients"); // ALWAYS subscribe clients for the census
    (void)subscription_->add(spec);

    std::string subscribe_err;
    if (!subscription_->start(subscribe_err))
    {
        // The feed failed to establish; keep the (attached) client, drop the consumer. The live feed is
        // simply empty until the next re-establish — the same reported-not-fatal posture as a fresh
        // attach with no subscribable stream.
        subscription_.reset();
    }
}

void DaemonLifecycle::tear_down_link()
{
    subscription_.reset();
    client_.reset();
    instance_ = client::InstanceInfo{};
    // child_ is kept: establish() consults child_.running() to choose reattach-owned vs respawn.
}

bool DaemonLifecycle::spawn_or_attach(const std::filesystem::path& project,
                                      const std::filesystem::path& daemon_binary, std::string& error)
{
    project_ = project;
    daemon_binary_ = daemon_binary;
    link_ = ReconnectController(reconnect_policy_);
    closed_ = false;

    std::string establish_err;
    if (!establish(establish_err))
    {
        last_error_ = establish_err;
        error = establish_err;
        link_.note_lost(0); // read-only immediately; pump() retries on the backoff schedule
        return false;
    }

    start_subscription();
    link_.note_attached(0);
    ++attach_generation_;
    if (on_attached_ && client_ != nullptr)
        on_attached_(*client_);
    return true;
}

void DaemonLifecycle::pump(std::int64_t now_ms)
{
    if (closed_)
        return;

    if (link_.state() == DaemonLinkState::attached)
    {
        if (client_ == nullptr)
        {
            link_.note_lost(now_ms);
            return;
        }
        bool lost = false;
        if (subscription_ != nullptr)
        {
            // A false pump is UNRECOVERABLE by the consumer's own definition (its backoff was exhausted,
            // or the daemon refused a re-subscribe) — a real daemon death, not a transient blip.
            std::string pump_err;
            if (!subscription_->pump(pump_err))
                lost = true;
        }
        if (!lost && !client_->connected())
            lost = true;
        if (lost)
        {
            tear_down_link();
            link_.note_lost(now_ms);
        }
        return;
    }

    // read_only: retry on the backoff schedule (03 §7).
    if (!link_.attempt_due(now_ms))
        return;
    std::string establish_err;
    const bool ok = establish(establish_err);
    link_.note_attempt_result(ok, now_ms);
    if (ok)
    {
        start_subscription();
        ++attach_generation_;
        if (on_attached_ && client_ != nullptr)
            on_attached_(*client_);
    }
    else
    {
        last_error_ = establish_err;
    }
}

void DaemonLifecycle::shutdown_at_exit()
{
    if (closed_)
        return;
    closed_ = true;

    const DaemonExitAction action = decide_daemon_exit_action(owns_daemon(), census_.others());

    if (action == DaemonExitAction::shutdown_owned && client_ != nullptr)
    {
        // Clean in-band shutdown of OUR daemon. call() uses a blocking read (wire.h), so the reply is
        // not lost even though the daemon exits in the same breath.
        std::string shutdown_err;
        (void)client_->call("shutdown", contract::Json::object(), shutdown_err);
    }

    subscription_.reset();
    client_.reset();

    if (child_.valid())
    {
        if (action == DaemonExitAction::shutdown_owned)
        {
            int code = -1;
            if (!child_.wait(kShutdownWaitMs, code))
                child_.terminate(); // it ignored the clean shutdown — make sure it is gone
        }
        else
        {
            child_.detach(); // leave it running for the other attached clients
        }
    }
    ownership_ = DaemonOwnership::none;
}

} // namespace context::editor::shell
