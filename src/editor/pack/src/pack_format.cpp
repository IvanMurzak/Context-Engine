// Little-endian byte primitives for the v1 pack format (see pack_format.h).

#include "context/editor/pack/pack_format.h"

namespace context::editor::pack
{

void put_u32(std::string& out, std::uint32_t v)
{
    out.push_back(static_cast<char>(v & 0xFFu));
    out.push_back(static_cast<char>((v >> 8) & 0xFFu));
    out.push_back(static_cast<char>((v >> 16) & 0xFFu));
    out.push_back(static_cast<char>((v >> 24) & 0xFFu));
}

void put_u64(std::string& out, std::uint64_t v)
{
    for (unsigned i = 0; i < 8; ++i)
        out.push_back(static_cast<char>((v >> (8u * i)) & 0xFFu));
}

bool read_u32(std::string_view bytes, std::size_t offset, std::uint32_t& out) noexcept
{
    if (offset > bytes.size() || bytes.size() - offset < 4)
        return false;
    out = static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset]))
          | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 1])) << 8)
          | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 2])) << 16)
          | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 3])) << 24);
    return true;
}

bool read_u64(std::string_view bytes, std::size_t offset, std::uint64_t& out) noexcept
{
    if (offset > bytes.size() || bytes.size() - offset < 8)
        return false;
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[offset + i])) << (8u * i);
    out = value;
    return true;
}

} // namespace context::editor::pack
