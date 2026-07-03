// Deterministic, portable seeded RNG for the fault-injection harness (R-QA-010 / R-QA-011).
//
// splitmix64 — a pure, dependency-free integer generator. Every operation is uint64 wrap-around
// arithmetic (no floats, no <random> distributions, which are NOT portable across standard-library
// implementations), so the sequence is BIT-IDENTICAL across the 3-OS CI matrix for a given seed.
// That reproducibility is the whole point: a failing scenario seed replays to the exact same fault
// schedule on any machine, so it can be minimized and committed as a regression case (R-QA-011).

#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace context::testing
{

// A tiny deterministic value-type PRNG. Copyable so a scenario can snapshot/branch its stream.
class SeededRng
{
public:
    explicit SeededRng(std::uint64_t seed) noexcept : state_(seed) {}

    // Next 64-bit value (splitmix64).
    std::uint64_t next_u64() noexcept
    {
        state_ += 0x9e3779b97f4a7c15ULL;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }

    // Uniform-ish value in [0, n); n == 0 yields 0. Modulo bias is irrelevant for fault scheduling.
    std::uint64_t bounded(std::uint64_t n) noexcept { return n == 0 ? 0 : next_u64() % n; }

    // Inclusive integer range [lo, hi]; requires lo <= hi.
    std::uint64_t range(std::uint64_t lo, std::uint64_t hi) noexcept
    {
        return lo + bounded(hi - lo + 1);
    }

    // True with probability num/den (den > 0).
    bool chance(std::uint64_t num, std::uint64_t den) noexcept { return bounded(den) < num; }

    // Deterministic in-place Fisher-Yates shuffle.
    template <class T>
    void shuffle(std::vector<T>& items) noexcept
    {
        for (std::size_t i = items.size(); i > 1; --i)
        {
            const std::size_t j = static_cast<std::size_t>(bounded(i));
            std::swap(items[i - 1], items[j]);
        }
    }

private:
    std::uint64_t state_;
};

} // namespace context::testing
