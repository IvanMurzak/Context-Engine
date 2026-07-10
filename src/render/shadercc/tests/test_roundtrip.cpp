// Round-trip proof for the real glslang + SPIRV-Cross + Tint backend (R-REND-005; sub-tasks C+D of
// #119, issues #130/#133): author -> SPIR-V -> {HLSL, MSL, GLSL, WGSL} over the REAL authored corpus
// (the same src/render/material/corpus/*.shader the fake backend exercises), plus
// determinism/stability + variant-sensitivity + a compute-stage path + malformed-input failure paths
// (R-QA-013: happy/edge/failure ship in the same PR). Every emitted WGSL module is validated TWICE:
// with the chosen tool's own validator (tint — validate_wgsl(), AC1 of #133) AND with the pinned
// naga, because the native path consumes WGSL through naga inside the in-tree wgpu-native
// (spikes/webgpu/FINDINGS.md — the emitted WGSL must satisfy BOTH consumers). This exe is registered
// under the `shader-crosscompile` ctest preset, so the existing `shader-crosscompile` CI job (issue
// #128) runs it via `ctest` with no new job. Satisfies AC #1 of #119 in full.

#include "context/render/material/material_ir.h"
#include "context/render/shadercc/cross_compiler.h"

#include "shadercc_test.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h> // WIFEXITED/WEXITSTATUS for decoding std::system()'s wait status
#endif

using namespace context::render::material;
using context::render::shadercc::CrossCompiledStage;
using context::render::shadercc::CrossCompileResult;
using context::render::shadercc::GlslangSpirvCrossCompiler;
using context::render::shadercc::ShaderCompileError;

namespace
{

constexpr std::uint32_t kSpirvMagic = 0x07230203u;

// Cross-validate one WGSL module under the pinned naga (the NATIVE consumer's WGSL frontend —
// `naga <file.wgsl>` parses + validates, exit 0 on success). The backend's own validate_wgsl()
// covers the chosen tool's validator; this guards the OTHER consumer. Test-local subprocess
// plumbing — the backend deliberately does not expose its internals.
bool naga_accepts_wgsl(const std::string& wgsl)
{
    static int counter = 0;
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("ctx-shadercc-xval-" + std::to_string(++counter) + ".wgsl");
    {
        std::ofstream out(path, std::ios::binary);
        out << wgsl;
        if (!out)
        {
            return false;
        }
    }

#ifdef _WIN32
    const char* null_dev = "NUL";
#else
    const char* null_dev = "/dev/null";
#endif
    // stdout (naga's per-file "Validation successful") goes to the null device; stderr stays on the
    // ctest output so a rejection is diagnosable.
    std::string command = std::string("\"") + CONTEXT_SHADERCC_NAGA_EXECUTABLE + "\" \"" +
                          path.string() + "\" > " + null_dev;
#ifdef _WIN32
    // cmd.exe strips the OUTERMOST quote pair from `cmd /c <line>`; wrap in one extra pair.
    command = "\"" + command + "\"";
    const int rc = std::system(command.c_str());
#else
    const int status = std::system(command.c_str());
    const int rc = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
#endif

    std::error_code ec;
    std::filesystem::remove(path, ec);
    return rc == 0;
}

// The chosen tool's validator (AC1): validate_wgsl() throws on an invalid module.
bool tint_accepts_wgsl(const std::string& wgsl)
{
    try
    {
        context::render::shadercc::validate_wgsl(wgsl);
        return true;
    }
    catch (const ShaderCompileError&)
    {
        return false;
    }
}

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

// Every stage cross-compiled to all four targets: real SPIR-V (magic-checked) + non-empty
// HLSL/MSL/GLSL/WGSL, with a reliable per-target sentinel (SPIRV-Cross GLSL always emits a
// `#version` line; MSL always emits the `metal_stdlib` include / `using namespace metal`; tint WGSL
// always carries a stage attribute). The WGSL leg is additionally VALIDATED under both consumers:
// tint's own validator (AC1 of #133) and the pinned native-consumer naga.
void assert_stage_ok(const CrossCompiledStage& s)
{
    CHECK(!s.spirv.empty());
    CHECK(!s.spirv.empty() && s.spirv.front() == kSpirvMagic);
    CHECK(!s.hlsl.empty());
    CHECK(!s.msl.empty());
    CHECK(!s.glsl.empty());
    CHECK(s.glsl.find("#version") != std::string::npos);
    CHECK(s.msl.find("metal") != std::string::npos);
    CHECK(!s.wgsl.empty());
    CHECK(s.wgsl.find("@") != std::string::npos); // @vertex/@fragment/@compute stage attribute
    CHECK(tint_accepts_wgsl(s.wgsl));
    CHECK(naga_accepts_wgsl(s.wgsl));
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
        CHECK(ca.stages[i].wgsl == cb.stages[i].wgsl);
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

    // The WGSL leg is variant-sensitive too: the keyword `#define`s reach the emitted WGSL, not
    // just the artifact hash — at least one stage's WGSL must differ between all-off and all-on
    // (unlit_color's FOG axis rewrites the fragment; INSTANCED rewrites the vertex inputs).
    const CrossCompileResult cc_first = backend.cross_compile(ir, variants.front());
    const CrossCompileResult cc_last = backend.cross_compile(ir, variants.back());
    CHECK(cc_first.stages.size() == cc_last.stages.size());
    bool any_wgsl_differs = false;
    for (std::size_t i = 0; i < cc_first.stages.size() && i < cc_last.stages.size(); ++i)
    {
        any_wgsl_differs = any_wgsl_differs || (cc_first.stages[i].wgsl != cc_last.stages[i].wgsl);
    }
    CHECK(any_wgsl_differs);
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
        CHECK(!ccc.stages.front().wgsl.empty());
        CHECK(ccc.stages.front().wgsl.find("@compute") != std::string::npos);
    }
}

// Failure path (WGSL leg, R-QA-013): garbage SPIR-V words must make the tint leg throw
// ShaderCompileError (carrying the tool's diagnostics) — never a crash or a silent empty result.
void test_wgsl_malformed_spirv_throws()
{
    const std::vector<std::uint32_t> garbage = {0xDEADBEEFu, 0x01020304u, 0x0BADF00Du};
    bool threw = false;
    try
    {
        (void)context::render::shadercc::spirv_to_wgsl(garbage);
    }
    catch (const ShaderCompileError&)
    {
        threw = true;
    }
    CHECK(threw);
}

// Failure path (WGSL validator, R-QA-013): the chosen tool's validator must REJECT a malformed
// module — proving the AC1 validity checks above can actually fail.
void test_validate_wgsl_rejects_malformed()
{
    CHECK(!tint_accepts_wgsl("this is not wgsl @@@"));
    CHECK(!naga_accepts_wgsl("this is not wgsl @@@"));
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
    test_wgsl_malformed_spirv_throws();
    test_validate_wgsl_rejects_malformed();
    SHADERCC_TEST_MAIN_END();
}
