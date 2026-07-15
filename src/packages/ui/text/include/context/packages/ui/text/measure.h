// The run-based measure seam for the runtime UI (M7 a7, R-UI-005; issue #237). measure() returns
// RUNS (not per-char advances): the RETURN TYPE is run-based + glyph-id + GPOS-offset bearing, so a8's
// shaper/bidi replaces a7's internals WITHOUT changing this signature or the atlas/emitter that consume
// it. a7's layout is metrics-only plumbing — no shaping, no kerning, no bidi (all land in a8).

#pragma once

#include "context/packages/ui/text/font.h"
#include "context/packages/ui/text/glyph.h"

#include <cstddef>
#include <string_view>
#include <vector>

namespace context::packages::ui::text
{

// A shaped run: a maximal sequence of glyphs sharing one font + pixel size. On a7 every measure() call
// produces exactly ONE run (there is no script/bidi segmentation yet); a8 splits the input into several
// per-script / per-font runs and sets `rtl` per bidi level. `glyphs` are in VISUAL order (a7: logical
// order == visual, since nothing is reordered).
struct ShapedRun
{
    const void* font = nullptr;       // FontFace::identity() — the atlas key + which face to rasterize
    float pixel_size = 0.0f;
    std::vector<GlyphMetrics> glyphs; // visual order; each carries glyph id + advance + GPOS offsets
    float width = 0.0f;               // sum of advances (pixels)
    FontMetrics metrics;              // vertical metrics at pixel_size
    bool rtl = false;                 // a7: always false; a8 sets it per bidi level
};

// Measure `utf8` in `font` at `pixel_size`, returning the shaped runs. a7 maps each decoded codepoint
// to its glyph id via the font's charmap and sums per-glyph advances into ONE run; `x_offset` /
// `y_offset` are 0 (no GPOS yet). Invalid/truncated UTF-8 is replaced with U+FFFD (one byte consumed).
// An EMPTY input yields a single empty run (width 0) carrying the font's vertical metrics, so a caller
// can still lay out an empty line.
[[nodiscard]] std::vector<ShapedRun> measure(const FontFace& font, std::string_view utf8,
                                             float pixel_size);

// Decode one UTF-8 codepoint from `s` starting at `pos`, advancing `pos` past it. Returns U+FFFD for an
// invalid lead byte, an out-of-range value, an overlong encoding, or a truncated tail (advancing a
// single byte so decoding always makes progress). Exposed for tests + a8's cluster segmentation.
[[nodiscard]] char32_t decode_utf8(std::string_view s, std::size_t& pos);

} // namespace context::packages::ui::text
