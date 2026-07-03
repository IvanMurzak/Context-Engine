// The M1 incremental derivation graph — file changes → canonical parse → derived World (L-19/L-22),
// with derivation-side backpressure (R-FILE-013) and the read-your-writes generation counter that
// the read barrier (R-CLI-006) resolves against.

#pragma once

#include "context/editor/derivation/canonical_parse.h"
#include "context/editor/filesync/reconciler.h"
#include "context/kernel/world.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
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
};

// The result of one coalesced derivation pass.
struct DerivePassResult
{
    std::uint64_t generation = 0;   // derived-world generation after this pass
    std::size_t nodes_derived = 0;  // nodes whose derived value was (re)computed this pass
    std::size_t nodes_skipped = 0;  // dirty nodes memoized away (canonical form unchanged, L-22)
    std::size_t nodes_removed = 0;  // nodes whose source was removed this pass
    std::size_t deferred = 0;       // nodes load-shed to a later pass (still pending after this one)
};

// The receipt an ingest returns — the R-CLI-006 own-write barrier key.
struct WriteTicket
{
    std::string path;
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
    explicit DerivationGraph(DerivationConfig config = {},
                             context::kernel::EventBus* bus = nullptr);

    // Ingest one reconciled change + its authored bytes. Parses to canonical form NOW so the returned
    // ticket carries the canonical hash (R-CLI-006), then coalesces into the pending dirty set — a later
    // write to the same path before the next pass replaces the earlier one (one batched pass per burst,
    // never one pass per event: R-FILE-013). A `removed` change enqueues a node removal (bytes ignored).
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

private:
    struct Node
    {
        context::kernel::Entity entity{};
        std::uint64_t canonical_hash = 0;
        std::uint64_t generation = 0;
        bool visible = false;
        bool alive = false; // an entity exists in the derived World for this source
    };

    struct Pending
    {
        CanonicalForm form;
        bool removal = false;
    };

    void derive_one(const std::string& path, const Pending& pending, std::uint64_t target_gen,
                    DerivePassResult& result);
    void reflect(std::uint64_t canonical_hash);
    void unreflect(std::uint64_t canonical_hash);
    void refresh_signal();
    void publish_pass_event(const DerivePassResult& result);

    DerivationConfig config_;
    context::kernel::EventBus* bus_;
    context::kernel::World world_;
    std::map<std::string, Node> nodes_;
    std::map<std::string, Pending> pending_;
    std::map<std::uint64_t, std::size_t> reflected_hashes_; // canonical hash -> live node count
    BackpressureSignal signal_;
    std::uint64_t generation_ = 0;
    std::uint64_t parse_invocations_ = 0;
    std::uint64_t derivations_ = 0;
};

} // namespace context::editor::derivation
