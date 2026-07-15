// render-ui-test_text_quads — the offset-bearing atlas-textured quad emitter: pen advance, glyph-bearing
// placement, atlas UVs, the GPOS x/y-offset application (a8-readiness), and the skip-but-advance
// behaviour for empty-bitmap / un-renderable glyphs (M7 a7 DoD, R-QA-013). FreeType-free (synthetic
// rasterizer).

#include "context/render/ui/text_quads.h"

#include "render_test.h"

#include <cstdint>
#include <optional>
#include <vector>

using namespace context::render::ui;
using context::packages::ui::Color;

namespace
{
// 8x8 for every glyph EXCEPT glyph 2 (empty bitmap) and glyph 4 (un-renderable). left=1, top=8.
GlyphRasterizer scripted_raster()
{
    return [](const AtlasKey& k) -> std::optional<GlyphCoverage>
    {
        if (k.glyph == 4u)
            return std::nullopt; // un-renderable
        GlyphCoverage c;
        c.left = 1;
        c.top = 8;
        c.advance = 9.0f;
        if (k.glyph == 2u)
            return c; // whitespace: width == height == 0
        c.width = 8;
        c.height = 8;
        c.pixels.assign(64u, static_cast<std::uint8_t>((k.glyph % 255u) + 1u));
        return c;
    };
}

const void* kFont = reinterpret_cast<const void*>(0x1000);
bool approx(float a, float b)
{
    const float d = a - b;
    return (d < 0.0f ? -d : d) < 1e-3f;
}
} // namespace

int main()
{
    // --- placement + advance + UVs ----------------------------------------------------------------
    {
        GlyphAtlas atlas(GlyphAtlas::Config{128, 128, 16, 1});
        auto raster = scripted_raster();
        std::vector<PositionedGlyph> run = {{1, 9.0f, 0.0f, 0.0f}, {3, 9.0f, 0.0f, 0.0f}};
        auto quads = emit_text_quads(run, kFont, 16, 100.0f, 50.0f, atlas, raster,
                                     Color{255, 0, 0, 255});
        CHECK(quads.size() == 2);
        // glyph 1: x0 = pen_x + x_offset + left(1); y0 = pen_y - y_offset - top(8)
        CHECK(approx(quads[0].x0, 101.0f));
        CHECK(approx(quads[0].y0, 42.0f));
        CHECK(approx(quads[0].x1, 109.0f));
        CHECK(approx(quads[0].y1, 50.0f));
        CHECK(quads[0].color == (Color{255, 0, 0, 255}));
        // glyph 3: pen advanced by 9 -> x0 = 100 + 9 + 1 = 110
        CHECK(approx(quads[1].x0, 110.0f));

        // UVs come from the glyph's atlas slot
        const AtlasSlot* s0 = atlas.peek(AtlasKey{kFont, 1, 16});
        CHECK(s0 != nullptr);
        CHECK(approx(quads[0].u0, static_cast<float>(s0->x) / 128.0f));
        CHECK(approx(quads[0].v0, static_cast<float>(s0->y) / 128.0f));
        CHECK(approx(quads[0].u1, static_cast<float>(s0->x + s0->w) / 128.0f));
        CHECK(approx(quads[0].v1, static_cast<float>(s0->y + s0->h) / 128.0f));
    }

    // --- GPOS offset-bearing: x_offset/y_offset shift the quad (a8-ready) --------------------------
    {
        GlyphAtlas atlas(GlyphAtlas::Config{128, 128, 16, 1});
        auto raster = scripted_raster();
        std::vector<PositionedGlyph> run = {{1, 9.0f, 5.0f, 3.0f}}; // x_offset 5, y_offset 3
        auto quads = emit_text_quads(run, kFont, 16, 100.0f, 50.0f, atlas, raster,
                                     Color{255, 255, 255, 255});
        CHECK(quads.size() == 1);
        CHECK(approx(quads[0].x0, 106.0f)); // 100 + 5(x_offset) + 1(left)
        CHECK(approx(quads[0].y0, 39.0f));  // 50 - 3(y_offset) - 8(top)
    }

    // --- empty-bitmap + un-renderable glyphs: no quad, but the pen still advances ------------------
    {
        GlyphAtlas atlas(GlyphAtlas::Config{128, 128, 16, 1});
        auto raster = scripted_raster();
        // glyph 2 = whitespace (empty bitmap), glyph 4 = un-renderable, both between inked glyphs
        std::vector<PositionedGlyph> run = {
            {1, 9.0f, 0.0f, 0.0f}, {2, 9.0f, 0.0f, 0.0f}, {4, 9.0f, 0.0f, 0.0f}, {3, 9.0f, 0.0f, 0.0f}};
        auto quads = emit_text_quads(run, kFont, 16, 100.0f, 50.0f, atlas, raster,
                                     Color{255, 255, 255, 255});
        CHECK(quads.size() == 2); // only glyphs 1 and 3 draw
        CHECK(approx(quads[0].x0, 101.0f));       // glyph 1 at pen 100
        CHECK(approx(quads[1].x0, 100.0f + 27.0f + 1.0f)); // glyph 3 after 3 advances of 9 -> pen 127
    }

    RENDER_TEST_MAIN_END();
}
