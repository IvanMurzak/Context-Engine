// EditorKernel daemon implementation (see daemon.h).

#include "context/editor/bridge/daemon.h"

namespace context::editor::bridge
{

Daemon::Daemon(const std::filesystem::path& project_root) : lock_(project_root) {}

StartOutcome Daemon::start(bool write_capable, ScopeSet launch_scopes)
{
    if (running_)
        return StartOutcome::booted;

    // The ATOMIC FIRST action of a write-capable instantiation: try-lock the single-instance lock.
    // A try-lock failure IS the "instance already live → attach" signal (R-BRIDGE-001 / R-ARCH-005).
    const LockMode mode = write_capable ? LockMode::exclusive : LockMode::shared;
    const LockOutcome outcome = lock_.try_acquire(mode);
    if (outcome == LockOutcome::already_held)
        return StartOutcome::attach;
    if (outcome == LockOutcome::error)
    {
        error_ = lock_.error_message();
        return StartOutcome::error;
    }

    // Lock held — bring up a fresh incarnation's event stream + the scope-enforcing dispatcher.
    // The dispatcher captures the composing layer's method backend (set before start()) AND the
    // launch-time scope ceiling, so a cross-process client's real verbs are served over the same one
    // JSON-RPC surface and clamped to least privilege on the wire exactly as in-process (R-SEC-007).
    launch_scopes_ = launch_scopes;
    events_.emplace();
    dispatcher_.emplace(&*events_, backend_, launch_scopes);
    running_ = true;

    // Announce the session on the client-facing stream.
    contract::Json started = contract::Json::object();
    started.set("event", contract::Json(std::string("session.started")));
    started.set("writeCapable", contract::Json(write_capable));
    events_->publish("session", std::move(started));

    return StartOutcome::booted;
}

void Daemon::stop()
{
    if (!running_)
        return;
    running_ = false;
    dispatcher_.reset();
    events_.reset();
    lock_.release();
}

Dispatcher::AttachResult Daemon::attach_client(const contract::ClientHandshake& client,
                                               ScopeSet requested) const
{
    // The dispatcher holds the launch-time scope ceiling and clamps to least privilege (R-SEC-007)
    // for BOTH this in-process path and the cross-process wire path, so the clamp lives in ONE place.
    return dispatcher_->attach(client, requested);
}

} // namespace context::editor::bridge
