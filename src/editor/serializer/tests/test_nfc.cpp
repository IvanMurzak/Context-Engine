// NFC normalization tests: the ASCII fast path, quick-check pass-through, composition (table +
// Hangul algorithmic), canonical reordering, composition exclusions, and idempotence (R-FILE-001,
// UAX #15). Non-ASCII bytes are spelled as \x escapes (source-encoding independence).

#include "context/editor/serializer/nfc.h"

#include "serializer_test.h"

#include <string>

using context::editor::serializer::is_ascii;
using context::editor::serializer::is_nfc_quick;
using context::editor::serializer::nfc_unicode_version;
using context::editor::serializer::normalize_nfc;

namespace
{

std::string nfc(const std::string& s)
{
    if (auto normalized = normalize_nfc(s))
        return *normalized;
    return s;
}

} // namespace

int main()
{
    // ASCII fast path: pure-ASCII text is NFC by construction and never allocates.
    CHECK(is_ascii(""));
    CHECK(is_ascii("hello, world! 123 {}[]"));
    CHECK(!is_ascii("caf\xC3\xA9"));
    CHECK(is_nfc_quick("plain ascii"));
    CHECK(!normalize_nfc("plain ascii").has_value()); // nullopt: already NFC, no allocation

    // Already-composed text passes the quick check untouched.
    CHECK(is_nfc_quick("caf\xC3\xA9"));                    // café (U+00E9 composed)
    CHECK(!normalize_nfc("caf\xC3\xA9").has_value());
    CHECK(is_nfc_quick("\xE6\x97\xA5\xE6\x9C\xAC"));       // 日本 (no combining marks)
    CHECK(!normalize_nfc("\xE6\x97\xA5\xE6\x9C\xAC").has_value());

    // Basic composition: e + COMBINING ACUTE (U+0301) -> é (U+00E9).
    CHECK(!is_nfc_quick("e\xCC\x81"));
    CHECK(nfc("e\xCC\x81") == "\xC3\xA9");
    CHECK(nfc("cafe\xCC\x81") == "caf\xC3\xA9");

    // Singleton decomposition: ANGSTROM SIGN (U+212B) normalizes to Å (U+00C5).
    CHECK(!is_nfc_quick("\xE2\x84\xAB"));
    CHECK(nfc("\xE2\x84\xAB") == "\xC3\x85");

    // Multi-mark composition: A (U+0041) + ring (U+030A) + acute (U+0301) -> Ǻ (U+01FA).
    CHECK(nfc("A\xCC\x8A\xCC\x81") == "\xC7\xBA");

    // Canonical REORDERING before composition: cedilla (U+0327, ccc 202) and acute (U+0301,
    // ccc 230) around c: c + acute + cedilla must equal c + cedilla + acute (both -> ḉ-like
    // sequence: U+00E7 ç + U+0301 acute... i.e. U+1E09).
    CHECK(nfc("c\xCC\x81\xCC\xA7") == nfc("c\xCC\xA7\xCC\x81"));
    CHECK(nfc("c\xCC\xA7\xCC\x81") == "\xE1\xB8\x89"); // U+1E09 LATIN SMALL C WITH CEDILLA+ACUTE

    // Blocking: an intervening mark with an equal-or-higher combining class blocks composition.
    // e + COMBINING DIAERESIS (U+0308, ccc 230) + COMBINING ACUTE (U+0301, ccc 230): the acute is
    // blocked from the e by the diaeresis (equal ccc), so NFC is ë (U+00EB) + U+0301, NOT é + ...
    CHECK(nfc("e\xCC\x88\xCC\x81") == "\xC3\xAB\xCC\x81");

    // Composition EXCLUSION: DEVANAGARI QA (U+0958) canonically decomposes to U+0915 + U+093C and
    // is excluded from recomposition — NFC keeps it DECOMPOSED.
    CHECK(nfc("\xE0\xA5\x98") == "\xE0\xA4\x95\xE0\xA4\xBC");
    // And re-normalizing the decomposed form does NOT compose it back.
    CHECK(nfc("\xE0\xA4\x95\xE0\xA4\xBC") == "\xE0\xA4\x95\xE0\xA4\xBC");

    // Hangul algorithmic composition: L (U+1100) + V (U+1161) -> 가 (U+AC00); LV + T (U+11A8) ->
    // 각 (U+AC01); the full jamo triple composes in one pass.
    CHECK(nfc("\xE1\x84\x80\xE1\x85\xA1") == "\xEA\xB0\x80");
    CHECK(nfc("\xEA\xB0\x80\xE1\x86\xA8") == "\xEA\xB0\x81");
    CHECK(nfc("\xE1\x84\x80\xE1\x85\xA1\xE1\x86\xA8") == "\xEA\xB0\x81");
    // Already-composed Hangul passes through.
    CHECK(is_nfc_quick("\xEA\xB0\x81"));
    CHECK(nfc("\xEA\xB0\x81") == "\xEA\xB0\x81");

    // Idempotence / fixpoint: normalizing normalized output is a byte-identical no-op.
    {
        const std::string samples[] = {
            "e\xCC\x81",                      // composes
            "\xE2\x84\xAB",                   // singleton
            "\xE0\xA5\x98",                   // exclusion
            "\xE1\x84\x80\xE1\x85\xA1\xE1\x86\xA8", // Hangul jamo
            "c\xCC\x81\xCC\xA7",              // reorder + compose
            "mixed ascii + caf\xC3\xA9 + \xE6\x97\xA5",
        };
        for (const std::string& s : samples)
        {
            // NOTE: is_nfc_quick(once) is NOT asserted — the quick check is deliberately
            // conservative: NFC text carrying a Maybe-class character (e.g. U+093C, which
            // composes in non-excluded pairs) reports false and takes the full pass, which
            // must then be a byte-identical no-op. THAT is the invariant:
            const std::string once = nfc(s);
            CHECK(nfc(once) == once);
        }
    }

    // Non-starter-only string edge: leading combining mark survives normalization.
    CHECK(nfc("\xCC\x81") == "\xCC\x81"); // lone U+0301 has nothing to compose with

    // The generated tables record their Unicode version.
    CHECK(nfc_unicode_version() != nullptr);
    CHECK(std::string(nfc_unicode_version()).find('.') != std::string::npos);

    SERIALIZER_TEST_MAIN_END();
}
