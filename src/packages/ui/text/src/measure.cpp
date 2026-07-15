// The run-based measure seam (a7: metrics-only plumbing). Maps each decoded UTF-8 codepoint to its
// glyph id and sums advances into ONE run; the run-based, glyph-id + GPOS-offset return type is what
// lets a8's shaper replace these internals with no signature change. No shaping/kerning/bidi here.

#include "context/packages/ui/text/measure.h"

#include <utility>

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

std::vector<ShapedRun> measure(const FontFace& font, std::string_view utf8, float pixel_size)
{
    ShapedRun run;
    run.font = font.identity();
    run.pixel_size = pixel_size;
    run.metrics = font.metrics(pixel_size);

    float width = 0.0f;
    std::size_t pos = 0;
    while (pos < utf8.size())
    {
        const std::size_t start = pos;
        const char32_t cp = decode_utf8(utf8, pos);
        const GlyphId glyph = font.glyph_index(cp);
        const float adv = font.advance(glyph, pixel_size);

        GlyphMetrics gm;
        gm.glyph = glyph;
        gm.advance = adv;
        gm.cluster = static_cast<std::uint32_t>(start);
        run.glyphs.push_back(gm);
        width += adv;
    }
    run.width = width;

    return std::vector<ShapedRun>{std::move(run)};
}

} // namespace context::packages::ui::text
