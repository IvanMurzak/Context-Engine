// The M1 incremental derivation graph — file changes → canonical parse → derived World (L-19/L-22),
// with derivation-side backpressure (R-FILE-013) and the read-your-writes generation counter that
// the read barrier (R-CLI-006) resolves against.

#pragma once

#include "context/editor/compose/flatten.h"
#include "context/editor/compose/scene_model.h"
#include "context/editor/derivation/canonical_parse.h"
#include "context/editor/filesync/reconciler.h"
#include "context/editor/schema/validator.h"
#include "context/kernel/world.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace context::kernel
{
class EventBus;
}

namespace context::editor::derivation
{

// The per-source derived output carried by that source's entity in the derived World. The derived
// World is `context::kernel::World` (L-24: the RuntimeKernel consumes derivation OUTPUT, never authored
// files); M1 keeps one entity per source file stamped with its canonical hash + the generation at
// which its derived value last changed. Richer per-kind derived components land with M2's schema model.
struct DerivedSource
{
    std::uint64_t canonical_hash = 0; // canonical-content hash of the source (canonical_parse)
    std::uint64_t generation = 0;     // derived-world generation this node's value last changed at
};

// Bounded-lag / queue-depth signal (R-FILE-013). Published so cooperative clients/agents self-throttle
// under sustained write load; also surfaced on the kernel EventBus as BackpressureEvent when supplied.
struct BackpressureSignal
{
    std::size_t queue_depth = 0;    // pending coalesced dirty nodes awaiting derivation (the lag)
    std::size_t high_watermark = 0; // queue_depth above this trips `overloaded` + load-shedding
    bool overloaded = false;        // true while queue_depth exceeds high_watermark
};

// Tuning for coalescing + load-shed. Defaults are deliberately small so a single burst demonstrates
// the whole policy; a real daemon sizes these to its latency budget (the R-FILE-011 bench task pins
// the documented maximum dirty-set latency these bound).
struct DerivationConfig
{
    std::size_t high_watermark = 64;      // overload threshold on the pending dirty set
    std::size_t max_batch_per_pass = 32;  // cap on the NON-VISIBLE fill per overloaded pass (load-shed);
                                          // visible/queried nodes always derive — never stall a query
    compose::ComposeLimits compose_limits{}; // the M2 compose node's R-FILE-011(e) invariants
};

// The result of one coalesced derivation pass.
struct DerivePassResult
{
    std::uint64_t generation = 0;   // derived-world generation after this pass
    std::size_t nodes_derived = 0;  // nodes whose derived value was (re)computed this pass
    std::size_t nodes_skipped = 0;  // dirty nodes memoized away (canonical form unchanged, L-22)
    std::size_t nodes_removed = 0;  // nodes whose source was removed this pass
    std::size_t deferred = 0;       // nodes load-shed to a later pass (still pending after this one)
    std::size_t nodes_invalid = 0;  // nodes whose payload FAILED schema validation this pass —
                                    // their last-good derived state was retained (R-FILE-003)
    std::size_t scenes_composed = 0; // scenes (re)flattened by the compose node this pass —
                                     // changed scenes plus their transitive dependents (fan-out)
};

// A node's schema-validation state, surfaced by DerivationGraph::validation() (the M2 validate
// node's R-FILE-003 output). `stable` mirrors the L-31 diagnostic `stability` field: false
// (provisional) while a re-derivation of the SAME path is still queued — a settling pass may clear
// the finding, so an agent should not act on it yet; true (stable) once the path has no pending
// work. `generation` is the derived-world generation the report was produced at.
struct NodeValidation
{
    schema::ValidationReport report;
    std::uint64_t generation = 0;
    bool stable = true;
};

// The compose node's flattened view of one scene (L-35: composition flattens in the derivation
// graph at zero runtime cost), surfaced by DerivationGraph::composed(). `scene` points at
// graph-owned storage valid until the next run_pass(). `generation` is the derived-world
// generation the flatten was produced at; `stable` mirrors the L-31 semantics — false while
// pending work anywhere in the scene's transitive template closure could still change the
// composed output (composition introduces the graph's first cross-file dependency edges).
struct ComposedView
{
    const compose::ComposedScene* scene = nullptr;
    std::uint64_t generation = 0;
    bool stable = true;
};

// The receipt an ingest returns — the R-CLI-006 own-write barrier key, carrying BOTH hashes of the
// R-FILE-001 two-hash split: `raw_hash` (the raw-byte identity of the ingested bytes — the
// watch/reconcile change-detector's and CAS `--if-match`'s key) and `canonical_hash` (the
// canonical-content identity — the derivation/cache key and the own-write barrier). For non-JSON
// content the two coincide by construction (no canonicalization pass — the binary-sidecar rule).
struct WriteTicket
{
    std::string path;
    std::uint64_t raw_hash = 0;         // raw-byte hash of the ingested bytes (0 for a removal)
    std::uint64_t canonical_hash = 0;   // barrier on THIS for own writes (`--after-hash`)
    std::uint64_t generation_after = 0; // best-effort target generation (`--after-generation`); under
                                        // load-shed the real generation may be later, so own writes
                                        // barrier on the hash (R-CLI-006), foreign gens on generation.
    bool removal = false;
};

// Event published on the kernel-internal EventBus after each non-empty pass — carries the derived-world
// generation the bridge-daemon's client event stream (R-BRIDGE-008) forwards. The bus here is the
// engine-internal one (event_bus.h); wiring it onto the client stream is the bridge's concern.
struct DerivationPassEvent
{
    std::uint64_t generation = 0;
    std::size_t nodes_derived = 0;
    std::size_t queue_depth = 0;
};

// Event published when the overload state transitions — the self-throttle signal (R-FILE-013).
struct BackpressureEvent
{
    std::size_t queue_depth = 0;
    std::size_t high_watermark = 0;
    bool overloaded = false;
};

// The incremental derivation graph. Ingest reconciled file changes (from the file-sync layer's
// ReconcileChange, consumed read-only), then run coalesced passes that (re)derive only the dirty
// subgraph into the derived World, advancing a monotonic generation counter. Single-threaded /
// deterministic by construction: the "bounded-block" of the read barrier is modeled as a bounded number
// of passes (see query_barrier.h), matching how a daemon drains its queue while a query waits.
class DerivationGraph
{
public:
    // `schemas` wires the M2 validate node: when non-null, every JSON ingest is validated against
    // the registered kind schemas (schema::engine_schemas() in the real EditorKernel composition)
    // and a failing payload RETAINS its last-good derived state (R-FILE-003). nullptr disables
    // validation (the M1 behavior — direct graph consumers and micro-benches are unchanged).
    // `ref_resolver` is the x-ctx-ref meta-lookup seam (R-DATA-006); nullptr until the asset
    // database lands, which limits typed-reference checks to shape only.
    explicit DerivationGraph(DerivationConfig config = {},
                             context::kernel::EventBus* bus = nullptr,
                             const schema::SchemaSet* schemas = nullptr,
                             const schema::RefTargetResolver* ref_resolver = nullptr);

