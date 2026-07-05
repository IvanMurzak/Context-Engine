// Per-platform transcode skeleton — the v1 platform set + the (kind x platform) format table.

#include "context/editor/import/platform_profile.h"

namespace context::editor::import
{

const std::vector<PlatformProfile>& platform_profiles()
{
    // The v1 targets. Android/iOS are intentionally ABSENT until their legs activate (R-BUILD-001):
    // a cache key must never name a platform the engine does not target yet. Web is the sole
    // memory-constrained wedge in v1 (R-ASSET-003).
    static const std::vector<PlatformProfile> profiles = {
        {"windows", false},
        {"linux", false},
        {"macos", false},
        {"web", true},
    };
    return profiles;
}

const PlatformProfile& host_platform_profile()
{
    // The desktop this build runs on — the default for an editor-side import. Compile-time selected;
    // the fallback (no known macro) is the first desktop profile so the reference is always valid.
#if defined(_WIN32)
    const char* host_id = "windows";
#elif defined(__APPLE__)
    const char* host_id = "macos";
#elif defined(__linux__)
    const char* host_id = "linux";
#else
    const char* host_id = "linux";
#endif
    const PlatformProfile* found = find_platform_profile(host_id);
    // find never fails for a table id, but stay total rather than deref a theoretical null.
    return found != nullptr ? *found : platform_profiles().front();
}

const PlatformProfile* find_platform_profile(std::string_view id)
{
    for (const PlatformProfile& profile : platform_profiles())
        if (profile.id == id)
            return &profile;
    return nullptr;
}

const std::vector<TranscodeTarget>& transcode_table()
{
    // Data-driven skeleton: one row per (kind, platform). Formats are the v1 INTENT the byte-level
    // encoder will honor; distinct per-platform choices are what make per-platform cache entries
    // meaningful. Kinds in enum order, platforms in platform_profiles() order → diff-stable.
    static const std::vector<TranscodeTarget> table = {
        // texture: block-compressed on desktop, ASTC on Apple/Web (the transcode follow-up encodes).
        {ArtifactKind::texture, "windows", "bc7"},
        {ArtifactKind::texture, "linux", "bc7"},
        {ArtifactKind::texture, "macos", "astc_8x8"},
        {ArtifactKind::texture, "web", "astc_8x8"},
        // mesh: quantized/optimized vertex streams (meshopt) everywhere in v1.
        {ArtifactKind::mesh, "windows", "meshopt"},
        {ArtifactKind::mesh, "linux", "meshopt"},
        {ArtifactKind::mesh, "macos", "meshopt"},
        {ArtifactKind::mesh, "web", "meshopt"},
        // audio: uncompressed PCM on desktop; compressed on the memory-constrained Web ceiling.
        {ArtifactKind::audio, "windows", "pcm16"},
        {ArtifactKind::audio, "linux", "pcm16"},
        {ArtifactKind::audio, "macos", "pcm16"},
        {ArtifactKind::audio, "web", "vorbis"},
    };
    return table;
}

const TranscodeTarget* transcode_target_for(ArtifactKind kind, std::string_view platform_id)
{
    for (const TranscodeTarget& target : transcode_table())
        if (target.kind == kind && target.platform_id == platform_id)
            return &target;
    return nullptr;
}

} // namespace context::editor::import
