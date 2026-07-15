// UAX #14 line breaking + greedy word wrap for the runtime UI text stack (M7 a8, R-UI-005; issue #237).
// HarfBuzz shapes runs but does NOT break lines; libunibreak supplies the break opportunities and this
// header wraps shaped text to a width. Dictionary-based breaking (Thai/Lao/Khmer), justification, and
// hyphenation are declared OUT of scope (a8 spec). The public API is dependency-free (libunibreak lives
// only in line_break.cpp), like the measure() seam.

#pragma once

#include "context/packages/ui/text/font.h"
#include "context/packages/ui/text/measure.h"

#include <cstddef>
#include <string_view>
#include <vector>

namespace context::packages::ui::text
{

// One wrapped line: its shaped runs (visual, left-to-right order — the same shape as measure()'s output)
// and total advance width (pixels). A caller stacks lines by `metrics.line_height`.
struct TextLine
{
    std::vector<ShapedRun> runs;
    float width = 0.0f;
};

// The byte offsets in `utf8` at which a soft line break is PERMITTED (UAX #14 mandatory + allowed
// opportunities) — i.e. positions `p` in (0, size) where breaking the text into [0,p) and [p,size) is
// legal. The end of the string is not included (it is always a break). Exposed for tests + custom
// wrappers. Invalid UTF-8 yields no spurious opportunities (libunibreak marks continuation bytes
// INSIDEACHAR).
[[nodiscard]] std::vector<std::size_t> break_opportunities(std::string_view utf8);

// Greedy line wrap: shape `utf8` in `font` at `pixel_size` and split it into lines each no wider than
// `max_width` (pixels), breaking ONLY at UAX #14 opportunities. A mandatory break (e.g. '\n') always
// starts a new line. A single word wider than `max_width` overflows its own line (no mid-word breaking /
// no hyphenation — out of scope). `max_width <= 0` yields one unwrapped line. Every line is INDEPENDENTLY
// shaped (bidi reordering + HarfBuzz), so per-line output matches measure() of that line's substring.
[[nodiscard]] std::vector<TextLine> wrap(const FontFace& font, std::string_view utf8, float pixel_size,
                                         float max_width);

} // namespace context::packages::ui::text
