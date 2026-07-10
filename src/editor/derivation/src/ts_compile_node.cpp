// The TypeScript-compile derivation node (R-FILE-010; issue #85). See ts_compile_node.h for the
// contract. Backend-agnostic — it holds a TsToolchain& and keys on (source hash | options | toolchain
// version), the exhaustively-enumerated R-FILE-010 inputs. Reuses render/material's content_hash_hex
// (context_derivation already links context_render_material) so the whole graph shares ONE content
// hasher rather than inventing a second — the same hasher the shader node keys on.

#include "context/editor/derivation/ts_compile_node.h"

#include "context/render/material/material_ir.h" // content_hash_hex

#include <string>
#include <utility>

namespace context::editor::derivation
{
namespace
{

// Canonicalize the transpile options into a short, stable byte string so distinct options produce
// distinct cache keys (R-FILE-010: the same source under different options is a different artifact).
std::string options_key(const TranspileOptions& opts)
{
    std::string k;
    k += opts.bundle ? "b1" : "b0";
    k += opts.format == context::runtime::ts::ModuleFormat::Iife ? "fi" : "fe";
    k += opts.sourcemap ? "s1" : "s0";
    return k;
}

} // namespace

TsCompileNode::TsCompileNode(TsToolchain& toolchain, std::string toolchain_version)
    : toolchain_(&toolchain), toolchain_version_(std::move(toolchain_version))
{
}

std::string TsCompileNode::cache_key(std::string_view ts_source, const TranspileOptions& opts,
                                     std::string_view toolchain_version)
{
    // Enumerate every input that affects the output (R-FILE-010): the source content, the transpile
    // options, and the toolchain version. The 0x1f unit separator keeps the components unambiguous —
    // the same construction as ShaderCompileNode::cache_key.
    std::string composite(ts_source);
    composite += '\x1f';
    composite += options_key(opts);
    composite += '\x1f';
    composite += std::string(toolchain_version);
    return context::render::material::content_hash_hex(composite);
}

const TranspileResult& TsCompileNode::resolve(const std::string& key, const std::string& ts_file_path,
                                              std::string_view /*ts_source*/,
                                              const TranspileOptions& opts)
{
    const auto it = entries_.lower_bound(key);
    if (it != entries_.end() && it->first == key)
    {
        ++hits_;
        return it->second; // cache hit — no re-transpile
    }

    ++misses_;
    // Miss: run the toolchain now and store the (content-addressed, deterministic) result. lower_bound
    // above doubles as the insertion hint, so the miss path walks the tree only once. A deterministic
    // transpile FAILURE is cached too — re-authoring changes the content and thus the key.
    const auto inserted = entries_.emplace_hint(it, key, toolchain_->transpile(ts_file_path, opts));
    return inserted->second;
}

const TranspileResult& TsCompileNode::get_or_compile(const std::string& ts_file_path,
                                                     std::string_view ts_source,
                                                     const TranspileOptions& opts)
{
    return resolve(cache_key(ts_source, opts, toolchain_version_), ts_file_path, ts_source, opts);
}

std::size_t TsCompileNode::invalidate(std::string_view ts_source, const TranspileOptions& opts)
{
    return entries_.erase(cache_key(ts_source, opts, toolchain_version_));
}

const TranspileResult* TsCompileNode::find(const std::string& key) const
{
    const auto it = entries_.find(key);
    return it == entries_.end() ? nullptr : &it->second;
}

bool TsCompileNode::contains(const std::string& key) const
{
    return entries_.find(key) != entries_.end();
}

} // namespace context::editor::derivation
