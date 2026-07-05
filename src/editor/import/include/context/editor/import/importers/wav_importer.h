// WAV importer (R-ASSET-001). Parses the RIFF/WAVE container fully — the `fmt ` chunk (format,
// channels, sample rate, bit depth) and the `data` chunk — into a canonical audio DESCRIPTOR plus
// the NORMALIZED PCM payload. Unlike the texture/mesh paths, WAV `data` IS raw PCM, so this is a
// COMPLETE import (no deferred transcode needed for the desktop pcm16 target): the descriptor is the
// engine audio format and the PCM artifact is the playable payload, both byte-deterministic.

#pragma once

#include "context/editor/import/importer.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace context::editor::import
{

// The structural facts extracted from a WAV's `fmt `/`data` chunks.
struct WavInfo
{
    std::uint16_t format = 0;          // 1 = PCM (WAVE_FORMAT_PCM); others reported unsupported
    std::uint16_t channels = 0;
    std::uint32_t sample_rate = 0;
    std::uint16_t bits_per_sample = 0;
    std::uint64_t frame_count = 0;     // data_size / (channels * bits_per_sample/8)
    std::size_t data_offset = 0;       // byte offset of the PCM payload in the source
    std::size_t data_size = 0;         // PCM payload length in bytes
};

// Parse a WAV. Returns true and fills `info` on a well-formed PCM stream; false with a diagnostic on
// a bad RIFF/WAVE header, a missing/short `fmt `, a missing `data`, a non-PCM format, or a
// zero/!aligned block. Never throws.
[[nodiscard]] bool parse_wav(std::string_view bytes, WavInfo& info,
                            std::vector<ImportDiagnostic>& diagnostics);

// The WAV importer: source ".wav" -> one ArtifactKind::audio descriptor + one PCM payload artifact.
class WavImporter final : public Importer
{
public:
    [[nodiscard]] std::string_view id() const noexcept override { return "wav"; }
    [[nodiscard]] std::uint32_t version() const noexcept override { return 1; }
    [[nodiscard]] std::vector<std::string> extensions() const override { return {".wav"}; }
    [[nodiscard]] std::uint32_t derived_format_version(ArtifactKind kind) const noexcept override;
    [[nodiscard]] ImportResult import(const ImportInput& input) const override;
};

} // namespace context::editor::import
