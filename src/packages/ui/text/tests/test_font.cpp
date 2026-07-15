// ui-test_font — the FreeType-backed FontFace over the embedded default fonts: load, family name,
// charmap lookup, vertical metrics, advances, and glyph rasterization (coverage + placement + the
// whitespace/empty-bitmap edge). Proves the a7 rasterizer works headless with no GPU (R-QA-013).

#include "context/packages/ui/text/embedded_fonts.h"
#include "context/packages/ui/text/font.h"

#include "ui_test.h"

#include <cstddef>
#include <span>
#include <utility>

using namespace context::packages::ui::text;

int main()
{
    // --- Noto Sans (Latin) ------------------------------------------------------------------------
    auto sans = FontFace::from_memory(noto_sans_regular());
    CHECK(sans.has_value());
    if (sans)
    {
        CHECK(sans->family_name() == "Noto Sans");
        CHECK(sans->glyph_count() > 0);

        const GlyphId a = sans->glyph_index(U'A');
        CHECK(a != kMissingGlyph);
        // A plane-16 private-use codepoint Noto Sans does not map -> .notdef.
        CHECK(sans->glyph_index(static_cast<char32_t>(0x10FFFDu)) == kMissingGlyph);

        const FontMetrics m = sans->metrics(16.0f);
        CHECK(m.ascent > 0.0f);
        CHECK(m.descent < 0.0f); // below the baseline
        CHECK(m.line_height > 0.0f);

        CHECK(sans->advance(a, 16.0f) > 0.0f);
        // Advances scale with pixel size.
        CHECK(sans->advance(a, 32.0f) > sans->advance(a, 16.0f));

        // 'A' rasterizes to an inked coverage bitmap.
        auto bmp = sans->rasterize(a, 32.0f);
        CHECK(bmp.has_value());
        if (bmp)
        {
            CHECK(bmp->width > 0);
            CHECK(bmp->height > 0);
            CHECK(bmp->coverage.size()
                  == static_cast<std::size_t>(bmp->width) * static_cast<std::size_t>(bmp->height));
            CHECK(bmp->advance > 0.0f);
            int ink = 0;
            for (std::uint8_t c : bmp->coverage)
                if (c > 0)
                    ++ink;
            CHECK(ink > 0);
        }

        // A space glyph: valid, non-zero advance, EMPTY bitmap.
        const GlyphId sp = sans->glyph_index(U' ');
        CHECK(sp != kMissingGlyph);
        auto spb = sans->rasterize(sp, 16.0f);
        CHECK(spb.has_value());
        if (spb)
        {
            CHECK(spb->width == 0);
            CHECK(spb->height == 0);
            CHECK(spb->coverage.empty());
            CHECK(spb->advance > 0.0f);
        }

        // Move-only RAII: a moved-into face still works, `identity()` is stable per face.
        const void* id_before = sans->identity();
        FontFace moved = std::move(*sans);
        CHECK(moved.glyph_index(U'A') == a);
        CHECK(moved.identity() == id_before);
    }

    // --- Noto Sans Arabic (complex script — a8 needs it; a7 just rasterizes) ----------------------
    auto arabic = FontFace::from_memory(noto_sans_arabic_regular());
    CHECK(arabic.has_value());
    if (arabic)
    {
        CHECK(arabic->family_name() == "Noto Sans Arabic");
        const GlyphId beh = arabic->glyph_index(static_cast<char32_t>(0x0628u)); // Arabic letter Beh
        CHECK(beh != kMissingGlyph);
        auto b = arabic->rasterize(beh, 24.0f);
        CHECK(b.has_value());
        if (b)
            CHECK(b->advance > 0.0f);
    }

    // --- failure path -----------------------------------------------------------------------------
    CHECK(!FontFace::from_memory(std::span<const std::byte>{}).has_value());

    UI_TEST_MAIN_END();
}
