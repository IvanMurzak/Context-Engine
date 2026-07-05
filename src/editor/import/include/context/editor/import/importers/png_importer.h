// PNG importer (R-ASSET-001). Parses the PNG container fully — 8-byte signature, every chunk
// (length + type + CRC-32 verified), IHDR geometry, and the color-space chunks — into a canonical
// texture DESCRIPTOR (the M2 engine texture format: dimensions, channel layout, colorspace, and the
// per-platform transcode target). The texel-level decode (DEFLATE inflate + filter reconstruction)
// and encode (BC7/ASTC) are the documented transcode follow-up; the descriptor + CRC-verified
// structure is a complete, deterministic, fuzz-hardened import on its own.

#pragma once

#include "context/editor/import/importer.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace context::editor::import
{

// The structural facts extracted from a PNG's IHDR + ancillary chunks. Pure geometry/format — no
// pixels (R-FILE-011(e) discipline: describe without materializing payload).
struct PngInfo
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint8_t bit_depth = 0;
    std::uint8_t color_type = 0; // 0 gray, 2 rgb, 3 palette, 4 gray+alpha, 6 rgba
    std::uint8_t interlace = 0;  // 0 none, 1 Adam7
    std::uint8_t channels = 0;   // derived from color_type (palette = 1 index channel)
    bool has_alpha = false;
    bool srgb = false;           // an sRGB chunk was present (authoritative colorspace intent)
};

// Parse + CRC-verify a PNG. Returns true and fills `info` on a well-formed stream; false with a
// diagnostic (`import.source_malformed` / `import.decode_failed`) on a bad signature, truncated or
// mis-ordered chunk, CRC mismatch, missing/!first IHDR, or zero dimension. Never throws.
[[nodiscard]] bool parse_png(std::string_view bytes, PngInfo& info,
                            std::vector<ImportDiagnostic>& diagnostics);

// The PNG importer: source ".png" -> one ArtifactKind::texture descriptor.
class PngImporter final : public Importer
{
public:
    [[nodiscard]] std::string_view id() const noexcept override { return "png"; }
    [[nodiscard]] std::uint32_t version() const noexcept override { return 1; }
    [[nodiscard]] std::vector<std::string> extensions() const override { return {".png"}; }
    [[nodiscard]] std::uint32_t derived_format_version(ArtifactKind kind) const noexcept override;
    [[nodiscard]] ImportResult import(const ImportInput& input) const override;
};

} // namespace context::editor::import
