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

// White-box unit test of the PURE assemble_stage_source() keyword-injection + entry-trampoline logic
// (the header documents it as "white-box testable — no native toolchain needed"). Realizes that
// documented benefit directly, rather than only exercising it indirectly through cross_compile(): it
// pins the boolean `#ifdef` idiom, the multi-value `#if KW == token` idiom (value tokens numbered),
// the entry-point trampoline, and the `#version`-stays-first rule — all deterministic string output.
void test_assemble_stage_source()
{
    using context::render::shadercc::assemble_stage_source;

    ShaderIr ir;
    ir.name = "inject_probe";
    ir.keywords = {
        {"FOG", {"off", "on"}},              // boolean axis -> `#ifdef KW` idiom
        {"QUALITY", {"low", "med", "high"}}, // multi-value axis -> `#if KW == token` idiom
    };

    ShaderStage stage;
    stage.kind = ShaderStageKind::Vertex;
    stage.entry_point = "vs_main";
    stage.source = "#version 450\nvoid vs_main() {}\n";

    // Boolean ENABLED + a selected multi-value token.
    VariantKey on;
    on.defines = {{"FOG", "on"}, {"QUALITY", "high"}};
    const std::string a = assemble_stage_source(stage, ir, on);
    CHECK(a.rfind("#version 450", 0) == 0);                      // #version stays the first line
    CHECK(a.find("#define FOG 1") != std::string::npos);         // boolean enabled -> defined to 1
    CHECK(a.find("#define low 0") != std::string::npos);         // value tokens numbered by ordinal
    CHECK(a.find("#define med 1") != std::string::npos);
    CHECK(a.find("#define high 2") != std::string::npos);
    CHECK(a.find("#define QUALITY high") != std::string::npos);  // keyword bound to its selected token
    CHECK(a.find("#define vs_main main") != std::string::npos);  // entry-point trampoline

    // Boolean DISABLED must be LEFT UNDEFINED (the `#ifdef KW` idiom).
    VariantKey off;
    off.defines = {{"FOG", "off"}, {"QUALITY", "low"}};
    const std::string b = assemble_stage_source(stage, ir, off);
    CHECK(b.find("#define FOG") == std::string::npos);

    // A `main` entry point needs no trampoline (no self-referential `#define main main`).
    ShaderStage main_stage;
    main_stage.kind = ShaderStageKind::Fragment;
    main_stage.entry_point = "main";
    main_stage.source = "#version 450\nvoid main() {}\n";
    const std::string c = assemble_stage_source(main_stage, ir, VariantKey{});
    CHECK(c.find("main main") == std::string::npos);
}

// White-box failure path for the token-numbering: value tokens are GLOBAL in the assembled source, so
// two keywords declaring the SAME token spelling at DIFFERENT ordinals cannot both be honoured by a
// single `#define`. assemble_stage_source() must throw ShaderCompileError on that authoring collision
// rather than silently mis-number one keyword against the other's ordinal.
void test_assemble_stage_source_token_collision_throws()
{
    using context::render::shadercc::assemble_stage_source;

    ShaderIr ir;
    ir.name = "collision_probe";
    ir.keywords = {
        {"QUALITY", {"low", "high"}}, // low=0, high=1
        {"DETAIL", {"high", "low"}},  // high=0, low=1  -> both tokens collide at conflicting ordinals
    };

    ShaderStage stage;
    stage.kind = ShaderStageKind::Vertex;
    stage.entry_point = "vs_main";
    stage.source = "#version 450\nvoid vs_main() {}\n";

    VariantKey key;
    key.defines = {{"QUALITY", "low"}, {"DETAIL", "high"}};

    bool threw = false;
    try
    {
        (void)assemble_stage_source(stage, ir, key);
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
    test_assemble_stage_source();
    test_assemble_stage_source_token_collision_throws();
    test_compile_failure_throws();
    SHADERCC_TEST_MAIN_END();
}
