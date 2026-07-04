// Asset GUID minting + validation (see guid.h).

#include "context/editor/assetdb/guid.h"

namespace context::editor::assetdb
{

namespace
{
constexpr std::size_t kGuidChars = 32;

[[nodiscard]] bool is_lower_hex(char c) noexcept
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

void append_hex64(std::uint64_t v, std::string& out)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    for (int shift = 60; shift >= 0; shift -= 4)
        out.push_back(kDigits[(v >> shift) & 0xFULL]);
}
} // namespace

bool is_guid(std::string_view s) noexcept
{
    if (s.size() != kGuidChars)
        return false;
    for (char c : s)
        if (!is_lower_hex(c))
            return false;
    return true;
}

std::string format_guid(std::uint64_t hi, std::uint64_t lo)
{
    std::string out;
    out.reserve(kGuidChars);
    append_hex64(hi, out);
    append_hex64(lo, out);
    return out;
}

RandomGuidGenerator::RandomGuidGenerator()
{
    std::random_device rd;
    std::seed_seq seed{rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd()};
    rng_.seed(seed);
}

std::string RandomGuidGenerator::next()
{
    return format_guid(rng_(), rng_());
}

std::string SequenceGuidGenerator::next()
{
    return format_guid(0, next_++);
}

} // namespace context::editor::assetdb
