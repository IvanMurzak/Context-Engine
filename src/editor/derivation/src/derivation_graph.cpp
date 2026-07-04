// The M1 incremental derivation graph — see derivation_graph.h.

#include "context/editor/derivation/derivation_graph.h"

#include "context/editor/migrate/migrate_document.h"
#include "context/kernel/event_bus.h"

#include <iterator>
#include <utility>
#include <vector>

namespace context::editor::derivation
{

using context::editor::filesync::ChangeType;
using context::editor::filesync::ReconcileChange;

namespace
{
// Fold parse-time migration findings into the node's R-FILE-003 validation report. Positions are
// located in the SOURCE bytes where the pointer still resolves (header stamps, override entries);
// a pointer only meaningful in the migrated in-memory view locates at 0/0 — migration findings
// describe the migrated view, an accepted precision trade the validate node's same-bytes contract
// does not cover.
void fold_migration_findings(const std::vector<migrate::MigrationDiagnostic>& findings,
                             std::string_view source_bytes, schema::ValidationReport& report)
{
    for (const migrate::MigrationDiagnostic& d : findings)
    {
        schema::ValidationDiagnostic out;
        out.code = d.code;
        out.message = d.message;
        out.pointer = d.pointer;
        (void)schema::locate_pointer(source_bytes, d.pointer, out.line, out.column);
        report.diagnostics.push_back(std::move(out));
        if (d.blocking)
            report.ok = false;
    }
}
} // namespace

DerivationGraph::DerivationGraph(DerivationConfig config, context::kernel::EventBus* bus,
                                 const schema::SchemaSet* schemas,
                                 const schema::RefTargetResolver* ref_resolver,
                                 const migrate::MigrationSet* migrations)
    : config_(config), bus_(bus), schemas_(schemas), ref_resolver_(ref_resolver),
      migrations_(migrations), set_hash_(config.registered_set_hash)
{
    signal_.high_watermark = config_.high_watermark;
}

WriteTicket DerivationGraph::apply(const ReconcileChange& change, std::string_view source_bytes)
{
    WriteTicket ticket;
    ticket.path = change.path;
    ticket.generation_after = generation_ + 1;

    Pending pending;
    if (change.type == ChangeType::removed)
    {
        pending.removal = true;
        ticket.removal = true;
    }
    else
    {
        pending.form = canonical_parse(source_bytes);
        ++parse_invocations_;
        ticket.canonical_hash = pending.form.canonical_hash;
        // The raw-byte half of the R-FILE-001 two-hash split, carried from the filesync change
        // (the reconcile pipeline's own content hash of the bytes on disk).
        ticket.raw_hash = change.content_hash;
        // The L-37 parse-time migration node (M2 wave 3): migrate stamped-older component payloads
        // IN MEMORY on the SAME parse — disk truth never mutates; the ticket's canonical hash stays
        // the AUTHORED identity (own-write barrier + memo key on what is actually on disk). The
        // validate node then sees CURRENT-version payloads. A BLOCKING migration finding
        // (newer-than / gap / failed / over-budget / id mutation) becomes a failing validation
        // report — skipping schema validation of the old-shape payload — so derive_one retains the
        // node's last-good derived state (R-FILE-003), exactly the L-37 downgrade semantics.
        bool migration_blocked = false;
        if (migrations_ != nullptr && pending.form.is_json)
        {
            const migrate::DocumentMigrationResult migrated =
                migrate::migrate_document(pending.form.root, *migrations_);
            if (!migrated.ok)
            {
                migration_blocked = true;
                pending.report = schema::ValidationReport{};
                fold_migration_findings(migrated.diagnostics, source_bytes, pending.report);
                pending.report.ok = false;
                pending.validated = true;
            }
            else if (!migrated.diagnostics.empty() && schemas_ != nullptr)
            {
                // Non-blocking findings (orphan overrides) ride the validation report below.
                // Deliberate asymmetry with the blocking branch above: with NO schema set wired
                // there is no validate node to carry advisories, so they are dropped — only
                // BLOCKING findings force a report (last-good retention needs the verdict).
                pending.report = schema::ValidationReport{};
                fold_migration_findings(migrated.diagnostics, source_bytes, pending.report);
            }
        }
        // The M2 validate node (R-DATA-006): schema-validate JSON payloads on the SAME parse the
        // canonical node produced (post-migration, so kind schemas judge current-version
        // payloads). Positions are located in the SOURCE bytes, so diagnostics point at what the
        // author actually wrote (R-FILE-003 pointer + line/column).
        if (!migration_blocked && schemas_ != nullptr && pending.form.is_json)
        {
            schema::ValidationReport validation = schema::validate_document(
                pending.form.root, source_bytes, *schemas_, ref_resolver_);
            // Prepend the migration findings gathered above (if any) onto the validate verdict.
            validation.diagnostics.insert(
                validation.diagnostics.begin(),
                std::make_move_iterator(pending.report.diagnostics.begin()),
                std::make_move_iterator(pending.report.diagnostics.end()));
            pending.report = std::move(validation);
            pending.validated = true;
        }
    }

    // Coalesce: the latest write to a path before the next pass wins (one batched pass per burst).
    pending_[change.path] = std::move(pending);
    refresh_signal();
    return ticket;
}

void DerivationGraph::set_visible(std::string_view path, bool visible)
{
    nodes_[std::string(path)].visible = visible;
}

DerivePassResult DerivationGraph::run_pass()
{
    DerivePassResult result;
    result.generation = generation_;
    if (pending_.empty())
    {
        refresh_signal();
        return result;
    }

    const std::uint64_t target_gen = generation_ + 1;
    const bool overloaded = pending_.size() > config_.high_watermark;

    // Select the batch for this pass. Not overloaded: drain everything. Overloaded: load-shed —
    // visible/queried nodes first (never stall a query), then a bounded fill of the rest; the
    // remainder defers to later passes (R-FILE-013).
    std::vector<std::string> batch;
    if (!overloaded)
    {
        batch.reserve(pending_.size());
        for (const auto& [path, _] : pending_)
            batch.push_back(path);
    }
    else
    {
        for (const auto& [path, _] : pending_)
        {
            auto it = nodes_.find(path);
            if (it != nodes_.end() && it->second.visible)
                batch.push_back(path);
        }
        for (const auto& [path, _] : pending_)
        {
            if (batch.size() >= config_.max_batch_per_pass)
                break;
            auto it = nodes_.find(path);
            if (it == nodes_.end() || !it->second.visible)
                batch.push_back(path);
        }
    }

    std::set<std::string> compose_seeds;
    for (const std::string& path : batch)
    {
        derive_one(path, pending_[path], target_gen, result, compose_seeds);
        pending_.erase(path);
    }

    // The compose node (M2 wave 3, L-35): re-flatten every scene the batch touched plus its
    // transitive dependents (template-instance fan-out; synchronous within the pass — the graph's
    // deterministic single-threaded style, like every other node).
    recompose(compose_seeds, target_gen, result);

    generation_ = target_gen;
    result.generation = target_gen;
    result.deferred = pending_.size();
    refresh_signal();
    publish_pass_event(result);
    return result;
}

void DerivationGraph::derive_one(const std::string& path, const Pending& pending,
                                 std::uint64_t target_gen, DerivePassResult& result,
                                 std::set<std::string>& compose_seeds)
{
    Node& node = nodes_[path];

    if (pending.removal)
    {
        if (node.alive)
        {
            unreflect(node.canonical_hash);
            world_.destroy(node.entity);
            node.alive = false;
            node.generation = target_gen;
            ++derivations_;
            ++result.nodes_removed;
        }
        // Removing a source clears its validation state with it (idempotent for unknown sources).
        node.report = schema::ValidationReport{};
        node.has_report = false;
        // A removed scene leaves composition: dependents re-flatten against the gone template
        // (compose.missing_scene).
        if (scene_docs_.erase(path) > 0)
        {
            unlink_instance_deps(path);
            instance_deps_.erase(path);
            compose_seeds.insert(path);
        }
        return;
    }

    // The validate node's verdict (R-FILE-003): record the report FIRST — diagnostics are derived
    // data an agent reads — and on a FAILED validation retain the node's last-good derived state
    // (an alive node keeps its value + generation; a new source does not derive until it is
    // valid). The self-correction loop: a later valid ingest derives normally below.
    if (pending.validated)
    {
        node.report = pending.report;
        node.report_generation = target_gen;
        node.has_report = true;
        if (!pending.report.ok)
        {
            ++result.nodes_invalid;
            return;
        }
    }
    else if (schemas_ != nullptr)
    {
        // Schemas are wired but THIS pass's bytes are not JSON (a deliberate non-JSON kind, or a
        // broken/truncated write) — no validation ran, so a PRIOR schema verdict no longer
        // describes the current content. Clear it so validation() returns nullopt (the header's
        // non-JSON-content contract) instead of a stale report, mirroring the removal branch.
        node.report = schema::ValidationReport{};
        node.has_report = false;
    }

    // Content-hash memoization: an unchanged canonical form means the downstream derivation is skipped
    // — this is what makes incremental re-derive recompute ONLY genuinely affected nodes (L-22).
    // The registered-set hash is the second key component (R-FILE-005): a node derived under a
    // DIFFERENT schema + migration set is stale even for identical content — a pass-0 change
    // (package upgrade) re-keys the dependent pass-1 subgraph instead of serving old derivations.
    if (node.alive && node.canonical_hash == pending.form.canonical_hash &&
        node.set_hash == set_hash_)
    {
        ++result.nodes_skipped;
        return;
    }

    if (!node.alive)
    {
        node.entity = world_.create();
        node.alive = true;
    }
    else
    {
        unreflect(node.canonical_hash);
    }

    node.canonical_hash = pending.form.canonical_hash;
    node.set_hash = set_hash_;
    node.generation = target_gen;
    world_.add(node.entity, DerivedSource{pending.form.canonical_hash, target_gen});
    reflect(node.canonical_hash);
    ++derivations_;
    ++result.nodes_derived;

    // The compose node's ingest half (L-35): a VALID scene document refreshes its retained
    // composition view + instance dependency edges and seeds re-flattening; a document that
    // STOPPED being a scene leaves composition. Only reached for a validation-passing (or
    // unvalidated) derive — a failing payload returned above, retaining the last-good SceneDoc
    // exactly like the derived node (R-FILE-003).
    if (pending.form.is_json)
    {
        if (std::optional<compose::SceneDoc> doc =
                compose::build_scene_doc(path, pending.form.root);
            doc.has_value())
        {
            std::vector<std::string> deps;
            deps.reserve(doc->instances.size());
            for (const compose::SceneInstance& inst : doc->instances)
                deps.push_back(inst.scene);
            scene_docs_[path] = std::move(*doc);
            unlink_instance_deps(path);
            for (const std::string& dep : deps)
                instance_dependents_[dep].insert(path);
            instance_deps_[path] = std::move(deps);
            compose_seeds.insert(path);
            return;
        }
    }
    if (scene_docs_.erase(path) > 0)
    {
        unlink_instance_deps(path);
        instance_deps_.erase(path);
        compose_seeds.insert(path);
    }
}

void DerivationGraph::unlink_instance_deps(const std::string& path)
{
    // Drop `path`'s reverse edges (template -> dependent) so instance_dependents_ stays the exact
    // inverse of instance_deps_ before the caller erases or replaces the forward entry.
    auto it = instance_deps_.find(path);
    if (it == instance_deps_.end())
        return;
    for (const std::string& dep : it->second)
    {
        auto rev = instance_dependents_.find(dep);
        if (rev == instance_dependents_.end())
            continue;
        rev->second.erase(path);
        if (rev->second.empty())
            instance_dependents_.erase(rev);
    }
}

void DerivationGraph::recompose(const std::set<std::string>& seeds, std::uint64_t target_gen,
                                DerivePassResult& result)
{
    if (seeds.empty())
        return;

    // Close the seed set over reverse instance edges: any scene whose transitive template set
    // intersects the seeds re-flattens (the L-35 fan-out; editing a template updates every
    // non-overriding instance). BFS over the instance_dependents_ reverse index, so the cost
    // tracks the affected fan-out — not the total number of scenes ever derived (L-22 discipline,
    // R-FILE-011 scale envelope). Cycles terminate: a scene enqueues at most once.
    std::set<std::string> dirty = seeds;
    std::vector<std::string> queue(seeds.begin(), seeds.end());
    while (!queue.empty())
    {
        const std::string scene = std::move(queue.back());
        queue.pop_back();
        auto rev = instance_dependents_.find(scene);
        if (rev == instance_dependents_.end())
            continue;
        for (const std::string& dependent : rev->second)
            if (dirty.insert(dependent).second)
                queue.push_back(dependent);
    }

    // The resolver over the graph's retained last-good scene views.
    class GraphResolver final : public compose::SceneResolver
    {
    public:
        explicit GraphResolver(const std::map<std::string, compose::SceneDoc>& docs) : docs_(docs)
        {
        }
        [[nodiscard]] const compose::SceneDoc* resolve(std::string_view path) const override
        {
            auto it = docs_.find(std::string(path));
            return it == docs_.end() ? nullptr : &it->second;
        }

    private:
        const std::map<std::string, compose::SceneDoc>& docs_;
    };
    const GraphResolver resolver(scene_docs_);

    for (const std::string& path : dirty)
    {
        auto doc = scene_docs_.find(path);
        if (doc == scene_docs_.end())
        {
            composed_.erase(path); // the scene left composition (removed / no longer a scene)
            continue;
        }
        ComposedRecord record;
        record.scene = compose::flatten(path, resolver, config_.compose_limits);
        record.generation = target_gen;
        composed_[path] = std::move(record);
        ++result.scenes_composed;
    }
}

bool DerivationGraph::scene_closure_pending(const std::string& path) const
{
    if (pending_.empty())
        return false;
    // Walk the scene's transitive template closure (itself included); any pending ingest inside
    // it can still change the composed output. A pending path UNKNOWN to the closure cannot —
    // except that a brand-new file could satisfy a currently-missing instance path, which is
    // exactly a pending path whose NAME is in the closure, covered below.
    std::set<std::string> closure;
    std::vector<std::string> walk{path};
    while (!walk.empty())
    {
        std::string current = std::move(walk.back());
        walk.pop_back();
        if (!closure.insert(current).second)
            continue;
        if (auto deps = instance_deps_.find(current); deps != instance_deps_.end())
            for (const std::string& dep : deps->second)
                walk.push_back(dep);
    }
    for (const std::string& member : closure)
        if (pending_.count(member) != 0)
            return true;
    return false;
}

bool DerivationGraph::reflects_hash(std::uint64_t canonical_hash) const
{
    auto it = reflected_hashes_.find(canonical_hash);
    return it != reflected_hashes_.end() && it->second > 0;
}

std::optional<DerivedSource> DerivationGraph::node(std::string_view path) const
{
    auto it = nodes_.find(std::string(path));
    if (it == nodes_.end() || !it->second.alive)
        return std::nullopt;
    return DerivedSource{it->second.canonical_hash, it->second.generation};
}

std::optional<NodeValidation> DerivationGraph::validation(std::string_view path) const
{
    auto it = nodes_.find(std::string(path));
    if (it == nodes_.end() || !it->second.has_report)
        return std::nullopt;
    NodeValidation out;
    out.report = it->second.report;
    out.generation = it->second.report_generation;
    // L-31 stability: a report is provisional while a re-derivation of the SAME path is still
    // queued (a settling pass may replace it); the M1-shape graph has no cross-file dependencies,
    // so other paths' pending work cannot invalidate this node's finding.
    out.stable = pending_.find(std::string(path)) == pending_.end();
    return out;
}

std::optional<ComposedView> DerivationGraph::composed(std::string_view path) const
{
    auto it = composed_.find(std::string(path));
    if (it == composed_.end())
        return std::nullopt;
    ComposedView out;
    out.scene = &it->second.scene;
    out.generation = it->second.generation;
    // Composition has cross-file dependency edges (unlike the per-path validate node): the view
    // is provisional while pending work anywhere in the scene's transitive template closure
    // could still change the flatten (L-31 stability semantics).
    out.stable = !scene_closure_pending(it->first);
    return out;
}

void DerivationGraph::reflect(std::uint64_t canonical_hash)
{
    ++reflected_hashes_[canonical_hash];
}

void DerivationGraph::unreflect(std::uint64_t canonical_hash)
{
    auto it = reflected_hashes_.find(canonical_hash);
    if (it == reflected_hashes_.end())
        return;
    if (--it->second == 0)
        reflected_hashes_.erase(it);
}

void DerivationGraph::refresh_signal()
{
    const bool overloaded = pending_.size() > config_.high_watermark;
    const bool transitioned = overloaded != signal_.overloaded;
    signal_.queue_depth = pending_.size();
    signal_.high_watermark = config_.high_watermark;
    signal_.overloaded = overloaded;
    if (transitioned && bus_ != nullptr)
    {
        bus_->publish(BackpressureEvent{signal_.queue_depth, signal_.high_watermark, overloaded});
    }
}

void DerivationGraph::publish_pass_event(const DerivePassResult& result)
{
    if (bus_ == nullptr)
        return;
    bus_->publish(
        DerivationPassEvent{result.generation, result.nodes_derived, signal_.queue_depth});
}

} // namespace context::editor::derivation
