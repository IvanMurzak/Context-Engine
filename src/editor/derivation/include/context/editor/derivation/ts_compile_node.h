// The TypeScript-compile derivation node (R-FILE-010; issue #85, the 2b-i follow-up). Wiring the
// TS->JS transpile/bundle as a first-class CACHED derivation artifact — content-addressed, keyed, and
// invalidatable — so UNCHANGED TypeScript is NOT re-transpiled. It is the exact peer of
// ShaderCompileNode (shader_compile_node.h): both re-home an expensive backend behind a content-
// addressed R-FILE-010 store; here the backend is the runtime/ts esbuild TsToolchain seam
// (ts_toolchain.h) rather than the render IShaderCompiler seam.
//
// R-FILE-010 keys derived state on "every input that affects the output, enumerated exhaustively".
// For a TS->JS compile those inputs are: the authored SOURCE bytes, the transpile OPTIONS
// (bundle/format/sourcemap — the same source under different options is a different artifact), and
// the TOOLCHAIN VERSION (an esbuild upgrade that changes the emitted JS yields NEW keys instead of
// serving artifacts transpiled by the old tool — the same "importer version" component the shader
// node and the asset importer use). The key is instance-independent, so a shared cache is sound.
//
// BUNDLE-MODE CAVEAT (issue #85 scopes the R-FILE-010 key to source|options|toolchain-version): the
// exhaustiveness above holds for a transpile-only compile (opts.bundle == false), where esbuild
// transpiles the given source bytes in isolation. When opts.bundle == true esbuild ALSO resolves +
// inlines the entry file's transitive imports FROM DISK, so the emitted JS additionally depends on
// every imported file's content — inputs this key does NOT enumerate. Two consequences a bundle-mode
// caller must own: (1) two entry files with byte-identical source but different local imports share a
// key, and (2) an edit to an imported file (the entry's own bytes unchanged) is NOT detected — so a
// bundle-mode caller MUST invalidate() whenever any transitively-imported file changes (the node
// cannot observe that on its own). Folding the entry file PATH into the key was considered and
// rejected: it still would not detect an imported-file edit (path unchanged), and it would break the
// content-addressed dedup of identical transpile-only source authored at two paths. Enumerating the
// resolved import closure as first-class derivation edges is the correct fix and a documented deferred
// extension (out of issue #85's R-FILE-010-cache scope). No production caller wires bundle mode yet.
//
// Backend-agnostic + single-threaded/deterministic by construction, like the rest of the derivation
// graph. This node keeps the SYNCHRONOUS get_or_compile() cache path (the R-FILE-010 DoD: unchanged TS
// is not re-transpiled); the R-FILE-013 backpressured request()/run_pass() queue the shader node adds
// is out of scope for issue #85 (a clean later extension over the shared BackpressureSignal vocabulary).

#pragma once

#include "context/runtime/ts/ts_toolchain.h"

#include <cstddef>
#include <map>
#include <string>
#include <string_view>

namespace context::editor::derivation
{

using context::runtime::ts::TranspileOptions;
using context::runtime::ts::TranspileResult;
using context::runtime::ts::TsToolchain;

// The TS-compile derivation node. Holds a TsToolchain& (the toolchain must outlive the node) and owns
// the content-addressed artifact store. A cache HIT never calls the toolchain — that is the whole
// point (unchanged TS is not re-transpiled). Because the toolchain (esbuild) is a subprocess that
// reads the source from a FILE, get_or_compile takes both the source bytes (the cache-key input) and
// the file path the toolchain reads on a miss; the caller guarantees the file holds those bytes.
class TsCompileNode
{
public:
    // `toolchain_version` is the exhaustive-key toolchain component (e.g. the esbuild `version()`
    // string). Fold in whatever identifies the emitting tool; a bump re-keys every entry (R-FILE-010).
    explicit TsCompileNode(TsToolchain& toolchain, std::string toolchain_version);

    // The content-addressed key for (source, options, toolchain version): a hash of
    // (content_hash | options | toolchain_version). Deterministic and instance-independent
    // (R-FILE-010) — exposed so callers/tools can address entries without transpiling.
    [[nodiscard]] static std::string cache_key(std::string_view ts_source,
                                               const TranspileOptions& opts,
                                               std::string_view toolchain_version);

    // Transpile `ts_source` (which the toolchain reads from `ts_file_path` on a MISS), or serve a
    // prior identical result. A cache HIT does NOT call the toolchain — unchanged TS is not
    // re-transpiled (the R-FILE-010 DoD). A deterministic transpile FAILURE is cached too (the same
    // bad source deterministically re-fails; re-authoring changes the content and thus the key). The
    // returned reference is stable until invalidate() evicts that entry (std::map nodes are otherwise
    // never relocated), so callers may hold it across further calls.
    [[nodiscard]] const TranspileResult& get_or_compile(const std::string& ts_file_path,
                                                        std::string_view ts_source,
                                                        const TranspileOptions& opts);

    // Drop the cached artifact for exactly (source, options) under the current toolchain version.
    // Returns the number of entries evicted (0 or 1). Re-authored source simply produces a NEW key, so
    // explicit invalidation is only needed to reclaim a superseded entry (R-FILE-010: nothing is truly
    // "invalidated"; changed inputs produce a new key — this is the reclaim hook).
    std::size_t invalidate(std::string_view ts_source, const TranspileOptions& opts);

    // --- introspection ------------------------------------------------------------------------

    [[nodiscard]] const TranspileResult* find(const std::string& key) const; // nullptr if absent
    [[nodiscard]] bool contains(const std::string& key) const;
    [[nodiscard]] std::size_t hits() const noexcept { return hits_; }
    [[nodiscard]] std::size_t misses() const noexcept { return misses_; }
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] const std::string& toolchain_version() const noexcept { return toolchain_version_; }

private:
    // Compile (miss) or serve (hit) the request under `key`; inserts on a miss. Returns the stored
    // entry. Shared so the store/counters stay in one place.
    const TranspileResult& resolve(const std::string& key, const std::string& ts_file_path,
                                   std::string_view ts_source, const TranspileOptions& opts);

    TsToolchain* toolchain_;
    std::string toolchain_version_;
    std::map<std::string, TranspileResult> entries_; // the content-addressed store
    std::size_t hits_ = 0;
    std::size_t misses_ = 0;
};

} // namespace context::editor::derivation
