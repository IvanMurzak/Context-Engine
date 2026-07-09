// The GPU-free fake/reference shader backend (R-REND-005, first slice; issue #121). Deterministic:
// (IR + variant) -> a stub artifact that is a pure function of the canonical IR serialization + the
// variant key + the backend id, so the R-FILE-010 content-addressed cache is sound.

#include "context/render/material/shader_compiler.h"

#include <sstream>
#include <utility>

namespace context::render::material
{

FakeShaderCompiler::FakeShaderCompiler(std::string id) : id_(std::move(id))
{
}

std::string FakeShaderCompiler::id() const
{
    return id_;
}

CompiledArtifact FakeShaderCompiler::compile(const ShaderIr& ir, const VariantKey& variant) const
{
    const std::string canonical_ir = serialize_shader(ir);
    const std::string variant_key = variant.canonical();

    CompiledArtifact art;
    art.compiler_id = id_;
    art.ir_hash = content_hash_hex(canonical_ir);
    art.variant_key = variant_key;

    // A deterministic, human-inspectable stub blob. The trailing `blob=` line folds the full IR AND
    // the variant into one hash, so two different variants of the same shader produce different
    // artifacts (a property the compile + cache tests assert). Never real SPIR-V — that is sub-task C.
    std::ostringstream os;
    os << "FAKE-SHADER-ARTIFACT v1\n";
    os << "compiler=" << id_ << "\n";
    os << "shader=" << ir.name << "\n";
    os << "ir=" << art.ir_hash << "\n";
    os << "variant=" << (variant_key.empty() ? "<default>" : variant_key) << "\n";
    for (const ShaderStage& s : ir.stages)
    {
        os << "stage=" << to_string(s.kind) << ":" << s.entry_point << "\n";
    }
    os << "blob=" << content_hash_hex(canonical_ir + "\x1f" + variant_key) << "\n";
    art.artifact = os.str();

    ++compile_count_;
    return art;
}

std::size_t FakeShaderCompiler::compile_count() const noexcept
{
    return compile_count_;
}

} // namespace context::render::material
