// Per-platform asset transcode (R-BUILD-003 / R-FILE-010, task a03). Turns one imported derived
// artifact into its per-platform VARIANT payload — the pack-ready bytes in the target's optimal
// engine format from the transcode_table() (platform_profile.h): BCn-class block-compressed textures
// on desktop vs ASTC on Apple/Web, verbatim PCM on desktop vs mu-law-companded audio on the
// memory-constrained Web ceiling. This is the byte-level encoder behind the M2 transcode SKELETON
// (import/README.md deferral #1): the table + the platform cache-key component already existed, so a
// variant drops in WITHOUT a cache-key change — it RIDES the existing per-platform key (R-FILE-010),
// never forks it.
//
// A transcode is a PURE, deterministic derivation node: same (artifact, platform) => byte-identical
// variant, so the R-ASSET-001 double-run byte-compare gate holds with transcode nodes in the graph
// and the shared cache (L-28) is sound. It NEVER throws (a bad descriptor is ok=false + an error
// code, not an exception).

#pragma once

#include "context/editor/import/artifact_kind.h"
#include "context/editor/import/cache_key.h"        // ImportCacheKey / ImportKeyContext (variant key)
#include "context/editor/import/importer.h"         // DerivedArtifact
#include "context/editor/import/platform_profile.h" // PlatformProfile / TranscodeTarget

#include <cstdint>
#include <string>

namespace context::editor::import
{

// The frozen version of the "CTXV" transcoded-variant container encoding (see transcode.cpp). A bump
// re-keys every variant (it folds into the variant's derived-format version) — additive, never a break.
inline constexpr std::uint32_t kTranscodeContainerVersion = 1;

// One transcoded per-platform variant of a derived artifact. `bytes` is a self-describing "CTXV"
// container (magic + kind + platform + format + payload) so a consumer — the pack writer — carries it
// as a first-class sidecar and can tell format/kind/platform without re-reading the descriptor.
struct TranscodedVariant
{
    std::string platform_id;      // the target platform this variant is for
    std::string target_format;    // the engine format the transcode table names (bc7/astc_8x8/pcm16/…)
    std::string applied_encoding; // what v1 ACTUALLY applied — may trail target_format where the
                                  // perceptual encoder is a tracked follow-up (e.g. target "vorbis",
                                  // applied "mulaw8"; target "bc7", applied "bcn_plan"). Honest, never
                                  // a silent stub: the SIZING/geometry is real + per-platform-distinct.
    std::string bytes;            // the CTXV variant container — the pack-ready payload
    std::uint64_t content_hash = 0; // canonical_hash_of(bytes): pack self-verify (R-FILE-010) + key input
};

// The outcome of a transcode. `ok == false` ⇒ `bytes` is empty and `error` names the failure:
// "transcode.no_target"     — the transcode table has no row for (kind, platform) (surfaced, never guessed);
// "transcode.bad_descriptor" — a described-kind artifact whose bytes are not a well-formed descriptor.
struct TranscodeResult
{
    bool ok = false;
    TranscodedVariant variant;
    std::string error;
};

// Transcode `artifact` to its per-platform variant for `platform` (R-BUILD-003). Pure + total +
// deterministic. The target engine format is transcode_target_for(artifact.kind, platform.id); a
// missing row is transcode.no_target. Per kind:
//  - texture: a real BCn/ASTC block-tiled variant container (block geometry + mip pyramid sizing from
//    the descriptor's width/height/srgb/mipmaps) — genuinely per-platform-distinct (4×4 BCn vs 8×8
//    ASTC blocks). The perceptual texel encoder awaits the deferred PNG texel DEFLATE-decode.
//  - audio: verbatim 16-bit PCM (desktop `pcm16`) vs G.711 mu-law companding (the memory-constrained
//    Web target — a real, deterministic, dependency-free 2:1 compression). Operates on the PCM PAYLOAD
//    artifact (name contains "pcm"); an audio DESCRIPTOR artifact passes through, format-tagged.
//  - mesh: a deterministic meshopt plan (platform-invariant in v1 — `meshopt` on every target).
[[nodiscard]] TranscodeResult transcode_variant(const DerivedArtifact& artifact,
                                               const PlatformProfile& platform);

// The R-FILE-010 cache key the variant `transcode_variant(artifact, platform)` would be stored under,
// given the source-side ImportKeyContext of the import that produced `artifact`. It RIDES the import
// key: `platform` sets the key's platform_profile component (already present — reuse, don't fork), and
// the variant keys apart from the base artifact by an artifact name of "<name>#<target_format>". So
// the same (source, settings, platform) always yields the same digest — a content-addressed cache HIT
// on repeat — and two platforms key to two distinct entries (per-platform variants coexist, R-FILE-010).
[[nodiscard]] ImportCacheKey transcode_cache_key(const ImportKeyContext& base_context,
                                               const DerivedArtifact& artifact,
                                               const PlatformProfile& platform);

} // namespace context::editor::import
