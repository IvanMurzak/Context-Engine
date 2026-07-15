// ui-test_shaping — the a8 shaping seam: HarfBuzz OpenType shaping (ligatures, GPOS mark positioning,
// cursive joining) + SheenBidi UAX #9 bidi run segmentation/reordering + UAX #24 script itemization. All
// HEADLESS (no GPU): measure() computes the same glyph ids + positions the GPU provider draws, so the
// null (headless) and GPU providers place text glyph-for-glyph identically (the a8 placement cliff).

#include "context/packages/ui/text/embedded_fonts.h"
#include "context/packages/ui/text/measure.h"

#include "ui_test.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

using namespace context::packages::ui::text;

namespace
{
// Total shaped glyphs across all runs.
std::size_t glyph_total(const std::vector<ShapedRun>& runs)
{
    std::size_t n = 0;
    for (const ShapedRun& r : runs)
        n += r.glyphs.size();
    return n;
}
} // namespace

int main()
{
    auto sans_opt = FontFace::from_memory(noto_sans_regular());
    auto arabic_opt = FontFace::from_memory(noto_sans_arabic_regular());
    CHECK(sans_opt.has_value());
    CHECK(arabic_opt.has_value());
    if (!sans_opt || !arabic_opt)
        UI_TEST_MAIN_END();
    const FontFace& sans = *sans_opt;
    const FontFace& arabic = *arabic_opt;

    // --- Ligature: "ffi" shapes to FEWER glyphs than codepoints (GSUB ligature substitution) ---------
    {
        auto runs = measure(sans, "ffi", 32.0f);
        CHECK(runs.size() == 1);
        CHECK(!runs[0].rtl);
        // Shaping collapsed f+f+i -> the ffi ligature: strictly fewer than 3 glyphs (a7's cmap map would
        // have produced exactly 3). This is the load-bearing "shaping happened" proof.
        CHECK(glyph_total(runs) < 3);
        CHECK(runs[0].width > 0.0f);
        // The ligature's cluster is the start of the source (byte 0).
        CHECK(runs[0].glyphs.front().cluster == 0u);
    }

    // --- Combining marks: an Arabic base + fatha positions the mark with GPOS offsets ----------------
    {
        // U+0628 beh + U+064E fatha (a combining mark above).
        auto runs = measure(arabic, "\xD8\xA8\xD9\x8E", 32.0f);
        CHECK(runs.size() == 1);
        CHECK(runs[0].rtl); // Arabic resolves RTL
        bool positioned_mark = false; // a glyph carrying a nonzero GPOS offset (the attached mark)
        bool zero_advance_mark = false;
        for (const GlyphMetrics& g : runs[0].glyphs)
        {
            if (g.x_offset != 0.0f || g.y_offset != 0.0f)
                positioned_mark = true;
            if (g.advance == 0.0f)
                zero_advance_mark = true;
        }
        CHECK(positioned_mark);  // the fatha is GPOS-attached, not laid out with a raw advance
        CHECK(zero_advance_mark); // a combining mark does not advance the pen
    }

    // --- Cursive shaping: an Arabic word joins into position-dependent forms (advance changes) --------
    {
        // "سلام" (seen-lam-alef-meem) — joins; the shaped run is RTL, glyphs in VISUAL order with
        // DESCENDING source clusters (the last logical char is leftmost).
        auto runs = measure(arabic, "\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85", 32.0f);
        CHECK(runs.size() == 1);
        CHECK(runs[0].rtl);
        CHECK(runs[0].glyphs.size() >= 3); // joined, but at least a few glyphs
        bool descending = true;
        for (std::size_t i = 1; i < runs[0].glyphs.size(); ++i)
            if (runs[0].glyphs[i].cluster > runs[0].glyphs[i - 1].cluster)
                descending = false;
        CHECK(descending); // RTL visual order => non-increasing clusters
        // No .notdef: the Arabic face covers these codepoints (real shaping, not tofu).
        for (const GlyphMetrics& g : runs[0].glyphs)
            CHECK(g.glyph != kMissingGlyph);
    }

    // --- Bidi: a mixed LTR/RTL/LTR line segments + reorders into 3 runs (visual order) ---------------
    {
        // "A" + "سلام" + "B": UAX #9 gives L, R, L; measure() returns them left-to-right (visual).
        auto runs = measure(arabic, "A\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85""B", 32.0f);
        CHECK(runs.size() == 3);
        CHECK(!runs[0].rtl); // "A" LTR, leftmost
        CHECK(runs[1].rtl);  // Arabic RTL, middle
        CHECK(!runs[2].rtl); // "B" LTR, rightmost
        // Exactly one RTL run.
        int rtl_count = 0;
        for (const ShapedRun& r : runs)
            rtl_count += r.rtl ? 1 : 0;
        CHECK(rtl_count == 1);
        // The leftmost run starts at byte 0 ("A"); the rightmost covers the final "B".
        CHECK(runs[0].glyphs.front().cluster == 0u);
    }

    // --- Determinism / provider independence: shaping is a pure function of (font, text, size) -------
    // The null (headless) provider and the GPU provider both consume THIS output, so identical shaping
    // here IS identical placement there — the a8 "identical rects across null and GPU providers" cliff.
    {
        std::string_view text = "Hello, \xD8\xB3\xD9\x84\xD8\xA7\xD9\x85 world";
        auto a = measure(arabic, text, 24.0f);
        auto b = measure(arabic, text, 24.0f);
        CHECK(a.size() == b.size());
        bool identical = a.size() == b.size();
        for (std::size_t i = 0; identical && i < a.size(); ++i)
        {
            identical = a[i].rtl == b[i].rtl && a[i].glyphs.size() == b[i].glyphs.size() &&
                        a[i].width == b[i].width;
            for (std::size_t j = 0; identical && j < a[i].glyphs.size(); ++j)
            {
                const GlyphMetrics& ga = a[i].glyphs[j];
                const GlyphMetrics& gb = b[i].glyphs[j];
                identical = ga.glyph == gb.glyph && ga.advance == gb.advance &&
                            ga.x_offset == gb.x_offset && ga.y_offset == gb.y_offset &&
                            ga.cluster == gb.cluster;
            }
        }
        CHECK(identical);
    }

    UI_TEST_MAIN_END();
}
