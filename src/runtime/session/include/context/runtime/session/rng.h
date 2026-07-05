// Deterministic PRNG for the headless session (R-SIM-005 / R-QA-005).
//
// splitmix64: identical output on every platform (pure 64-bit integer ops — no float, no host
// entropy), so a session is fully reproducible from its seed alone. The RNG state is part of the
// serialized session state, so stepping across a save/load boundary is bit-identical to stepping
// straight through (the property the one-shot CLI `session step` relies on).

#pragma once

#include <cstdint>

namespace context::runtime::session
{

class Rng
{
public:
    explicit constexpr Rng(std::uint64_t seed) noexcept : state_(seed) {}

    constexpr std::uint64_t next_u64() noexcept
    {
        state_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    // A signed value in the inclusive range [lo, hi]. Integer domain (no float) so it is
    // deterministic across platforms. A degenerate range (hi <= lo) returns lo.
    constexpr std::int64_t next_range(std::int64_t lo, std::int64_t hi) noexcept
    {
        if (hi <= lo)
            return lo;
        const std::uint64_t span = static_cast<std::uint64_t>(hi - lo) + 1ULL;
        return lo + static_cast<std::int64_t>(next_u64() % span);
    }

    [[nodiscard]] constexpr std::uint64_t state() const noexcept { return state_; }
    constexpr void set_state(std::uint64_t s) noexcept { state_ = s; }

private:
    std::uint64_t state_;
};

} // namespace context::runtime::session
