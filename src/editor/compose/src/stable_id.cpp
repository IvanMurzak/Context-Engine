// Stable intra-file ids — see stable_id.h.

#include "context/editor/compose/stable_id.h"

#include <chrono>
#include <random>

namespace context::editor::compose
{

namespace
{

[[nodiscard]] bool is_lower_hex(char c) noexcept
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

} // namespace

bool is_stable_id(std::string_view id) noexcept
{
    if (id.size() < 16 || id.size() > 32)
        return false;
    for (char c : id)
        if (!is_lower_hex(c))
            return false;
    return true;
}

std::string format_stable_id(std::uint64_t bits)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(16, '0');
    for (std::size_t i = 0; i < 16; ++i)
        out[15 - i] = kHex[(bits >> (i * 4)) & 0xF];
    return out;
}

std::string mint_stable_id()
{
    // Two independent 32-bit draws from the OS entropy source, stirred with a steady-clock tick.
    // The stir defends against a hypothetical deterministic random_device (a historic MinGW
    // failure mode) — it can never REDUCE entropy. Deliberately not a PRNG stream: ids must not
    // be sequential or seed-reproducible (L-33).
    std::random_device rd;
    std::uint64_t bits = (static_cast<std::uint64_t>(rd()) << 32) ^ static_cast<std::uint64_t>(rd());
    bits ^= static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return format_stable_id(bits);
}

} // namespace context::editor::compose
