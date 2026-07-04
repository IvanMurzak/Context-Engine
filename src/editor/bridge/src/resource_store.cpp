// R-CLI-017 resource-store implementation (see resource_store.h).

#include "context/editor/bridge/resource_store.h"

#include <algorithm>
#include <fstream>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace context::editor::bridge
{

std::string hex_encode(std::string_view bytes)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const char ch : bytes)
    {
        const auto b = static_cast<unsigned char>(ch);
        out.push_back(kDigits[b >> 4]);
        out.push_back(kDigits[b & 0x0f]);
    }
    return out;
}

std::optional<std::string> hex_decode(std::string_view hex)
{
    if (hex.size() % 2 != 0)
        return std::nullopt;
    const auto nibble = [](char ch) -> int
    {
        if (ch >= '0' && ch <= '9')
            return ch - '0';
        if (ch >= 'a' && ch <= 'f')
            return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F')
            return ch - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2)
    {
        const int hi = nibble(hex[i]);
        const int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0)
            return std::nullopt;
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return out;
}

ResourceStore::ResourceStore(fs::path dir, std::string instance_id)
    : dir_(std::move(dir)), instance_id_(std::move(instance_id))
{
    // A previous incarnation's spool files back handles that are ALREADY invalid (the instance id
    // in every URI changed), so clear the residue rather than letting it accumulate. Best-effort.
    std::error_code ec;
    fs::remove_all(dir_, ec);
    fs::create_directories(dir_, ec);
}

fs::path ResourceStore::spool_path(std::uint64_t payload_id) const
{
    return dir_ / (std::to_string(payload_id) + ".res");
}

std::optional<contract::ResourceHandle> ResourceStore::put(std::string_view payload)
{
    const std::uint64_t id = next_payload_++;
    const fs::path path = spool_path(id);
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out)
            return std::nullopt;
        if (!payload.empty())
            out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        out.flush();
        if (!out)
        {
            out.close();
            std::error_code ec;
            fs::remove(path, ec); // no partial spool residue
            return std::nullopt;
        }
    }
    sizes_[id] = payload.size();

    contract::ResourceHandle handle;
    handle.instance_id = instance_id_;
    handle.payload_id = id;
    handle.size_bytes = payload.size();
    return handle;
}

std::optional<ResourceStore::ReadResult> ResourceStore::read(const contract::ResourceHandle& handle,
                                                             std::uint64_t offset,
                                                             std::uint64_t max_len) const
{
    // Same-instance only (v1 same-FS resolution): a foreign / stale-incarnation handle is unknown.
    if (handle.instance_id != instance_id_)
        return std::nullopt;
    const auto it = sizes_.find(handle.payload_id);
    if (it == sizes_.end())
        return std::nullopt;
    const std::uint64_t total = it->second;
    if (offset > total)
        return std::nullopt;

    const std::uint64_t cap = kResourceReadMaxChunkBytes;
    const std::uint64_t want = max_len == 0 ? cap : std::min(max_len, cap);
    const std::uint64_t len = std::min(want, total - offset);

    ReadResult out;
    out.offset = offset;
    out.total = total;

    if (len > 0)
    {
        std::ifstream in(spool_path(handle.payload_id), std::ios::binary);
        if (!in)
            return std::nullopt; // spool file vanished — treat as unknown
        in.seekg(static_cast<std::streamoff>(offset));
        out.bytes.resize(static_cast<std::size_t>(len));
        in.read(out.bytes.data(), static_cast<std::streamsize>(len));
        if (in.gcount() != static_cast<std::streamsize>(len))
            return std::nullopt;
    }
    out.eof = offset + len >= total;
    return out;
}

std::string ResourceStore::local_path_hint(const contract::ResourceHandle& handle) const
{
    if (handle.instance_id != instance_id_ || sizes_.find(handle.payload_id) == sizes_.end())
        return std::string();
    std::error_code ec;
    const fs::path abs = fs::absolute(spool_path(handle.payload_id), ec);
    return ec ? std::string() : abs.generic_string();
}

} // namespace context::editor::bridge
