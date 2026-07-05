// The derived-artifact kinds an importer emits (R-ASSET-001). Each kind carries its own
// derived-format version — a cache-key component (R-FILE-010) so a format bump re-keys only that
// kind's entries, never the whole cache.

#pragma once

#include <cstdint>
#include <string_view>

namespace context::editor::import
{

// The engine-format artifact families the M2 importers produce. The set is intentionally small and
// closed for v1 (texture / mesh / audio — the glTF, PNG, WAV first cut); a new kind is an additive
// enumerator plus its transcode-table + derived-format-version rows.
enum class ArtifactKind : std::uint8_t
{
    texture, // decoded/described image data (PNG first) — the R-REND texture path
    mesh,    // described geometry (glTF first) — reserves a UV2 lightmap channel (R-REND-006)
    audio,   // described + normalized PCM (WAV first) — the R-AUDIO path
};

// A stable, lowercase, ASCII identifier for a kind — used in cache paths, transcode-table rows, and
// derived-artifact names, so it must never change once shipped (it is part of the on-disk key form).
[[nodiscard]] constexpr std::string_view artifact_kind_name(ArtifactKind kind) noexcept
{
    switch (kind)
    {
    case ArtifactKind::texture:
        return "texture";
    case ArtifactKind::mesh:
        return "mesh";
    case ArtifactKind::audio:
        return "audio";
    }
    return "unknown";
}

} // namespace context::editor::import
