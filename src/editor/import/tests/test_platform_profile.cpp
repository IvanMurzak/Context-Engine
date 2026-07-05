// Platform-profile + transcode-table skeleton tests.

#include "context/editor/import/platform_profile.h"

#include "import_test.h"

using namespace context::editor::import;

int main()
{
    // The v1 platform set: the desktop trio + the Web memory-constrained wedge, and nothing else
    // (no Android/iOS pre-declared — a key must never name an untargeted platform).
    const auto& profiles = platform_profiles();
    CHECK(profiles.size() == 4);
    CHECK(find_platform_profile("windows") != nullptr);
    CHECK(find_platform_profile("linux") != nullptr);
    CHECK(find_platform_profile("macos") != nullptr);
    const PlatformProfile* web = find_platform_profile("web");
    CHECK(web != nullptr);
    CHECK(web->memory_constrained);              // Web is the memory-constrained wedge (R-ASSET-003)
    CHECK(!find_platform_profile("windows")->memory_constrained);
    CHECK(find_platform_profile("android") == nullptr); // not a v1 target
    CHECK(find_platform_profile("") == nullptr);

    // The host profile is one of the known ids.
    const PlatformProfile& host = host_platform_profile();
    CHECK(find_platform_profile(host.id) != nullptr);

    // The transcode table has a row for EVERY (kind x platform) — no gaps in the skeleton.
    const ArtifactKind kinds[] = {ArtifactKind::texture, ArtifactKind::mesh, ArtifactKind::audio};
    for (ArtifactKind kind : kinds)
        for (const PlatformProfile& profile : profiles)
        {
            const TranscodeTarget* target = transcode_target_for(kind, profile.id);
            CHECK(target != nullptr);
            CHECK(!target->format.empty());
        }
    CHECK(transcode_table().size() == 3 * profiles.size());

    // Per-platform distinction is real (what makes per-platform cache entries meaningful): texture
    // differs desktop vs Apple/Web, audio differs desktop vs Web.
    CHECK(transcode_target_for(ArtifactKind::texture, "windows")->format !=
          transcode_target_for(ArtifactKind::texture, "web")->format);
    CHECK(transcode_target_for(ArtifactKind::audio, "windows")->format !=
          transcode_target_for(ArtifactKind::audio, "web")->format);

    // An unknown platform has no row (surfaced, never guessed).
    CHECK(transcode_target_for(ArtifactKind::texture, "playstation") == nullptr);

    // Kind names are the stable on-disk ids.
    CHECK(artifact_kind_name(ArtifactKind::texture) == "texture");
    CHECK(artifact_kind_name(ArtifactKind::mesh) == "mesh");
    CHECK(artifact_kind_name(ArtifactKind::audio) == "audio");

    IMPORT_TEST_MAIN_END();
}
