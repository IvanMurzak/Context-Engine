// The real glslang + SPIRV-Cross + Tint backend behind the material/ IShaderCompiler seam
// (R-REND-005; sub-tasks C+D of #119, issues #130/#133). See cross_compiler.h for the design + the
// keyword-injection convention; docs/wgsl-tool-decision.md for the measured Tint-vs-Naga ruling.
// Built ONLY under CONTEXT_BUILD_SHADER_CROSSCOMPILE (the shadercc/ dir is gated), so this TU never
// compiles on the default dev gate — its authoritative signal is the `shader-crosscompile` CI job
// (glslang/SPIRV-Cross from the vcpkg manifest feature; tint staged by tools/fetch_tint.py).

#include "context/render/shadercc/cross_compiler.h"

#include "context/common/subprocess.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <spirv_cross/spirv_glsl.hpp>
#include <spirv_cross/spirv_hlsl.hpp>
#include <spirv_cross/spirv_msl.hpp>

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The pinned WGSL tool, baked in by src/render/shadercc/CMakeLists.txt: the tint executable path
// (found/staged at configure time) and the human-readable pin string (the Dawn release tag from
// tools/tint-toolchain.json) that is folded into the backend's default id() — and thus into every
// R-FILE-010 cache key — plus every artifact's `wgsltool=` line. tint has no --version self-report,
// so the pin is enforced at acquisition (tools/fetch_tint.py verifies the pinned commit, fail-closed)
// and recorded here for cache-key hygiene.
#ifndef CONTEXT_SHADERCC_WGSL_TOOL_EXECUTABLE
#error "CONTEXT_SHADERCC_WGSL_TOOL_EXECUTABLE must be defined (see src/render/shadercc/CMakeLists.txt)"
#endif
#ifndef CONTEXT_SHADERCC_WGSL_TOOL_PIN
#error "CONTEXT_SHADERCC_WGSL_TOOL_PIN must be defined (see src/render/shadercc/CMakeLists.txt)"
#endif

