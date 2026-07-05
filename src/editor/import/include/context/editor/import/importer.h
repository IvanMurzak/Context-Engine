// The importer contract (R-ASSET-001). A package-based importer converts source-asset bytes into
// engine-format derived artifacts, RUN-DETERMINISTICALLY: an import is a PURE function of its input
// (source bytes + resolved settings + target platform) — fixed seeds, no threads, no clock, no
// environment, no filesystem, no ambient state — so two runs byte-match (the CI double-run gate,
// R-ASSET-001) and the shared cache (L-28) is sound. Importers run headless in EditorKernel, under
// the R-SEC-006 isolation slice (see sandbox.h / isolated_runner.h).

#pragma once

#include "context/editor/import/artifact_kind.h"
#include "context/editor/import/import_settings.h"
#include "context/editor/import/platform_profile.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace context::editor::import
{

// One machine-readable importer diagnostic (R-FILE-003 shape). `code` is a stable dotted id drawn
// from the shared error catalog (contract/error_catalog.cpp — the `import.*` cluster); imports over
// binary have no line/column, so this shape is code + message only.
struct ImportDiagnostic
{
    std::string code;
    std::string message;
};

// One derived engine-format artifact. `bytes` is a byte container (std::string carries binary fine,
// the codebase idiom): a canonical-JSON descriptor for described kinds, raw normalized payload for
// PCM. `name` is stable within an import ("texture", "mesh", "audio", "audio.pcm") so a multi-artifact
// import keys each piece deterministically.
struct DerivedArtifact
{
    ArtifactKind kind = ArtifactKind::texture;
    std::string name;
    std::string bytes;
    // The per-artifact-kind derived-format version this artifact was produced under — copied into
    // its ImportCacheKey (R-FILE-010). Carried on the artifact so the framework keys each piece
    // without re-asking the importer per kind.
    std::uint32_t derived_format_version = 0;
};

// The result of one import. `ok` is false when the source could not be decoded; `diagnostics` then
// names why (never a throw, never a partial-but-unmarked output). A successful import always yields
// at least one artifact.
struct ImportResult
{
    bool ok = false;
    std::vector<DerivedArtifact> artifacts;
    std::vector<ImportDiagnostic> diagnostics;
};

// The pure input an importer sees (R-SEC-006: nothing else). `source_bytes` is the ONLY view of the
// asset; `settings` and `platform` are the resolved knobs. No path handle, no env, no clock.
struct ImportInput
{
    std::string source_path;        // logical path — diagnostics + extension routing only
    std::string_view source_bytes;  // the asset bytes (the sole input surface)
    ImportSettings settings;        // resolved per-asset + per-platform settings (cache-keyed)
    PlatformProfile platform;       // the target platform profile (cache-keyed)
};

// A package-based importer. Stateless + const: `import()` MUST NOT mutate the importer, so one
// instance serves every asset and the purity contract holds by construction.
class Importer
{
public:
    virtual ~Importer() = default;

    // The stable importer id ("png"/"wav"/"gltf") — part of the cache key and the diagnostics.
    [[nodiscard]] virtual std::string_view id() const noexcept = 0;

    // The importer version — a cache-key component (R-FILE-010). BUMP when the output bytes for a
    // fixed input change; a bump re-keys this importer's entries without touching others.
    [[nodiscard]] virtual std::uint32_t version() const noexcept = 0;

    // The source-file extensions this importer claims, lowercase WITH the dot (".png"). The registry
    // routes by these; two importers claiming one extension is a registration error (registry).
    [[nodiscard]] virtual std::vector<std::string> extensions() const = 0;

    // The derived-format version for `kind` (R-FILE-010 per-artifact-kind component). BUMP when the
    // descriptor/payload SHAPE for that kind changes even if the importer logic did not.
    [[nodiscard]] virtual std::uint32_t derived_format_version(ArtifactKind kind) const noexcept = 0;

    // Convert `input.source_bytes` to derived artifacts. PURE + total: deterministic for a fixed
    // input, and it NEVER throws — a bad source is `ok=false` + diagnostics, not an exception.
    [[nodiscard]] virtual ImportResult import(const ImportInput& input) const = 0;
};

} // namespace context::editor::import
