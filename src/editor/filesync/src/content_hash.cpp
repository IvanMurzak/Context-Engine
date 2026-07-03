// FNV-1a 64-bit content hash implementation.

#include "context/editor/filesync/content_hash.h"

namespace context::editor::filesync
{

std::uint64_t content_hash(std::string_view bytes) noexcept
{
    constexpr std::uint64_t kOffsetBasis = 1469598103934665603ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;

    std::uint64_t hash = kOffsetBasis;
    for (char ch : bytes)
    {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
        hash *= kPrime;
    }
    return hash;
}

} // namespace context::editor::filesync
