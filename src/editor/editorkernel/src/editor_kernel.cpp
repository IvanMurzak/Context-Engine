// EditorKernel composition implementation (see editor_kernel.h).

#include "context/editor/editorkernel/editor_kernel.h"

#include "context/editor/bridge/scope.h"
#include "context/editor/contract/json.h"
#include "context/editor/filesync/content_hash.h"
#include "context/editor/filesync/file_store.h"
#include "context/editor/filesync/path_jail.h"

#include <string>
#include <utility>

namespace context::editor::editorkernel
{

using contract::Envelope;
using contract::Json;

Envelope EditOutcome::envelope() const
{
    if (ok)
    {
        Json data = Json::object();
        data.set("path", Json(ticket.path));
        // Serialize the 64-bit content hash as a decimal STRING: Json's number type is double-backed,
        // so a full-range canonical hash (routinely > 2^53) would lose precision — and this is exactly
        // the own-write replay key a caller feeds back as `--after-hash` (R-CLI-006), which must
        // round-trip losslessly.
        data.set("canonicalHash", Json(std::to_string(ticket.canonical_hash)));
        data.set("removal", Json(ticket.removal));
        // generationAfter is the derived-world generation the write will be incorporated into
        // (R-CLI-006 own-write barrier target).
        return Envelope::success(std::move(data), ticket.generation_after);
    }
    // Failure: the catalog fills message/retriable/exit-code from `error_code` (e.g. scope.denied ->
    // permission exit 6, R-SEC-007).
    return Envelope::failure(error_code);
}

EditorKernel::EditorKernel(filesync::FileStore& fs, filesync::Watcher& watcher,
                           kernel::Clock& clock, kernel::TaskRunner& tasks,
                           EditorKernelConfig config, kernel::EventBus* bus)
    : fs_(fs), config_(std::move(config)), daemon_(config_.project_root),
      reconciler_(fs, watcher, clock, tasks, config_.filesync_root, config_.index_path, bus),
      graph_(config_.derivation, bus)
{
}

bridge::StartOutcome EditorKernel::start(bridge::ScopeSet launch_scopes)
{
    const bridge::StartOutcome outcome = daemon_.start(/*write_capable=*/true, launch_scopes);
    if (outcome == bridge::StartOutcome::booted)
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

    // Write THROUGH filesync atomic-IO (temp+fsync+rename, R-FILE-004). apply_write registers the
    // write as self-echo, so the reconcile crawl will NOT re-surface our own write as external.
    if (!reconciler_.apply_write(key, data))
    {
        out.error_code = "internal.error";
        return out;
    }

    // Ingest our own write directly into the derivation graph (it will not come back through the
    // reconciler). The ticket carries the canonical hash the own-write read barrier keys on (R-CLI-006).
    const filesync::ReconcileChange change{key, filesync::ChangeType::modified,
                                           filesync::content_hash(data)};
    out.ticket = graph_.apply(change, data);
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

std::vector<filesync::ReconcileChange> EditorKernel::ingest_external()
{
    std::vector<filesync::ReconcileChange> changes = reconciler_.reconcile_hints();
    // The full re-hash crawl is the dropped-event safety net: it converges even when every watcher
    // hint was lost (the portable NullWatcher is always degraded). Already-current paths yield no
    // change, so a path caught by both a hint and the crawl is not double-reported.
    std::vector<filesync::ReconcileChange> crawled = reconciler_.crawl(/*gated=*/false);
    changes.insert(changes.end(), crawled.begin(), crawled.end());

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
    const derivation::BarrierResult result =
        derivation::wait_for_hash(graph_, canonical_hash, config_.barrier_max_passes);
    graph_.set_visible(path, false);
    if (!result.ok())
        return std::nullopt;
    return graph_.node(path);
}

} // namespace context::editor::editorkernel
