// EditorKernel composition implementation (see editor_kernel.h).

#include "context/editor/editorkernel/editor_kernel.h"

#include "context/editor/bridge/scope.h"
#include "context/editor/contract/json.h"
#include "context/editor/filesync/content_hash.h"
#include "context/editor/filesync/file_store.h"
#include "context/editor/filesync/path_jail.h"
#include "context/kernel/platform.h" // Clock is only forward-declared in the header

#include <string>
#include <unordered_set>
#include <utility>

namespace context::editor::editorkernel
{

using contract::Envelope;
using contract::Json;

Json diagnostic_json(const filesync::Diagnostic& diagnostic)
{
    Json entry = Json::object();
    entry.set("code", Json(diagnostic.code));
    entry.set("opId", Json(diagnostic.op_id));
    entry.set("message", Json(diagnostic.message));
    Json remaining = Json::array();
    for (const std::string& p : diagnostic.remaining_writes)
        remaining.push_back(Json(p));
    entry.set("remainingWrites", std::move(remaining));
    return entry;
}

Envelope EditOutcome::envelope() const
{
    if (ok)
    {
        Json data = Json::object();
        data.set("path", Json(ticket.path));
        // The R-FILE-001 two-hash split, both labelled, both as decimal STRINGS (Json's number
        // type is double-backed, so a full-range 64-bit hash — routinely > 2^53 — would lose
        // precision, and these are exactly the keys a caller feeds back):
        //   rawHash       — raw-byte identity of the bytes ON DISK after this tool save: the
        //                   watch/reconcile change-detector's identity and the CAS `--if-match` key.
        //   canonicalHash — canonical-content identity: derivation/cache keys and the own-write
        //                   replay key fed back as `--after-hash` (R-CLI-006).
        // For non-JSON content (binary sidecars) the two are EQUAL by construction.
        data.set("rawHash", Json(std::to_string(ticket.raw_hash)));
        data.set("canonicalHash", Json(std::to_string(ticket.canonical_hash)));
        data.set("removal", Json(ticket.removal));
        // Machine-readable encoding heals from canonicalizing this save (R-FILE-003: e.g.
        // encoding.bom / encoding.crlf — already fixed in the bytes written to disk).
        if (!encoding_diagnostics.empty())
        {
            Json diags = Json::array();
            for (const serializer::Diagnostic& d : encoding_diagnostics)
            {
                Json entry = Json::object();
                entry.set("code", Json(d.code));
                entry.set("message", Json(d.message));
                entry.set("line", Json(static_cast<std::uint64_t>(d.line)));
                entry.set("column", Json(static_cast<std::uint64_t>(d.column)));
                diags.push_back(std::move(entry));
            }
            data.set("diagnostics", std::move(diags));
        }
        // generationAfter is the derived-world generation the write will be incorporated into
        // (R-CLI-006 own-write barrier target).
        return Envelope::success(std::move(data), ticket.generation_after);
    }
    // Failure: the catalog fills message/retriable/exit-code from `error_code` (e.g. scope.denied ->
    // permission exit 6, R-SEC-007).
    return Envelope::failure(error_code);
}

namespace
{
// The R-FILE-002 watcher.degraded diagnostic must NAME the effective crawl fallback cadence, so the
// reconciler's note reflects this composition's actual policy (see EditorKernelConfig).
filesync::ReconcilerConfig reconciler_config_for(const EditorKernelConfig& cfg)
{
    filesync::ReconcilerConfig rc;
    if (cfg.min_crawl_interval_nanos > 0)
    {
        rc.crawl_fallback_note =
            "the full re-hash crawl (low-frequency safety net: at most once per " +
            std::to_string(cfg.min_crawl_interval_nanos / 1000000ULL) +
            " ms; on-demand via the reconcile verb)";
    }
    return rc; // interval 0: the default note ("runs with every reconcile pass") stays accurate
}
} // namespace

EditorKernel::EditorKernel(filesync::FileStore& fs, filesync::Watcher& watcher,
                           kernel::Clock& clock, kernel::TaskRunner& tasks,
                           EditorKernelConfig config, kernel::EventBus* bus)
    : fs_(fs), clock_(clock), config_(std::move(config)), daemon_(config_.project_root),
      reconciler_(fs, watcher, clock, tasks, config_.filesync_root, config_.index_path, bus,
                  reconciler_config_for(config_)),
      graph_(config_.derivation, bus)
{
}

std::string EditorKernel::editor_dir() const
{
    // The `.editor/` control dir under the FileStore seam — the same convention the reconcile index
    // follows (config default ".editor/index" for root ".").
    return config_.filesync_root == "." ? std::string(".editor")
                                        : config_.filesync_root + "/.editor";
}

bridge::StartOutcome EditorKernel::start(bridge::ScopeSet launch_scopes)
{
    const bridge::StartOutcome outcome = daemon_.start(/*write_capable=*/true, launch_scopes);
    if (outcome != bridge::StartOutcome::booted)
        return outcome;

    // R-FILE-004 crash recovery — runs AFTER the lock is won (owning the Project) and BEFORE any
    // client is served: resume-or-diagnose every intent-log entry a previous incarnation left
    // mid-flight. Each entry is HMAC-integrity-checked and each planned write re-jailed + re-CAS'd
    // before a byte is re-applied; an op that cannot be fully + safely resumed is left on disk and
    // surfaced as a machine-readable diagnostic naming it (R-FILE-003), never a forced state.
    const std::string dir = editor_dir();
    intent_log_.emplace(fs_, dir, filesync::ensure_hmac_key(fs_, dir));
    write_queue_.emplace(fs_, config_.filesync_root, *intent_log_, clock_);
    recovery_diagnostics_ = write_queue_->recover();
    for (const filesync::Diagnostic& d : recovery_diagnostics_)
    {
        Json payload = diagnostic_json(d);
        payload.set("event", Json(std::string("recovery.diagnostic")));
        daemon_.events().publish("diagnostics", std::move(payload));
    }

    reconciler_.attach(); // warm-attach the persisted index (no cold re-hash when it is valid)
    return outcome;
}

void EditorKernel::stop()
{
    daemon_.stop();
}

bridge::Dispatcher::AttachResult EditorKernel::attach(const contract::ClientHandshake& client,
                                                      bridge::ScopeSet requested) const
{
    return daemon_.attach_client(client, requested);
}

EditOutcome EditorKernel::edit_file(std::string_view path, std::string_view data,
                                    const bridge::ScopeSet& caller_scopes)
{
    EditOutcome out;

    // R-SEC-007: authored-file writes require the file_write scope. Enforced HERE (not only at an
    // adapter, which is bypassable via direct RPC). A read/query token is refused with the permission-
    // class catalog code so the caller's exit code classes as 6.
    if (!caller_scopes.has(bridge::Scope::file_write))
    {
        out.error_code = bridge::kScopeDeniedCode;
        return out;
    }

    const std::string key = filesync::normalize_path(path);

    // Structural path jail (R-SEC-008): a write may never escape the project root.
    if (!filesync::is_inside_jail(config_.filesync_root, key))
    {
        out.error_code = "path.jail_violation";
        return out;
    }

    // Tool saves canonicalize the WHOLE file they write (R-FILE-001): JSON content lands on disk
    // in THE canonical form — which is also how external non-canonical formatting normalizes on
    // the first tool save. Non-JSON content (binary sidecars, the L-32 carve-outs) writes
    // verbatim (no canonicalization pass). Encoding heals surface on the envelope (R-FILE-003).
    derivation::CanonicalForm form = derivation::canonical_parse(data);

    // Write THROUGH filesync atomic-IO (temp+fsync+rename, R-FILE-004). apply_write registers the
    // write as self-echo, so the reconcile crawl will NOT re-surface our own write as external.
    if (!reconciler_.apply_write(key, form.bytes))
    {
        out.error_code = "internal.error";
        return out;
    }

    // Ingest our own write directly into the derivation graph (it will not come back through the
    // reconciler). The ticket carries BOTH hashes of the R-FILE-001 split: the raw-byte hash of
    // the bytes just written, and the canonical hash the own-write read barrier keys on
    // (R-CLI-006). The graph's parse node re-canonicalizes the already-canonical bytes — a
    // deliberate fixpoint no-op that keeps the seam single-sourced.
    const filesync::ReconcileChange change{key, filesync::ChangeType::modified,
                                           filesync::content_hash(form.bytes)};
    out.ticket = graph_.apply(change, form.bytes);
    // Only JSON content carries envelope findings (the encoding heals). A non-JSON payload's
    // parse-failure diagnostic is NOT a finding — sidecars/TS text are legal non-JSON kinds, and
    // whether a path SHOULD be JSON is the schema model's knowledge (a later M2 task).
    if (form.is_json)
        out.encoding_diagnostics = std::move(form.diagnostics);
    out.ok = true;
    return out;
}

EditBatchOutcome EditorKernel::edit_files(const std::vector<BatchEdit>& edits,
                                          const bridge::ScopeSet& caller_scopes)
{
    EditBatchOutcome out;

    // R-SEC-007: a multi-file write is a write — same file_write gate as edit_file, enforced here
    // in addition to the dispatcher so no composing layer can bypass it.
    if (!caller_scopes.has(bridge::Scope::file_write))
    {
        out.error_code = bridge::kScopeDeniedCode;
        return out;
    }
    if (!running() || !write_queue_.has_value())
    {
        out.error_code = "internal.error";
        out.error_detail = "the intent-logged write path is only available on a booted daemon";
        return out;
    }

    // Plan the whole batch up front: jail-check every path (R-SEC-008) and record the planning-time
    // CAS hash of each target (R-FILE-004 — a resume never clobbers a file that moved on).
    std::vector<filesync::PlannedWrite> writes;
    writes.reserve(edits.size());
    std::unordered_set<std::string> planned_paths;
    planned_paths.reserve(edits.size());
    for (const BatchEdit& e : edits)
    {
        const std::string key = filesync::normalize_path(e.path);
        if (!filesync::is_inside_jail(config_.filesync_root, key))
        {
            out.error_code = "path.jail_violation";
            out.error_detail = e.path;
            return out;
        }
        // One entry per path: every planning-time CAS hash is computed against the PRE-batch file,
        // so a path applied twice would leave the second entry's recovery re-CAS staring at the
        // FIRST apply's bytes after a mid-batch crash — misreported as a moved-on file instead of
        // resumed to the batch's intended final content (R-FILE-004). Refuse up front.
        if (!planned_paths.insert(key).second)
        {
            out.error_code = "usage.invalid";
            out.error_detail = "duplicate path in batch: " + e.path;
            return out;
        }
        // Tool saves canonicalize the whole file they write (R-FILE-001) — the batch path plans,
        // intent-logs, CAS-es, and writes the CANONICAL bytes, so a crash-recovery resume re-lands
        // the identical canonical content. Non-JSON payloads pass through verbatim.
        derivation::CanonicalForm form = derivation::canonical_parse(e.data);
        const std::optional<std::string> current = fs_.read(key);
        filesync::PlannedWrite w;
        w.path = key;
        w.expected_prev_hash = filesync::content_hash(current ? *current : std::string());
        w.target_hash = filesync::content_hash(form.bytes);
        w.data = std::move(form.bytes);
        writes.push_back(std::move(w));
    }

    // One op id per batch, unique across incarnations (the crashed incarnation's pending entry must
    // never be overwritten by the next incarnation's first batch).
    out.op_id = "batch-" + daemon_.events().incarnation_id() + "-" + std::to_string(++next_batch_op_);

    // fsync the intent entry BEFORE the first write; apply per-file-atomically in order; clear AFTER
    // the last durable rename (R-FILE-004). A crash anywhere in between leaves the entry for the
    // next incarnation's start() recovery pass.
    if (!write_queue_->execute(out.op_id, writes))
    {
        out.error_code = "internal.error";
        out.error_detail = "the intent-logged batch did not complete durably (op " + out.op_id + ")";
        return out;
    }

    // Ingest each write into the derivation graph (the WriteQueue path does not register reconciler
    // self-echo; a later crawl re-hash of these paths memoizes to a no-op, so no double-derive).
    out.tickets.reserve(writes.size());
    for (const filesync::PlannedWrite& w : writes)
    {
        const filesync::ReconcileChange change{w.path, filesync::ChangeType::modified,
                                               w.target_hash};
        out.tickets.push_back(graph_.apply(change, w.data));
    }
    out.ok = true;
    return out;
}

void EditorKernel::ingest_change(const filesync::ReconcileChange& change)
{
    if (change.type == filesync::ChangeType::removed)
    {
        // A removal carries no content: content_hash is 0 by the ReconcileChange contract and the
        // derivation graph ignores it for removals, so forward the change as-is.
        graph_.apply(change, std::string_view{});
        return;
    }
    // Content hash is authoritative (R-FILE-002): read the path's CURRENT bytes and derive from them.
    const std::optional<std::string> bytes = fs_.read(change.path);
    graph_.apply(change, bytes ? std::string_view(*bytes) : std::string_view{});
}

std::vector<filesync::ReconcileChange> EditorKernel::ingest_external(CrawlMode mode)
{
    std::vector<filesync::ReconcileChange> changes = reconciler_.reconcile_hints();

    // The full re-hash crawl is the dropped-event SAFETY NET (R-FILE-002), not the ingest cadence:
    // it converges even when every watcher hint was lost (the portable NullWatcher is always
    // degraded). With real OS watchers live, hints carry routine detection and the crawl runs
    // LOW-FREQUENCY by policy (config.min_crawl_interval_nanos); CrawlMode::force is the on-demand
    // bulk-op path (the `reconcile` verb, e.g. after a git branch switch) and ignores the cadence.
    // Already-current paths yield no change, so a path caught by both a hint and the crawl is not
    // double-reported.
    const std::uint64_t now = clock_.now_nanos();
    const bool due = !crawled_once_ || config_.min_crawl_interval_nanos == 0 ||
                     now - last_crawl_nanos_ >= config_.min_crawl_interval_nanos;
    if (mode == CrawlMode::force || due)
    {
        std::vector<filesync::ReconcileChange> crawled = reconciler_.crawl(/*gated=*/false);
        changes.insert(changes.end(), crawled.begin(), crawled.end());
        crawled_once_ = true;
        last_crawl_nanos_ = now;
    }

    for (const filesync::ReconcileChange& change : changes)
        ingest_change(change);
    return changes;
}

derivation::BarrierResult EditorKernel::await_hash(std::uint64_t canonical_hash)
{
    const derivation::BarrierResult result =
        derivation::wait_for_hash(graph_, canonical_hash, config_.barrier_max_passes);
    settle();
    return result;
}

derivation::BarrierResult EditorKernel::await_generation(std::uint64_t generation)
{
    const derivation::BarrierResult result =
        derivation::wait_for_generation(graph_, generation, config_.barrier_max_passes);
    settle();
    return result;
}

std::uint64_t EditorKernel::settle()
{
    // Drain the whole pending dirty set (load-shed can defer nodes across passes) so the derived
    // world is fully quiescent before the settle fact is published.
    while (graph_.pending_count() > 0)
        graph_.run_pass();

    const std::uint64_t generation = graph_.generation();

    // Forward the derived-world generation onto the client-facing event stream as the R-BRIDGE-008
    // quiescence fact. (The bridge's own EventStream generation counter is a distinct concept driven
    // by pure-bridge tests; the composition point is carrying the DERIVED-WORLD generation here.)
    if (running())
    {
        Json settled = Json::object();
        settled.set("event", Json(std::string("derivation.settled")));
        settled.set("generation", Json(generation));
        daemon_.events().publish("derivation", std::move(settled));
    }
    return generation;
}

std::optional<derivation::DerivedSource>
EditorKernel::query_after_hash(std::string_view path, std::uint64_t canonical_hash)
{
    // Prioritize the queried path in the derivation load-shed: under sustained write load
    // (R-FILE-013) a non-visible node can be repeatedly deferred while the barrier burns through its
    // pass budget, stalling the read. DerivationGraph derives visible/queried nodes first — never
    // stall a query — so mark this path visible for the duration of the read. Scoped (unmarked below)
    // so a one-off read does not permanently exempt the path from load-shedding.
    graph_.set_visible(path, true);
    // The world-global hash barrier is the fast path: it pumps bounded passes until the write is
    // incorporated SOMEWHERE (R-CLI-006). Its verdict is deliberately discarded — resolving on
    // ANOTHER node's identical content says nothing about THIS path (see below); the per-path
    // check after the drain is authoritative either way.
    (void)derivation::wait_for_hash(graph_, canonical_hash, config_.barrier_max_passes);

    // The hash barrier is world-global (reflects_hash answers "does ANY alive node carry this
    // canonical hash"), so when ANOTHER file already holds the identical canonical content the
    // barrier resolves instantly — possibly BEFORE this path's own pending ingest has derived
    // (surfaced by the M1 exit gate: two clients writing the same bytes to two paths). Read-your-
    // writes is per-PATH: drain remaining passes (same bounded budget) until THIS path's node
    // reflects the hash it just wrote, so an own-write read never observes the pre-write node.
    for (std::uint64_t budget = config_.barrier_max_passes; budget > 0; --budget)
    {
        const std::optional<derivation::DerivedSource> node = graph_.node(path);
        if ((node.has_value() && node->canonical_hash == canonical_hash) ||
            graph_.pending_count() == 0)
            break;
        graph_.run_pass();
    }
    graph_.set_visible(path, false);

    // Read-your-writes is strict: only a node CARRYING the hash is a success. A non-matching (or
    // absent) node after the bounded drain means THIS path never derived the write within the
    // budget (or the write was superseded) — returning it would hand the caller a possibly
    // PRE-write state with success semantics, the exact staleness class this query exists to
    // prevent. Callers distinguish "not reflected" from a node read via plain query().
    const std::optional<derivation::DerivedSource> node = graph_.node(path);
    if (node.has_value() && node->canonical_hash == canonical_hash)
        return node;
    return std::nullopt;
}

} // namespace context::editor::editorkernel
