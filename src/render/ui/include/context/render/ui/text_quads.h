// The offset-bearing atlas-textured quad emitter (M7 a7, R-UI-005; issue #237). Turns a run of
// positioned glyphs into textured quads sampling the glyph atlas. The emitter applies each glyph's
// GPOS x/y offset (0 on a7's unshaped path), so a8's shaped output flows through UNCHANGED — an
// offset-bearing emitter, not a per-char advance loop that a8 would have to replace.
//
// FreeType-FREE: the input `PositionedGlyph` is the FreeType-decoupled hand-off from the text package's
// ShapedRun (the caller converts a run's GlyphMetrics into these), so context_render_ui carries no
// context_ui_text / FreeType dependency.

#pragma once

#include "context/packages/ui/ui_node.h" // Color
#include "context/render/ui/glyph_atlas.h"

#include <cstdint>
#include <span>
#include <vector>

namespace context::render::ui
{

// One glyph positioned by measure()/shaping: glyph id + pen advance + GPOS offsets. Byte-identical in
// spirit to text::GlyphMetrics, re-declared here to keep the render backend text-package-free.
struct PositionedGlyph
{
    GlyphId glyph = 0;
    float advance = 0.0f;
    float x_offset = 0.0f; // GPOS x (a7: 0; a8 fills)
    float y_offset = 0.0f; // GPOS y (a7: 0)
};

// A textured quad for one glyph: screen-space rect (pixels, y-down, top-left origin — the a1 Rect
// convention) + atlas UVs (normalized 0..1) + tint.
struct TextQuad
{
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 0.0f;
    float v1 = 0.0f;
    packages::ui::Color color;
};

// Emit atlas-textured quads for `glyphs`, starting at the pen origin (`pen_x`, `pen_y` = the BASELINE
// position, y-down). Each glyph is placed at `pen_x + x_offset + slot.left` (x) and
// `pen_y - y_offset - slot.top` (y — the baseline-up `top` bearing mapped into y-down screen space),
// then the pen advances by `advance`. Because the GPOS x/y offsets are applied, a8's shaped output
// needs no emitter change. `font`/`px` identify the atlas face+size for keying; `raster` fills atlas
// misses. A glyph the atlas can't place (un-renderable / oversize) OR with an empty bitmap contributes
// NO quad but STILL advances the pen. Colours are the passed tint.
[[nodiscard]] std::vector<TextQuad> emit_text_quads(std::span<const PositionedGlyph> glyphs,
                                                    const void* font, std::uint32_t px, float pen_x,
                                                    float pen_y, GlyphAtlas& atlas,
                                                    const GlyphRasterizer& raster,
                                                    packages::ui::Color color);

} // namespace context::render::ui
