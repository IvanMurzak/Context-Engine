// Per-platform asset transcode (see transcode.h): the byte-level encoders behind the M2 transcode
// skeleton. Pure + deterministic + total — the R-ASSET-001 double-run gate holds, and the variant
// rides the existing R-FILE-010 per-platform cache key.

#include "context/editor/import/transcode.h"

#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_tree.h"

#include "detail/json_detail.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace context::editor::import
{
namespace
{
namespace serializer = context::editor::serializer;
using serializer::JsonValue;

// --- little-endian writers (a fixed byte order ⇒ identical variant bytes on every host) -----------

using detail::put_u32le; // the module's shared LE writer (detail/json_detail.h)

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
    // Every v1 target format is a 128-bit block (BC7 and ASTC alike, whatever the ASTC footprint),
    // so this is a constant rather than a per-branch assignment. A future 64-bit-block format (bc1)
    // makes it a real branch at the point it stops being constant.
    std::uint32_t bytes_per_block = 16;
};

[[nodiscard]] BlockGeometry block_geometry_for(std::string_view format)
{
    BlockGeometry g;
    if (format.rfind("bc", 0) == 0) // bc1/bc3/bc7 … — the BCn family is 4×4
    {
        g.block_w = 4;
        g.block_h = 4;
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
        }
        return g;
    }
    return g; // unknown → block_w == 0 (an uncompressed/unmapped target, e.g. rgba8)
}

// Ceil-div in 64-bit: `extent + block - 1` WRAPS in 32-bit for an extent near UINT32_MAX (the PNG
// importer validates only width/height != 0, so a crafted IHDR reaches here), which would silently
// report 0 blocks for the largest textures. The quotient always fits u32. block >= 1 per the caller.
[[nodiscard]] std::uint32_t blocks_along(std::uint32_t extent, std::uint32_t block)
{
    return static_cast<std::uint32_t>((static_cast<std::uint64_t>(extent) + block - 1U) / block);
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
    if (width == 0 || height == 0)
        return false;
    const bool srgb = detail::as_bool(detail::member(descriptor, "srgb"), true);
    const bool mipmaps = detail::as_bool(detail::member(descriptor, "mipmaps"), true);

    // The mip chain: full pyramid down to 1×1 when mipmaps are requested, else the base level only.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> mips;
    for (std::uint32_t w = width, h = height;;)
    {
        mips.emplace_back(w, h);
        if (!mipmaps || (w == 1U && h == 1U))
            break;
        w = std::max(w / 2U, 1U);
        h = std::max(h / 2U, 1U);
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
        // The encoded byte length this level occupies. Computed in 64-bit: bx * by * bytes_per_block
        // overflows u32 well before the dimensions do (16384² blocks × 16B already wraps to 0). The
        // v1 container's length field is u32, so a level that genuinely cannot be described is
        // REFUSED (bad_descriptor) rather than recorded as a wrapped, silently-wrong size.
        const std::uint64_t level_bytes =
            static_cast<std::uint64_t>(bx) * by * g.bytes_per_block;
        if (level_bytes > 0xFFFFFFFFULL)
            return false;
        put_u32le(out, mip.first);
        put_u32le(out, mip.second);
        put_u32le(out, bx);
        put_u32le(out, by);
        put_u32le(out, static_cast<std::uint32_t>(level_bytes));
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

// Returns what it ACTUALLY applied, so "every path is labelled" is enforced by the compiler rather
// than by reading both branches.
[[nodiscard]] std::string_view encode_audio_payload(std::string_view pcm, bool compress,
                                                    std::string& out)
{
    if (compress && !pcm.empty() && (pcm.size() % 2U) == 0U)
    {
        // The output size is exact and known: the format byte + one µ-law code per 16-bit sample.
        // Without this a multi-MB PCM buffer pays ~20-34 geometric reallocs and 2-3x its own size in
        // pure memcpy churn on the one path that actually encodes sample-by-sample.
        out.reserve(1U + pcm.size() / 2U);
        out.push_back(static_cast<char>(AudioSampleFormat::mulaw8));
        for (std::size_t i = 0; i + 1U < pcm.size(); i += 2U)
        {
            const auto lo = static_cast<std::uint8_t>(pcm[i]);
            const auto hi = static_cast<std::uint8_t>(pcm[i + 1U]);
            const auto sample = static_cast<std::int16_t>(static_cast<std::uint16_t>(lo) |
                                                          (static_cast<std::uint16_t>(hi) << 8));
            out.push_back(static_cast<char>(linear_to_mulaw(sample)));
        }
        return "mulaw8";
    }
    out.push_back(static_cast<char>(AudioSampleFormat::pcm16));
    out.append(pcm.data(), pcm.size());
    return "pcm16";
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

    // `payload_buf` owns an ENCODED payload; `payload` views whichever buffer the arm settled on, so
    // the arms that merely forward the artifact's bytes (mesh, audio descriptor) copy nothing. Assign
    // the view only once an arm's buffer is final — a view taken of payload_buf up front would be
    // stale on both data() and size().
    std::string payload_buf;
    std::string_view payload;
    std::string_view applied_encoding;

    switch (artifact.kind)
    {
    case ArtifactKind::texture:
    {
        const BlockGeometry g = block_geometry_for(target->format);
        if (g.block_w == 0 || g.block_h == 0)
        {
            // The TABLE named a target this encoder has no block geometry for (e.g. an uncompressed
            // `rgba8` row). That is an unsupported target format, NOT a malformed source asset —
            // surfaced as itself rather than blamed on the descriptor.
            result.error = "transcode.unsupported_format";
            return result;
        }
        serializer::CanonicalizeResult parsed = serializer::canonicalize(artifact.bytes);
        if (!parsed.is_json)
        {
            result.error = "transcode.bad_descriptor";
            return result;
        }
        if (!encode_texture_payload(parsed.root, g, payload_buf))
        {
            result.error = "transcode.bad_descriptor";
            return result;
        }
        payload = payload_buf;
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
            applied_encoding = encode_audio_payload(artifact.bytes, compress, payload_buf);
            payload = payload_buf;
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
    variant.applied_encoding = applied_encoding;
    variant.bytes = build_container(artifact.kind, platform.id, target->format,
                                    variant.applied_encoding, payload);
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

    // The variant's derived format is the CTXV container, so BOTH the source artifact's
    // derived-format version AND the container encoding version determine the variant bytes — pack
    // them injectively (16 bits each) into the single derived-format-version component. Folding the
    // container version is LOAD-BEARING, not decorative: build_container() writes it into the bytes,
    // and nothing else in the key covers it (importer_build_hash is a constant fold of the epoch +
    // TOOLCHAIN stamp, not a per-build stamp — see cache_key.cpp). Without this a container bump
    // would leave the digest unchanged while the bytes changed, and the L-28 shared cache would
    // serve stale containers — the exact unsoundness R-FILE-010 forbids.
    static_assert(kTranscodeContainerVersion <= 0xFFFFU, "container version must fit the low 16 bits");
    variant_artifact.derived_format_version =
        (artifact.derived_format_version << 16) | (kTranscodeContainerVersion & 0xFFFFU);
    return make_cache_key(ctx, variant_artifact);
}

} // namespace context::editor::import
