// The run-based measure seam (a8: real shaping). Segments the input into per-(bidi-level, script) runs
// in VISUAL order (SheenBidi UAX #9 bidi + UAX #24 script itemization), shapes each with HarfBuzz (the
// OpenType shaper — ligatures, marks, cursive joining, GPOS positioning), and returns them left-to-right.
// The run-based, glyph-id + GPOS-offset return type is a7's; a8 fills it with real shaped output. The
// glyph ids are the font's native indices (shared by HarfBuzz + FreeType), so they key the atlas + the
// FreeType rasterizer unchanged. Line breaking / wrapping lives in line_break.cpp.

#include "context/packages/ui/text/measure.h"

#include <SheenBidi/SheenBidi.h>
#include <hb.h>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace context::packages::ui::text
{

char32_t decode_utf8(std::string_view s, std::size_t& pos)
{
    constexpr char32_t kReplacement = 0xFFFDu;
    if (pos >= s.size())
    {
        ++pos;
        return kReplacement;
    }

    const auto b0 = static_cast<unsigned char>(s[pos]);
    if (b0 < 0x80u)
    {
        ++pos;
        return static_cast<char32_t>(b0);
    }

    int len = 0;
    char32_t cp = 0;
    char32_t min = 0;
    if ((b0 & 0xE0u) == 0xC0u)
    {
        len = 2;
        cp = b0 & 0x1Fu;
        min = 0x80u;
    }
    else if ((b0 & 0xF0u) == 0xE0u)
    {
        len = 3;
        cp = b0 & 0x0Fu;
        min = 0x800u;
    }
    else if ((b0 & 0xF8u) == 0xF0u)
    {
        len = 4;
        cp = b0 & 0x07u;
        min = 0x10000u;
    }
    else
    {
        ++pos; // invalid lead byte — consume one, keep making progress
        return kReplacement;
    }

    if (pos + static_cast<std::size_t>(len) > s.size())
    {
        ++pos; // truncated tail
        return kReplacement;
    }
    for (int i = 1; i < len; ++i)
    {
        const auto bi = static_cast<unsigned char>(s[pos + static_cast<std::size_t>(i)]);
        if ((bi & 0xC0u) != 0x80u)
        {
            ++pos; // invalid continuation byte
            return kReplacement;
        }
        cp = (cp << 6) | (bi & 0x3Fu);
    }
    if (cp < min || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu))
    {
        ++pos; // overlong, out-of-range, or surrogate
        return kReplacement;
    }
    pos += static_cast<std::size_t>(len);
    return cp;
}

namespace
{

// The integer pixel size FreeType rasterizes at (font.cpp's to_pixel_size), so the shaper's advances +
// the glyph atlas agree on size. HarfBuzz's scale is this in 26.6 fixed point.
int quantize_px(float pixel_size) noexcept
{
    long r = std::lround(pixel_size);
    if (r < 1)
        r = 1;
    return static_cast<int>(r);
}

// Shape ONE single-direction, single-script byte segment [begin,end) of `utf8` with HarfBuzz into a
// ShapedRun. Clusters are re-based to ABSOLUTE byte offsets into `utf8` (HarfBuzz reports them relative
// to the sub-slice). `hb` is the face's cached shaper; its scale is set here.
ShapedRun shape_segment(hb_font_t* hb, const FontFace& font, std::string_view utf8, std::size_t begin,
                        std::size_t end, int px, bool rtl, float pixel_size)
{
    ShapedRun run;
    run.font = font.identity();
    run.pixel_size = pixel_size;
    run.metrics = font.metrics(pixel_size);
    run.rtl = rtl;

    hb_font_set_scale(hb, px * 64, px * 64);

    hb_buffer_t* buf = hb_buffer_create();
    // Pass only the sub-slice (its clusters come out 0-based; we add `begin` for absolute offsets). Script
    // boundaries are also joining boundaries, so no cross-segment shaping context is lost.
    hb_buffer_add_utf8(buf, utf8.data() + begin, static_cast<int>(end - begin), 0,
                       static_cast<int>(end - begin));
    // Direction is authoritative from the bidi level; script/language guessed from content. Guess first
    // (it would otherwise overwrite direction), then override direction so a neutral/number in an RTL run
    // is placed by its resolved level, not its intrinsic script direction.
    hb_buffer_guess_segment_properties(buf);
    hb_buffer_set_direction(buf, rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
    hb_shape(hb, buf, nullptr, 0);

    const unsigned int n = hb_buffer_get_length(buf);
    const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, nullptr);
    const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, nullptr);
    run.glyphs.reserve(n);
    float width = 0.0f;
    for (unsigned int i = 0; i < n; ++i)
    {
        GlyphMetrics gm;
        gm.glyph = static_cast<GlyphId>(info[i].codepoint); // HarfBuzz gid == font's native gid
        gm.advance = static_cast<float>(pos[i].x_advance) / 64.0f;
        gm.x_offset = static_cast<float>(pos[i].x_offset) / 64.0f;
        gm.y_offset = static_cast<float>(pos[i].y_offset) / 64.0f; // HB y-up == our baseline-up offset
        gm.cluster = static_cast<std::uint32_t>(begin + info[i].cluster);
        run.glyphs.push_back(gm);
        width += gm.advance;
    }
    run.width = width;
    hb_buffer_destroy(buf);
    return run;
}

