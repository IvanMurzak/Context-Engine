// EditorKernel: the M1 composition that wires the merged file-authoritative libraries into ONE
// runnable headless attach path (R-ARCH-005 / R-BRIDGE-008).
//
// The six M1 components ship as standalone libraries; this class is the integration that composes
// them into the loop the design authority describes:
//
//   file  --(context_filesync watch-hash-reconcile, R-FILE-002/004)-->  reconciled change
//         --(context_derivation incremental graph, L-19/L-22)-->  derived context::kernel::World
//              (+ the monotonic generation counter, R-CLI-006)
//         --(context_bridge daemon, R-BRIDGE-001)-->  single-instance lock + client event stream
//              + the R-SEC-007 scope-enforcing dispatcher a client attaches to over the handshake.
//
// Everything is built on the injectable platform seams (FileStore / Watcher / Clock / TaskRunner),
// so the whole loop runs deterministically headless (R-HEAD-001 / R-QA-010) with no GPU, display, or
// real wall-clock. Three write paths reach the derived World: edit_file() is the daemon-initiated
// "CLI-verb" write (through filesync atomic-IO, scope-checked); edit_files() is its MULTI-file
// sibling, serialized through the R-FILE-004 crash-recovery intent log (fsync-before / clear-after,
// resumed-or-diagnosed by start() on the next incarnation); and ingest_external() folds in a raw
// out-of-band edit the reconcile crawl detected (content hash is authoritative, R-FILE-002).

#pragma once

#include "context/editor/bridge/daemon.h"
#include "context/editor/contract/envelope.h"
#include "context/editor/derivation/derivation_graph.h"
#include "context/editor/derivation/query_barrier.h"
#include "context/editor/filesync/diagnostic.h"
#include "context/editor/filesync/intent_log.h"
#include "context/editor/filesync/reconciler.h"
#include "context/editor/serializer/json_tree.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace context::kernel
{
class Clock;
class TaskRunner;
class EventBus;
class World;
} // namespace context::kernel

namespace context::editor::filesync
{
class FileStore;
class Watcher;
} // namespace context::editor::filesync

namespace context::editor::migrate
{
class MigrationSet;
class MigrationRunner;
} // namespace context::editor::migrate

namespace context::editor::editorkernel
{

// Composition configuration.
//
// `project_root` keys the daemon's single-instance OS lock and lives on the REAL filesystem (L-26:
// each git worktree is its own daemon). `filesync_root` is the logical root the reconcile pipeline
// path-jails writes against, over the injectable FileStore seam — a plain in-memory FileStore roots
// there while the daemon lock lives under a real directory. When a native FileStore lands the two
// coincide on the same on-disk directory; keeping them distinct is an M1 seam artifact, not a design
// split.
struct EditorKernelConfig
{
    std::filesystem::path project_root = "."; // daemon single-instance lock identity (real FS)
    std::string filesync_root = ".";          // logical FileStore root the path jail is measured from
    std::string index_path = ".editor/index"; // reconcile index (under the FileStore seam)
    derivation::DerivationConfig derivation{};          // coalescing / load-shed tuning
    std::uint64_t barrier_max_passes = 256;             // read-your-writes bound (R-CLI-006)

    // R-FILE-002 crawl-cadence policy: the full re-hash crawl is the dropped-event SAFETY NET, not
    // the ingest cadence. 0 (default) = crawl on EVERY ingest_external() pass — correct for
    // compositions with no real watcher (NullWatcher is always degraded, so the crawl IS change
    // detection). N > 0 = with real OS watchers live, the full crawl runs at most once per N
    // nanoseconds of injected-Clock time; between crawls ingest_external() drains watcher hints
    // only. Bulk ops (the `reconcile` verb, e.g. after a git branch switch) bypass the cadence via
    // CrawlMode::force — crawl-on-demand is always available. The very first pass always crawls.
    std::uint64_t min_crawl_interval_nanos = 0;

