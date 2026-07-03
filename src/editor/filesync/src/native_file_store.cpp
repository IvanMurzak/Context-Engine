// Native (on-disk) FileStore implementation — real atomic write-temp-rename + fsync durability.

#include "context/editor/filesync/native_file_store.h"

#include "context/editor/filesync/path_jail.h"

#include <algorithm>
#include <cstdio> // std::rename (POSIX atomic replace)
#include <fstream>
#include <iterator>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace context::editor::filesync
{

NativeFileStore::NativeFileStore(fs::path base) : base_(std::move(base)) {}

fs::path NativeFileStore::resolve(std::string_view logical) const
{
    const std::string norm = normalize_path(logical);
    if (norm.empty())
        return base_;
    // norm is a forward-slash logical path (relative for every path the jail admits); operator/ appends
    // it under base_. std::filesystem::path parses the '/'-separated form on every platform.
    return base_ / fs::path(norm);
}

bool NativeFileStore::exists(std::string_view path) const
{
    std::error_code ec;
    // File-only, matching MemoryFileStore (which stores files, never directories).
    return fs::is_regular_file(resolve(path), ec);
}

std::optional<std::string> NativeFileStore::read(std::string_view path) const
{
    const fs::path target = resolve(path);
    std::error_code ec;
    if (!fs::is_regular_file(target, ec))
        return std::nullopt;

    std::ifstream in(target, std::ios::binary);
    if (!in)
        return std::nullopt;
    // istreambuf_iterator reads raw bytes (binary mode: no CRLF translation) and handles empty files.
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad())
        return std::nullopt;
    return data;
}

std::optional<FileStat> NativeFileStore::stat(std::string_view path) const
{
    const fs::path target = resolve(path);
    std::error_code ec;
    if (!fs::is_regular_file(target, ec))
        return std::nullopt;

    const std::uintmax_t size = fs::file_size(target, ec);
    if (ec)
        return std::nullopt;
    const fs::file_time_type mtime = fs::last_write_time(target, ec);
    if (ec)
        return std::nullopt;

    FileStat st;
    st.size = static_cast<std::uint64_t>(size);
    // file_time_type's rep/period is implementation-defined; the FileStat contract only ever compares
    // mtime for EQUALITY against the index, so a stable per-file value suffices. Cast through a concrete
    // integer (long long) first — std::to_string / implicit conversions on the raw rep are ambiguous
    // under libc++ (conventions.md § Coding conventions).
    st.mtime_nanos = static_cast<std::uint64_t>(static_cast<long long>(mtime.time_since_epoch().count()));
    return st;
}

std::vector<std::string> NativeFileStore::list(std::string_view dir) const
{
    const fs::path root = resolve(dir);
    std::vector<std::string> out;
    std::error_code ec;

    if (fs::is_regular_file(root, ec))
    {
        out.push_back(normalize_path(dir));
        return out;
    }
    if (!fs::is_directory(root, ec))
        return out; // missing / not-a-dir -> empty, mirroring MemoryFileStore on an unseen prefix.

    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec),
         end;
         it != end; it.increment(ec))
    {
        if (ec)
            break;
        std::error_code fec;
        if (!it->is_regular_file(fec) || fec)
            continue;
        // Purely lexical relative path (no FS access / symlink resolution): the iterator's entries are
        // built by appending to `base_`-rooted `root`, so lexically_relative(base_) is well-defined.
        const std::string rel = it->path().lexically_relative(base_).generic_string();
        if (rel.empty() || rel == "." || rel.rfind("..", 0) == 0)
            continue; // never surface a path that escapes base_.
        out.push_back(normalize_path(rel));
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool NativeFileStore::write(std::string_view path, std::string_view data)
{
    const fs::path target = resolve(path);
    std::error_code ec;
    if (target.has_parent_path())
        fs::create_directories(target.parent_path(), ec); // idempotent; a real failure surfaces below.

    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    if (!data.empty())
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.flush();
    if (!out)
    {
        // A real I/O failure mid-write (e.g. ENOSPC): don't leave a partially-written file behind.
        // Every write() target is an atomic .tmp, so a stray residue would outlive the failed op.
        out.close();
        fs::remove(target, ec);
        return false;
    }
    return true;
}

bool NativeFileStore::rename(std::string_view from, std::string_view to)
{
    const fs::path src = resolve(from);
    const fs::path dst = resolve(to);
    std::error_code ec;
    if (dst.has_parent_path())
        fs::create_directories(dst.parent_path(), ec);

#ifdef _WIN32
    // MoveFileExW atomically replaces an existing destination on the same volume. Used directly (not
    // std::filesystem::rename) because MinGW libstdc++ maps rename -> _wrename, which FAILS when the
    // destination exists; MOVEFILE_WRITE_THROUGH flushes the metadata before returning.
    const BOOL ok =
        MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    return ok != 0;
#else
    // POSIX rename(2) atomically replaces the destination within a single filesystem.
    return std::rename(src.c_str(), dst.c_str()) == 0;
#endif
}

bool NativeFileStore::remove(std::string_view path)
{
    const fs::path target = resolve(path);
    std::error_code ec;
    // File-only, matching MemoryFileStore: never remove a directory (mirrors exists()/read()/stat()).
    if (!fs::is_regular_file(target, ec))
        return false;
    return fs::remove(target, ec); // true iff the file was removed.
}

void NativeFileStore::fsync(std::string_view path)
{
    const fs::path target = resolve(path);
    std::error_code ec;
    if (!fs::is_regular_file(target, ec))
        return; // nothing durable to flush (e.g. a temp already renamed away). Best-effort, never throws.

#ifdef _WIN32
    // FlushFileBuffers requires the handle to hold GENERIC_WRITE access — a GENERIC_READ-only handle
    // fails with ERROR_ACCESS_DENIED, silently turning the durability barrier into a no-op. Open R+W.
    HANDLE h = CreateFileW(target.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    FlushFileBuffers(h);
    CloseHandle(h);
#else
    const int fd = ::open(target.c_str(), O_RDONLY);
    if (fd >= 0)
    {
        (void)::fsync(fd);
        (void)::close(fd);
    }
    // Best-effort fsync of the file's IMMEDIATE parent directory so its create/rename directory entry is
    // durable (POSIX). Only that one level is flushed — freshly-created ANCESTOR directories above it are
    // not — and macOS F_FULLFSYNC would be a stronger barrier; both are documented hardening follow-ups.
    if (target.has_parent_path())
    {
        const int dfd = ::open(target.parent_path().c_str(), O_RDONLY);
        if (dfd >= 0)
        {
            (void)::fsync(dfd);
            (void)::close(dfd);
        }
    }
#endif
}

} // namespace context::editor::filesync
