// Glyph-level value types for the M7 runtime-UI text substrate (M7 a7, R-UI-005; issue #237). SOURCE OF
// TRUTH for the GLYPH-ID type + the run's per-glyph metrics + a rasterized coverage bitmap. Everything
// here is GLYPH-ID-keyed (a font-specific glyph index, NOT a Unicode codepoint) and carries GPOS x/y
// offsets, so a8's shaper (glyph ids + GPOS offsets) drops in without changing these types.

#pragma once

#include <cstdint>
#include <vector>

namespace context::packages::ui::text
{

// A font-specific glyph index (the value FreeType's charmap maps a codepoint to), NOT a Unicode
// codepoint. The atlas + quad emitter are keyed on this so shaped output flows through unchanged.
using GlyphId = std::uint32_t;

// FreeType's glyph 0 is always `.notdef` (the missing-glyph box); an unmapped codepoint resolves here.
inline constexpr GlyphId kMissingGlyph = 0;

// One positioned glyph in a shaped run (26.6 fixed-point converted to float pixels). Engineered for a8:
// `advance` is the pen advance; `x_offset`/`y_offset` are GPOS position adjustments (0 on a7's unshaped
// path, filled by a8's shaper). `cluster` is the byte offset of the source codepoint in the measured
// UTF-8 (for hit-testing + a8 cluster mapping).
struct GlyphMetrics
{
    GlyphId glyph = kMissingGlyph;
    float advance = 0.0f;
    float x_offset = 0.0f;
    float y_offset = 0.0f;
    std::uint32_t cluster = 0;
};

// A rasterized glyph coverage bitmap: 8-bit alpha (0..255), row-major, `width * height` bytes, plus the
// pen-relative placement. `left` is the x bearing from the pen origin to the bitmap's left edge; `top`
// is the y bearing from the baseline UP to the bitmap's top edge (both pixels). A whitespace glyph has
// an empty bitmap (width == height == 0) but a non-zero `advance`.
struct GlyphBitmap
{
    int width = 0;
    int height = 0;
    int left = 0;
    int top = 0;
    float advance = 0.0f;
    std::vector<std::uint8_t> coverage;
};

} // namespace context::packages::ui::text
