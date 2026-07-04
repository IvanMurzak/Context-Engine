// Table-driven Unicode NFC normalization (UAX #15) with the ASCII fast path — see nfc.h.

#include "context/editor/serializer/nfc.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace context::editor::serializer
{

namespace
{

#include "nfc_tables.inc"

// Hangul syllable composition/decomposition is algorithmic (UAX #15 §3.12), not table-driven.
constexpr char32_t kHangulSBase = 0xAC00;
constexpr char32_t kHangulLBase = 0x1100;
constexpr char32_t kHangulVBase = 0x1161;
constexpr char32_t kHangulTBase = 0x11A7;
constexpr char32_t kHangulLCount = 19;
constexpr char32_t kHangulVCount = 21;
constexpr char32_t kHangulTCount = 28;
constexpr char32_t kHangulNCount = kHangulVCount * kHangulTCount; // 588
constexpr char32_t kHangulSCount = kHangulLCount * kHangulNCount; // 11172

[[nodiscard]] std::uint8_t combining_class(char32_t cp) noexcept
{
    const CccEntry* end = kCccEntries + std::size(kCccEntries);
    const CccEntry* it = std::lower_bound(kCccEntries, end, cp,
                                          [](const CccEntry& e, char32_t v) { return e.cp < v; });
    return (it != end && it->cp == cp) ? it->ccc : 0;
}

[[nodiscard]] bool quick_check_flagged(char32_t cp) noexcept
{
    const QcRange* end = kQcNoOrMaybe + std::size(kQcNoOrMaybe);
    const QcRange* it = std::lower_bound(kQcNoOrMaybe, end, cp,
                                         [](const QcRange& r, char32_t v) { return r.hi < v; });
    return it != end && it->lo <= cp;
}

// The primary composite of (first, second), or 0 when the pair does not compose (0 is never a
// composed character). Table pairs only — Hangul is handled algorithmically by the caller.
[[nodiscard]] char32_t compose_pair(char32_t first, char32_t second) noexcept
{
    const std::uint64_t key =
        (static_cast<std::uint64_t>(first) << 21) | static_cast<std::uint64_t>(second);
    const CompEntry* end = kCompEntries + std::size(kCompEntries);
    const CompEntry* it = std::lower_bound(kCompEntries, end, key,
                                           [](const CompEntry& e, std::uint64_t v)
                                           { return e.key < v; });
    return (it != end && it->key == key) ? it->composed : 0;
}

// Append the FULL canonical decomposition of `cp` (NFD; recursive expansion is pre-baked into the
// table, Hangul is algorithmic).
void decompose_to(char32_t cp, std::vector<char32_t>& out)
{
    if (cp >= kHangulSBase && cp < kHangulSBase + kHangulSCount)
    {
        const char32_t index = cp - kHangulSBase;
        out.push_back(kHangulLBase + index / kHangulNCount);
        out.push_back(kHangulVBase + (index % kHangulNCount) / kHangulTCount);
        const char32_t trailing = index % kHangulTCount;
        if (trailing != 0)
            out.push_back(kHangulTBase + trailing);
        return;
    }
    const DecompEntry* end = kDecompEntries + std::size(kDecompEntries);
    const DecompEntry* it = std::lower_bound(kDecompEntries, end, cp,
                                             [](const DecompEntry& e, char32_t v)
                                             { return e.cp < v; });
    if (it != end && it->cp == cp)
    {
        for (std::uint32_t i = 0; i < it->length; ++i)
            out.push_back(kDecompData[it->offset + i]);
        return;
    }
    out.push_back(cp);
}

// Decode one UTF-8 codepoint at `i` (which the parser already validated); advances `i`.
[[nodiscard]] char32_t decode_utf8(std::string_view text, std::size_t& i) noexcept
{
    const auto byte = [&](std::size_t k) { return static_cast<unsigned char>(text[k]); };
    const unsigned char b0 = byte(i);
    if (b0 < 0x80)
    {
        i += 1;
        return b0;
    }
    if ((b0 & 0xE0) == 0xC0)
    {
        const char32_t cp = ((b0 & 0x1Fu) << 6) | (byte(i + 1) & 0x3Fu);
        i += 2;
        return cp;
    }
    if ((b0 & 0xF0) == 0xE0)
    {
        const char32_t cp =
            ((b0 & 0x0Fu) << 12) | ((byte(i + 1) & 0x3Fu) << 6) | (byte(i + 2) & 0x3Fu);
        i += 3;
        return cp;
    }
    const char32_t cp = ((b0 & 0x07u) << 18) | ((byte(i + 1) & 0x3Fu) << 12) |
                        ((byte(i + 2) & 0x3Fu) << 6) | (byte(i + 3) & 0x3Fu);
    i += 4;
    return cp;
}

void encode_utf8(char32_t cp, std::string& out)
{
    if (cp < 0x80)
    {
        out.push_back(static_cast<char>(cp));
    }
    else if (cp < 0x800)
    {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else if (cp < 0x10000)
    {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else
    {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Canonical ordering (UAX #15 D109): stable-sort every maximal run of non-starters by combining
// class. Runs are short (a handful of marks), so insertion sort is the right tool.
void canonical_reorder(std::vector<char32_t>& cps)
{
    for (std::size_t i = 1; i < cps.size(); ++i)
    {
        const std::uint8_t cc = combining_class(cps[i]);
        if (cc == 0)
            continue;
        std::size_t j = i;
        while (j > 0 && combining_class(cps[j - 1]) > cc)
        {
            std::swap(cps[j - 1], cps[j]);
            --j;
        }
    }
}

// Canonical composition (UAX #15 D117): walk the reordered NFD sequence, composing each character
// with the last STARTER when no intervening character has an equal-or-higher combining class.
void canonical_compose(std::vector<char32_t>& cps)
{
    if (cps.empty())
        return;

    std::vector<char32_t> out;
    out.reserve(cps.size());
    // Index into `out` of the last starter, or npos when none seen yet.
    std::size_t last_starter = std::string::npos;
    std::uint8_t last_cc = 0; // combining class of the previous character in `out` after the starter

    for (char32_t cp : cps)
    {
        const std::uint8_t cc = combining_class(cp);
        if (last_starter != std::string::npos)
        {
            const char32_t starter = out[last_starter];
            const bool blocked = (out.size() - 1 > last_starter) && last_cc >= cc;
            if (!blocked)
            {
                // Hangul LV / LVT algorithmic composition.
                if (starter >= kHangulLBase && starter < kHangulLBase + kHangulLCount &&
                    cp >= kHangulVBase && cp < kHangulVBase + kHangulVCount)
                {
                    out[last_starter] = kHangulSBase + ((starter - kHangulLBase) * kHangulVCount +
                                                        (cp - kHangulVBase)) *
                                                           kHangulTCount;
                    continue;
                }
                if (starter >= kHangulSBase && starter < kHangulSBase + kHangulSCount &&
                    (starter - kHangulSBase) % kHangulTCount == 0 && cp > kHangulTBase &&
                    cp < kHangulTBase + kHangulTCount)
                {
                    out[last_starter] = starter + (cp - kHangulTBase);
                    continue;
                }
                const char32_t composed = compose_pair(starter, cp);
                if (composed != 0)
                {
                    out[last_starter] = composed;
                    continue;
                }
            }
        }
        if (cc == 0)
        {
            last_starter = out.size();
            last_cc = 0;
        }
        else
        {
            last_cc = cc;
        }
        out.push_back(cp);
    }
    cps = std::move(out);
}

} // namespace

bool is_ascii(std::string_view text) noexcept
{
    for (char c : text)
    {
        if (static_cast<unsigned char>(c) >= 0x80)
            return false;
    }
    return true;
}

bool is_nfc_quick(std::string_view text) noexcept
{
    if (is_ascii(text))
        return true;
    std::size_t i = 0;
    std::uint8_t prev_cc = 0;
    while (i < text.size())
    {
        const char32_t cp = decode_utf8(text, i);
        if (cp >= 0x80) // ASCII inside mixed text keeps the cheap path per character
        {
            if (quick_check_flagged(cp))
                return false;
            const std::uint8_t cc = combining_class(cp);
            if (cc != 0 && prev_cc > cc)
                return false; // canonically mis-ordered marks
            prev_cc = cc;
        }
        else
        {
            prev_cc = 0;
        }
    }
    return true;
}

std::optional<std::string> normalize_nfc(std::string_view text)
{
    if (is_nfc_quick(text))
        return std::nullopt;

    // Full UAX #15 pipeline: NFD (canonical decompose) -> canonical reorder -> canonical compose.
    std::vector<char32_t> cps;
    cps.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size())
        decompose_to(decode_utf8(text, i), cps);

    canonical_reorder(cps);
    canonical_compose(cps);

    std::string out;
    out.reserve(text.size());
    for (char32_t cp : cps)
        encode_utf8(cp, out);
    return out;
}

const char* nfc_unicode_version() noexcept
{
    return kNfcUnicodeVersion;
}

} // namespace context::editor::serializer