    // Ingest one reconciled change + its authored bytes. Parses to canonical form NOW so the returned
    // ticket carries the canonical hash (R-CLI-006), then coalesces into the pending dirty set — a later
    // write to the same path before the next pass replaces the earlier one (one batched pass per burst,
    // never one pass per event: R-FILE-013). A `removed` change enqueues a node removal (bytes ignored).
    // With a schema set wired, the ingest also runs the validate node on the SAME parse (R-DATA-006).
    WriteTicket apply(const context::editor::filesync::ReconcileChange& change,
                      std::string_view source_bytes);

    // Run ONE coalesced derivation pass over the pending dirty set. Under overload it load-sheds:
    // visible/queried nodes derive first, then a bounded fill of the rest; the remainder defers to
    // later passes. Advances the generation by one iff it incorporated a non-empty batch.
    DerivePassResult run_pass();

    // Mark a path as visible/queried so the load-shed policy prioritizes it (R-FILE-013).
    void set_visible(std::string_view path, bool visible);

    // --- derived output + concurrency primitives (R-CLI-006) ----------------------------------

    [[nodiscard]] const context::kernel::World& world() const noexcept { return world_; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] const BackpressureSignal& backpressure() const noexcept { return signal_; }

    // Read-your-writes predicates the QueryBarrier drives. has_generation: the derived world has
    // reached generation g (`--after-generation`). reflects_hash: some alive derived node currently
    // carries canonical hash h (`--after-hash`, the own-write barrier).
    [[nodiscard]] bool has_generation(std::uint64_t g) const noexcept { return generation_ >= g; }
    [[nodiscard]] bool reflects_hash(std::uint64_t canonical_hash) const;