namespace context::render::shadercc
{
namespace
{

namespace subprocess = context::common::subprocess;

// glslang wants exactly one InitializeProcess()/FinalizeProcess() bracket per process. A function-
// local static guard makes the first backend call initialize the process and normal program exit
// finalize it — sound for the single-threaded, deterministic derivation graph this backend serves
// (C++11 guarantees thread-safe function-local static init).
struct GlslangProcessGuard
{
    GlslangProcessGuard()
    {
        glslang::InitializeProcess();
    }
    ~GlslangProcessGuard()
    {
        glslang::FinalizeProcess();
    }
    GlslangProcessGuard(const GlslangProcessGuard&) = delete;
    GlslangProcessGuard& operator=(const GlslangProcessGuard&) = delete;
};

void ensure_glslang_initialized()
{
    static GlslangProcessGuard guard;
    (void)guard;
}

EShLanguage to_glslang_stage(ShaderStageKind kind)
{
    switch (kind)
    {
    case ShaderStageKind::Vertex:
        return EShLangVertex;
    case ShaderStageKind::Fragment:
        return EShLangFragment;
    case ShaderStageKind::Compute:
        return EShLangCompute;
    }
    // The switch is exhaustive over today's ShaderStageKind and has no `default`, so -Wswitch already
    // fails a future enumerator at compile time on the GCC/Clang legs. This throw is the belt to that
    // suspenders: it keeps -Werror quiet here (a non-returning terminator satisfies "control reaches
    // end of non-void function") AND, per material_ir.h ("the real backend adds more as needed"), fails
    // LOUDLY at runtime rather than silently mis-lowering a new stage to Vertex on a leg where the
    // missing-case warning is off by default (MSVC C4062).
    throw ShaderCompileError("to_glslang_stage: unhandled ShaderStageKind");
}

std::string to_lower(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

bool is_boolean_token(const std::string& lowered)
{
    return lowered == "off" || lowered == "on" || lowered == "0" || lowered == "1" ||
           lowered == "false" || lowered == "true" || lowered == "no" || lowered == "yes" ||
           lowered == "disable" || lowered == "enable" || lowered == "disabled" ||
           lowered == "enabled";
}

bool is_enabled_token(const std::string& lowered)
{
    return lowered == "on" || lowered == "1" || lowered == "true" || lowered == "yes" ||
           lowered == "enable" || lowered == "enabled";
}

// glslang: lower assembled GLSL to SPIR-V for `stage`. Throws ShaderCompileError (with the info log)
// on any parse/link failure. Mirrors the proven smoke.cpp setup (Vulkan 1.0 / SPIR-V 1.0 env).
std::vector<std::uint32_t> lower_to_spirv(EShLanguage stage, const std::string& source,
                                          const std::string& shader_name,
                                          const std::string& entry_point)
{
    glslang::TShader shader(stage);
    const char* strings[] = {source.c_str()};
    shader.setStrings(strings, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

    const auto messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
    // GetDefaultResources() is in the GLOBAL namespace (glslang/Public/ResourceLimits.h), provided by
    // the glslang::glslang-default-resource-limits target.
    if (!shader.parse(GetDefaultResources(), 100, false, messages))
    {
        throw ShaderCompileError("glslang parse failed for shader '" + shader_name + "' (entry '" +
                                 entry_point + "'):\n" + shader.getInfoLog());
    }

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages))
    {
        throw ShaderCompileError("glslang link failed for shader '" + shader_name + "':\n" +
                                 program.getInfoLog());
    }

    std::vector<std::uint32_t> spirv;
    glslang::GlslangToSpv(*program.getIntermediate(stage), spirv);
    if (spirv.empty())
    {
        throw ShaderCompileError("glslang produced empty SPIR-V for shader '" + shader_name + "'");
    }
    return spirv;
}

std::string spirv_to_hlsl(const std::vector<std::uint32_t>& spirv)
{
    spirv_cross::CompilerHLSL compiler(spirv.data(), spirv.size());
    spirv_cross::CompilerGLSL::Options common;
    compiler.set_common_options(common);
    spirv_cross::CompilerHLSL::Options hlsl_options;
    hlsl_options.shader_model = 50;
    compiler.set_hlsl_options(hlsl_options);
    return compiler.compile();
}

std::string spirv_to_msl(const std::vector<std::uint32_t>& spirv)
{
    spirv_cross::CompilerMSL compiler(spirv.data(), spirv.size());
    spirv_cross::CompilerMSL::Options msl_options;
    compiler.set_msl_options(msl_options);
    return compiler.compile();
}

std::string spirv_to_glsl(const std::vector<std::uint32_t>& spirv)
{
    spirv_cross::CompilerGLSL compiler(spirv.data(), spirv.size());
    spirv_cross::CompilerGLSL::Options options;
    options.version = 450;
    options.es = false;
    compiler.set_common_options(options);
    return compiler.compile();
}

// ---- the WGSL leg: pinned-tint subprocess plumbing (sub-task D, issue #133) ----------------------
// The std::system runner, scratch-file RAII (make_scratch_path/ScratchFile), quoting/metacharacter
// policy, exit-code mapping, and file read/write all come from the shared context::common::subprocess
// module (issue #146). This backend was that module's HARDENED REFERENCE (its quoting policy is the
// reconciled-strictest), so migrating to it leaves compile()'s output + deterministic id
// byte-identical; only the backend-specific error mapping (ShaderCompileError) stays local.

// Quote one tint argument via the shared fail-closed policy, re-throwing its MetacharacterError as a
// ShaderCompileError to preserve this backend's error contract. Scratch paths are ours and the tool
// path comes from CMake — none may legitimately contain metacharacters, so a throw is a real defect.
std::string quote_argument(const std::string& arg)
{
    try
    {
        return subprocess::quote_argument(arg);
    }
    catch (const subprocess::MetacharacterError& e)
    {
        throw ShaderCompileError(std::string("wgsl leg: ") + e.what());
    }
}

// Write scratch bytes, throwing ShaderCompileError on failure (an empty input still creates the empty
// file for the tool to reject loudly). Preserves the pre-consolidation error contract.
void write_scratch(const std::filesystem::path& path, const void* data, std::size_t bytes)
{
    if (!subprocess::write_file(path, data, bytes))
    {
        throw ShaderCompileError("wgsl leg: cannot write scratch file " + path.string());
    }
}

// Invoke the pinned tint on `input`, writing WGSL to `output` (the validate path writes to the null
// device). Returns tint's stderr text on failure via `diagnostics`.
int run_tint(const std::filesystem::path& input, const std::string& output,
             std::string& diagnostics)
{
    const subprocess::ScratchFile err(subprocess::make_scratch_path("ctx-shadercc", ".stderr"));
    const std::string command = quote_argument(CONTEXT_SHADERCC_WGSL_TOOL_EXECUTABLE) +
                                " --format wgsl -o " + quote_argument(output) + " " +
                                quote_argument(input.string()) + " 2> " +
                                quote_argument(err.path().string());
    const int rc = subprocess::run_command(command);
    diagnostics = (rc == 0) ? std::string{} : subprocess::read_file(err.path());
    return rc;
}

const char* null_device()
{
#ifdef _WIN32
    return "NUL";
#else
    return "/dev/null";
#endif
}

} // namespace

std::string assemble_stage_source(const ShaderStage& stage, const ShaderIr& ir,
                                  const VariantKey& variant)
{
    std::ostringstream defines;
    // Value token -> the ordinal it was `#define`d to. Value tokens are GLOBAL in the assembled source
    // (the `#if KW == token` idiom compares against a bare `token`), so a token spelling can carry only
    // ONE ordinal for the whole stage. Two keywords may legitimately SHARE a token at the SAME ordinal
    // (deduped below); but two keywords declaring the same token at DIFFERENT positions cannot both be
    // honoured — that is an authoring collision we fail LOUDLY on rather than silently mis-numbering.
    std::map<std::string, std::size_t> defined_tokens;

    for (const auto& [name, value] : variant.defines)
    {
        // Recover the keyword's full declared value set (needed to tell a boolean axis from a
        // multi-value one + to number multi-value tokens). Missing => treat as a lone value.
        const std::vector<std::string>* values = nullptr;
        for (const auto& kw : ir.keywords)
        {
            if (kw.name == name)
            {
                values = &kw.values;
                break;
            }
        }

        const std::string selected_lower = to_lower(value);
        bool boolean = false;
        if (values != nullptr && values->size() == 2)
        {
            boolean = is_boolean_token(to_lower((*values)[0])) && is_boolean_token(to_lower((*values)[1]));
        }

        if (boolean)
        {
            // `#ifdef KW` idiom: define (to 1) only when enabled; leave undefined when disabled.
            if (is_enabled_token(selected_lower))
            {
                defines << "#define " << name << " 1\n";
            }
        }
        else
        {
            // `#if KW == token` idiom: number every value token, then bind the keyword to its selected
            // token, so the comparison is a well-formed constant expression (no undefined identifiers).
            if (values != nullptr)
            {
                for (std::size_t i = 0; i < values->size(); ++i)
                {
                    const std::string& token = (*values)[i];
                    const auto [it, inserted] = defined_tokens.emplace(token, i);
                    if (inserted)
                    {
                        defines << "#define " << token << " " << i << "\n";
                    }
                    else if (it->second != i)
                    {
                        // Another keyword already `#define`d this token to a different ordinal; a single
                        // global macro cannot satisfy both, so `#if KW == token` would silently resolve
                        // against the wrong keyword's numbering. Fail loudly instead of mis-numbering.
                        throw ShaderCompileError("assemble_stage_source: value token '" + token +
                                                 "' is declared at conflicting ordinals across keywords "
                                                 "of shader '" + ir.name + "'");
                    }
                }
            }
            defines << "#define " << name << " " << value << "\n";
        }
    }

    // Entry-point trampoline: the GLSL frontend compiles `main`; a corpus stage authors e.g.
    // `void vs_main()`, so `#define <entry> main` renames it with zero reliance on glslang's
    // source-entry-point remapping. Skipped when the entry already is `main`.
    std::string entry_define;
    if (!stage.entry_point.empty() && stage.entry_point != "main")
    {
        entry_define = "#define " + stage.entry_point + " main\n";
    }

    // Keep the authored `#version` directive as the first line; inject right after it.
    const std::string& src = stage.source;
    const std::size_t vpos = src.find("#version");
    if (vpos != std::string::npos)
    {
        const std::size_t eol = src.find('\n', vpos);
        const std::size_t insert_pos = (eol == std::string::npos) ? src.size() : eol + 1;
        std::ostringstream out;
        out << src.substr(0, insert_pos) << defines.str() << entry_define << src.substr(insert_pos);
        return out.str();
    }

    // No authored #version (defensive; the corpus always has one): synthesize a core 450 version.
    std::ostringstream out;
    out << "#version 450\n" << defines.str() << entry_define << src;
    return out.str();
}

std::string spirv_to_wgsl(const std::vector<std::uint32_t>& spirv)
{
    const subprocess::ScratchFile in(subprocess::make_scratch_path("ctx-shadercc", ".spv"));
    const subprocess::ScratchFile out(subprocess::make_scratch_path("ctx-shadercc", ".wgsl"));
    write_scratch(in.path(), spirv.data(), spirv.size() * sizeof(std::uint32_t));

    std::string diagnostics;
    const int rc = run_tint(in.path(), out.path().string(), diagnostics);
    if (rc != 0)
    {
        throw ShaderCompileError("tint SPIR-V->WGSL translation failed (exit " +
                                 std::to_string(rc) + "):\n" + diagnostics);
    }
    std::string wgsl = subprocess::read_file(out.path());
    if (wgsl.empty())
    {
        throw ShaderCompileError("tint produced an empty WGSL translation");
    }
    return wgsl;
}

void validate_wgsl(const std::string& wgsl)
{
    const subprocess::ScratchFile in(subprocess::make_scratch_path("ctx-shadercc", ".wgsl"));
    write_scratch(in.path(), wgsl.data(), wgsl.size());

    // tint has no validate-only switch; parse+resolve+re-emit to the null device is the working
    // equivalent (probed against the pinned Dawn tag — see docs/wgsl-tool-decision.md).
    std::string diagnostics;
    const int rc = run_tint(in.path(), null_device(), diagnostics);
    if (rc != 0)
    {
        throw ShaderCompileError("tint WGSL validation failed (exit " + std::to_string(rc) +
                                 "):\n" + diagnostics);
    }
}

std::string GlslangSpirvCrossCompiler::default_id()
{
    // `glslang-spirvcross-v2` = the artifact SHAPE tag (v2 grew the WGSL leg); the appended
    // CMake-baked tool pin makes a tint bump change the id — and with it every R-FILE-010 cache key
    // (ShaderCompileNode hashes (ir | variant | id)) — with no manual version bump to forget. The
    // artifact's `wgsltool=` line echoes the same pin for inspection; only the id is keyed.
    return "glslang-spirvcross-v2+" CONTEXT_SHADERCC_WGSL_TOOL_PIN;
}

GlslangSpirvCrossCompiler::GlslangSpirvCrossCompiler(std::string id) : id_(std::move(id))
{
}

std::string GlslangSpirvCrossCompiler::id() const
{
    return id_;
}

CrossCompileResult GlslangSpirvCrossCompiler::cross_compile(const ShaderIr& ir,
                                                            const VariantKey& variant) const
{
    ensure_glslang_initialized();

    CrossCompileResult result;
    result.stages.reserve(ir.stages.size());

    for (const ShaderStage& stage : ir.stages)
    {
        const std::string source = assemble_stage_source(stage, ir, variant);
        const EShLanguage lang = to_glslang_stage(stage.kind);

        CrossCompiledStage out;
        out.kind = stage.kind;
        out.entry_point = stage.entry_point;
        out.spirv = lower_to_spirv(lang, source, ir.name, stage.entry_point);

        try
        {
            out.hlsl = spirv_to_hlsl(out.spirv);
            out.msl = spirv_to_msl(out.spirv);
            out.glsl = spirv_to_glsl(out.spirv);
            out.wgsl = spirv_to_wgsl(out.spirv);
        }
        catch (const std::exception& e)
        {
            // SPIRV-Cross throws spirv_cross::CompilerError (a std::runtime_error) and the WGSL leg
            // throws ShaderCompileError on failure; wrap either with the shader/stage context so the
            // ctest failure is self-explanatory.
            throw ShaderCompileError("cross-compile failed for shader '" + ir.name + "' (entry '" +
                                     stage.entry_point + "'): " + e.what());
        }

        if (out.hlsl.empty() || out.msl.empty() || out.glsl.empty() || out.wgsl.empty())
        {
            throw ShaderCompileError("cross-compile produced an empty translation for shader '" +
                                     ir.name + "' (entry '" + stage.entry_point + "')");
        }

        result.stages.push_back(std::move(out));
    }

    return result;
}

CompiledArtifact GlslangSpirvCrossCompiler::compile(const ShaderIr& ir, const VariantKey& variant) const
{
    const CrossCompileResult cc = cross_compile(ir, variant);

    CompiledArtifact art;
    art.compiler_id = id_;
    art.ir_hash = context::render::material::ir_content_hash(ir);
    art.variant_key = variant.canonical();

    // A deterministic, human-inspectable manifest of the REAL outputs: per stage, the content hash +
    // size of the SPIR-V and of each cross-compiled target. Pure function of (ir, variant, id), so the
    // R-FILE-010 content-addressed cache (ShaderCompileNode) stays sound with the real backend. Hashes
    // rather than the full sources keep the artifact compact while still proving all four targets were
    // produced and pinning them against drift. The `wgsltool=` line records the pinned WGSL tool in
    // the artifact bytes for inspection; cache soundness across tool bumps comes from the pin living
    // in the default id() (a cache-key input — see default_id()), not from this line.
    std::ostringstream os;
    os << "SPIRV-CROSS-ARTIFACT v2\n";
    os << "compiler=" << id_ << "\n";
    os << "wgsltool=" << CONTEXT_SHADERCC_WGSL_TOOL_PIN << "\n";
    os << "shader=" << ir.name << "\n";
    os << "ir=" << art.ir_hash << "\n";
    os << "variant=" << (art.variant_key.empty() ? "<default>" : art.variant_key) << "\n";
    for (const CrossCompiledStage& s : cc.stages)
    {
        const std::string_view spirv_bytes(reinterpret_cast<const char*>(s.spirv.data()),
                                           s.spirv.size() * sizeof(std::uint32_t));
        os << "stage=" << context::render::material::to_string(s.kind) << ":" << s.entry_point << "\n";
        os << "  spirv=" << context::render::material::content_hash_hex(spirv_bytes)
           << " words=" << s.spirv.size() << "\n";
        os << "  hlsl=" << context::render::material::content_hash_hex(s.hlsl)
           << " bytes=" << s.hlsl.size() << "\n";
        os << "  msl=" << context::render::material::content_hash_hex(s.msl)
           << " bytes=" << s.msl.size() << "\n";
        os << "  glsl=" << context::render::material::content_hash_hex(s.glsl)
           << " bytes=" << s.glsl.size() << "\n";
        os << "  wgsl=" << context::render::material::content_hash_hex(s.wgsl)
           << " bytes=" << s.wgsl.size() << "\n";
    }
    art.artifact = os.str();
    return art;
}

} // namespace context::render::shadercc
