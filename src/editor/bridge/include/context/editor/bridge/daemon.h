// EditorKernel daemon-per-worktree (R-BRIDGE-001 / R-ARCH-005 / L-26): the write-capable instance
// that owns one Project's derived world and serves attached clients.
//
// start() performs the R-ARCH-005 startup algorithm's decisive step: the ATOMIC FIRST action of a
// write-capable instantiation is a try-lock of the single-instance `.editor/lock` (R-BRIDGE-001).
// A try-lock FAILURE is itself the "an instance is already live → attach" signal — one gate, no
// separate liveness probe to race. On success the daemon brings up its client-facing event stream
// (a fresh incarnation) and the scope-enforcing RPC dispatcher; a client attaches over JSON-RPC 2.0.
// The operator's launch-time scope ceiling (R-SEC-007) clamps what any attaching client may be
// granted. Instance identity is keyed by canonical project path, so each git worktree is its own
// daemon (L-26).

#pragma once

#include "context/editor/bridge/dispatcher.h"
#include "context/editor/bridge/event_stream.h"
#include "context/editor/bridge/lock.h"
#include "context/editor/bridge/scope.h"
#include "context/editor/contract/handshake.h"

#include <filesystem>
#include <optional>
#include <string>

namespace context::editor::bridge
{

enum class StartOutcome
{
    booted, // the lock was acquired; this daemon owns the Project
    attach, // an instance is already live — the caller should attach instead (R-BRIDGE-001)
    error,  // an unexpected OS/filesystem error prevented startup (see error_message())
};

class Daemon
{
public:
    explicit Daemon(const std::filesystem::path& project_root);

    Daemon(const Daemon&) = delete;
    Daemon& operator=(const Daemon&) = delete;

    // Bring the daemon up. The lock try-acquire is the atomic FIRST action: exclusive for a
    // write-capable instance, shared for a read-only one. `launch_scopes` is the operator's scope
    // ceiling (default: all scopes). Returns `attach` on a try-lock failure (do NOT boot a second
    // instance), `booted` on success, `error` otherwise.
    StartOutcome start(bool write_capable = true, ScopeSet launch_scopes = ScopeSet::all());

    // Release the lock and tear down the stream/dispatcher. Idempotent.
    void stop();

    [[nodiscard]] bool running() const noexcept { return running_; }
    [[nodiscard]] const ProjectLock& lock() const noexcept { return lock_; }
    [[nodiscard]] ScopeSet launch_scopes() const noexcept { return launch_scopes_; }
    [[nodiscard]] const std::string& error_message() const noexcept { return error_; }

    // Valid only while running(). The event stream + dispatcher come up on a successful start().
    [[nodiscard]] EventStream& events() { return *events_; }
    [[nodiscard]] const Dispatcher& dispatcher() const { return *dispatcher_; }

    // Attach a client, clamping its requested scopes to the launch-time operator ceiling (R-SEC-007
    // least privilege). Valid only while running().
    [[nodiscard]] Dispatcher::AttachResult attach_client(const contract::ClientHandshake& client,
                                                         ScopeSet requested) const;

private:
    ProjectLock lock_;
    bool running_ = false;
    ScopeSet launch_scopes_;
    std::string error_;
    std::optional<EventStream> events_;
    std::optional<Dispatcher> dispatcher_;
};

} // namespace context::editor::bridge