    // The L-37 migration set this composition runs under (M2 wave 3). nullptr (the default)
    // resolves to migrate::MigrationSet::engine_set() — the engine-shipped set; tests inject their
    // own. Wired into (a) the derivation graph's parse-time migration node, (b) the tool-save
    // canonicalize+stamp path (edit_file / edit_files), and (c) the R-FILE-005 pass-0
    // registered-set hash (with the engine kind schemas) that keys pass-1 derivation. In the
    // R-FILE-005 cold-start order this registration point sits where "VM → registration (pass 0)"
    // boots.
    const migrate::MigrationSet* migrations = nullptr;

    // The sandboxed-tier VM — the R-FILE-005 cold-start "VM" slot (lock → index → watcher → VM →
    // registration → content parse), issue #71: the embedder boots the runner (the wasmtime
    // WasmRunner, src/runtime/wasm/ — WasmRunner::create() IS the VM init; tests inject a mock)
    // BEFORE constructing the kernel and injects it here, so it exists ahead of pass-0
    // registration and every pass-1 parse. Threaded into (a) the derivation graph's parse-time
    // migration node and (b) the tool-save canonicalize+stamp path, where package_sandboxed steps
    // route through it under the frozen guest ABI (migration_runner.h). nullptr (the default)
    // keeps the honest migration.runner_unavailable refusal — package migrations never run
    // unsandboxed (L-37). Owned by the caller; must outlive the kernel.
    migrate::MigrationRunner* migration_runner = nullptr;
    // (`derivation.registered_set_hash` — the R-FILE-005 pass-0 hash — is COMPUTED at construction
    // from the engine kind schemas + the resolved migration set when left 0; a non-zero value in
    // the embedded DerivationConfig is honored as an explicit override.)
};

// How ingest_external() treats the full re-hash crawl (R-FILE-002).
enum class CrawlMode
{
    policy, // honor min_crawl_interval_nanos (the routine ingest cadence)
    force,  // run the full crawl regardless — the on-demand path for bulk ops / `reconcile`
};

// The result of a file write through the kernel's daemon-initiated ("CLI-verb") path.
struct EditOutcome
{
    bool ok = false;
    derivation::WriteTicket ticket{}; // valid iff ok; carries BOTH R-FILE-001 hashes — raw_hash
                                      // (on-disk bytes; CAS `--if-match`) and canonical_hash (the
                                      // own-write barrier / derivation key)
    std::string error_code;           // catalog code when !ok (scope.denied / path.jail_violation / …)
    // Machine-readable encoding heals from canonicalizing this save (R-FILE-003 shape); populated
    // only for JSON content (e.g. encoding.bom / encoding.crlf — already fixed on disk).
    std::vector<serializer::Diagnostic> encoding_diagnostics;

    // The uniform R-CLI-008 result envelope a CLI/RPC caller returns. Success carries the derived
    // generation the write targets + the two-hash split (`rawHash` + `canonicalHash`, labelled) +
    // any encoding diagnostics; failure carries the catalog code so Envelope::exit_code() classes
    // it (e.g. scope.denied -> permission exit 6, R-SEC-007).
    [[nodiscard]] contract::Envelope envelope() const;
};

// One file inside a multi-file ("batch") write.
struct BatchEdit
{
    std::string path;
    std::string data;
};

// The result of a MULTI-file write through the kernel's intent-logged path (R-FILE-004).
struct EditBatchOutcome
{
    bool ok = false;
    std::string op_id;                            // the intent-log op id this batch ran under
    std::vector<derivation::WriteTicket> tickets; // one per edit, in order; valid iff ok
    std::string error_code;   // catalog code when !ok (scope.denied / path.jail_violation / …)
    std::string error_detail; // e.g. the offending path for a jail violation
};

// The uniform machine-readable JSON shape of ONE R-FILE-004/R-FILE-003 recovery diagnostic —
// {code, opId, message, remainingWrites} — shared by the boot-time `diagnostics` event topic and
// the `snapshot` verb's recoveryDiagnostics array (KernelServer), so the two surfaces never drift.
[[nodiscard]] contract::Json diagnostic_json(const filesync::Diagnostic& diagnostic);

class EditorKernel
{
public:
    // Non-owning w.r.t. the injected seams: the caller keeps `fs`, `watcher`, `clock`, `tasks` (and
    // the optional kernel `bus`) alive for the kernel's lifetime.
    EditorKernel(filesync::FileStore& fs, filesync::Watcher& watcher, kernel::Clock& clock,
                 kernel::TaskRunner& tasks, EditorKernelConfig config = {},
                 kernel::EventBus* bus = nullptr);

