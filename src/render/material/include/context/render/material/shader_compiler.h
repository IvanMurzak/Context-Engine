// The shader-compiler seam (R-REND-005) + a deterministic, GPU-free fake/reference backend. Every
// consumer — including the R-FILE-010 shader-compile node (ShaderCompileNode in src/editor/derivation/,
// issue #126) — depends ONLY on the IShaderCompiler interface, so the real glslang/SPIRV-Cross backend
// (a later sub-task) drops in behind it without touching callers. This slice is BACKEND-FREE: the fake
// backend maps (IR + variant) -> a deterministic STUB artifact (never real SPIR-V), which is enough to
// exercise the whole author -> enumerate -> compile -> cache pipeline with no native toolchain.

#pragma once

#include "context/render/material/material_ir.h"

#include <cstddef>
#include <string>

namespace context::render::material
{

// A stand-in compiled shader artifact. Same shape the real backend will fill with real bytes; here the
// fake backend fills `artifact` with a deterministic stub blob.
struct CompiledArtifact
{
    std::string compiler_id; // which backend produced it (a cache-key component, R-FILE-010)
    std::string ir_hash;     // content hash of the source IR (ir_content_hash)
    std::string variant_key; // canonical variant key this artifact was compiled for
    std::string artifact;    // the compiled bytes — a deterministic stub in the fake backend

    bool operator==(const CompiledArtifact&) const = default;
};

// The compiler seam. compile() MUST be a pure deterministic function of (ir, variant, id()) so the
// content-addressed cache is sound (R-FILE-010 / R-FILE-005 derivation purity).
class IShaderCompiler
{
public:
    virtual ~IShaderCompiler() = default;

    // Stable backend identifier — a cache-key component, so two backends never share cache entries.
    [[nodiscard]] virtual std::string id() const = 0;

    // Compile one shader variant into a stand-in/real artifact.
    [[nodiscard]] virtual CompiledArtifact compile(const ShaderIr& ir,
                                                   const VariantKey& variant) const = 0;
};

// A deterministic, GPU-free reference backend: maps (IR + variant) -> a stub artifact that embeds the
// IR content hash + the active defines, so the full pipeline is exercised with no native toolchain. It
// COUNTS its real compile() calls so tests can prove the cache actually skips recompute.
class FakeShaderCompiler final : public IShaderCompiler
{
public:
    explicit FakeShaderCompiler(std::string id = "fake-ref-v1");

    [[nodiscard]] std::string id() const override;
    [[nodiscard]] CompiledArtifact compile(const ShaderIr& ir,
                                           const VariantKey& variant) const override;

    // Number of times compile() actually ran (a real compile, not a cache hit).
    [[nodiscard]] std::size_t compile_count() const noexcept;

private:
    std::string id_;
    mutable std::size_t compile_count_ = 0;
};

} // namespace context::render::material
