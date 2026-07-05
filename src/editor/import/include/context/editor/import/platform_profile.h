// Per-platform transcode SKELETON (R-ASSET-001 / R-FILE-010). The v1 platform set and the
// (artifact-kind x platform) -> engine-format table are DATA-DRIVEN here; the target platform
// profile is a cache-key component so per-platform variants coexist as separate cache entries
// (R-FILE-010: "platform switches instant after first import"). The actual byte-level transcode
// (BC7/ASTC encode, mesh quantization, audio resample) is the documented follow-up — this milestone
// stakes out the format table + the key so it lands cheaply, never a silent stub (see README).

#pragma once

#include "context/editor/import/artifact_kind.h"

#include <string>
#include <string_view>
#include <vector>

namespace context::editor::import
{

// One target platform. `memory_constrained` marks the ceilings where streaming/compressed formats
// are MUST (R-ASSET-003): Web is the v1 memory-constrained wedge; Android/iOS join as they land.
struct PlatformProfile
{
    std::string id;                  // stable lowercase id — part of the on-disk cache key
    bool memory_constrained = false; // hard, comparatively small memory ceiling (Web in v1)
};

// The v1 platform set (data-driven). Desktop trio + the Web memory-constrained wedge. Android/iOS
// are deliberately absent until their platform legs activate (R-BUILD-001 / R-ASSET-003) — never
// pre-declared, so a cache key can never name a platform the engine does not target yet.
[[nodiscard]] const std::vector<PlatformProfile>& platform_profiles();

// The default platform for a host-side import when a task names none — the desktop this build runs
// on. Distinct from the transcode targets: importing "for the editor" uses this, and a build for a
// shipped platform re-imports under that platform's profile (its own cache entry).
[[nodiscard]] const PlatformProfile& host_platform_profile();

// Look up a platform by id; nullptr when unknown (an unknown platform is a caller error, never a
// silent fallback that would mint a wrong-keyed cache entry).
[[nodiscard]] const PlatformProfile* find_platform_profile(std::string_view id);

// One transcode target: the concrete engine format the SKELETON will produce for (kind, platform).
// v1 fills the table; the byte-level encoder behind each row is the follow-up.
struct TranscodeTarget
{
    ArtifactKind kind;
    std::string platform_id;
    std::string format; // e.g. texture: "bc7"/"astc_8x8"/"rgba8"; mesh: "meshopt"; audio: "pcm16"
};

// The full (kind x platform) transcode table — the data behind the skeleton. Deterministic order
// (kinds in enum order, platforms in platform_profiles() order) so it is diff-stable + test-pinnable.
[[nodiscard]] const std::vector<TranscodeTarget>& transcode_table();

// The transcode target for (kind, platform); nullptr when the table has no row (a gap that a
// transcode follow-up must fill — surfaced, never guessed).
[[nodiscard]] const TranscodeTarget* transcode_target_for(ArtifactKind kind,
                                                          std::string_view platform_id);

} // namespace context::editor::import
