// WAV importer — RIFF/WAVE chunk walk into a canonical audio descriptor + the raw-PCM payload.

#include "context/editor/import/importers/wav_importer.h"

#include "context/editor/import/platform_profile.h"
#include "context/editor/serializer/canonical.h"

#include "../detail/json_detail.h"

#include <cstdint>
#include <string>

namespace context::editor::import
{
namespace
{
using detail::jobject;
using detail::jstr;
using detail::juint;
using detail::put;

std::uint32_t read_u32le(std::string_view b, std::size_t at) noexcept
{
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at])) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at + 1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at + 2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at + 3])) << 24);
}

std::uint16_t read_u16le(std::string_view b, std::size_t at) noexcept
{
    return static_cast<std::uint16_t>(static_cast<std::uint8_t>(b[at]) |
                                      (static_cast<std::uint8_t>(b[at + 1]) << 8));
}

void fail(std::vector<ImportDiagnostic>& diagnostics, std::string code, std::string message)
{
    diagnostics.push_back({std::move(code), std::move(message)});
}
} // namespace

bool parse_wav(std::string_view bytes, WavInfo& info, std::vector<ImportDiagnostic>& diagnostics)
{
    if (bytes.size() < 12 || bytes.substr(0, 4) != "RIFF" || bytes.substr(8, 4) != "WAVE")
    {
        fail(diagnostics, "import.source_malformed", "not a WAV (bad RIFF/WAVE header)");
        return false;
    }

    std::size_t pos = 12; // first sub-chunk follows the 12-byte RIFF/WAVE header
    bool have_fmt = false;
    bool have_data = false;
    while (pos + 8 <= bytes.size())
    {
        const std::string_view id = bytes.substr(pos, 4);
        const std::uint32_t size = read_u32le(bytes, pos + 4);
        const std::size_t data_at = pos + 8;
        if (bytes.size() - data_at < static_cast<std::size_t>(size))
        {
            fail(diagnostics, "import.source_malformed", "truncated WAV chunk");
            return false;
        }

        if (id == "fmt ")
        {
            if (size < 16)
            {
                fail(diagnostics, "import.source_malformed", "WAV fmt chunk shorter than 16 bytes");
                return false;
            }
            info.format = read_u16le(bytes, data_at);
            info.channels = read_u16le(bytes, data_at + 2);
            info.sample_rate = read_u32le(bytes, data_at + 4);
            info.bits_per_sample = read_u16le(bytes, data_at + 14);
            have_fmt = true;
        }
        else if (id == "data")
        {
            info.data_offset = data_at;
            info.data_size = static_cast<std::size_t>(size);
            have_data = true;
        }

        // RIFF chunks are word-aligned: an odd size carries a trailing pad byte.
        pos = data_at + static_cast<std::size_t>(size) + (size & 1U);
    }

    if (!have_fmt)
    {
        fail(diagnostics, "import.source_malformed", "WAV has no fmt chunk");
        return false;
    }
    if (!have_data)
    {
        fail(diagnostics, "import.source_malformed", "WAV has no data chunk");
        return false;
    }
    if (info.format != 1) // WAVE_FORMAT_PCM; EXTENSIBLE / float / ADPCM are v1-unsupported
    {
        fail(diagnostics, "import.unsupported_format",
             "WAV format " + std::to_string(info.format) + " is not PCM (unsupported in v1)");
        return false;
    }
    if (info.channels == 0 || info.bits_per_sample == 0 || (info.bits_per_sample % 8U) != 0)
    {
        fail(diagnostics, "import.source_malformed", "WAV has invalid PCM channel/bit-depth values");
        return false;
    }
    const std::uint32_t block = static_cast<std::uint32_t>(info.channels) * (info.bits_per_sample / 8U);
    if (block == 0 || (info.data_size % block) != 0)
    {
        fail(diagnostics, "import.source_malformed", "WAV data size is not frame-aligned");
        return false;
    }
    info.frame_count = info.data_size / block;
    return true;
}

std::uint32_t WavImporter::derived_format_version(ArtifactKind kind) const noexcept
{
    return kind == ArtifactKind::audio ? 1U : 0U;
}

ImportResult WavImporter::import(const ImportInput& input) const
{
    ImportResult result;
    WavInfo info;
    if (!parse_wav(input.source_bytes, info, result.diagnostics))
    {
        result.ok = false;
        return result;
    }

    detail::JsonValue desc = jobject();
    put(desc, "kind", jstr("audio"));
    put(desc, "source", jstr("wav"));
    put(desc, "format", jstr("pcm"));
    put(desc, "channels", juint(info.channels));
    put(desc, "sampleRate", juint(info.sample_rate));
    put(desc, "bitsPerSample", juint(info.bits_per_sample));
    put(desc, "frameCount", juint(info.frame_count));

    detail::JsonValue transcode = jobject();
    put(transcode, "platform", jstr(input.platform.id));
    const TranscodeTarget* target = transcode_target_for(ArtifactKind::audio, input.platform.id);
    put(transcode, "format", jstr(target != nullptr ? target->format : std::string("unmapped")));
    put(desc, "transcode", std::move(transcode));

    std::string canonical;
    if (!serializer::serialize_canonical(desc, canonical))
    {
        result.ok = false;
        result.diagnostics.push_back(
            {"import.decode_failed", "failed to serialize the audio descriptor"});
        return result;
    }

    DerivedArtifact descriptor;
    descriptor.kind = ArtifactKind::audio;
    descriptor.name = "audio";
    descriptor.bytes = std::move(canonical);
    descriptor.derived_format_version = derived_format_version(ArtifactKind::audio);
    result.artifacts.push_back(std::move(descriptor));

    // The PCM payload — WAV `data` IS raw PCM, so this is a COMPLETE import (no deferred decode). A
    // sibling artifact of the same kind, distinguished by name (keyed apart via ImportCacheKey).
    DerivedArtifact pcm;
    pcm.kind = ArtifactKind::audio;
    pcm.name = "audio.pcm";
    pcm.bytes = std::string(input.source_bytes.substr(info.data_offset, info.data_size));
    pcm.derived_format_version = derived_format_version(ArtifactKind::audio);
    result.artifacts.push_back(std::move(pcm));

    result.ok = true;
    return result;
}

} // namespace context::editor::import
