// The shader-compile derivation node (R-FILE-005 / R-FILE-010 / R-FILE-013; issue #126, Part of #119).
// See shader_compile_node.h for the contract. Backend-agnostic — it holds an IShaderCompiler& and keys
// on (IR hash | variant | compiler id), exactly the T3a content-addressed key it re-homes.

#include "context/editor/derivation/shader_compile_node.h"

#include <utility>

namespace context::editor::derivation
{

ShaderCompileNode::ShaderCompileNode(const IShaderCompiler& compiler, ShaderCompileConfig config)
    : compiler_(&compiler), config_(config)
{
    refresh_signal();
}

std::string ShaderCompileNode::cache_key(const ShaderIr& ir, const VariantKey& variant,
                                         const IShaderCompiler& compiler)
{
    // Enumerate every input that affects the output (R-FILE-010): the IR content hash, the variant
    // key, and the compiler id. The 0x1f unit separator keeps the components unambiguous. Identical to
    // the T3a ShaderCompileCache::cache_key it replaces, so keys are stable across the re-home.
    std::string composite = context::render::material::ir_content_hash(ir);
    composite += '\x1f';
    composite += variant.canonical();
    composite += '\x1f';
    composite += compiler.id();
    return context::render::material::content_hash_hex(composite);
}

const CompiledArtifact& ShaderCompileNode::resolve(const std::string& key, const ShaderIr& ir,
                                                   const VariantKey& variant)
{
    const auto it = entries_.lower_bound(key);
    if (it != entries_.end() && it->first == key)
    {
        ++hits_;
        return it->second; // cache hit — no recompute
    }

    ++misses_;
    // Write-once + content-addressed: a fresh compile of the same inputs is byte-identical. lower_bound
    // above doubles as the insertion hint, so the miss path walks the tree only once.
    const auto inserted = entries_.emplace_hint(it, key, compiler_->compile(ir, variant));
    // Index the entry under its IR content hash so a later invalidate_ir() can evict every variant of a
    // re-authored shader in one shot (R-FILE-005).
    ir_index_[inserted->second.ir_hash].insert(key);
    return inserted->second;
}

const CompiledArtifact& ShaderCompileNode::get_or_compile(const ShaderIr& ir,
                                                          const VariantKey& variant)
{
    return resolve(cache_key(ir, variant, *compiler_), ir, variant);
}

void ShaderCompileNode::request(const ShaderIr& ir, const VariantKey& variant, bool priority)
{
    const std::string key = cache_key(ir, variant, *compiler_);

    // Coalesce: an already-cached key needs no work; an already-pending key is not enqueued twice —
    // one batched pass per burst, never one pass per event (R-FILE-013).
    if (entries_.find(key) != entries_.end())
    {
        return;
    }
    const auto it = pending_.find(key);
    if (it != pending_.end())
    {
        it->second.priority = it->second.priority || priority; // upgrade to priority if requested
        return;
    }

    pending_.emplace(key, Pending{ir, variant, priority});
    refresh_signal();
}

ShaderPassResult ShaderCompileNode::run_pass()
{
    ShaderPassResult result;
    const bool overloaded = pending_.size() > config_.high_watermark;
    result.overloaded = overloaded ? 1 : 0;

    // Two sweeps so priority (visible/queried) requests never stall behind a burst: compile ALL
    // priority requests first, then fill non-priority up to the per-pass budget when overloaded (the
    // rest defers), or drain the whole non-priority remainder when not overloaded.
    std::size_t nonpriority_budget = overloaded ? config_.max_compiles_per_pass : pending_.size();

    for (bool priority_sweep : {true, false})
    {
        for (auto it = pending_.begin(); it != pending_.end();)
        {
            const Pending& req = it->second;
            if (req.priority != priority_sweep)
            {
                ++it;
                continue;
            }
            if (!priority_sweep && nonpriority_budget == 0)
            {
                break; // load-shed: budget spent, defer the rest of the non-priority queue
            }

            const std::size_t misses_before = misses_;
            resolve(it->first, req.ir, req.variant);
            if (misses_ > misses_before)
            {
                ++result.compiled;
            }
            else
            {
                ++result.served; // a synchronous get_or_compile() beat this request to the store
            }
            if (!priority_sweep)
            {
                --nonpriority_budget;
            }
            it = pending_.erase(it);
        }
    }

    result.deferred = pending_.size();
    refresh_signal();
    return result;
}

std::size_t ShaderCompileNode::invalidate_ir(const ShaderIr& superseded_ir)
{
    const std::string ir_hash = context::render::material::ir_content_hash(superseded_ir);

    std::size_t evicted = 0;
    const auto idx = ir_index_.find(ir_hash);
    if (idx != ir_index_.end())
    {
        for (const std::string& key : idx->second)
        {
            evicted += entries_.erase(key);
        }
        ir_index_.erase(idx);
    }

    // Drop any still-pending requests for the superseded content too, so a queued burst never compiles
    // artifacts the caller has already declared stale.
    for (auto it = pending_.begin(); it != pending_.end();)
    {
        if (context::render::material::ir_content_hash(it->second.ir) == ir_hash)
        {
            it = pending_.erase(it);
        }
        else
        {
            ++it;
        }
    }

    refresh_signal();
    return evicted;
}

const CompiledArtifact* ShaderCompileNode::find(const std::string& key) const
{
    const auto it = entries_.find(key);
    return it == entries_.end() ? nullptr : &it->second;
}

bool ShaderCompileNode::contains(const std::string& key) const
{
    return entries_.find(key) != entries_.end();
}

void ShaderCompileNode::refresh_signal() noexcept
{
    signal_.queue_depth = pending_.size();
    signal_.high_watermark = config_.high_watermark;
    signal_.overloaded = signal_.queue_depth > config_.high_watermark;
}

} // namespace context::editor::derivation
