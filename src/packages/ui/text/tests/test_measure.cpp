// ui-test_measure — the UTF-8 decoder + the run-based measure() seam. Pins the a7 contract the a8
// shaper must preserve: measure() returns RUNS (one on a7), glyph-id keyed, with per-glyph advances +
// zero GPOS offsets + byte-offset clusters; empty input still yields a metrics-bearing run (R-QA-013).

#include "context/packages/ui/text/embedded_fonts.h"
#include "context/packages/ui/text/measure.h"

#include "ui_test.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

using namespace context::packages::ui::text;

namespace
{
char32_t decode_one(std::string_view s)
{
    std::size_t pos = 0;
    return decode_utf8(s, pos);
}
bool approx(float a, float b)
{
    const float d = a - b;
    return (d < 0.0f ? -d : d) < 1e-3f;
}
} // namespace

int main()
{
    // --- decode_utf8 ------------------------------------------------------------------------------
    {
        std::size_t pos = 0;
        CHECK(decode_utf8("A", pos) == U'A');
        CHECK(pos == 1);
    }
    CHECK(decode_one("\xC3\xA9") == 0x00E9u);         // é  (2-byte)
    CHECK(decode_one("\xE2\x82\xAC") == 0x20ACu);     // €  (3-byte)
    CHECK(decode_one("\xF0\x9F\x98\x80") == 0x1F600u); // 😀 (4-byte)
    {
        std::size_t pos = 0; // invalid lead byte -> U+FFFD, one byte consumed
        CHECK(decode_utf8("\xFF", pos) == 0xFFFDu);
        CHECK(pos == 1);
    }
    {
        std::size_t pos = 0; // truncated 3-byte sequence
        CHECK(decode_utf8("\xE2\x82", pos) == 0xFFFDu);
        CHECK(pos == 1);
    }
    CHECK(decode_one("\xC0\xAF") == 0xFFFDu);         // overlong '/'
    CHECK(decode_one("\xED\xA0\x80") == 0xFFFDu);     // UTF-8-encoded surrogate U+D800

    // --- measure() --------------------------------------------------------------------------------
    auto font = FontFace::from_memory(noto_sans_regular());
    CHECK(font.has_value());
    if (font)
    {
        auto runs = measure(*font, "Hello", 16.0f);
        CHECK(runs.size() == 1); // a7: exactly one run (no bidi/script segmentation yet)
        const ShapedRun& r = runs[0];
        CHECK(r.font == font->identity());
        CHECK(r.pixel_size == 16.0f);
        CHECK(!r.rtl);
        CHECK(r.glyphs.size() == 5);
        CHECK(r.metrics.line_height > 0.0f);

        float sum = 0.0f;
        for (const GlyphMetrics& g : r.glyphs)
        {
            CHECK(g.glyph != kMissingGlyph);
            CHECK(g.x_offset == 0.0f); // a7: no GPOS
            CHECK(g.y_offset == 0.0f);
            sum += g.advance;
        }
        CHECK(r.width > 0.0f);
        CHECK(approx(r.width, sum));

        // byte-offset clusters 0..4 for the ASCII string
        for (std::uint32_t i = 0; i < 5; ++i)
            CHECK(r.glyphs[i].cluster == i);

        // "Hello": the two 'l's are the SAME glyph id (glyph-id-keyed consistency)
        CHECK(r.glyphs[2].glyph == r.glyphs[3].glyph);

        // empty input -> a single empty run that still carries vertical metrics
        auto empty = measure(*font, "", 16.0f);
        CHECK(empty.size() == 1);
        CHECK(empty[0].glyphs.empty());
        CHECK(empty[0].width == 0.0f);
        CHECK(empty[0].metrics.line_height > 0.0f);

        // multibyte clusters: "café" = c(0) a(1) f(2) é(byte 3) -> 4 glyphs, last cluster at byte 3
        auto cafe = measure(*font, "caf\xC3\xA9", 16.0f);
        CHECK(cafe.size() == 1);
        CHECK(cafe[0].glyphs.size() == 4);
        CHECK(cafe[0].glyphs[3].cluster == 3);
    }

    UI_TEST_MAIN_END();
}
