// KernelServer: hosts a composed EditorKernel OVER the real IPC transport (R-BRIDGE-003/007) so a
// SEPARATE client process attaches to it over the wire (R-ARCH-005 / R-BRIDGE-002).
//
// It is the composing layer the bridge deliberately cannot reach on its own: it implements
// bridge::MethodBackend (supplying the real backing for the operational cross-process verbs — edit /
// query / shutdown) and drives the accept→serve loop over a bridge::TransportServer, delegating every
// framed message to the EXISTING JSON-RPC 2.0 dispatcher (attach handshake, scope enforcement,
// describe, error envelopes) — the transport is framed AROUND that one dispatcher, not a second one.
//
// Operational verb surface (registered in the ONE registry as the explicit "operational, unstable"
// section — R-CLI-009 honesty; served HERE, by the daemon's method backend):
//   * attach   — the capability handshake (served by the dispatcher itself).
//   * edit      {path, content}  -> daemon-initiated file write (filesync atomic-IO) + read-your-writes
//                                   barrier; requires the file_write scope (R-SEC-007).
//   * edit-batch {files:[{path, content}…]} -> MULTI-file write serialized through the R-FILE-004
//                                   crash-recovery intent log; requires file_write (R-SEC-007 — the
//                                   method is in the dispatcher's file-write family, scope.cpp).
//   * query     {path}           -> the derived node (canonical hash + generation) + world stats; read.
//   * snapshot                   -> the R-BRIDGE-008 current-state snapshot (incarnationId, generation,
//                                   lastSeq, worldEntities, worldGeneration) + the boot recovery
//                                   diagnostics; read.
//   * reconcile                  -> fold external (out-of-band) edits into the derived world via the
//                                   watch-hash-reconcile pipeline, FORCING the full re-hash crawl
//                                   (the R-FILE-002 crawl-on-demand path for bulk ops), then settle;
//                                   read (it mutates no authored state — the daemon notices truth).
//   * resource.read {handle, range} -> read a chunk of an oversized spooled result by its
//                                   R-CLI-017 opaque handle (STABLE contract verb, served here
//                                   because the store lives with the composed daemon); read.
//   * shutdown                   -> ask the serve loop to stop; requires session_control.
//   * describe / anything else   -> the dispatcher's default registry routing.
//
// Large results (R-CLI-017 / R-BRIDGE-007): serve() passes every response through
// finalize_response(), which spools a response bigger than the (settable) threshold into the
// ResourceStore and replaces it with a small `largeResult` envelope carrying the opaque handle; the
// client fetches the payload over the SAME channel via resource.read (CLI: `context fetch`).
//
// Serial single-connection model (M1): one client at a time, served to disconnect. See transport.h.

#pragma once

#include "context/editor/bridge/dispatcher.h"
#include "context/editor/bridge/resource_store.h"
#include "context/editor/bridge/transport.h"
#include "context/editor/editorkernel/editor_kernel.h"

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>

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
    // dispatcher, then through finalize_response() (the R-CLI-017 large-result spool). Returns 0 on
    // a clean stop, non-zero if the listener fails (server.error() set). Blocking — runs on the
    // calling thread.
    int serve(bridge::TransportServer& server);

    // The R-CLI-017 oversized-response gate: a JSON-RPC response whose serialized size exceeds the
    // threshold has its success `result` spooled into the ResourceStore and replaced by a small
    // `largeResult` envelope (same id; generationAfter/warnings preserved; a note appended). Error
    // responses, notifications, and resource.read chunk replies pass through unchanged (spooling a
    // chunk would recurse the fetch). Public so the round-trip is unit-testable without a wire.
    [[nodiscard]] std::string finalize_response(std::string response) const;

    // Override the spool threshold (bytes). Operational/test knob — the default is
    // bridge::kLargeResultThresholdBytes; e2e tests lower it to force the largeResult path.
    void set_large_result_threshold(std::uint64_t bytes) noexcept
    {
        large_result_threshold_ = bytes;
    }

    // Read whether the serve loop has been asked to stop (the `shutdown` verb flips `stop_` directly
    // from the const invoke() path, since it is mutable).
    [[nodiscard]] bool stop_requested() const noexcept { return stop_.load(); }

private:
    // The R-CLI-017 spool: lives under the REAL `<project_root>/.editor/resources/` control dir
    // (outside the reconcile crawl root), keyed by this daemon lifetime's incarnation id — a
    // restart invalidates every previously-minted handle by construction. Constructed LAZILY on
    // first use: the incarnation id lives on the daemon's EventStream, which only exists once
    // kernel.start() has run, while THIS server must be constructed BEFORE start() (the dispatcher
    // captures the backend at boot). Every use site (invoke / finalize_response) runs on a started
    // daemon. Mutable: put() runs from the const invoke()/finalize_response() paths.
    [[nodiscard]] bridge::ResourceStore& resources() const;

    EditorKernel& kernel_;
    mutable std::optional<bridge::ResourceStore> resources_;
    std::uint64_t large_result_threshold_ = bridge::kLargeResultThresholdBytes;
    // Mutable so the `shutdown` verb can flip it from the const invoke() path; the serve loop reads it.
    mutable std::atomic<bool> stop_{false};
};

} // namespace context::editor::editorkernel
