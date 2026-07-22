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
//   * editor.select / editor.selection-get / editor.camera-set / editor.cameras-get /
//     editor.play|pause|stop|step -> the M9 e08a DAEMON SESSION STATE (D7 tier 1): the semantic
//                                   human state — selection, cameras, play — held HERE so every
//                                   client shares one truth. Each real change publishes a `session`
//                                   topic fact stamped with the acting client's id as `origin` (the
//                                   echo-suppression contract). session_control scope.
//   * shutdown                   -> ask the serve loop to stop; requires session_control.
//   * describe / anything else   -> the dispatcher's default registry routing.
//
// Session-state PERSISTENCE (03 §1 — the daemon is the SINGLE writer of `.editor/session.json`; the
// Shell owns `config.json`/layout and never this file): serve() restores it before accepting the
// first client and writes it back on a clean stop. A corrupt file is renamed aside, defaults are
// loaded, and the recovery is announced LOUDLY on the `diagnostics` topic + stderr (07 §6) —
// never silently, never blocking the boot.
//
// Large results (R-CLI-017 / R-BRIDGE-007): serve() passes every response through
// finalize_response(), which spools a response bigger than the (settable) threshold into the
// ResourceStore and replaces it with a small `largeResult` envelope carrying the opaque handle; the
// client fetches the payload over the SAME channel via resource.read (CLI: `context fetch`).
//
// Multi-client concurrent fan-in (M9 D19): serve() accepts N concurrent connections up to a bound;
// each connection is served by ONE thread that owns its handle for BOTH reads and writes — a bounded
// timed read interleaved with flushing an outbound queue, deliberately NOT a reader/writer split on a
// duplicated handle (a concurrent read + write on one synchronous Windows pipe file object
// deadlocks). ALL request dispatch is serialized through ONE
// mutex, so the mutation model stays single-threaded (L-50 preserved — concurrency lives at the
// TRANSPORT, never the write queue). After every dispatch, newly-published events are fanned out to
// each subscribed connection's bounded outbound queue; a stuck client's queue fills and it gets a
// re-snapshot gap marker instead of ever stalling the daemon (R-BRIDGE-008). Bounds — max concurrent
// connections + a per-connection outbound event-frame budget — are configurable with sane defaults.
// Attach-token enforcement (D20) lives in the dispatcher (see Daemon::set_attach_auth); this loop is
// auth-agnostic.

#pragma once

#include "context/editor/bridge/dispatcher.h"
#include "context/editor/bridge/resource_store.h"
#include "context/editor/bridge/transport.h"
#include "context/editor/editorkernel/editor_kernel.h"
#include "context/editor/editorkernel/editor_session_state.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace context::editor::editorkernel
{

// The D19 connection-bound refusal (promote-a-local-string into the versioned error-code catalog,
// like bridge::kScopeDeniedCode): the (N+1)th attach over max_connections() is refused with this
// catalog code (transient — a slot frees when a client detaches). Grep-stable, identical to the
// catalog entry, so the serve loop references the one catalog row by value.
inline constexpr const char* kDaemonBusyCode = "daemon.busy";

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
    // Accepts up to max_connections() concurrent clients (D19); each gets a fresh Session + a single
    // connection thread (a timed read interleaved with an outbound-queue flush, one handle, no dup),
    // and every framed request funnels through ONE serialized dispatch
    // (L-50) then finalize_response() (the R-CLI-017 large-result spool). Events published by any
    // client's dispatch are fanned out to every subscribed client. Returns 0 on a clean stop,
    // non-zero if the listener fails (server.error() set). Blocking — runs on the calling thread.
    int serve(bridge::TransportServer& server);

    // D19 bounds (config with sane defaults). max_connections: the cap on concurrently-served
    // clients; the (N+1)th attach is refused `daemon.busy` (transient) rather than served. Default 16.
    // max_outbound_frames: the per-connection outbound EVENT-frame budget — once this many undelivered
    // event frames are queued for a slow client, further events are dropped and the client is sent a
    // re-snapshot gap marker (never stalling the daemon). Responses are never dropped. Default 256.
    // Both clamp to >= 1. Set BEFORE serve() (they are read on the serve/connection threads).
    void set_max_connections(std::size_t n) noexcept { max_connections_ = (n == 0 ? 1 : n); }
    void set_max_outbound_frames(std::size_t n) noexcept { max_outbound_frames_ = (n == 0 ? 1 : n); }
    [[nodiscard]] std::size_t max_connections() const noexcept { return max_connections_; }
    [[nodiscard]] std::size_t max_outbound_frames() const noexcept { return max_outbound_frames_; }

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

    // The M9 e08a daemon session state (selection / cameras / play). Exposed for tests and for the
    // composing layer; every wire mutation goes through invoke(), which is serialized with the rest
    // of dispatch (L-50). Reading this from another thread while serve() runs is NOT safe.
    [[nodiscard]] const EditorSessionState& session_state() const noexcept { return session_; }

    // Restore the session state from `.editor/session.json` (serve() calls this before accepting the
    // first client). Exposed so a test can drive the recovery path without a serve loop; the report
    // is what the caller announces LOUDLY on a `recovered` outcome (07 §6).
    SessionRestoreReport restore_session();

    // Persist the session state to `.editor/session.json` (serve() calls this on a clean stop).
    // Returns false + fills `error` when the write failed; a failed persist is reported, never fatal.
    bool persist_session(std::string& error) const;

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
    // The M9 e08a editor session state (D7 tier 1). Mutable because the `editor.*` verbs mutate it
    // from the const invoke() path — the same reason `stop_` is mutable. Every access is serialized
    // by the serve loop's single dispatch mutex (L-50), so it needs no lock of its own.
    mutable EditorSessionState session_;
    mutable std::optional<bridge::ResourceStore> resources_;
    std::uint64_t large_result_threshold_ = bridge::kLargeResultThresholdBytes;
    // Mutable so the `shutdown` verb can flip it from the const invoke() path; the serve loop reads it.
    mutable std::atomic<bool> stop_{false};
    // D19 bounds (see set_max_connections / set_max_outbound_frames).
    std::size_t max_connections_ = 16;
    std::size_t max_outbound_frames_ = 256;
};

} // namespace context::editor::editorkernel
