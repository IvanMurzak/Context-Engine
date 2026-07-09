// R-FILE-010 content-addressed shader-compile cache: cache-hit skips recompute, keys enumerate every
// input, entries are stable and content-addressed (R-REND-005; issue #121).

#include "context/render/material/material_ir.h"
#include "context/render/material/shader_cache.h"
#include "context/render/material/shader_compiler.h"

#include "material_test.h"

#include <string>
#include <vector>

using namespace context::render::material;

namespace
{

ShaderIr make_ir()
{
    ShaderIr ir;
    ir.name = "lit_pbr";
    ir.keywords.push_back({"NORMAL_MAP", {"off", "on"}});
    ir.keywords.push_back({"QUALITY", {"low", "med", "high"}});
    ShaderStage vs;
    vs.kind = ShaderStageKind::Vertex;
    vs.entry_point = "vs_main";
    vs.source = "void vs_main() {}";
    ir.stages.push_back(vs);
    return ir;
}

// The headline behaviour: a repeated (ir, variant) request is served from the cache without a second
// compile() call.
void test_cache_hit_skips_recompute()
{
    const ShaderIr ir = make_ir();
    const std::vector<VariantKey> variants = enumerate_variants(ir);
    CHECK(!variants.empty());

    FakeShaderCompiler compiler;
    ShaderCompileCache cache(compiler);

    // First request: a miss -> one real compile, one entry.
    const CompiledArtifact first = cache.get_or_compile(ir, variants.front());
    CHECK(cache.misses() == 1);
    CHECK(cache.hits() == 0);
    CHECK(cache.size() == 1);
    CHECK(compiler.compile_count() == 1);

    // Second identical request: a hit -> NO new compile, same artifact.
    const CompiledArtifact second = cache.get_or_compile(ir, variants.front());
    CHECK(cache.hits() == 1);
    CHECK(cache.misses() == 1);
    CHECK(cache.size() == 1);
    CHECK(compiler.compile_count() == 1); // still 1 — recompute skipped
    CHECK(first == second);
}

// A different variant is a distinct cache entry (distinct key) => a fresh compile.
void test_distinct_variant_is_a_miss()
{
    const ShaderIr ir = make_ir();
    const std::vector<VariantKey> variants = enumerate_variants(ir);
    CHECK(variants.size() >= 2);

    FakeShaderCompiler compiler;
    ShaderCompileCache cache(compiler);

    (void)cache.get_or_compile(ir, variants[0]);
    (void)cache.get_or_compile(ir, variants[1]);
    CHECK(cache.misses() == 2);
    CHECK(cache.size() == 2);
    CHECK(compiler.compile_count() == 2);
}

// Enumerate-then-compile-all, then re-request all: exactly one compile per variant, the rest hits.
void test_full_variant_space_then_all_hits()
{
    const ShaderIr ir = make_ir();
    const std::vector<VariantKey> variants = enumerate_variants(ir);
    CHECK(variants.size() == 6); // 2 x 3

    FakeShaderCompiler compiler;
    ShaderCompileCache cache(compiler);

    for (const VariantKey& v : variants)
    {
        (void)cache.get_or_compile(ir, v);
    }
    CHECK(cache.size() == variants.size());
    CHECK(cache.misses() == variants.size());
    CHECK(compiler.compile_count() == variants.size());

    for (const VariantKey& v : variants)
    {
        (void)cache.get_or_compile(ir, v);
    }
    CHECK(cache.hits() == variants.size());
    CHECK(compiler.compile_count() == variants.size()); // no further compiles
}

// The cache key enumerates every input (R-FILE-010): IR hash + variant + compiler id. It is
// deterministic and content-addressed (instance-independent).
void test_cache_key_enumerates_inputs()
{
    const ShaderIr ir = make_ir();
    const std::vector<VariantKey> variants = enumerate_variants(ir);
    FakeShaderCompiler compiler;

    // Deterministic + instance-independent.
    const std::string k0 = ShaderCompileCache::cache_key(ir, variants[0], compiler);
    CHECK(k0 == ShaderCompileCache::cache_key(ir, variants[0], compiler));
    CHECK(!k0.empty());

    // Variant is a key component.
    CHECK(k0 != ShaderCompileCache::cache_key(ir, variants[1], compiler));

    // Compiler id is a key component.
    FakeShaderCompiler other("fake-ref-alt");
    CHECK(k0 != ShaderCompileCache::cache_key(ir, variants[0], other));

    // IR content is a key component.
    ShaderIr ir2 = ir;
    ir2.name = "lit_pbr_v2";
    CHECK(k0 != ShaderCompileCache::cache_key(ir2, variants[0], compiler));

    // The key a get_or_compile() stored under is discoverable via contains().
    ShaderCompileCache cache(compiler);
    (void)cache.get_or_compile(ir, variants[0]);
    CHECK(cache.contains(k0));
    CHECK(!cache.contains(ShaderCompileCache::cache_key(ir, variants[1], compiler)));
}

} // namespace

int main()
{
    test_cache_hit_skips_recompute();
    test_distinct_variant_is_a_miss();
    test_full_variant_space_then_all_hits();
    test_cache_key_enumerates_inputs();
    MATERIAL_TEST_MAIN_END();
}
