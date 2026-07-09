// The REAL shader cross-compile backend (R-REND-005; sub-task C of #119, issue #130): a concrete
// IShaderCompiler that lowers each authored stage to SPIR-V via glslang, then cross-compiles the
// SPIR-V to HLSL, MSL, and GLSL via SPIRV-Cross. It lives ENTIRELY behind
// CONTEXT_BUILD_SHADER_CROSSCOMPILE (this directory is added to the build only under that flag), so
// the backend-free src/render/material/ seam and the default local dev gate never pull the native
// toolchain — the fake/reference backend (context::render::material::FakeShaderCompiler) stays the
// default-OFF/local path, untouched.
//
// Determinism: assembling the stage source, glslang GLSL->SPIR-V, and SPIRV-Cross SPIR-V->{HLSL,MSL,
// GLSL} are all pure functions of their inputs, so compile() is a pure deterministic function of
// (ir, variant, id()). That keeps the R-FILE-010 content-addressed shader cache (the derivation
// graph's ShaderCompileNode) sound with the real backend dropped in behind the seam — no caller
// changes, exactly as the seam was designed for (see material/shader_compiler.h).
//
// The WGSL leg + the Tint-vs-Naga tool decision are a SEPARATE later sub-task (D) and are deliberately
// NOT implemented here.

#pragma once

#include "context/render/material/material_ir.h"
#include "context/render/material/shader_compiler.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace context::render::shadercc
{

using context::render::material::CompiledArtifact;
using context::render::material::IShaderCompiler;
using context::render::material::ShaderIr;
using context::render::material::ShaderStage;
using context::render::material::ShaderStageKind;
using context::render::material::VariantKey;

// The cross-compiled forms of ONE shader stage: the glslang SPIR-V module and its SPIRV-Cross
// translations. Every string / vector is non-empty on success.
struct CrossCompiledStage
{
    ShaderStageKind kind = ShaderStageKind::Vertex;
    std::string entry_point;          // the authored entry point (renamed to `main` in the SPIR-V)
    std::vector<std::uint32_t> spirv; // glslang SPIR-V words (first word is the 0x07230203 magic)
    std::string hlsl;                 // SPIRV-Cross -> HLSL (Shader Model 5.0)
    std::string msl;                  // SPIRV-Cross -> Metal Shading Language
    std::string glsl;                 // SPIRV-Cross -> GLSL 450
};

// The full author -> SPIR-V -> {HLSL,MSL,GLSL} result for one (ir, variant): one entry per authored
// stage, in authored order.
struct CrossCompileResult
{
    std::vector<CrossCompiledStage> stages;
};

// Thrown when glslang fails to parse/link a stage or SPIRV-Cross fails to translate the module. The
// message carries the backend log so an authoring/corpus error is diagnosable straight from the ctest
// output.
class ShaderCompileError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// The real backend. Aside from glslang's once-per-process init (owned by an internal RAII guard), it
// is stateless, so one instance is reusable and every call is independent + deterministic. Implements
// the material/ IShaderCompiler seam WITHOUT modifying it.
class GlslangSpirvCrossCompiler final : public IShaderCompiler
{
public:
    explicit GlslangSpirvCrossCompiler(std::string id = "glslang-spirvcross-v1");

    [[nodiscard]] std::string id() const override;

    // IShaderCompiler seam: compile every stage of `ir` for `variant`, then fold a DETERMINISTIC,
    // human-inspectable manifest of the SPIR-V + HLSL/MSL/GLSL outputs (per-stage content hashes +
    // word/byte counts) into CompiledArtifact::artifact. Throws ShaderCompileError on any failure.
    [[nodiscard]] CompiledArtifact compile(const ShaderIr& ir,
                                           const VariantKey& variant) const override;

    // The richer entry the round-trip tests use: returns the actual SPIR-V + cross-compiled sources so
    // callers can assert each target is non-empty / inspect them. Throws ShaderCompileError on failure.
    [[nodiscard]] CrossCompileResult cross_compile(const ShaderIr& ir,
                                                   const VariantKey& variant) const;

private:
    std::string id_;
};

// Assemble the preprocessed GLSL fed to glslang for one stage under `variant`. The stage's own
// `#version` directive stays the first line (GLSL requires it), then the variant's keyword `#define`s
// are injected, then an entry-point trampoline (`#define <entry> main`) so a non-`main` authored entry
// compiles under the GLSL frontend, then the remaining authored body.
//
// Keyword injection convention (deterministic; `ir` supplies each keyword's full value set):
//   * A BOOLEAN keyword (exactly two values, both drawn from {off,on,0,1,false,true,no,yes,disable,
//     enable,disabled,enabled}) matches the `#ifdef KW` authoring idiom: it is `#define`d (to `1`)
//     ONLY when its selected value is enabled, and left UNDEFINED when disabled.
//   * A NON-BOOLEAN keyword (e.g. QUALITY {low,med,high}) matches the `#if KW == token` idiom: each of
//     its value tokens is `#define`d to a distinct ordinal and the keyword to its selected token, so
//     `#if KW == token` resolves to a well-formed constant comparison with no undefined identifiers.
// Pure function — no native toolchain needed, so its behaviour is white-box testable.
[[nodiscard]] std::string assemble_stage_source(const ShaderStage& stage, const ShaderIr& ir,
                                                const VariantKey& variant);

} // namespace context::render::shadercc