// A fallback single LTR run mapping each codepoint to its glyph id (a7 behavior), used only if HarfBuzz
// rejects the face — so text still lays out (no shaping/bidi) instead of vanishing.
ShapedRun fallback_run(const FontFace& font, std::string_view utf8, float pixel_size)
{
    ShapedRun run;
    run.font = font.identity();
    run.pixel_size = pixel_size;
    run.metrics = font.metrics(pixel_size);
    float width = 0.0f;
    std::size_t p = 0;
    while (p < utf8.size())
    {
        const std::size_t start = p;
        const char32_t cp = decode_utf8(utf8, p);
        const GlyphId glyph = font.glyph_index(cp);
        GlyphMetrics gm;
        gm.glyph = glyph;
        gm.advance = font.advance(glyph, pixel_size);
        gm.cluster = static_cast<std::uint32_t>(start);
        run.glyphs.push_back(gm);
        width += gm.advance;
    }
    run.width = width;
    return run;
}

} // namespace

std::vector<ShapedRun> measure(const FontFace& font, std::string_view utf8, float pixel_size)
{
    const int px = quantize_px(pixel_size);

    // Empty input -> a single empty run that still carries vertical metrics (a7 contract).
    if (utf8.empty())
    {
        ShapedRun run;
        run.font = font.identity();
        run.pixel_size = pixel_size;
        run.metrics = font.metrics(pixel_size);
        return std::vector<ShapedRun>{std::move(run)};
    }

    auto* hb = static_cast<hb_font_t*>(font.hb_font());
    if (hb == nullptr)
        return std::vector<ShapedRun>{fallback_run(font, utf8, pixel_size)};

    const std::size_t len = utf8.size();
    SBCodepointSequence seq{SBStringEncodingUTF8, const_cast<char*>(utf8.data()),
                            static_cast<SBUInteger>(len)};

    // --- UAX #24 script boundaries (logical order) ------------------------------------------------
    // Collect the byte offset where each script run starts (>0), so a bidi run is split at real script
    // changes — HarfBuzz shapes one script per buffer.
    std::vector<std::size_t> script_starts;
    {
        SBScriptLocatorRef loc = SBScriptLocatorCreate();
        SBScriptLocatorLoadCodepoints(loc, &seq);
        const SBScriptAgent* agent = SBScriptLocatorGetAgent(loc);
        while (SBScriptLocatorMoveNext(loc))
        {
            if (agent->offset > 0)
                script_starts.push_back(static_cast<std::size_t>(agent->offset));
        }
        SBScriptLocatorRelease(loc);
    }
    std::sort(script_starts.begin(), script_starts.end());

    // --- UAX #9 bidi visual runs ------------------------------------------------------------------
    SBAlgorithmRef algo = SBAlgorithmCreate(&seq);
    SBParagraphRef para = SBAlgorithmCreateParagraph(algo, 0, static_cast<SBUInteger>(len),
                                                     SBLevelDefaultLTR);
    const SBUInteger para_len = SBParagraphGetLength(para);
    SBLineRef line = SBParagraphCreateLine(para, 0, para_len);
    const SBUInteger run_count = SBLineGetRunCount(line);
    const SBRun* runs = SBLineGetRunsPtr(line);

    std::vector<ShapedRun> out;
    for (SBUInteger r = 0; r < run_count; ++r)
    {
        const std::size_t o = static_cast<std::size_t>(runs[r].offset);
        const std::size_t l = static_cast<std::size_t>(runs[r].length);
        const bool rtl = (runs[r].level & 1u) != 0u;
        const std::size_t run_end = o + l;

        // Split [o, run_end) at the script boundaries falling strictly inside it -> per-script segments.
        std::vector<std::pair<std::size_t, std::size_t>> segs;
        std::size_t s = o;
        for (const std::size_t b : script_starts)
        {
            if (b <= s)
                continue;
            if (b >= run_end)
                break;
            segs.emplace_back(s, b);
            s = b;
        }
        segs.emplace_back(s, run_end);

        // Visual order: LTR runs place segments in logical (ascending) order; RTL runs place them in
        // reverse (the last logical segment is leftmost).
        if (rtl)
            std::reverse(segs.begin(), segs.end());

        for (const auto& [b0, b1] : segs)
        {
            if (b1 > b0)
                out.push_back(shape_segment(hb, font, utf8, b0, b1, px, rtl, pixel_size));
        }
    }

    SBLineRelease(line);
    SBParagraphRelease(para);
    SBAlgorithmRelease(algo);

    // A degenerate paragraph (e.g. all separators) could yield no runs — still return a metrics-bearing
    // empty run so callers can lay out a line.
    if (out.empty())
    {
        ShapedRun run;
        run.font = font.identity();
        run.pixel_size = pixel_size;
        run.metrics = font.metrics(pixel_size);
        out.push_back(std::move(run));
    }
    return out;
}

} // namespace context::packages::ui::text
