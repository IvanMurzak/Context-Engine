// Per-platform asset transcode (see transcode.h): the byte-level encoders behind the M2 transcode
// skeleton. Pure + deterministic + total — the R-ASSET-001 double-run gate holds, and the variant
// rides the existing R-FILE-010 per-platform cache key.

#include "context/editor/import/transcode.h"

#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_tree.h"

#include "detail/json_detail.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace context::editor::import
{
namespace
{
namespace serializer = context::editor::serializer;
using serializer::JsonValue;

// --- little-endian writers (a fixed byte order ⇒ identical variant bytes on every host) -----------

void put_u32le(std::string& out, std::uint32_t v)
{
    out.push_back(static_cast<char>(v & 0xFFU));
    out.push_back(static_cast<char>((v >> 8) & 0xFFU));
    out.push_back(static_cast<char>((v >> 16) & 0xFFU));
    out.push_back(static_cast<char>((v >> 24) & 0xFFU));
}

// A length-prefixed string (u32 length + raw bytes) so component boundaries never collide.
void put_str(std::string& out, std::string_view s)
{
    put_u32le(out, static_cast<std::uint32_t>(s.size()));
    out.append(s.data(), s.size());
}

// The self-describing "CTXV" transcoded-variant container (frozen v1, see transcode.h): magic +
// container version + kind + platform + target format + applied encoding + the per-format payload.
[[nodiscard]] std::string build_container(ArtifactKind kind, std::string_view platform_id,
                                          std::string_view target_format,
                                          std::string_view applied_encoding,
                                          std::string_view payload)
{
    std::string out;
    out.append("CTXV", 4);
    put_u32le(out, kTranscodeContainerVersion);
    put_u32le(out, static_cast<std::uint32_t>(kind));
    put_str(out, platform_id);
    put_str(out, target_format);
    put_str(out, applied_encoding);
    put_u32le(out, static_cast<std::uint32_t>(payload.size()));
    out.append(payload.data(), payload.size());
    return out;
}

// --- descriptor readers -------------------------------------------------------------------------

// A boolean member read (default `fallback` for a missing / wrong-typed member).
[[nodiscard]] bool read_bool(const JsonValue& obj, std::string_view key, bool fallback)
{
    const JsonValue* v = detail::member(obj, key);
    if (v != nullptr && v->type == JsonValue::Type::boolean)
        return v->boolean_value;
    return fallback;
}

// A non-negative dimension read; 0 when absent / non-positive (the caller treats 0 as malformed).
[[nodiscard]] std::uint32_t read_dim(const JsonValue& obj, std::string_view key)
{
    const std::int64_t n = detail::as_int64(detail::member(obj, key), 0);
    if (n <= 0 || n > 0xFFFFFFFFLL)
        return 0;
    return static_cast<std::uint32_t>(n);
}

// --- texture: a real BCn/ASTC block-tiled variant container -------------------------------------

// The block geometry of a target texture format. ASTC blocks vary (astc_8x8 = 8×8); every BCn block
// is 4×4. Both encode a block in 16 bytes in v1 (BC7 + ASTC are 128-bit blocks). A block dim of 0
// marks an unknown format (never guessed — the caller surfaces it).
struct BlockGeometry
{
    std::uint32_t block_w = 0;
    std::uint32_t block_h = 0;
    std::uint32_t bytes_per_block = 16;
};

[[nodiscard]] BlockGeometry block_geometry_for(std::string_view format)
{
    BlockGeometry g;
    if (format.rfind("bc", 0) == 0) // bc1/bc3/bc7 … — the BCn family is 4×4
    {
        g.block_w = 4;
        g.block_h = 4;
        g.bytes_per_block = 16; // BC7 (v1) — 128-bit blocks
        return g;
    }
    if (format.rfind("astc_", 0) == 0) // astc_WxH — parse the block footprint from the id
    {
        const std::string_view dims = format.substr(5);
        const std::size_t x = dims.find('x');
        if (x != std::string_view::npos)
        {
            std::uint32_t w = 0;
            std::uint32_t h = 0;
            for (char c : dims.substr(0, x))
                if (c >= '0' && c <= '9')
                    w = w * 10U + static_cast<std::uint32_t>(c - '0');
            for (char c : dims.substr(x + 1))
                if (c >= '0' && c <= '9')
                    h = h * 10U + static_cast<std::uint32_t>(c - '0');
            g.block_w = w;
            g.block_h = h;
            g.bytes_per_block = 16; // ASTC blocks are always 128-bit regardless of footprint
        }
        return g;
    }
    return g; // unknown → block_w == 0
}

[[nodiscard]] std::uint32_t blocks_along(std::uint32_t extent, std::uint32_t block)
{
    return (extent + block - 1U) / block; // ceil-div; block >= 1 guaranteed by the caller
}

// The texture variant payload: the real block-tiled sizing PLAN for the target format — a mip
// pyramid, each level's block grid + the byte length its encoded (BCn/ASTC) form will occupy. Bytes
// are a deterministic function of (width, height, srgb, mipmaps, block footprint), so bc7 (4×4) and
// astc_8x8 (8×8) produce genuinely different variant bytes — the per-platform payload the pack
// selects. The perceptual texel encoder (the actual compressed pixels) awaits the deferred PNG texel
// DEFLATE-decode (import/README.md deferral #1) — this stakes out the container + geometry, honestly.
[[nodiscard]] bool encode_texture_payload(const JsonValue& descriptor, const BlockGeometry& g,
                                          std::string& out)
{
    const std::uint32_t width = read_dim(descriptor, "width");
    const std::uint32_t height = read_dim(descriptor, "height");
    if (width == 0 || height == 0 || g.block_w == 0 || g.block_h == 0)
        return false;
    const bool srgb = read_bool(descriptor, "srgb", true);
    const bool mipmaps = read_bool(descriptor, "mipmaps", true);

    // The mip chain: full pyramid down to 1×1 when mipmaps are requested, else the base level only.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> mips;
    std::uint32_t w = width;
    std::uint32_t h = height;
    for (;;)
    {
        mips.emplace_back(w, h);
        if (!mipmaps || (w == 1U && h == 1U))
            break;
        w = w > 1U ? w / 2U : 1U;
        h = h > 1U ? h / 2U : 1U;
    }

    put_u32le(out, width);
    put_u32le(out, height);
    out.push_back(static_cast<char>(srgb ? 1 : 0));
    put_u32le(out, g.block_w);
    put_u32le(out, g.block_h);
    put_u32le(out, g.bytes_per_block);
    put_u32le(out, static_cast<std::uint32_t>(mips.size()));
    for (const auto& mip : mips)
    {
        const std::uint32_t bx = blocks_along(mip.first, g.block_w);
        const std::uint32_t by = blocks_along(mip.second, g.block_h);
        put_u32le(out, mip.first);
        put_u32le(out, mip.second);
        put_u32le(out, bx);
        put_u32le(out, by);
        put_u32le(out, bx * by * g.bytes_per_block); // the encoded byte length this level occupies
    }
    return true;
}

// --- audio: verbatim PCM16 (desktop) vs G.711 mu-law companding (memory-constrained Web) ---------

// The canonical Sun/ITU-T G.711 µ-law encoder (14-bit intermediate) of one 16-bit linear PCM sample →
// one 8-bit code. The standard, deterministic, dependency-free 2:1 audio compression — a real
// transcode for the memory-constrained Web ceiling, not a stub. Works in `int`, so the INT16_MIN
// negation cannot overflow; C++20 makes the signed `>> 2` a defined arithmetic shift.
[[nodiscard]] std::uint8_t linear_to_mulaw(std::int16_t pcm) noexcept
{
    static constexpr int kSegEnd[8] = {0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF};
    int val = pcm >> 2; // 16-bit linear → the 14-bit G.711 intermediate
    int mask = 0xFF;    // sign bit into the complemented code (0xFF positive, 0x7F negative)
    if (val < 0)
    {
        val = -val;
        mask = 0x7F;
    }
    if (val > 8159) // clip to the 14-bit µ-law range
        val = 8159;
    val += 0x84 >> 2; // the G.711 bias (33 at 14-bit scale)
    int seg = 8;
    for (int i = 0; i < 8; ++i)
        if (val <= kSegEnd[i])
        {
            seg = i;
            break;
        }
    if (seg >= 8)
        return static_cast<std::uint8_t>(0x7F ^ mask);
    const int uval = (seg << 4) | ((val >> (seg + 1)) & 0x0F);
    return static_cast<std::uint8_t>(uval ^ mask);
}

// The audio variant payload: [sampleFormat u8][samples]. sampleFormat 0 = verbatim little-endian
// 16-bit PCM (the complete desktop `pcm16` transcode — WAV `data` IS raw PCM); 1 = µ-law-companded
// 8-bit (the Web target's real 2:1 compression). µ-law needs a 16-bit-aligned payload; a non-aligned
// PCM (never produced by the v1 WAV importer) falls back to verbatim, recorded honestly.
enum class AudioSampleFormat : std::uint8_t
{
    pcm16 = 0,
    mulaw8 = 1,
};

void encode_audio_payload(std::string_view pcm, bool compress, std::string& out,
                          std::string& applied_encoding)
{
    if (compress && !pcm.empty() && (pcm.size() % 2U) == 0U)
    {
        out.push_back(static_cast<char>(AudioSampleFormat::mulaw8));
        for (std::size_t i = 0; i + 1U < pcm.size(); i += 2U)
        {
            const auto lo = static_cast<std::uint8_t>(pcm[i]);
            const auto hi = static_cast<std::uint8_t>(pcm[i + 1U]);
            const auto sample = static_cast<std::int16_t>(static_cast<std::uint16_t>(lo) |
                                                          (static_cast<std::uint16_t>(hi) << 8));
            out.push_back(static_cast<char>(linear_to_mulaw(sample)));
        }
        applied_encoding = "mulaw8";
        return;
    }
    out.push_back(static_cast<char>(AudioSampleFormat::pcm16));
    out.append(pcm.data(), pcm.size());
    applied_encoding = "pcm16";
}

[[nodiscard]] bool name_is_pcm_payload(std::string_view name) noexcept
{
    return name.find("pcm") != std::string_view::npos;
}

} // namespace

TranscodeResult transcode_variant(const DerivedArtifact& artifact, const PlatformProfile& platform)
{
    TranscodeResult result;

    const TranscodeTarget* target = transcode_target_for(artifact.kind, platform.id);
    if (target == nullptr)
    {
        result.error = "transcode.no_target";
        return result;
    }

    std::string payload;
    std::string applied_encoding;

    switch (artifact.kind)
    {
    case ArtifactKind::texture:
    {
        serializer::CanonicalizeResult parsed = serializer::canonicalize(artifact.bytes);
        if (!parsed.is_json)
        {
            result.error = "transcode.bad_descriptor";
            return result;
        }
        const BlockGeometry g = block_geometry_for(target->format);
        if (!encode_texture_payload(parsed.root, g, payload))
        {
            result.error = "transcode.bad_descriptor";
            return result;
        }
        applied_encoding = "block_plan";
        break;
    }
    case ArtifactKind::audio:
    {
        if (name_is_pcm_payload(artifact.name))
        {
            // A memory-constrained target compresses (µ-law); a desktop target keeps verbatim PCM.
            // Driven by the TARGET FORMAT (table), not the flag directly: pcm16 → verbatim, else compress.
            const bool compress = target->format != "pcm16";
            encode_audio_payload(artifact.bytes, compress, payload, applied_encoding);
        }
        else
        {
            // An audio DESCRIPTOR artifact — metadata, carried through format-tagged (platform-neutral
            // bytes; the payload variant above is what differs per platform).
            payload = artifact.bytes;
            applied_encoding = "passthrough";
        }
        break;
    }
    case ArtifactKind::mesh:
    {
        // Mesh is `meshopt` on every v1 target (platform-invariant): the descriptor IS the v1 meshopt
        // plan; the vertex-stream quantizer is the tracked follow-up (import/README.md deferral #1).
        payload = artifact.bytes;
        applied_encoding = "meshopt_plan";
        break;
    }
    }

    TranscodedVariant& variant = result.variant;
    variant.platform_id = platform.id;
    variant.target_format = target->format;
    variant.applied_encoding = std::move(applied_encoding);
    variant.bytes = build_container(artifact.kind, platform.id, target->format,
                                    variant.applied_encoding, payload);
    variant.content_hash = serializer::canonical_hash_of(variant.bytes);
    result.ok = true;
    return result;
}

ImportCacheKey transcode_cache_key(const ImportKeyContext& base_context,
                                   const DerivedArtifact& artifact, const PlatformProfile& platform)
{
    const TranscodeTarget* target = transcode_target_for(artifact.kind, platform.id);
    const std::string format = target != nullptr ? target->format : std::string("unmapped");

    // Ride the import key: the platform sets the existing platform_profile component (reuse, don't
    // fork), and the variant keys apart from the base artifact by "<name>#<target_format>" — so per
    // platform + per target the variant lands in its own content-addressed entry (R-FILE-010).
    ImportKeyContext ctx = base_context;
    ctx.platform_profile = platform.id;

    // Only the kind / name / derived-format version reach the key (make_cache_key never reads the
    // payload — the source's bytes enter via ctx.source_bytes_hash), so build the naming stand-in
    // rather than copying the artifact and its payload.
    DerivedArtifact variant_artifact;
    variant_artifact.kind = artifact.kind;
    variant_artifact.name = artifact.name + "#" + format;
    variant_artifact.derived_format_version = artifact.derived_format_version;
    return make_cache_key(ctx, variant_artifact);
}

} // namespace context::editor::import
