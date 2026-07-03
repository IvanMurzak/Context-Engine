// HMAC-SHA256 implementation (RFC 2104).

#include "context/editor/filesync/hmac.h"

#include "context/editor/filesync/sha256.h"

#include <cstddef>
#include <string>

namespace context::editor::filesync
{

std::string hmac_sha256_hex(std::string_view key, std::string_view message)
{
    constexpr std::size_t kBlockSize = 64;

    // Keys longer than the block size are hashed down first (RFC 2104).
    std::string block_key;
    if (key.size() > kBlockSize)
    {
        const Sha256Digest hashed = sha256(key);
        block_key.assign(reinterpret_cast<const char*>(hashed.data()), hashed.size());
    }
    else
    {
        block_key.assign(key);
    }
    block_key.resize(kBlockSize, '\0');

    std::string inner_pad(kBlockSize, '\0');
    std::string outer_pad(kBlockSize, '\0');
    for (std::size_t i = 0; i < kBlockSize; ++i)
    {
        const auto k = static_cast<unsigned char>(block_key[i]);
        inner_pad[i] = static_cast<char>(k ^ 0x36u);
        outer_pad[i] = static_cast<char>(k ^ 0x5cu);
    }

    const Sha256Digest inner = sha256(inner_pad + std::string{message});
    std::string outer_input = outer_pad;
    outer_input.append(reinterpret_cast<const char*>(inner.data()), inner.size());
    return to_hex(sha256(outer_input));
}

} // namespace context::editor::filesync
