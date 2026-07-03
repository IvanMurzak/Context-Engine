// KernelServer: hosts a composed EditorKernel OVER the real IPC transport (R-BRIDGE-003/007) so a
// SEPARATE client process attaches to it over the wire (R-ARCH-005 / R-BRIDGE-002).
//
// It is the composing layer the bridge deliberately cannot reach on its own: it implements
// bridge::MethodBackend (supplying the real backing for the operational cross-process verbs — edit /
// query / shutdown) and drives the accept→serve loop over a bridge::TransportServer, delegating every
// framed message to the EXISTING JSON-RPC 2.0 dispatcher (attach handshake, scope enforcement,
// describe, error envelopes) — the transport is framed AROUND that one dispatcher, not a second one.
//
// Operational verb surface (NOT the R-CLI-009 contract registry — the daemon-driver surface, the
// cross-process analogue of `context editor smoke`):
//   * attach   — the capability handshake (served by the dispatcher itself).
//   * edit      {path, content}  -> daemon-initiated file write (filesync atomic-IO) + read-your-writes
//                                   barrier; requires the file_write scope (R-SEC-007).
//   * query     {path}           -> the derived node (canonical hash + generation) + world stats; read.
//   * shutdown                   -> ask the serve loop to stop; requires session_control.
//   * describe / anything else   -> the dispatcher's default registry routing.
//
// Serial single-connection model (M1): one client at a time, served to disconnect. See transport.h.

#pragma once

#include "context/editor/bridge/dispatcher.h"
#include "context/editor/bridge/transport.h"
#include "context/editor/editorkernel/editor_kernel.h"

#include <atomic>

namespace context::editor::editorkernel
{

class KernelServer final : public bridge::MethodBackend
{
public:
    // Non-owning: `kernel` must outlive the server. Registers itself as the kernel's method backend,
    // so it MUST be constructed BEFORE kernel.start() (the dispatcher captures the backend at boot).
    explicit KernelServer(EditorKernel& kernel);

    KernelServer(const KernelServer&) = delete;
    KernelServer& operator=(const KernelServer&) = delete;

    // bridge::MethodBackend — serve the operational verbs; nullopt for anything else (the dispatcher
    // then applies its default registry routing). Runs after the dispatcher's R-SEC-007 scope check.
    [[nodiscard]] std::optional<contract::Envelope>
    invoke(const std::string& method, const contract::Json& params,
           const bridge::Session& session) const override;

    // Accept + serve clients over `server` until a `shutdown` message (or stop()) breaks the loop.
    // Each connection gets a fresh Session; every framed request goes through the composed kernel's
    // dispatcher. Returns 0 on a clean stop. Blocking — runs on the calling thread.
    int serve(bridge::TransportServer& server);

    // Ask the serve loop to stop (thread-safe). Also invoked by the `shutdown` verb.
    void request_stop() noexcept { stop_.store(true); }
    [[nodiscard]] bool stop_requested() const noexcept { return stop_.load(); }

private:
    EditorKernel& kernel_;
    // Mutable so the `shutdown` verb can flip it from the const invoke() path; the serve loop reads it.
    mutable std::atomic<bool> stop_{false};
};

} // namespace context::editor::editorkernel
