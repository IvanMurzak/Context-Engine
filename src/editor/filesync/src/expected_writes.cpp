// Expected-writes table implementation.

#include "context/editor/filesync/expected_writes.h"

namespace context::editor::filesync
{

void ExpectedWrites::register_write(std::string_view path, std::uint64_t content_hash,
                                    std::uint64_t now_nanos)
{
    table_[std::string{path}] = Entry{content_hash, now_nanos + ttl_nanos_};
}

bool ExpectedWrites::is_self_echo(std::string_view path, std::uint64_t content_hash,
                                  std::uint64_t now_nanos)
{
    auto it = table_.find(std::string{path});
    if (it == table_.end())
        return false;

    // Expired: never a valid self-echo — drop it so a later external edit is reconciled normally.
    if (now_nanos >= it->second.expiry_nanos)
    {
        table_.erase(it);
        return false;
    }

    if (it->second.content_hash != content_hash)
        return false;

    table_.erase(it); // consume: one registered write suppresses exactly one echo.
    return true;
}

void ExpectedWrites::expire(std::uint64_t now_nanos)
{
    for (auto it = table_.begin(); it != table_.end();)
    {
        if (now_nanos >= it->second.expiry_nanos)
            it = table_.erase(it);
        else
            ++it;
    }
}

} // namespace context::editor::filesync
