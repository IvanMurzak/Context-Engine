// The shader-compile derivation node (R-FILE-005 / R-FILE-010 / R-FILE-013; issue #126, Part of #119).
// Shader compilation is a first-class derived artifact in the derivation graph — keyed, content-
// addressed, cached, invalidatable, and backpressured — NOT a standalone per-build side cache. The node
// wraps the backend-free IShaderCompiler seam (src/render/material/shader_compiler.h): its inputs are
// (authoring IR, variant key, compiler id) and its output is the CompiledArtifact, cached under the
// R-FILE-010 content-addressed key. This re-homes the T3a standalone ShaderCompileCache behind the
// derivation-graph node contract — the node OWNS the content-addressed store — and adds the two
// derivation-graph behaviours the standalone cache lacked: R-FILE-005 invalidation (a re-authored
// shader drops its stale variant artifacts) and R-FILE-013 backpressure (a coalesced request queue
// drained under a per-pass compile budget with a bounded-lag signal + load-shed), exactly like the
// canonical-parse / validate / compose / migrate nodes that live beside it.

#pragma once

#include "context/editor/derivation/derivation_graph.h" // BackpressureSignal (shared node vocabulary)
#include "context/render/material/material_ir.h"
#include "context/render/material/shader_compiler.h"

#include <cstddef>
#include <map>
#include <set>
#include <string>

namespace context::editor::derivation
{

using context::render::material::CompiledArtifact;
using context::render::material::IShaderCompiler;
using context::render::material::ShaderIr;
using context::render::material::VariantKey;

// Tuning for the R-FILE-013 backpressure path. Defaults are deliberately small so a single burst
// demonstrates the whole coalesce → overload → load-shed policy; a real daemon sizes these to its
// compile-latency budget. Mirrors DerivationConfig's high_watermark / max_batch_per_pass shape so the
// shader node reads like the other derived nodes.
struct ShaderCompileConfig
{
    std::size_t high_watermark = 64;       // pending compile requests above this trips overload
    std::size_t max_compiles_per_pass = 32; // cap on NON-priority real compiles per OVERLOADED pass
                                            // (load-shed); priority requests always compile
};

// The result of one coalesced compile pass (peer of DerivePassResult).
struct ShaderPassResult
{
    std::size_t compiled = 0;  // real compiler.compile() calls this pass (cache misses that ran)
    std::size_t served = 0;    // requests satisfied from the cache this pass (no recompute)
    std::size_t deferred = 0;  // requests load-shed to a later pass (still pending after this one)
    std::size_t overloaded = 0; // 1 iff the pass ran under overload (queue > high_watermark), else 0
};

// The shader-compile derivation node. Holds an IShaderCompiler& (the compiler must outlive the node)
// and owns the content-addressed artifact store re-homed from T3a. Two entry paths share the ONE store:
//   * get_or_compile() — the synchronous path (the T3a semantics, preserved): compile-or-serve now, a
//     cache hit skips recompute. Used by a query that needs the artifact immediately (never stalled).
//   * request() + run_pass() — the backpressured path (R-FILE-013): enqueue coalesced requests, then
//     drain them under a per-pass budget, load-shedding non-priority work under overload.
// Single-threaded / deterministic by construction, like the rest of the derivation graph.
class ShaderCompileNode
{
public:
    explicit ShaderCompileNode(const IShaderCompiler& compiler, ShaderCompileConfig config = {});

    // The content-addressed key for (ir, variant, compiler): a hash of
    // (ir_content_hash | variant.canonical() | compiler.id()). Deterministic and instance-independent
    // (R-FILE-010) — exposed so callers/tools can address entries without compiling. Unchanged from T3a.
    [[nodiscard]] static std::string cache_key(const ShaderIr& ir, const VariantKey& variant,
                                               const IShaderCompiler& compiler);

    // Compile `variant` of `ir`, or serve a prior identical result (a hit does NOT call compile()).
    // The returned reference is stable until an invalidate_ir() evicts that entry (std::map nodes are
    // otherwise never relocated), so callers may hold it across further calls. This is the T3a
    // get_or_compile() contract, now living on the derivation node.
    [[nodiscard]] const CompiledArtifact& get_or_compile(const ShaderIr& ir,
                                                         const VariantKey& variant);

    // Enqueue a (ir, variant) compile request for a later coalesced pass (R-FILE-013). Coalesces: a
    // key already cached OR already pending is not enqueued twice; a `priority` request already pending
    // as non-priority is upgraded (visible/queried variants derive first, never stall behind a burst).
    void request(const ShaderIr& ir, const VariantKey& variant, bool priority = false);

    // Run ONE coalesced compile pass over the pending queue. Priority requests always compile; under
    // overload (pending > high_watermark) the non-priority fill is capped at max_compiles_per_pass and
    // the remainder defers to a later pass; without overload the whole queue drains.
    ShaderPassResult run_pass();

    // R-FILE-005 invalidation: drop every cached variant artifact of a shader whose authoring IR
    // changed (all entries under `superseded_ir`'s content hash), so the next request recompiles under
    // the new content. Also drops any still-pending requests for that superseded content. Returns the
    // number of cached entries evicted.
    std::size_t invalidate_ir(const ShaderIr& superseded_ir);

    // --- introspection ------------------------------------------------------------------------

    [[nodiscard]] const CompiledArtifact* find(const std::string& key) const; // nullptr if absent
    [[nodiscard]] bool contains(const std::string& key) const;
    [[nodiscard]] std::size_t hits() const noexcept { return hits_; }
    [[nodiscard]] std::size_t misses() const noexcept { return misses_; }
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] std::size_t pending_count() const noexcept { return pending_.size(); }
    [[nodiscard]] const BackpressureSignal& backpressure() const noexcept { return signal_; }

private:
    struct Pending
    {
        ShaderIr ir;
        VariantKey variant;
        bool priority = false;
    };

    // Compile (miss) or serve (hit) the request under `key`; inserts + indexes on a miss. Returns the
    // stored entry. Shared by both entry paths so the store/index/counters stay in one place.
    const CompiledArtifact& resolve(const std::string& key, const ShaderIr& ir,
                                    const VariantKey& variant);
    void refresh_signal() noexcept;

    const IShaderCompiler* compiler_;
    ShaderCompileConfig config_;
    std::map<std::string, CompiledArtifact> entries_;      // the content-addressed store (re-homed T3a)
    std::map<std::string, std::set<std::string>> ir_index_; // ir_content_hash -> keys (for invalidation)
    std::map<std::string, Pending> pending_;               // coalesced request queue (key -> request)
    BackpressureSignal signal_;
    std::size_t hits_ = 0;
    std::size_t misses_ = 0;
};

} // namespace context::editor::derivation
