// The offset-bearing atlas-textured quad emitter (see text_quads.h). Applies each glyph's GPOS x/y
// offset so a8's shaped output flows through unchanged; skips glyphs with no atlas placement or an
// empty bitmap while still advancing the pen.

#include "context/render/ui/text_quads.h"

namespace context::render::ui
{

std::vector<TextQuad> emit_text_quads(std::span<const PositionedGlyph> glyphs, const void* font,
                                      std::uint32_t px, float pen_x, float pen_y, GlyphAtlas& atlas,
                                      const GlyphRasterizer& raster, packages::ui::Color color)
{
    std::vector<TextQuad> quads;
    quads.reserve(glyphs.size());

    const float atlas_w = static_cast<float>(atlas.width());
    const float atlas_h = static_cast<float>(atlas.height());

    for (const PositionedGlyph& g : glyphs)
    {
        const AtlasKey key{font, g.glyph, px};
        const AtlasSlot* slot = atlas.acquire(key, raster);
        if (slot != nullptr && slot->w > 0 && slot->h > 0)
        {
            TextQuad q;
            // GPOS-offset-bearing placement (a7: offsets are 0; a8 fills them → no emitter change).
            q.x0 = pen_x + g.x_offset + static_cast<float>(slot->left);
            q.y0 = pen_y - g.y_offset - static_cast<float>(slot->top); // baseline-up bearing -> y-down
            q.x1 = q.x0 + static_cast<float>(slot->w);
            q.y1 = q.y0 + static_cast<float>(slot->h);
            q.u0 = static_cast<float>(slot->x) / atlas_w;
            q.v0 = static_cast<float>(slot->y) / atlas_h;
            q.u1 = static_cast<float>(slot->x + slot->w) / atlas_w;
            q.v1 = static_cast<float>(slot->y + slot->h) / atlas_h;
            q.color = color;
            quads.push_back(q);
        }
        pen_x += g.advance;
    }
    return quads;
}

} // namespace context::render::ui
