// ui-test_line_break — the a8 UAX #14 line-break opportunities (libunibreak) + greedy word wrap into
// shaped lines. HEADLESS: wrapping shapes each line with measure(), so the wrapped layout is the same one
// the null + GPU providers place. Dictionary breaking / hyphenation / justification are out of scope.

#include "context/packages/ui/text/embedded_fonts.h"
#include "context/packages/ui/text/line_break.h"

#include "ui_test.h"

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <vector>

using namespace context::packages::ui::text;

namespace
{
bool contains(const std::vector<std::size_t>& v, std::size_t x)
{
    return std::find(v.begin(), v.end(), x) != v.end();
}
float line_width(const FontFace& font, std::string_view s, float px)
{
    float w = 0.0f;
    for (const ShapedRun& r : measure(font, s, px))
        w += r.width;
    return w;
}
} // namespace

int main()
{
    auto sans_opt = FontFace::from_memory(noto_sans_regular());
    CHECK(sans_opt.has_value());
    if (!sans_opt)
        UI_TEST_MAIN_END();
    const FontFace& sans = *sans_opt;

    // --- break opportunities: allowed AFTER each inter-word space, not inside words -------------------
    {
        // "hello world foo": spaces at bytes 5 and 11 -> break opportunities at 6 and 12.
        auto opps = break_opportunities("hello world foo");
        CHECK(contains(opps, 6));
        CHECK(contains(opps, 12));
        // No opportunity strictly inside "hello" (bytes 1..5) — words are unbreakable here.
        CHECK(!contains(opps, 1));
        CHECK(!contains(opps, 2));
        CHECK(!contains(opps, 3));
        // The end of the string is not reported as an opportunity.
        CHECK(!contains(opps, 15));
    }

    // --- mandatory break: '\n' forces a new line -----------------------------------------------------
    {
        auto opps = break_opportunities("ab\ncd");
        CHECK(contains(opps, 3)); // break after the newline (byte 2) -> opportunity at 3
    }

    // --- wrap: a huge width yields ONE unwrapped line ------------------------------------------------
    {
        auto lines = wrap(sans, "hello world foo", 16.0f, 100000.0f);
        CHECK(lines.size() == 1);
        CHECK(lines[0].runs.size() >= 1);
        CHECK(lines[0].width > 0.0f);
    }

    // --- wrap: a width fitting only one word at a time yields one word per line ----------------------
    {
        const float px = 16.0f;
        const float w_hello = line_width(sans, "hello", px);
        const float w_world = line_width(sans, "world", px);
        const float w_foo = line_width(sans, "foo", px);
        const float widest = std::max({w_hello, w_world, w_foo});
        // Fits any single word but never two words together.
        const float max_w = widest + 2.0f;
        auto lines = wrap(sans, "hello world foo", px, max_w);
        CHECK(lines.size() == 3);
        for (const TextLine& ln : lines)
            CHECK(ln.width <= max_w + 0.5f); // every wrapped line honors the width (each word fits)
    }

    // --- wrap: an over-long single word overflows its own line (no mid-word break) -------------------
    {
        auto lines = wrap(sans, "supercalifragilisticexpialidocious", 16.0f, 10.0f);
        CHECK(lines.size() == 1); // one unbreakable word -> one (overflowing) line
    }

    // --- wrap: a mandatory '\n' splits even when the text fits ---------------------------------------
    {
        auto lines = wrap(sans, "ab\ncd", 16.0f, 100000.0f);
        CHECK(lines.size() == 2);
    }

    // --- wrap: empty input yields one empty metrics-bearing line -------------------------------------
    {
        auto lines = wrap(sans, "", 16.0f, 100.0f);
        CHECK(lines.size() == 1);
        CHECK(lines[0].runs.size() == 1);
        CHECK(lines[0].runs[0].metrics.line_height > 0.0f);
    }

    UI_TEST_MAIN_END();
}
