// render-ui-test_glyph_atlas_font — the end-to-end a7 text path over a REAL embedded font: measure ->
// glyph-id-keyed atlas build (via the FontFace rasterizer) -> offset-bearing quad emit. Proves the
// FreeType-decoupled render backend composes with the FreeType text package, and that repeated glyph
// ids share ONE atlas cell (the glyph-id-keying payoff). Links context_render_ui + context_ui_text.

#include "context/packages/ui/text/embedded_fonts.h"
#include "context/packages/ui/text/font.h"
#include "context/packages/ui/text/measure.h"
#include "context/render/ui/glyph_atlas.h"
#include "context/render/ui/text_quads.h"

#include "render_test.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace tx = context::packages::ui::text;
using namespace context::render::ui;
using context::packages::ui::Color;

namespace
{
std::vector<PositionedGlyph> to_positioned(const tx::ShapedRun& run)
{
    std::vector<PositionedGlyph> out;
    out.reserve(run.glyphs.size());
    for (const tx::GlyphMetrics& g : run.glyphs)
        out.push_back(PositionedGlyph{g.glyph, g.advance, g.x_offset, g.y_offset});
    return out;
}
} // namespace

int main()
{
    auto font = tx::FontFace::from_memory(tx::noto_sans_regular());
    CHECK(font.has_value());
    if (font)
    {
        const std::uint32_t px = 24u;

        // The atlas rasterizer bridges to the REAL FreeType face (FreeType stays inside context_ui_text).
        GlyphRasterizer raster = [&](const AtlasKey& k) -> std::optional<GlyphCoverage>
        {
            auto b = font->rasterize(k.glyph, static_cast<float>(k.px));
            if (!b)
                return std::nullopt;
            GlyphCoverage c;
            c.width = b->width;
            c.height = b->height;
            c.left = b->left;
            c.top = b->top;
            c.advance = b->advance;
            c.pixels = std::move(b->coverage);
            return c;
        };

        // "Hello": 5 glyphs, all inked (no spaces), the two 'l's share a glyph id.
        auto runs = tx::measure(*font, "Hello", static_cast<float>(px));
        CHECK(runs.size() == 1);
        const tx::ShapedRun& run = runs[0];

        GlyphAtlas atlas(GlyphAtlas::Config{256, 256, 64, 1});
        const std::vector<PositionedGlyph> pg = to_positioned(run);
        auto quads = emit_text_quads(pg, run.font, px, 10.0f, 40.0f, atlas, raster,
                                     Color{255, 255, 255, 255});

        CHECK(quads.size() == 5);            // 5 inked glyphs -> 5 quads
        CHECK(atlas.count() == 4);           // H, e, l, o (the two 'l's coalesce, glyph-id-keyed)
        CHECK(atlas.rasterizations() == 4);  // the 2nd 'l' was a cache hit
        CHECK(atlas.evictions() == 0);

        std::uint64_t ink = 0;
        for (std::uint8_t v : atlas.pixels())
            if (v > 0)
                ++ink;
        CHECK(ink > 0); // real glyph coverage landed in the R8 buffer

        // A space contributes an atlas cell but NO quad (empty bitmap).
        auto runs2 = tx::measure(*font, "A B", static_cast<float>(px));
        GlyphAtlas atlas2(GlyphAtlas::Config{256, 256, 64, 1});
        const std::vector<PositionedGlyph> pg2 = to_positioned(runs2[0]);
        auto quads2 = emit_text_quads(pg2, runs2[0].font, px, 0.0f, 0.0f, atlas2, raster,
                                      Color{255, 255, 255, 255});
        CHECK(quads2.size() == 2); // 'A' and 'B' draw; the space does not
    }

    RENDER_TEST_MAIN_END();
}
