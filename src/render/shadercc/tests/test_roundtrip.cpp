// Round-trip proof for the real glslang + SPIRV-Cross backend (R-REND-005; sub-task C of #119, issue
// #130): author -> SPIR-V -> {HLSL, MSL, GLSL} over the REAL authored corpus (the same
// src/render/material/corpus/*.shader the fake backend exercises), plus determinism/stability +
// variant-sensitivity + a compute-stage path + a malformed-source failure path (R-QA-013:
// happy/edge/failure ship in the same PR). This exe is registered under the `shader-crosscompile`
// ctest preset, so the existing `shader-crosscompile` CI job (issue #128) runs it via `ctest` with NO
// .github/** change. Satisfies AC #1 of #119 minus the WGSL leg (sub-task D).

#include "context/render/material/material_ir.h"
#include "context/render/shadercc/cross_compiler.h"

#include "shadercc_test.h"

#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace context::render::material;
using context::render::shadercc::CrossCompiledStage;
using context::render::shadercc::CrossCompileResult;
using context::render::shadercc::GlslangSpirvCrossCompiler;
using context::render::shadercc::ShaderCompileError;

namespace
{

constexpr std::uint32_t kSpirvMagic = 0x07230203u;

std::optional<std::string> read_file(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

ShaderIr load_corpus(const std::string& name)
{
    const std::string dir = CONTEXT_MATERIAL_CORPUS_DIR;
    const std::optional<std::string> text = read_file(dir + "/" + name);
    CHECK(text.has_value());
    const std::optional<ShaderIr> ir = parse_shader(text.value_or(std::string{}));
    CHECK(ir.has_value());
    return ir.value_or(ShaderIr{});
}

// Every stage cross-compiled to all three C++ targets: real SPIR-V (magic-checked) + non-empty
// HLSL/MSL/GLSL, with a reliable per-target sentinel (SPIRV-Cross GLSL always emits a `#version`
// line; MSL always emits the `metal_stdlib` include / `using namespace metal`).
void assert_stage_ok(const CrossCompiledStage& s)
{
    CHECK(!s.spirv.empty());
    CHECK(!s.spirv.empty() && s.spirv.front() == kSpirvMagic);
    CHECK(!s.hlsl.empty());
    CHECK(!s.msl.empty());
    CHECK(!s.glsl.empty());
    CHECK(s.glsl.find("#version") != std::string::npos);
    CHECK(s.msl.find("metal") != std::string::npos);
}

// The whole authored corpus, EVERY variant, author -> SPIR-V -> HLSL/MSL/GLSL succeeds; the seam
// artifact is well-formed and carries the resolution fields.
void test_corpus_cross_compiles()
{
    const std::vector<std::string> names = {"unlit_color.shader", "lit_pbr.shader",
                                            "postprocess_blit.shader"};
    GlslangSpirvCrossCompiler backend;

    for (const std::string& name : names)
    {
        const ShaderIr ir = load_corpus(name);
        const std::vector<VariantKey> variants = enumerate_variants(ir);
        CHECK(!variants.empty());

        for (const VariantKey& v : variants)
        {
            const CrossCompileResult cc = backend.cross_compile(ir, v);
            CHECK(cc.stages.size() == ir.stages.size());
            for (const CrossCompiledStage& s : cc.stages)
            {
                assert_stage_ok(s);
            }

            const CompiledArtifact art = backend.compile(ir, v);
            CHECK(art.compiler_id == backend.id());
            CHECK(art.ir_hash == ir_content_hash(ir));
            CHECK(art.variant_key == v.canonical());
            CHECK(!art.artifact.empty());
        }
    }
}

// Determinism + stability: the SAME (ir, variant) yields a byte-identical CompiledArtifact and
// byte-identical cross-compiled sources — across repeated calls AND two backend instances. This is
// exactly what makes the R-FILE-010 content-addressed cache sound with the real backend.
void test_determinism()
{
    const ShaderIr ir = load_corpus("lit_pbr.shader");
    const std::vector<VariantKey> variants = enumerate_variants(ir);
    CHECK(!variants.empty());
    const VariantKey v = variants.empty() ? VariantKey{} : variants.front();

    GlslangSpirvCrossCompiler a;
    GlslangSpirvCrossCompiler b;

    const CompiledArtifact a1 = a.compile(ir, v);
    const CompiledArtifact a2 = a.compile(ir, v); // repeated call, same instance
    const CompiledArtifact b1 = b.compile(ir, v); // different instance
    CHECK(a1 == a2);
    CHECK(a1 == b1);

    const CrossCompileResult ca = a.cross_compile(ir, v);
    const CrossCompileResult cb = b.cross_compile(ir, v);
    CHECK(ca.stages.size() == cb.stages.size());
    for (std::size_t i = 0; i < ca.stages.size() && i < cb.stages.size(); ++i)
    {
        CHECK(ca.stages[i].spirv == cb.stages[i].spirv);
        CHECK(ca.stages[i].hlsl == cb.stages[i].hlsl);
        CHECK(ca.stages[i].msl == cb.stages[i].msl);
        CHECK(ca.stages[i].glsl == cb.stages[i].glsl);
    }
}

// Keyword `#define` injection genuinely reaches the SPIR-V: the all-off variant and the all-on variant
// of a multi-axis shader produce DIFFERENT artifacts (a stronger proof than the fake backend's, which
// only folds the variant key into a stub hash — here the compiled bytes actually differ).
void test_variants_differ()
{
    const ShaderIr ir = load_corpus("unlit_color.shader");
    const std::vector<VariantKey> variants = enumerate_variants(ir);
    CHECK(variants.size() == 4);
    if (variants.size() < 2)
    {
        return;
    }

    GlslangSpirvCrossCompiler backend;
    const CompiledArtifact first = backend.compile(ir, variants.front());
    const CompiledArtifact last = backend.compile(ir, variants.back());
    CHECK(first.variant_key != last.variant_key);
    CHECK(first.artifact != last.artifact);
}

// Edge: a no-keyword shader compiles to its single default variant; and a COMPUTE stage exercises the
// EShLangCompute mapping + cross-compiles to all three targets (the corpus has only vertex/fragment).
void test_programmatic_shaders()
{
    GlslangSpirvCrossCompiler backend;

    ShaderIr vs_only;
    vs_only.name = "prog_unlit";
    ShaderStage vs;
    vs.kind = ShaderStageKind::Vertex;
    vs.entry_point = "vs";
    vs.source = "#version 450\n"
                "layout(location = 0) in vec3 p;\n"
                "void vs() { gl_Position = vec4(p, 1.0); }\n";
    vs_only.stages.push_back(vs);

    const std::vector<VariantKey> variants = enumerate_variants(vs_only);
    CHECK(variants.size() == 1);
    const CrossCompileResult cc = backend.cross_compile(vs_only, variants.front());
    CHECK(cc.stages.size() == 1);
    if (!cc.stages.empty())
    {
        assert_stage_ok(cc.stages.front());
    }

    ShaderIr cs_ir;
    cs_ir.name = "prog_compute";
    ShaderStage cs;
    cs.kind = ShaderStageKind::Compute;
    cs.entry_point = "cs";
    cs.source = "#version 450\n"
                "layout(local_size_x = 1) in;\n"
                "void cs() {}\n";
    cs_ir.stages.push_back(cs);

    const CrossCompileResult ccc = backend.cross_compile(cs_ir, VariantKey{});
    CHECK(ccc.stages.size() == 1);
    if (!ccc.stages.empty())
    {
        CHECK(!ccc.stages.front().spirv.empty());
        CHECK(!ccc.stages.front().spirv.empty() && ccc.stages.front().spirv.front() == kSpirvMagic);
        CHECK(!ccc.stages.front().hlsl.empty());
        CHECK(!ccc.stages.front().msl.empty());
        CHECK(!ccc.stages.front().glsl.empty());
    }
}

// Failure path: malformed GLSL throws ShaderCompileError, not a crash or a silent empty artifact
// (R-QA-013 failure coverage).
void test_compile_failure_throws()
{
    ShaderIr bad;
    bad.name = "broken";
    ShaderStage s;
    s.kind = ShaderStageKind::Vertex;
    s.entry_point = "vs";
    s.source = "#version 450\nvoid vs() { this is not valid glsl @@@ }\n";
    bad.stages.push_back(s);

    GlslangSpirvCrossCompiler backend;
    bool threw = false;
    try
    {
        (void)backend.cross_compile(bad, VariantKey{});
    }
    catch (const ShaderCompileError&)
    {
        threw = true;
    }
    CHECK(threw);
}

} // namespace

int main()
{
    test_corpus_cross_compiles();
    test_determinism();
    test_variants_differ();
    test_programmatic_shaders();
    test_compile_failure_throws();
    SHADERCC_TEST_MAIN_END();
}
