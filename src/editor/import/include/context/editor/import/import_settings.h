// Import settings resolved from an asset's `<asset>.meta.json` sidecar (L-36) — the per-asset knobs
// an importer honors (sRGB, mip generation, ...), plus their canonical hash, a cache-key component
// (R-FILE-010: "the same texture with different compression settings yields a different key").

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace context::editor::import
{

// The resolved import settings for one (asset, platform) pair. Built from the meta's `importSettings`
// object with the reserved-in-M2 `platforms.<id>` block shallow-merged over it (L-36: the assetdb
// meta writes both, empty), so a per-platform override changes the settings hash for that platform
// ONLY. Importers read the typed convenience fields; the framework keys the cache on `hash`.
struct ImportSettings
{
    // The canonical bytes of the effective (post platform-merge) `importSettings` object. Canonical
    // so two byte-different-but-equivalent authorings key identically (R-FILE-001 fixpoint). Defaults
    // to the canonical empty object.
    std::string canonical_bytes = "{}\n";
    std::uint64_t hash = 0; // canonical_hash_of(canonical_bytes) — the R-FILE-010 settings component

    // --- typed convenience reads (defaults chosen so an empty settings block is a sane import) ----
    // These are DESCRIPTOR POLICY the derived artifact records for the transcode stage to honor —
    // not transforms this framework applies to bytes (v1 emits descriptors, not transcoded texels).
    bool srgb = true;             // texture: mark the source as sRGB-encoded color (vs linear data)
    bool generate_mipmaps = true; // texture: request a mip chain in the transcode plan
};

// Resolve settings from raw meta sidecar bytes for `platform_id`. Total + deterministic (never
// throws): malformed / missing meta yields the defaults (an asset with no meta still imports). A
// present `platforms.<platform_id>` object shallow-overrides `importSettings`; unknown keys are
// carried through the hash (so a future importer's knob still re-keys) but ignored by the typed
// fields (a wrong-typed known knob simply keeps its default while still hashing its authored bytes).
[[nodiscard]] ImportSettings resolve_import_settings(std::string_view meta_bytes,
                                                    std::string_view platform_id);

} // namespace context::editor::import
