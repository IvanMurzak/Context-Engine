// The R-FILE-010 content-addressed shader-compile cache (issue #121). Backend-agnostic — it holds an
// IShaderCompiler& and keys on (IR hash | variant | compiler id).

#include "context/render/material/shader_cache.h"

namespace context::render::material
{

ShaderCompileCache::ShaderCompileCache(const IShaderCompiler& compiler) : compiler_(&compiler)
{
}

std::string ShaderCompileCache::cache_key(const ShaderIr& ir, const VariantKey& variant,
                                          const IShaderCompiler& compiler)
{
    // Enumerate every input that affects the output (R-FILE-010): the IR content hash, the variant
    // key, and the compiler id. The 0x1f unit separator keeps the components unambiguous.
    std::string composite = ir_content_hash(ir);
    composite += '\x1f';
    composite += variant.canonical();
    composite += '\x1f';
    composite += compiler.id();
    return content_hash_hex(composite);
}

const CompiledArtifact& ShaderCompileCache::get_or_compile(const ShaderIr& ir,
                                                           const VariantKey& variant)
{
    const std::string key = cache_key(ir, variant, *compiler_);

    const auto it = entries_.find(key);
    if (it != entries_.end())
    {
        ++hits_;
        return it->second; // cache hit — no recompute
    }

    ++misses_;
    // Write-once: the key is content-addressed, so a fresh compile of the same inputs is byte-identical.
    const auto [inserted, ok] = entries_.emplace(key, compiler_->compile(ir, variant));
    (void)ok;
    return inserted->second;
}

std::size_t ShaderCompileCache::hits() const noexcept
{
    return hits_;
}

std::size_t ShaderCompileCache::misses() const noexcept
{
    return misses_;
}

std::size_t ShaderCompileCache::size() const noexcept
{
    return entries_.size();
}

bool ShaderCompileCache::contains(const std::string& key) const
{
    return entries_.find(key) != entries_.end();
}

} // namespace context::render::material
