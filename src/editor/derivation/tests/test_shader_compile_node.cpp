// The shader-compile derivation node (R-FILE-005 / R-FILE-010 / R-FILE-013; issue #126, Part of #119).
// This exercises, THROUGH the derivation node, the T3a content-addressed cache semantics that the
// standalone ShaderCompileCache used to carry (cache-hit skips recompute; the key enumerates every
// input; distinct variants are distinct entries), PLUS the two derivation-graph behaviours the node
// adds: R-FILE-005 invalidation (a re-authored shader drops its stale variants) and R-FILE-013
// backpressure (a coalesced request queue drained under a per-pass budget with load-shed).

#include "context/editor/derivation/shader_compile_node.h"
#include "context/render/material/material_ir.h"
#include "context/render/material/shader_compiler.h"

#include "derivation_test.h"

#include <string>
#include <vector>

using namespace context::editor::derivation;
using context::render::material::CompiledArtifact;
using context::render::material::enumerate_variants;
using context::render::material::FakeShaderCompiler;
using context::render::material::ShaderIr;
using context::render::material::ShaderStage;
using context::render::material::ShaderStageKind;
using context::render::material::VariantKey;

namespace
{

ShaderIr make_ir(const std::string& name = "lit_pbr")
{
    ShaderIr ir;
    ir.name = name;
    ir.keywords.push_back({"NORMAL_MAP", {"off", "on"}});
    ir.keywords.push_back({"QUALITY", {"low", "med", "high"}});
    ShaderStage vs;
    vs.kind = ShaderStageKind::Vertex;
    vs.entry_point = "vs_main";
    vs.source = "void vs_main() {}";
    ir.stages.push_back(vs);
    return ir;
}

// The headline T3a behaviour, now on the node: a repeated (ir, variant) request is served from the
// cache without a second compile() call — and the artifact round-trips the backend exactly.
void test_cache_hit_skips_recompute()
{
    const ShaderIr ir = make_ir();
    const std::vector<VariantKey> variants = enumerate_variants(ir);
    CHECK(!variants.empty());

    FakeShaderCompiler compiler;
    ShaderCompileNode node(compiler);

    const CompiledArtifact first = node.get_or_compile(ir, variants.front());
    CHECK(node.misses() == 1);
    CHECK(node.hits() == 0);
    CHECK(node.size() == 1);
    CHECK(compiler.compile_count() == 1);
    // Round-trip: the node's stored artifact is exactly what the backend produces for these inputs.
    CHECK(first == compiler.compile(ir, variants.front()));

    const CompiledArtifact second = node.get_or_compile(ir, variants.front());
    CHECK(node.hits() == 1);
    CHECK(node.misses() == 1);
    CHECK(node.size() == 1);
    CHECK(compiler.compile_count() == 2); // the round-trip CHECK above called compile() once directly
    CHECK(first == second);
}

// A different variant is a distinct cache entry (distinct key) => a fresh compile.
void test_distinct_variant_is_a_miss()
{
    const ShaderIr ir = make_ir();
    const std::vector<VariantKey> variants = enumerate_variants(ir);
    CHECK(variants.size() >= 2);

    FakeShaderCompiler compiler;
    ShaderCompileNode node(compiler);

    (void)node.get_or_compile(ir, variants[0]);
    (void)node.get_or_compile(ir, variants[1]);
    CHECK(node.misses() == 2);
    CHECK(node.size() == 2);
    CHECK(compiler.compile_count() == 2);
}

// Enumerate-then-compile-all, then re-request all: exactly one compile per variant, the rest hits.
void test_full_variant_space_then_all_hits()
{
    const ShaderIr ir = make_ir();
    const std::vector<VariantKey> variants = enumerate_variants(ir);
    CHECK(variants.size() == 6); // 2 x 3

    FakeShaderCompiler compiler;
    ShaderCompileNode node(compiler);

    for (const VariantKey& v : variants)
    {
        (void)node.get_or_compile(ir, v);
    }
    CHECK(node.size() == variants.size());
    CHECK(node.misses() == variants.size());
    CHECK(compiler.compile_count() == variants.size());

    for (const VariantKey& v : variants)
    {
        (void)node.get_or_compile(ir, v);
    }
    CHECK(node.hits() == variants.size());
    CHECK(compiler.compile_count() == variants.size()); // no further compiles
}

// The content-addressed key enumerates every input (R-FILE-010): IR hash + variant + compiler id. It
// is deterministic and instance-independent, and discoverable via contains()/find().
void test_cache_key_enumerates_inputs()
{
    const ShaderIr ir = make_ir();
    const std::vector<VariantKey> variants = enumerate_variants(ir);
    FakeShaderCompiler compiler;

    const std::string k0 = ShaderCompileNode::cache_key(ir, variants[0], compiler);
    CHECK(k0 == ShaderCompileNode::cache_key(ir, variants[0], compiler));
    CHECK(!k0.empty());

    CHECK(k0 != ShaderCompileNode::cache_key(ir, variants[1], compiler)); // variant is a component

    FakeShaderCompiler other("fake-ref-alt");
    CHECK(k0 != ShaderCompileNode::cache_key(ir, variants[0], other)); // compiler id is a component

    const ShaderIr ir2 = make_ir("lit_pbr_v2");
    CHECK(k0 != ShaderCompileNode::cache_key(ir2, variants[0], compiler)); // IR content is a component

    ShaderCompileNode node(compiler);
    const CompiledArtifact& stored = node.get_or_compile(ir, variants[0]);
    CHECK(node.contains(k0));
    CHECK(node.find(k0) != nullptr);
    CHECK(*node.find(k0) == stored);
    CHECK(!node.contains(ShaderCompileNode::cache_key(ir, variants[1], compiler)));
    CHECK(node.find(ShaderCompileNode::cache_key(ir, variants[1], compiler)) == nullptr);
}

// R-FILE-013 backpressure: a burst of requests coalesces, drains in one un-overloaded pass, and a
// duplicate request (or a request for an already-cached key) is a no-op.
void test_request_queue_coalesces_and_drains()
{
    const ShaderIr ir = make_ir();
    const std::vector<VariantKey> variants = enumerate_variants(ir);
    CHECK(variants.size() == 6);

    FakeShaderCompiler compiler;
    ShaderCompileNode node(compiler);

    // Enqueue every variant TWICE — coalescing keeps the queue at the distinct-key count.
    for (const VariantKey& v : variants)
    {
        node.request(ir, v);
        node.request(ir, v); // duplicate — coalesced away
    }
    CHECK(node.pending_count() == variants.size());
    CHECK(!node.backpressure().overloaded); // 6 <= default high_watermark (64)

    const ShaderPassResult r = node.run_pass();
    CHECK(r.compiled == variants.size());
    CHECK(r.deferred == 0);
    CHECK(r.overloaded == 0);
    CHECK(node.pending_count() == 0);
    CHECK(node.size() == variants.size());
    CHECK(compiler.compile_count() == variants.size());

    // A request for an already-cached key does not re-enqueue.
    node.request(ir, variants[0]);
    CHECK(node.pending_count() == 0);
}

// R-FILE-013 load-shed: under overload the pass compiles priority requests + a bounded non-priority
// fill, deferring the rest to later passes; the queue drains monotonically across passes.
void test_overload_load_sheds_bounded_fill()
{
    FakeShaderCompiler compiler;
    // Tiny budget so a small burst overloads: high_watermark 2, 3 non-priority compiles per pass.
    ShaderCompileNode node(compiler, ShaderCompileConfig{/*high_watermark=*/2, /*max_per_pass=*/3});

    // 10 distinct shaders (distinct IR content => distinct keys), one default variant each.
    std::vector<ShaderIr> irs;
    for (int i = 0; i < 10; ++i)
    {
        irs.push_back(make_ir("shader_" + std::to_string(i)));
    }
    const VariantKey def = enumerate_variants(irs[0]).front();

    for (const ShaderIr& s : irs)
    {
        node.request(s, def);
    }
    // Mark one as priority — it must compile on the first pass regardless of the shed budget.
    node.request(irs[9], def, /*priority=*/true);

    CHECK(node.pending_count() == 10);
    CHECK(node.backpressure().overloaded); // 10 > high_watermark(2)

    const ShaderPassResult r1 = node.run_pass();
    CHECK(r1.overloaded == 1);
    // 1 priority + 3 non-priority fill = 4 compiles; 6 deferred.
    CHECK(r1.compiled == 4);
    CHECK(r1.deferred == 6);
    CHECK(node.pending_count() == 6);
    // The priority shader landed in the store on pass 1.
    CHECK(node.contains(ShaderCompileNode::cache_key(irs[9], def, compiler)));

    // Drain the rest; the queue only shrinks.
    std::size_t guard = 0;
    while (node.pending_count() > 0 && guard++ < 100)
    {
        const ShaderPassResult rn = node.run_pass();
        CHECK(rn.compiled >= 1); // forward progress every pass
    }
    CHECK(node.pending_count() == 0);
    CHECK(node.size() == 10);
    CHECK(compiler.compile_count() == 10);
    CHECK(!node.backpressure().overloaded);
}

// R-FILE-005 invalidation: a re-authored shader drops its stale variant artifacts, so the next
// request recompiles under the new content hash. Other shaders' entries are untouched.
void test_invalidate_ir_drops_stale_variants()
{
    FakeShaderCompiler compiler;
    ShaderCompileNode node(compiler);

    const ShaderIr a = make_ir("mat_a");
    const ShaderIr b = make_ir("mat_b");
    const std::vector<VariantKey> variants = enumerate_variants(a);

    for (const VariantKey& v : variants)
    {
        (void)node.get_or_compile(a, v);
        (void)node.get_or_compile(b, v);
    }
    CHECK(node.size() == 2 * variants.size());
    const std::size_t compiles_before = compiler.compile_count();

    // Re-author `a` (its IR content changes) and invalidate the superseded content.
    const std::size_t evicted = node.invalidate_ir(a);
    CHECK(evicted == variants.size());
    CHECK(node.size() == variants.size()); // only b's entries remain
    // b is still fully cached — a re-request is a pure hit.
    for (const VariantKey& v : variants)
    {
        (void)node.get_or_compile(b, v);
    }
    CHECK(compiler.compile_count() == compiles_before); // no recompute for b

    // a's variants are gone — re-requesting them recompiles.
    for (const VariantKey& v : variants)
    {
        (void)node.get_or_compile(a, v);
    }
    CHECK(compiler.compile_count() == compiles_before + variants.size());
    CHECK(node.size() == 2 * variants.size());
}

// invalidate_ir also drops still-PENDING requests for the superseded content (a queued burst must not
// compile artifacts the caller already declared stale).
void test_invalidate_ir_drops_pending_requests()
{
    FakeShaderCompiler compiler;
    ShaderCompileNode node(compiler);

    const ShaderIr a = make_ir("mat_a");
    const ShaderIr b = make_ir("mat_b");
    const std::vector<VariantKey> variants = enumerate_variants(a);

    for (const VariantKey& v : variants)
    {
        node.request(a, v);
        node.request(b, v);
    }
    CHECK(node.pending_count() == 2 * variants.size());

    const std::size_t evicted = node.invalidate_ir(a); // a not compiled yet => 0 cached entries dropped
    CHECK(evicted == 0);
    CHECK(node.pending_count() == variants.size()); // only b's requests survive

    const ShaderPassResult r = node.run_pass();
    CHECK(r.compiled == variants.size());
    CHECK(node.pending_count() == 0);
}

} // namespace

int main()
{
    test_cache_hit_skips_recompute();
    test_distinct_variant_is_a_miss();
    test_full_variant_space_then_all_hits();
    test_cache_key_enumerates_inputs();
    test_request_queue_coalesces_and_drains();
    test_overload_load_sheds_bounded_fill();
    test_invalidate_ir_drops_stale_variants();
    test_invalidate_ir_drops_pending_requests();
    DERIVATION_TEST_MAIN_END();
}
