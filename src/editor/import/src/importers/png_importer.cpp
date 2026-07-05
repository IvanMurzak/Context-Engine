// PNG importer — full container walk (signature, CRC-32-verified chunks, IHDR geometry) into a
// canonical texture descriptor. The texel decode/encode is the transcode follow-up (see header).

#include "context/editor/import/importers/png_importer.h"

#include "context/editor/import/platform_profile.h"
#include "context/editor/serializer/canonical.h"

#include "../detail/json_detail.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace context::editor::import
{
namespace
{
using detail::jbool;
using detail::jobject;
using detail::jstr;
using detail::juint;
using detail::put;

// CRC-32 (ISO 3309, the PNG chunk CRC): reflected, poly 0xEDB88320, init/final 0xFFFFFFFF. A tiny
// table built once — deterministic, no dependency. Verifying it makes the importer robust to
// corruption + a meaningful fuzz target (a bit-flip in a chunk is caught, not silently imported).
std::uint32_t crc32(std::string_view data) noexcept
{
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i)
        {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1U) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t crc = 0xFFFFFFFFU;
    for (char ch : data)
        crc = table[(crc ^ static_cast<std::uint8_t>(ch)) & 0xFFU] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFU;
}

std::uint32_t read_u32be(std::string_view b, std::size_t at) noexcept
{
    return (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at])) << 24) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at + 1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at + 2])) << 8) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at + 3])));
}

void fail(std::vector<ImportDiagnostic>& diagnostics, std::string code, std::string message)
{
    diagnostics.push_back({std::move(code), std::move(message)});
}
} // namespace

bool parse_png(std::string_view bytes, PngInfo& info, std::vector<ImportDiagnostic>& diagnostics)
{
    static constexpr unsigned char kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (bytes.size() < 8 || std::memcmp(bytes.data(), kSignature, 8) != 0)
    {
        fail(diagnostics, "import.source_malformed", "not a PNG (bad 8-byte signature)");
        return false;
    }

    std::size_t pos = 8;
    bool seen_ihdr = false;
    bool seen_iend = false;
    while (pos + 8 <= bytes.size())
    {
        const std::uint32_t len = read_u32be(bytes, pos);
        const std::string_view type = bytes.substr(pos + 4, 4);
        const std::size_t data_at = pos + 8;
        // Bounds: data + 4-byte CRC must fit. size_t math (len promoted) never overflows on 64-bit.
        if (bytes.size() - data_at < static_cast<std::size_t>(len) + 4)
        {
            fail(diagnostics, "import.source_malformed", "truncated PNG chunk");
            return false;
        }
        const std::uint32_t crc_stored = read_u32be(bytes, data_at + len);
        const std::uint32_t crc_calc = crc32(bytes.substr(pos + 4, static_cast<std::size_t>(len) + 4));
        if (crc_calc != crc_stored)
        {
            fail(diagnostics, "import.decode_failed",
                 "PNG chunk CRC mismatch (" + detail::ascii_or_hex(type) + ")");
            return false;
        }

        if (type == "IHDR")
        {
            if (seen_ihdr || pos != 8)
            {
                fail(diagnostics, "import.source_malformed", "IHDR must be the single first chunk");
                return false;
            }
            if (len != 13)
            {
                fail(diagnostics, "import.source_malformed", "IHDR length is not 13");
                return false;
            }
            info.width = read_u32be(bytes, data_at);
            info.height = read_u32be(bytes, data_at + 4);
            info.bit_depth = static_cast<std::uint8_t>(bytes[data_at + 8]);
            info.color_type = static_cast<std::uint8_t>(bytes[data_at + 9]);
            info.interlace = static_cast<std::uint8_t>(bytes[data_at + 12]);
            if (info.width == 0 || info.height == 0)
            {
                fail(diagnostics, "import.source_malformed", "PNG has a zero dimension");
                return false;
            }
            switch (info.color_type)
            {
            case 0: // grayscale
                info.channels = 1;
                info.has_alpha = false;
                break;
            case 2: // truecolor RGB
                info.channels = 3;
                info.has_alpha = false;
                break;
            case 3: // palette (one index channel)
                info.channels = 1;
                info.has_alpha = false;
                break;
            case 4: // grayscale + alpha
                info.channels = 2;
                info.has_alpha = true;
                break;
            case 6: // truecolor RGBA
                info.channels = 4;
                info.has_alpha = true;
                break;
            default:
                fail(diagnostics, "import.source_malformed", "unknown PNG color type");
                return false;
            }
            seen_ihdr = true;
        }
        else if (type == "IDAT")
        {
            if (!seen_ihdr)
            {
                fail(diagnostics, "import.source_malformed", "IDAT before IHDR");
                return false;
            }
        }
        else if (type == "sRGB")
        {
            info.srgb = true;
        }
        else if (type == "IEND")
        {
            seen_iend = true;
        }

        pos = data_at + len + 4;
        if (seen_iend)
            break;
    }

    if (!seen_ihdr)
    {
        fail(diagnostics, "import.source_malformed", "PNG has no IHDR chunk");
        return false;
    }
    if (!seen_iend)
    {
        fail(diagnostics, "import.source_malformed", "PNG has no IEND chunk");
        return false;
    }
    return true;
}

std::uint32_t PngImporter::derived_format_version(ArtifactKind kind) const noexcept
{
    return kind == ArtifactKind::texture ? 1U : 0U;
}

ImportResult PngImporter::import(const ImportInput& input) const
{
    ImportResult result;
    PngInfo info;
    if (!parse_png(input.source_bytes, info, result.diagnostics))
    {
        result.ok = false;
        return result;
    }

    detail::JsonValue desc = jobject();
    put(desc, "kind", jstr("texture"));
    put(desc, "source", jstr("png"));
    put(desc, "width", juint(info.width));
    put(desc, "height", juint(info.height));
    put(desc, "bitDepth", juint(info.bit_depth));
    put(desc, "colorType", juint(info.color_type));
    put(desc, "channels", juint(info.channels));
    put(desc, "hasAlpha", jbool(info.has_alpha));
    put(desc, "interlaced", jbool(info.interlace != 0));
    put(desc, "srgb", jbool(input.settings.srgb));              // engine treatment (import setting)
    put(desc, "sourceSrgbChunk", jbool(info.srgb));             // provenance (the file's own intent)
    put(desc, "mipmaps", jbool(input.settings.generate_mipmaps)); // policy for the transcode stage

    detail::JsonValue transcode = jobject();
    put(transcode, "platform", jstr(input.platform.id));
    const TranscodeTarget* target = transcode_target_for(ArtifactKind::texture, input.platform.id);
    put(transcode, "format", jstr(target != nullptr ? target->format : std::string("unmapped")));
    put(desc, "transcode", std::move(transcode));

    std::string canonical;
    if (!serializer::serialize_canonical(desc, canonical))
    {
        result.ok = false;
        result.diagnostics.push_back(
            {"import.decode_failed", "failed to serialize the texture descriptor"});
        return result;
    }

    DerivedArtifact artifact;
    artifact.kind = ArtifactKind::texture;
    artifact.name = "texture";
    artifact.bytes = std::move(canonical);
    artifact.derived_format_version = derived_format_version(ArtifactKind::texture);
    result.artifacts.push_back(std::move(artifact));
    result.ok = true;
    return result;
}

} // namespace context::editor::import
