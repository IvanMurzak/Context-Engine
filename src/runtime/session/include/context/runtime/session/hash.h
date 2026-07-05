// Deterministic incremental FNV-1a — the state-hash primitive (R-QA-005 / L-54).
//
// Same 64-bit FNV-1a family as serializer::canonical_hash_of, but INCREMENTAL so the hierarchical
// state hash composes per-archetype / per-system sub-hashes without materializing intermediate
// buffers. Integers are folded in FIXED big-endian byte order, so a digest is bit-identical on
// x86-64 and arm64 — the state hash must match across the Linux-x64 / Win-x64 / macOS-arm64
// determinism matrix and must NEVER depend on host endianness (the cross-platform determinism law).

#pragma once

#include <cstdint>
#include <initializer_list>
#include <string_view>

namespace context::runtime::session
{

class Fnv1a
{
public:
    static constexpr std::uint64_t kOffsetBasis = 1469598103934665603ULL;
    static constexpr std::uint64_t kPrime = 1099511628211ULL;

    constexpr void update_byte(std::uint8_t byte) noexcept
    {
        state_ ^= byte;
        state_ *= kPrime;
    }

    void update_bytes(std::string_view bytes) noexcept
    {
        for (char c : bytes)
            update_byte(static_cast<std::uint8_t>(c));
    }

    // Fold a 64-bit value in fixed big-endian order (endianness-independent, arch-independent).
    constexpr void update_u64(std::uint64_t v) noexcept
    {
        for (int shift = 56; shift >= 0; shift -= 8)
            update_byte(static_cast<std::uint8_t>((v >> shift) & 0xFFu));
    }

    constexpr void update_i64(std::int64_t v) noexcept
    {
        update_u64(static_cast<std::uint64_t>(v));
    }

    [[nodiscard]] constexpr std::uint64_t digest() const noexcept { return state_; }

private:
    std::uint64_t state_ = kOffsetBasis;
};

// Compose an ordered list of sub-hashes into one parent hash (the hierarchical combine). Order is
// significant — the caller sorts its children into a canonical order before combining.
[[nodiscard]] inline std::uint64_t combine_hashes(std::initializer_list<std::uint64_t> subs) noexcept
{
    Fnv1a h;
    for (std::uint64_t s : subs)
        h.update_u64(s);
    return h.digest();
}

// FNV-1a over a byte string, one-shot (matches serializer::canonical_hash_of on the same bytes —
// both are the same 64-bit FNV-1a; provided here so the session layer stays serializer-independent
// for pure byte hashing).
[[nodiscard]] inline std::uint64_t hash_bytes(std::string_view bytes) noexcept
{
    Fnv1a h;
    h.update_bytes(bytes);
    return h.digest();
}

} // namespace context::runtime::session