    // --- introspection ------------------------------------------------------------------------

    [[nodiscard]] std::size_t node_count() const noexcept { return world_.alive_count(); }
    [[nodiscard]] std::size_t pending_count() const noexcept { return pending_.size(); }
    [[nodiscard]] std::uint64_t parse_invocations() const noexcept { return parse_invocations_; }
    [[nodiscard]] std::uint64_t derivations() const noexcept { return derivations_; }
    [[nodiscard]] std::optional<DerivedSource> node(std::string_view path) const;

    // The validate node's R-FILE-003 output for `path`: the last schema-validation report a pass
    // produced (pointer + line/column diagnostics), its generation, and its L-31 stability.
    // nullopt when the path never passed through a validating pass (no schema set wired, non-JSON
    // content, or the path is unknown). A failing report means the node's derived value is its
    // LAST-GOOD state (or that the source never derived).
    [[nodiscard]] std::optional<NodeValidation> validation(std::string_view path) const;

    // The compose node's flattened output for scene `path` (L-35 read path): its composed
    // entities (id-path identity, overrides applied, provenance chains) + diagnostics. Every
    // scene document that derives gets a composed view — the degenerate flatten of an
    // instance-free scene is its own entities. nullopt when the path never derived as a scene.
    // A scene whose LATEST ingest failed validation retains its last-good composed view
    // (R-FILE-003), exactly like its derived node. The returned pointer is invalidated by the
    // next run_pass().
    [[nodiscard]] std::optional<ComposedView> composed(std::string_view path) const;

private:
    struct Node
    {
        context::kernel::Entity entity{};
        std::uint64_t canonical_hash = 0;
        std::uint64_t generation = 0;
        bool visible = false;
        bool alive = false; // an entity exists in the derived World for this source
        schema::ValidationReport report; // validate-node output (meaningful iff has_report)
        std::uint64_t report_generation = 0;
        bool has_report = false;
    };

    struct Pending
    {
        CanonicalForm form;
        bool removal = false;
        schema::ValidationReport report; // computed at ingest, on the SAME parse (iff validated)
        bool validated = false;
    };

    // The compose node's per-scene record: the retained composition view of the LAST-GOOD ingest
    // (the flatten input) and the flattened output with the generation it was produced at.
    struct ComposedRecord
    {
        compose::ComposedScene scene;
        std::uint64_t generation = 0;
    };

    void derive_one(const std::string& path, const Pending& pending, std::uint64_t target_gen,
                    DerivePassResult& result, std::set<std::string>& compose_seeds);
    void recompose(const std::set<std::string>& seeds, std::uint64_t target_gen,
                   DerivePassResult& result);
    [[nodiscard]] bool scene_closure_pending(const std::string& path) const;
    void reflect(std::uint64_t canonical_hash);
    void unreflect(std::uint64_t canonical_hash);
    void refresh_signal();
    void publish_pass_event(const DerivePassResult& result);

    DerivationConfig config_;
    context::kernel::EventBus* bus_;
    const schema::SchemaSet* schemas_;              // validate node's kind set (nullptr = off)
    const schema::RefTargetResolver* ref_resolver_; // x-ctx-ref meta-lookup seam (nullptr until
                                                    // the asset database lands)
    context::kernel::World world_;
    std::map<std::string, Node> nodes_;
    std::map<std::string, Pending> pending_;
    std::map<std::uint64_t, std::size_t> reflected_hashes_; // canonical hash -> live node count
    // --- the compose node (M2 wave 3, L-35) — the graph's first cross-file dependency edges ----
    std::map<std::string, compose::SceneDoc> scene_docs_;   // last-good scene views (flatten input)
    std::map<std::string, ComposedRecord> composed_;        // flattened outputs per scene
    std::map<std::string, std::vector<std::string>> instance_deps_; // scene -> direct templates
    BackpressureSignal signal_;
    std::uint64_t generation_ = 0;
    std::uint64_t parse_invocations_ = 0;
    std::uint64_t derivations_ = 0;
};

} // namespace context::editor::derivation
