// The import cache key (R-FILE-010). The hash of EVERY input that affects a derived artifact's
// bytes, enumerated exhaustively so the machine-level content-addressed shared cache (L-28) can be
// sound: two runs that agree on every component MUST produce identical bytes (the R-ASSET-001
// double-run gate proves it), and any component change simply mints a NEW key — nothing is ever
// "invalidated" (R-FILE-010). This EXTENDS the engine's existing content-hash machinery
// (filesync::content_hash / serializer::canonical_hash_of, FNV-1a 64-bit) — it introduces no new
// hash family and does not restructure the derivation graph's per-source memoization.

#pragma once

#include "context/editor/import/artifact_kind.h"
#include "context/editor/import/importer.h" // DerivedArtifact (make_cache_key input)

#include <cstdint>
#include <string>
#include <string_view>

namespace context::editor::import
{

// The raw-byte source hash for the cache key's `source_bytes_hash` component (R-FILE-010). Wraps the
// engine's raw-byte content hash (filesync::content_hash, FNV-1a 64-bit) so a caller keys a source
// without reaching into the file-sync layer — the import framework owns "how a source is hashed".
[[nodiscard]] std::uint64_t hash_source_bytes(std::string_view bytes) noexcept;

// The CPU instruction-set-architecture tag of THIS build (compile-time). A cache-key component
// because cross-machine strict-FP import determinism is explicitly deferred (R-FILE-010): an
// artifact produced on x86_64 is not assumed byte-identical to one produced on arm64, so they key
// separately. Stable lowercase ids ("x86_64", "arm64", "x86", "unknown").
[[nodiscard]] std::string_view current_cpu_isa() noexcept;

// A stable hash identifying the compiled importer set of THIS engine build (R-FILE-010's "importer
// build hash"). v1 derives it from a build stamp baked at compile time (see cache_key.cpp) so a
// rebuilt engine with changed importer code re-keys; a real per-importer object-hash lands with the
// native build pipeline (documented — never silently assumed to already be object-exact).
[[nodiscard]] std::uint64_t importer_build_hash() noexcept;

// Every input that affects one derived artifact's bytes — the exhaustive R-FILE-010 enumeration.
// A field left at its zero value means "not applicable to this artifact" (e.g. registered_set_hash
// is 0 for a pure binary importer that does not depend on the schema/migration stratum) and folds
// in as that zero, so the key stays well-defined and stable.
struct ImportCacheKey
{
    std::uint64_t source_bytes_hash = 0;     // raw source asset bytes (filesync::content_hash)
    std::uint64_t import_settings_hash = 0;  // sidecar meta / import settings (ImportSettings::hash)
    std::string importer_id;                 // which importer produced it ("png"/"wav"/"gltf")
    std::uint32_t importer_version = 0;      // that importer's version
    std::string platform_profile;            // target platform profile id (per-platform variants)
    std::uint64_t importer_build_hash = 0;   // the compiled-importer build hash (this build)
    std::string cpu_isa;                     // the producing CPU ISA (cross-machine determinism scope)
    ArtifactKind artifact_kind = ArtifactKind::texture; // which kind ...
    std::string artifact_name;               // ... its stable name within the import (descriptor vs
                                             // payload) — distinguishes SIBLING artifacts of one
                                             // import that share a kind, so they key + address apart
    std::uint32_t derived_format_version = 0;           // ... and its per-kind derived-format version
    // The R-FILE-005 derived stratum: the content-hash of the registered schema + migration set. 0
    // for importers that do not consume it; a package upgrade that changes a migration re-keys the
    // artifacts that DO (R-FILE-010) rather than serving data derived under the old set.
    std::uint64_t registered_set_hash = 0;

    // The stable 64-bit content-address digest folded over every component above (FNV-1a combine).
    // Deterministic + order-fixed: the same components always yield the same digest, and any change
    // yields a different one (the soundness the shared cache rests on).
    [[nodiscard]] std::uint64_t digest() const noexcept;

    // The content-addressed relative cache path for this artifact: "<importer_id>/<kind>/<hexdigest>".
    // Namespaced by importer + kind so a `cache` GC/verify tool can scope by producer, and immutable
    // (write-once) by construction — the digest (which folds the artifact name) fully determines it.
    [[nodiscard]] std::string cache_path() const;
};

// The source-side inputs a key shares across every artifact of one import — supplied by the caller
// (the framework knows the source bytes hash, resolved settings hash, importer identity, and target
// platform). Combined with a specific DerivedArtifact by make_cache_key below.
struct ImportKeyContext
{
    std::uint64_t source_bytes_hash = 0;
    std::uint64_t import_settings_hash = 0;
    std::string importer_id;
    std::uint32_t importer_version = 0;
    std::string platform_profile;
    std::uint64_t registered_set_hash = 0; // 0 unless the importer consumes the R-FILE-005 stratum
};

// Build the R-FILE-010 key for `artifact` under `context`. Fills importer_build_hash() +
// current_cpu_isa() (this build's determinism scope) automatically, and takes the per-artifact-kind
// derived-format version + kind + name from the artifact — so every derived artifact of an import
// gets a correct, distinct key with one call.
[[nodiscard]] ImportCacheKey make_cache_key(const ImportKeyContext& context,
                                           const DerivedArtifact& artifact);

} // namespace context::editor::import
