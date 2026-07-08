// Minimal POSIX ustar reader/writer (see tar.h).

#include "context/editor/pkg/tar.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace context::editor::pkg
{
namespace
{

constexpr std::size_t kBlock = 512;
constexpr std::size_t kNameOff = 0;
constexpr std::size_t kNameLen = 100;
constexpr std::size_t kSizeOff = 124;
constexpr std::size_t kSizeLen = 12;
constexpr std::size_t kChksumOff = 148;
constexpr std::size_t kChksumLen = 8;
constexpr std::size_t kTypeOff = 156;
constexpr std::size_t kMagicOff = 257;
constexpr std::size_t kPrefixOff = 345;
constexpr std::size_t kPrefixLen = 155;

bool is_zero_block(const char* block)
{
    for (std::size_t i = 0; i < kBlock; ++i)
        if (block[i] != '\0')
            return false;
    return true;
}

// Read a NUL-terminated (or field-filling) string field.
std::string read_field(const char* block, std::size_t off, std::size_t len)
{
    std::size_t n = 0;
    while (n < len && block[off + n] != '\0')
        ++n;
    return std::string(block + off, n);
}

// Parse an octal ASCII field (used for size). Returns false on a non-octal digit.
bool read_octal(const char* block, std::size_t off, std::size_t len, std::uint64_t& out)
{
    std::uint64_t value = 0;
    std::size_t i = off;
    const std::size_t end = off + len;
    while (i < end && (block[i] == ' ' || block[i] == '\0'))
        ++i;
    bool any = false;
    while (i < end && block[i] != ' ' && block[i] != '\0')
    {
        const char c = block[i];
        if (c < '0' || c > '7')
            return false;
        value = (value << 3) | static_cast<std::uint64_t>(c - '0');
        any = true;
        ++i;
    }
    if (!any)
        return false;
    out = value;
    return true;
}

bool stored_checksum_ok(const char* block)
{
    // The stored checksum is the octal sum of every header byte with the checksum field taken as 8
    // spaces. Both the historical unsigned and signed sums are accepted (POSIX ustar producers
    // differ on the signedness of the byte sum), matching real-world archives.
    std::uint64_t stored = 0;
    if (!read_octal(block, kChksumOff, kChksumLen, stored))
        return false;
    std::uint32_t unsigned_sum = 0;
    std::int32_t signed_sum = 0;
    for (std::size_t i = 0; i < kBlock; ++i)
    {
        const char raw = (i >= kChksumOff && i < kChksumOff + kChksumLen) ? ' ' : block[i];
        unsigned_sum += static_cast<unsigned char>(raw);
        signed_sum += static_cast<int>(static_cast<signed char>(raw));
    }
    return stored == unsigned_sum ||
           stored == static_cast<std::uint64_t>(static_cast<std::uint32_t>(signed_sum));
}

void write_field(char* block, std::size_t off, std::size_t len, const std::string& value)
{
    std::memset(block + off, 0, len);
    const std::size_t n = value.size() < len ? value.size() : len;
    std::memcpy(block + off, value.data(), n);
}

// Write an octal field with a trailing NUL (ustar convention: `len-1` octal digits + NUL).
void write_octal(char* block, std::size_t off, std::size_t len, std::uint64_t value)
{
    std::memset(block + off, '0', len - 1);
    block[off + len - 1] = '\0';
    std::size_t pos = off + len - 2;
    while (true)
    {
        block[pos] = static_cast<char>('0' + static_cast<char>(value & 0x7u));
        value >>= 3;
        if (pos == off || value == 0)
            break;
        --pos;
    }
}

} // namespace

std::optional<std::vector<TarEntry>> tar_read(std::string_view archive)
{
    std::vector<TarEntry> entries;
    std::size_t offset = 0;
    while (offset + kBlock <= archive.size())
    {
        const char* block = archive.data() + offset;
        if (is_zero_block(block))
            break; // the terminating zero block(s)
        offset += kBlock;

        const std::string magic = read_field(block, kMagicOff, 5);
        if (magic != "ustar")
            return std::nullopt;
        if (!stored_checksum_ok(block))
            return std::nullopt;

        std::uint64_t size = 0;
        if (!read_octal(block, kSizeOff, kSizeLen, size))
            return std::nullopt;

        const std::string name = read_field(block, kNameOff, kNameLen);
        const std::string prefix = read_field(block, kPrefixOff, kPrefixLen);
        const char type = block[kTypeOff];

        TarEntry entry;
        entry.path = prefix.empty() ? name : prefix + "/" + name;
        entry.is_dir = (type == '5');

        if (!entry.is_dir && size > 0)
        {
            if (offset + size > archive.size())
                return std::nullopt; // truncated body
            entry.data.assign(archive.data() + offset, static_cast<std::size_t>(size));
            const std::size_t padded = (static_cast<std::size_t>(size) + kBlock - 1) / kBlock * kBlock;
            offset += padded;
        }
        // A regular-file typeflag is '0' or '\0'; anything other than a directory we still record as
        // a file entry (its bytes, if any, were consumed above).
        entries.push_back(std::move(entry));
    }
    return entries;
}

std::optional<std::string> tar_write(const std::vector<TarEntry>& entries)
{
    std::string out;
    for (const TarEntry& entry : entries)
    {
        std::array<char, kBlock> header{};
        header.fill('\0');

        std::string name = entry.path;
        std::string prefix;
        if (name.size() > kNameLen)
        {
            // Split at a '/' so name<=100 and prefix<=155 (the ustar long-path convention).
            const std::size_t split = name.rfind('/', kNameLen);
            if (split == std::string::npos || split > kPrefixLen ||
                name.size() - split - 1 > kNameLen)
                return std::nullopt;
            prefix = name.substr(0, split);
            name = name.substr(split + 1);
        }

        write_field(header.data(), kNameOff, kNameLen, name);
        write_field(header.data(), kPrefixOff, kPrefixLen, prefix);
        write_octal(header.data(), 100, 8, entry.is_dir ? 0755u : 0644u); // mode
        write_octal(header.data(), 108, 8, 0u);                           // uid
        write_octal(header.data(), 116, 8, 0u);                           // gid
        write_octal(header.data(), kSizeOff, kSizeLen, entry.is_dir ? 0u : entry.data.size());
        write_octal(header.data(), 136, 12, 0u); // mtime
        header[kTypeOff] = entry.is_dir ? '5' : '0';
        std::memcpy(header.data() + kMagicOff, "ustar", 5);
        header[kMagicOff + 5] = '\0';
        header[263] = '0'; // version "00"
        header[264] = '0';

        // Checksum: spaces in the field, sum unsigned, then write the octal sum.
        std::memset(header.data() + kChksumOff, ' ', kChksumLen);
        std::uint32_t sum = 0;
        for (char c : header)
            sum += static_cast<unsigned char>(c);
        write_octal(header.data(), kChksumOff, 7, sum); // 6 octal digits + NUL in [148,155)
        header[kChksumOff + 7] = ' ';

        out.append(header.data(), kBlock);
        if (!entry.is_dir && !entry.data.empty())
        {
            out.append(entry.data);
            const std::size_t pad = (kBlock - entry.data.size() % kBlock) % kBlock;
            out.append(pad, '\0');
        }
    }
    out.append(2 * kBlock, '\0'); // two terminating zero blocks
    return out;
}

} // namespace context::editor::pkg
