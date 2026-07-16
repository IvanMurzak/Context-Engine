// Internal helpers shared by the runtime content module's .cpp files (NOT a public header — it lives
// under src/, not include/). Small JSON-tree accessors + a stable-id hex parser + an FNV-1a-64 mixer
// used to fold the feed-independent world hash. Header-only, internal linkage.

#pragma once

#include "context/editor/serializer/json_tree.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace context::runtime::content::detail
{

namespace serializer = context::editor::serializer;

// Look up an object member by key (linear over members — objects here are tiny + fixed-shape).
// Returns nullptr when `object` is not an object or the key is absent.
[[nodiscard]] inline const serializer::JsonValue* find_member(const serializer::JsonValue& object,
                                                              std::string_view key)
{
    if (object.type != serializer::JsonValue::Type::object)
        return nullptr;
    for (const serializer::JsonMember& m : object.members)
        if (m.key == key)
            return &m.value;
    return nullptr;
}

// Parse a canonical 16-char lowercase-hex stable id (compose::format_stable_id output) back to its
// u64 bits. Strict: EXACTLY 16 lowercase-hex chars, else nullopt (a malformed chunk is refused).
[[nodiscard]] inline std::optional<std::uint64_t> parse_stable_id(std::string_view id)
{
    if (id.size() != 16)
        return std::nullopt;
    std::uint64_t value = 0;
    for (char c : id)
    {
        std::uint64_t digit = 0;
        if (c >= '0' && c <= '9')
            digit = static_cast<std::uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f')
            digit = static_cast<std::uint64_t>(c - 'a') + 10;
        else
            return std::nullopt; // uppercase / non-hex is not the canonical form
        value = (value << 4) | digit;
    }
    return value;
}

// FNV-1a-64 constants (the same primitive the serializer's canonical_hash_of uses); a fixed byte
// order makes the fold identical on every OS leg.
inline constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
inline constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

// Fold raw bytes into a running FNV-1a-64 accumulator.
inline void mix_bytes(std::uint64_t& h, std::string_view bytes) noexcept
{
    for (char c : bytes)
    {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        h *= kFnvPrime;
    }
}

// Fold a u64 (fixed little-endian byte order — host-independent) into the accumulator.
inline void mix_u64(std::uint64_t& h, std::uint64_t value) noexcept
{
    for (unsigned i = 0; i < 8; ++i)
    {
        h ^= (value >> (8u * i)) & 0xFFu;
        h *= kFnvPrime;
    }
}

} // namespace context::runtime::content::detail
