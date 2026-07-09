// A content-addressed shader-compile cache (R-FILE-010). Shader compilation + variant generation are
// derivation-graph nodes (R-FILE-005) — cached, not an unbudgeted per-build side pipeline (R-REND-005).
// The cache key enumerates every input that affects the output: the IR content hash + the variant key
// + the compiler id. A repeated (ir, variant) request under the same backend is served from the cache
// with NO recompute; entries are write-once (immutable). Wired GPU-free here around the fake backend
// and unchanged when the native backend lands behind the IShaderCompiler seam.

#pragma once

#include "context/render/material/material_ir.h"
#include "context/render/material/shader_compiler.h"

#include <cstddef>
#include <map>
#include <string>

namespace context::render::material
{

class ShaderCompileCache
{
public:
    // The cache borrows the compiler; the compiler must outlive the cache.
    explicit ShaderCompileCache(const IShaderCompiler& compiler);

    // Compile `variant` of `ir`, or serve a prior identical result. A cache-hit does NOT call
    // compiler.compile(). The returned reference is stable for the cache's lifetime (std::map nodes
    // are never relocated), so callers may hold it across further get_or_compile() calls.
    [[nodiscard]] const CompiledArtifact& get_or_compile(const ShaderIr& ir,
                                                         const VariantKey& variant);

    // The content-addressed key for (ir, variant, compiler): a hash of
    // (ir_content_hash | variant.canonical() | compiler.id()). Deterministic and independent of the
    // cache instance — exposed so tests/tools can address entries without compiling.
    [[nodiscard]] static std::string cache_key(const ShaderIr& ir, const VariantKey& variant,
                                               const IShaderCompiler& compiler);

    [[nodiscard]] std::size_t hits() const noexcept;
    [[nodiscard]] std::size_t misses() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept; // number of distinct cached entries
    [[nodiscard]] bool contains(const std::string& key) const;

private:
    const IShaderCompiler* compiler_;
    std::map<std::string, CompiledArtifact> entries_;
    std::size_t hits_ = 0;
    std::size_t misses_ = 0;
};

} // namespace context::render::material