    EditorKernel(const EditorKernel&) = delete;
    EditorKernel& operator=(const EditorKernel&) = delete;

    // Wire the daemon's real method backing (see bridge::MethodBackend) BEFORE start(), so a
    // cross-process client's operational verbs (edit/query/…) are served over the composed kernel
    // instead of returning contract.unimplemented. Non-owning; the backend must outlive serving.
    void set_method_backend(const bridge::MethodBackend* backend) noexcept
    {
        daemon_.set_method_backend(backend);
    }

    // Boot the daemon: the atomic single-instance try-lock (R-BRIDGE-001), then the R-FILE-004
    // crash-recovery pass (resume-or-diagnose any intent-log entries a previous incarnation left
    // mid-flight — each entry HMAC-integrity-checked, re-jailed, and re-CAS'd before any byte is
    // re-applied), then a warm attach of the persisted reconcile index. `booted` = this instance owns
    // the Project; `attach` = an instance is already live (do NOT run a second); `error` = an
    // OS/filesystem error. `launch_scopes` is the operator's scope ceiling clamped onto every
    // attaching client (R-SEC-007, default: all).
    [[nodiscard]] bridge::StartOutcome
    start(bridge::ScopeSet launch_scopes = bridge::ScopeSet::all());
    void stop();
    [[nodiscard]] bool running() const noexcept { return daemon_.running(); }

    // The machine-readable diagnostics the boot-time recovery pass produced (R-FILE-004 /
    // R-FILE-003): empty == clean recovery (no pending op, or every pending op fully resumed). Each
    // is also published on the client `diagnostics` event topic at boot, naming the incomplete op.
    [[nodiscard]] const std::vector<filesync::Diagnostic>& recovery_diagnostics() const noexcept
    {
        return recovery_diagnostics_;
    }

    // Attach a client over the capability-negotiation handshake, clamped to the launch ceiling. The
    // result is a negotiated Session or the R-CLI-008 hard-fail envelope (handshake mismatch).
    [[nodiscard]] bridge::Dispatcher::AttachResult
    attach(const contract::ClientHandshake& client, bridge::ScopeSet requested) const;

    // Daemon-initiated ("CLI-verb") edit: scope-check (file_write, R-SEC-007) -> write THROUGH
    // filesync atomic-IO (R-FILE-004, path-jailed) -> ingest into the derivation graph -> return the
    // write ticket whose canonical_hash is the own-write read barrier key (R-CLI-006). A write with a
    // caller scope lacking file_write is refused with scope.denied; a jail escape with
    // path.jail_violation. The change does NOT reach the derived World until derivation runs a pass
    // (await_hash / settle).
    [[nodiscard]] EditOutcome edit_file(std::string_view path, std::string_view data,
                                        const bridge::ScopeSet& caller_scopes);

    // Daemon-initiated MULTI-file write (R-FILE-004): scope-check (file_write) -> jail-check every
    // path up front (a batch naming the same path twice is refused with usage.invalid — each entry's
    // planning-time CAS hash is measured against the PRE-batch file, so a duplicate would poison
    // crash recovery's re-CAS) -> serialize the whole batch through the crash-recovery intent log
    // (fsync the planned writes BEFORE the first byte, per-file-atomic apply in the given
    // dependency-safe order, clear AFTER the last durable rename) -> ingest each write into the
    // derivation graph.
    // A kill -9 mid-batch leaves the fsync'd intent entry on disk; the next incarnation's start()
    // resumes it to completion (or diagnoses, never clobbering a moved-on file). Valid only while
    // running() (the intent log comes up with the daemon).
    [[nodiscard]] EditBatchOutcome edit_files(const std::vector<BatchEdit>& edits,
                                              const bridge::ScopeSet& caller_scopes);

