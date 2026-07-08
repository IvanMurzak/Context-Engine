// Standard base64 (RFC 4648) implementation (see base64.h).

#include "context/editor/pkg/base64.h"

#include <array>
#include <cstdint>

namespace context::editor::pkg
{
namespace
{

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Reverse map: ASCII value -> 6-bit sextet, or -1 for a non-alphabet byte. `=` is handled by the
// decoder directly, not via this table.
std::array<int, 256> make_reverse_table()
{
    std::array<int, 256> table{};
    for (int& slot : table)
        slot = -1;
    for (int i = 0; i < 64; ++i)
        table[static_cast<unsigned char>(kAlphabet[i])] = i;
    return table;
}

const std::array<int, 256>& reverse_table()
{
    static const std::array<int, 256> table = make_reverse_table();
    return table;
}

} // namespace

std::string base64_encode(std::string_view bytes)
{
    std::string out;
    out.reserve((bytes.size() + 2u) / 3u * 4u);

    std::size_t i = 0;
    while (i + 3u <= bytes.size())
    {
        const std::uint32_t triple =
            (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i])) << 16) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i + 1])) << 8) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i + 2])));
        out.push_back(kAlphabet[(triple >> 18) & 0x3fu]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3fu]);
        out.push_back(kAlphabet[(triple >> 6) & 0x3fu]);
        out.push_back(kAlphabet[triple & 0x3fu]);
        i += 3u;
    }

    const std::size_t remaining = bytes.size() - i;
    if (remaining == 1u)
    {
        const std::uint32_t triple =
            static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i])) << 16;
        out.push_back(kAlphabet[(triple >> 18) & 0x3fu]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3fu]);
        out.push_back('=');
        out.push_back('=');
    }
    else if (remaining == 2u)
    {
        const std::uint32_t triple =
            (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i])) << 16) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i + 1])) << 8);
        out.push_back(kAlphabet[(triple >> 18) & 0x3fu]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3fu]);
        out.push_back(kAlphabet[(triple >> 6) & 0x3fu]);
        out.push_back('=');
    }
    return out;
}

std::optional<std::string> base64_decode(std::string_view text)
{
    if (text.size() % 4u != 0u)
        return std::nullopt;
    if (text.empty())
        return std::string();

    const std::array<int, 256>& rev = reverse_table();
    std::string out;
    out.reserve(text.size() / 4u * 3u);

    for (std::size_t i = 0; i < text.size(); i += 4u)
    {
        const char c0 = text[i];
        const char c1 = text[i + 1];
        const char c2 = text[i + 2];
        const char c3 = text[i + 3];

        // Padding may appear only in the last quad, only in the c2/c3 slots, and never before a
        // non-padding byte within the quad.
        const bool last_quad = (i + 4u == text.size());
        const int v0 = rev[static_cast<unsigned char>(c0)];
        const int v1 = rev[static_cast<unsigned char>(c1)];
        if (v0 < 0 || v1 < 0)
            return std::nullopt; // the first two sextets are never padding

        if (c2 == '=')
        {
            if (!last_quad || c3 != '=')
                return std::nullopt; // "xx=y" or a non-terminal "xx==" is malformed
            out.push_back(static_cast<char>((v0 << 2) | (v1 >> 4)));
            continue;
        }
        const int v2 = rev[static_cast<unsigned char>(c2)];
        if (v2 < 0)
            return std::nullopt;

        if (c3 == '=')
        {
            if (!last_quad)
                return std::nullopt;
            out.push_back(static_cast<char>((v0 << 2) | (v1 >> 4)));
            out.push_back(static_cast<char>(((v1 & 0x0f) << 4) | (v2 >> 2)));
            continue;
        }
        const int v3 = rev[static_cast<unsigned char>(c3)];
        if (v3 < 0)
            return std::nullopt;

        out.push_back(static_cast<char>((v0 << 2) | (v1 >> 4)));
        out.push_back(static_cast<char>(((v1 & 0x0f) << 4) | (v2 >> 2)));
        out.push_back(static_cast<char>(((v2 & 0x03) << 6) | v3));
    }
    return out;
}

} // namespace context::editor::pkg