    // Fold EXTERNAL (out-of-band) edits into the derived index. ALWAYS drains watcher hints; runs
    // the full re-hash crawl per the R-FILE-002 cadence policy (config.min_crawl_interval_nanos —
    // see EditorKernelConfig) or unconditionally under CrawlMode::force (the on-demand bulk-op
    // path). Reads each changed path's current bytes and ingests them into the derivation graph.
    // This is how a raw edit that bypassed edit_file() reaches the World (content hash is
    // authoritative over watchers). Like edit_file, the World updates only when a pass runs.
    std::vector<filesync::ReconcileChange> ingest_external(CrawlMode mode = CrawlMode::policy);

    // Bounded-block until the derived world reflects `canonical_hash` (own-write barrier) or
    // `generation` (a foreign generation stamp), running derivation passes between checks up to
    // config.barrier_max_passes, then settle. Times out explicitly rather than hanging (R-CLI-006).
    derivation::BarrierResult await_hash(std::uint64_t canonical_hash);
    derivation::BarrierResult await_generation(std::uint64_t generation);

    // Drain the pending derivation queue fully, then publish the R-BRIDGE-008 quiescence fact
    // (`derivation.settled{generation}`, carrying the DERIVED-WORLD generation) on the client event
    // stream. Returns the settled derived-world generation. A no-op event when not running().
    std::uint64_t settle();

    // --- headless derived-World query (R-HEAD-001) ----------------------------------------------
    [[nodiscard]] const kernel::World& world() const noexcept { return graph_.world(); }
    [[nodiscard]] std::uint64_t generation() const noexcept { return graph_.generation(); }
    // The derived node for a source path (its canonical hash + the generation it last changed at), or
    // nullopt when no live node exists.
    [[nodiscard]] std::optional<derivation::DerivedSource> query(std::string_view path) const
    {
        return graph_.node(path);
    }
    // Read-your-writes query (R-CLI-006): bounded-block until THIS path's node carries
    // `canonical_hash`, then return it. nullopt whenever the bounded drain expires without the path
    // reflecting that hash (barrier timeout, absent node, or a superseded write) — never a stale
    // non-matching node with success semantics.
    [[nodiscard]] std::optional<derivation::DerivedSource>
    query_after_hash(std::string_view path, std::uint64_t canonical_hash);

    // --- accessors (valid views into the composed subsystems) -----------------------------------
    [[nodiscard]] bridge::EventStream& events() { return daemon_.events(); }
    [[nodiscard]] derivation::DerivationGraph& graph() noexcept { return graph_; }
    [[nodiscard]] filesync::Reconciler& reconciler() noexcept { return reconciler_; }
    [[nodiscard]] const bridge::Daemon& daemon() const noexcept { return daemon_; }
    [[nodiscard]] const EditorKernelConfig& config() const noexcept { return config_; }

    // D20: configure attach-token enforcement on the composed daemon's dispatcher (forwards to
    // bridge::Daemon::set_attach_auth). Mutator, so it is exposed here rather than via the const
    // daemon() view. Call after start() (the dispatcher exists) and BEFORE serve() spawns the
    // per-connection threads. Default OFF — see Daemon::set_attach_auth (the C-F1 sequencing).
    void set_attach_auth(std::string expected, bool required)
    {
        daemon_.set_attach_auth(std::move(expected), required);
    }

private:
    void ingest_change(const filesync::ReconcileChange& change);
    [[nodiscard]] std::string editor_dir() const;

    filesync::FileStore& fs_;
    kernel::Clock& clock_;
    EditorKernelConfig config_;
    bridge::Daemon daemon_;
    filesync::Reconciler reconciler_;
    derivation::DerivationGraph graph_;

    // The R-FILE-004 intent-logged multi-file write path. Brought up in start() AFTER the
    // single-instance lock is won (the lock stays the atomic FIRST action of a write-capable
    // instantiation, R-ARCH-005 — the HMAC key file must not be touched before owning the Project).
    std::optional<filesync::IntentLog> intent_log_;
    std::optional<filesync::WriteQueue> write_queue_;
    std::vector<filesync::Diagnostic> recovery_diagnostics_;
    std::uint64_t next_batch_op_ = 0;

    // R-FILE-002 crawl-cadence state: when the last full crawl ran (injected-Clock time). The first
    // ingest_external() always crawls (crawled_once_ starts false).
    bool crawled_once_ = false;
    std::uint64_t last_crawl_nanos_ = 0;
};

} // namespace context::editor::editorkernel
