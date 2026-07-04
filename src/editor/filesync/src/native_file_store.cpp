// Native (on-disk) FileStore implementation — real atomic write-temp-rename + fsync durability,
// with the R-SEC-008 TOCTOU-safe path jail on every write-side operation (write/rename/remove).
//
// The jail here is resolve-then-verify-by-fd/handle, NOT validate-the-string-then-open (a
// time-of-check/time-of-use race a symlink swap defeats):
//   * POSIX  — open the parent directory (following any INTERNAL, non-escaping links), verify the
//     OPENED fd's fully-resolved path (/proc/self/fd readlink on Linux, F_GETPATH on macOS) sits
//     inside the resolved jail root, then act RELATIVE to that pinned fd (openat with O_NOFOLLOW on
//     the leaf / renameat / unlinkat). The verified fd pins the directory inode, so a post-verify
//     symlink swap of any ancestor cannot redirect the operation.
//   * Windows — "handle-reopen + re-realpath validation": open the target itself with
//     FILE_FLAG_OPEN_REPARSE_POINT (a reparse-point LEAF is refused — the O_NOFOLLOW analogue),
//     then verify the OPENED handle's GetFinalPathNameByHandleW result sits inside the resolved
//     jail root BEFORE any byte is written / any disposition is set; renames go handle-relative
//     (SetFileInformationByHandle + FILE_RENAME_INFO.RootDirectory = the verified destination
//     directory handle).
// Root-ESCAPING symlinks/junctions are refused; internal links that resolve inside the jail are
// allowed (R-SEC-008 outlaws escapes, not links). When the jail root itself cannot be resolved, or
// a platform offers no fd->path probe, the jail FAILS CLOSED (the write-side op refuses) rather
// than degrading to the lexical check. Read-side ops (read/stat/list/fsync) keep the lexical
// mapping — hardening them is a documented follow-up; they never land or redirect content.
//
// Logical <-> real mapping: every seam path is a LOGICAL string (e.g. "proj/a.scene"), normalized
// the same way MemoryFileStore keys and the reconcile index are, and resolved under `base` on real
// disk. The LOGICAL jail (normalize + is_inside_jail) stays applied by the callers (reconciler /
// WriteQueue); this layer adds the physical TOCTOU-safe verification beneath it.
//
// Atomicity + durability (R-FILE-004): rename() is an atomic same-volume replace; fsync() is a real
// durability barrier (POSIX fsync on the file + best-effort parent-dir fsync; Windows
// FlushFileBuffers). Bytes are read/written in BINARY mode so content hashes are byte-exact and
// identical across platforms (no CRLF translation).

#include "context/editor/filesync/native_file_store.h"

#include "context/editor/filesync/path_jail.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cwctype>
#else
#include <fcntl.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/param.h> // MAXPATHLEN (F_GETPATH buffer)
#endif
#endif

namespace fs = std::filesystem;

namespace context::editor::filesync
{

namespace
{

#ifdef _WIN32

// The fully-resolved ("final") path of an open handle, in two forms: `raw` (the verbatim
// "\\?\"-prefixed string, usable in a subsequent API call) and `folded` (verbatim prefix stripped,
// separators unified to '/', case-lowered) for jail-prefix comparison. NTFS is case-insensitive by
// default; a fold mismatch can only cause a FALSE REFUSAL — fail closed — never an escape.
struct FinalPath
{
    std::wstring raw;
    std::wstring folded;
};

std::optional<FinalPath> final_path_of(HANDLE handle)
{
    std::vector<wchar_t> buf(32768);
    const DWORD n = GetFinalPathNameByHandleW(handle, buf.data(), static_cast<DWORD>(buf.size()),
                                              FILE_NAME_NORMALIZED);
    if (n == 0 || n >= buf.size())
        return std::nullopt;
    FinalPath out;
    out.raw.assign(buf.data(), n);
    std::wstring s = out.raw;
    if (s.rfind(LR"(\\?\UNC\)", 0) == 0)
        s = L"\\\\" + s.substr(8);
    else if (s.rfind(LR"(\\?\)", 0) == 0)
        s = s.substr(4);
    for (wchar_t& ch : s)
    {
        if (ch == L'\\')
            ch = L'/';
        else
            ch = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(ch)));
    }
    out.folded = std::move(s);
    return out;
}

bool inside_folded_base(const std::wstring& final_path, const std::wstring& base)
{
    if (base.empty())
        return false; // unresolvable jail root -> fail closed
    if (final_path == base)
        return true;
    return final_path.rfind(base + L"/", 0) == 0;
}

// RAII handle.
struct UniqueHandle
{
    HANDLE h = INVALID_HANDLE_VALUE;
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) noexcept : h(handle) {}
    ~UniqueHandle()
    {
        if (h != INVALID_HANDLE_VALUE && h != nullptr)
            CloseHandle(h);
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    [[nodiscard]] bool valid() const noexcept { return h != INVALID_HANDLE_VALUE && h != nullptr; }
};

// Mark the object behind `handle` (opened with DELETE access) for deletion on close.
bool dispose_on_close(HANDLE handle)
{
    FILE_DISPOSITION_INFO dispose{};
    dispose.DeleteFile = TRUE;
    return SetFileInformationByHandle(handle, FileDispositionInfo, &dispose, sizeof(dispose)) != 0;
}

#else // POSIX

// The fully-resolved path of an open descriptor — the "re-realpath after open" half of the jail.
// Linux: readlink /proc/self/fd/<fd>; macOS: fcntl F_GETPATH. On a POSIX platform with neither
// probe the jail fails closed (nullopt -> refuse).
std::optional<std::string> final_path_of_fd(int fd)
{
#if defined(__linux__)
    char buf[4096];
    const std::string link = "/proc/self/fd/" + std::to_string(fd);
    const ssize_t n = ::readlink(link.c_str(), buf, sizeof(buf) - 1);
    if (n <= 0)
        return std::nullopt;
    buf[static_cast<std::size_t>(n)] = '\0';
    return std::string(buf);
#elif defined(__APPLE__)
    char buf[MAXPATHLEN];
    if (::fcntl(fd, F_GETPATH, buf) == -1)
        return std::nullopt;
    return std::string(buf);
#else
    (void)fd;
    return std::nullopt;
#endif
}

bool inside_resolved_base(const std::string& final_path, const std::string& base)
{
    if (base.empty())
        return false; // unresolvable jail root -> fail closed
    if (final_path == base)
        return true;
    return final_path.rfind(base + "/", 0) == 0;
}

// RAII fd.
struct UniqueFd
{
    int fd = -1;
    UniqueFd() = default;
    explicit UniqueFd(int f) noexcept : fd(f) {}
    ~UniqueFd()
    {
        if (fd >= 0)
            ::close(fd);
    }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    // Move-only (copies stay deleted): the user-declared destructor suppresses the implicit move
    // ctor, and open_verified_dir() returns a named UniqueFd by value.
    UniqueFd(UniqueFd&& other) noexcept : fd(other.fd) { other.fd = -1; }
    UniqueFd& operator=(UniqueFd&& other) noexcept
    {
        if (this != &other)
        {
            if (fd >= 0)
                ::close(fd);
            fd = other.fd;
            other.fd = -1;
        }
        return *this;
    }
    [[nodiscard]] bool valid() const noexcept { return fd >= 0; }
};

// Open a directory (following internal links) and verify the OPENED fd resolves inside the jail.
// The returned fd pins the verified directory inode; -1 on open failure or a jail escape.
UniqueFd open_verified_dir(const fs::path& dir, const std::string& base)
{
    UniqueFd out(::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (!out.valid())
        return UniqueFd();
    const std::optional<std::string> real = final_path_of_fd(out.fd);
    if (!real.has_value() || !inside_resolved_base(*real, base))
        return UniqueFd(); // escape (or no probe): fail closed
    return out;
}

bool write_all_fd(int fd, std::string_view data)
{
    std::size_t off = 0;
    while (off < data.size())
    {
        const ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n < 0)
            return false;
        off += static_cast<std::size_t>(n);
    }
    return true;
}

#endif

// Pre-gate for ON-DEMAND parent-directory creation (write()/rename() create missing parents):
// create_directories traverses the LEXICAL path, so behind a pre-existing root-escaping link it
// would leave empty-DIRECTORY residue outside the jail even though the content itself is refused by
// the fd/handle verification below. Verify the deepest EXISTING ancestor of `parent` through the
// SAME opened-fd/handle probe every other check uses (links followed, then the OPENED object's
// resolved location compared against the jail root) before any missing tail is created. The
// per-operation fd/handle verification stays the authoritative TOCTOU gate — there is no
// handle-relative mkdir (Win32), so the race window left here is litter-only (empty directories),
// never content.
bool parent_chain_inside_jail(const fs::path& parent, const fs::path& real_base)
{
    fs::path probe = parent;
    std::error_code ec;
    while (!fs::exists(probe, ec))
    {
        const fs::path up = probe.parent_path();
        if (up.empty() || up == probe)
            return false; // nothing existing to verify against -> fail closed
        probe = up;
    }
#ifdef _WIN32
    UniqueHandle h(CreateFileW(probe.c_str(), FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!h.valid())
        return false;
    const std::optional<FinalPath> real = final_path_of(h.h);
    return real.has_value() && inside_folded_base(real->folded, real_base.wstring());
#else
    // O_NONBLOCK: the deepest existing ancestor is normally a directory (ignored for those), but a
    // pathological FIFO at that position must not hang the gate. Not O_DIRECTORY — a FILE ancestor
    // still verifies (create_directories then fails on it, surfacing below like any mkdir failure).
    UniqueFd fd(::open(probe.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
    if (!fd.valid())
        return false;
    const std::optional<std::string> real = final_path_of_fd(fd.fd);
    return real.has_value() && inside_resolved_base(*real, real_base.string());
#endif
}

} // namespace

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

const fs::path& NativeFileStore::real_base() const
{
    if (!real_base_.empty())
        return real_base_;

    // base_ may not exist yet (write() creates parents on demand) — materialize it, then resolve it
    // through the SAME probe the per-operation verification uses, so both sides of every prefix
    // comparison are folded identically. Left empty on failure: the jail then fails closed.
    std::error_code ec;
    fs::create_directories(base_, ec);
#ifdef _WIN32
    UniqueHandle h(CreateFileW(base_.c_str(), FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (h.valid())
    {
        if (const std::optional<FinalPath> real = final_path_of(h.h); real.has_value())
            real_base_ = fs::path(real->folded);
    }
#else
    UniqueFd fd(::open(base_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (fd.valid())
    {
        if (const std::optional<std::string> real = final_path_of_fd(fd.fd); real.has_value())
            real_base_ = fs::path(*real);
    }
#endif
    return real_base_;
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
    // Create missing parents only behind the pre-gate (see parent_chain_inside_jail): an existing
    // parent needs no mkdir (and the target open below verifies it), so the gate runs only when a
    // directory would actually be created.
    if (target.has_parent_path() && !fs::exists(target.parent_path(), ec))
    {
        if (!parent_chain_inside_jail(target.parent_path(), real_base()))
            return false;
        fs::create_directories(target.parent_path(), ec); // a real failure surfaces below.
    }

#ifdef _WIN32
    const std::wstring base = real_base().wstring();

    // Handle-reopen + re-realpath validation (R-SEC-008): open the target itself WITHOUT following a
    // reparse-point leaf, then verify the OPENED handle before a single byte is written. DELETE
    // access lets a failed verification clean up a just-created stray via delete-on-close.
    UniqueHandle h(CreateFileW(target.c_str(), GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!h.valid())
        return false;
    const bool created = GetLastError() != ERROR_ALREADY_EXISTS;

    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileInformationByHandle(h.h, &info) == 0)
        return false;
    const bool is_reparse = (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    const bool is_dir = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const std::optional<FinalPath> real = final_path_of(h.h);
    if (is_reparse || is_dir || !real.has_value() || !inside_folded_base(real->folded, base))
    {
        // A symlink/junction leaf (the O_NOFOLLOW analogue), a directory, or a resolved location
        // outside the jail. Content untouched (no truncate happened); remove only what WE created.
        if (created)
            (void)dispose_on_close(h.h);
        return false;
    }

    // Verified: now (and only now) replace the content — rewind, write, truncate the tail.
    LARGE_INTEGER zero{};
    if (SetFilePointerEx(h.h, zero, nullptr, FILE_BEGIN) == 0)
        return false;
    std::size_t off = 0;
    bool ok = true;
    while (ok && off < data.size())
    {
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(data.size() - off, 1u << 30));
        DWORD written = 0;
        ok = WriteFile(h.h, data.data() + off, chunk, &written, nullptr) != 0 && written != 0;
        off += written;
    }
    if (ok)
        ok = SetEndOfFile(h.h) != 0;
    if (!ok)
    {
        // A real I/O failure mid-write (e.g. ENOSPC): don't leave a partially-written file behind.
        // Every write() target is an atomic .tmp, so a stray residue would outlive the failed op.
        (void)dispose_on_close(h.h);
        return false;
    }
    return true;
#else
    // Resolve-then-verify-by-fd (R-SEC-008): pin + verify the parent directory, then open the leaf
    // RELATIVE to the verified fd with O_NOFOLLOW (an escaping ancestor cannot redirect an openat on
    // a pinned fd; a symlink leaf is refused).
    const std::string base = real_base().string();
    const fs::path parent = target.has_parent_path() ? target.parent_path() : base_;
    UniqueFd parent_fd = open_verified_dir(parent, base);
    if (!parent_fd.valid())
        return false;

    const std::string leaf = target.filename().string();
    UniqueFd fd(::openat(parent_fd.fd, leaf.c_str(), O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC,
                         0666));
    if (!fd.valid())
        return false; // ELOOP: a symlink leaf was refused (never followed)

    // Re-realpath after open (belt + braces on top of the pinned-parent chain), then truncate ONLY
    // after the opened inode verified inside the jail — never destroy pre-verification content.
    const std::optional<std::string> real = final_path_of_fd(fd.fd);
    if (!real.has_value() || !inside_resolved_base(*real, base))
        return false;
    if (::ftruncate(fd.fd, 0) != 0)
        return false;
    if (!write_all_fd(fd.fd, data))
    {
        // A real I/O failure mid-write (e.g. ENOSPC): don't leave a partially-written file behind.
        // Every write() target is an atomic .tmp, so a stray residue would outlive the failed op.
        (void)::unlinkat(parent_fd.fd, leaf.c_str(), 0);
        return false;
    }
    return true;
#endif
}

bool NativeFileStore::rename(std::string_view from, std::string_view to)
{
    const fs::path src = resolve(from);
    const fs::path dst = resolve(to);
    std::error_code ec;
    // Same mkdir pre-gate as write(): never create missing destination parents through a
    // root-escaping link (see parent_chain_inside_jail).
    if (dst.has_parent_path() && !fs::exists(dst.parent_path(), ec))
    {
        if (!parent_chain_inside_jail(dst.parent_path(), real_base()))
            return false;
        fs::create_directories(dst.parent_path(), ec);
    }

#ifdef _WIN32
    const std::wstring base = real_base().wstring();

    // Open + verify the SOURCE object itself (OPEN_REPARSE_POINT: a link moves as a link, its
    // target untouched), rename it BY HANDLE (SetFileInformationByHandle + FILE_RENAME_INFO with an
    // absolute FileName built from the VERIFIED destination parent's resolved final path), then
    // RE-VERIFY through the still-open handle: after the rename, the handle's final path reflects
    // where the file ACTUALLY landed, so a mid-window junction/symlink swap that redirected the
    // destination is detected and undone (delete-on-close of our own temp content) instead of
    // leaving bytes outside the jail. Replaces MoveFileExW(MOVEFILE_REPLACE_EXISTING), which
    // re-traversed both full path strings at use time with no post-verification.
    UniqueHandle hsrc(CreateFileW(src.c_str(), DELETE | SYNCHRONIZE | FILE_READ_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                  nullptr));
    if (!hsrc.valid())
        return false;
    const std::optional<FinalPath> src_real = final_path_of(hsrc.h);
    if (!src_real.has_value() || !inside_folded_base(src_real->folded, base))
        return false;

    const fs::path dst_parent = dst.has_parent_path() ? dst.parent_path() : base_;
    UniqueHandle hdst(CreateFileW(dst_parent.c_str(), FILE_READ_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!hdst.valid())
        return false;
    const std::optional<FinalPath> dst_real = final_path_of(hdst.h);
    if (!dst_real.has_value() || !inside_folded_base(dst_real->folded, base))
        return false;

    // Absolute, resolved destination: the verified parent's verbatim final path + the leaf. The
    // "\\?\" form is exactly what FILE_RENAME_INFO accepts (and dodges MAX_PATH).
    const std::wstring full_dst = dst_real->raw + L"\\" + dst.filename().wstring();
    const std::size_t name_bytes = full_dst.size() * sizeof(wchar_t);
    std::vector<char> buf(sizeof(FILE_RENAME_INFO) + name_bytes);
    auto* rename_info = reinterpret_cast<FILE_RENAME_INFO*>(buf.data());
    rename_info->ReplaceIfExists = TRUE;  // atomic same-volume replace (R-FILE-004)
    rename_info->RootDirectory = nullptr; // FileName is absolute
    rename_info->FileNameLength = static_cast<DWORD>(name_bytes);
    std::copy(full_dst.begin(), full_dst.end(), rename_info->FileName);
    if (SetFileInformationByHandle(hsrc.h, FileRenameInfo, buf.data(),
                                   static_cast<DWORD>(buf.size())) == 0)
        return false;

    // Post-rename re-verification through the pinned handle (the R-SEC-008 "re-realpath after" —
    // the handle now reports the file's REAL post-rename location).
    const std::optional<FinalPath> landed = final_path_of(hsrc.h);
    if (!landed.has_value() || !inside_folded_base(landed->folded, base))
    {
        (void)dispose_on_close(hsrc.h); // never leave our bytes outside the jail
        return false;
    }
    return true;
#else
    // Pin + verify BOTH parents, then renameat relative to the two verified fds — POSIX rename(2)
    // semantics (atomic same-filesystem replace; a destination-leaf symlink is replaced, never
    // followed), immune to post-verification ancestor swaps.
    const std::string base = real_base().string();
    const fs::path src_parent = src.has_parent_path() ? src.parent_path() : base_;
    const fs::path dst_parent = dst.has_parent_path() ? dst.parent_path() : base_;
    UniqueFd src_fd = open_verified_dir(src_parent, base);
    UniqueFd dst_fd = open_verified_dir(dst_parent, base);
    if (!src_fd.valid() || !dst_fd.valid())
        return false;
    return ::renameat(src_fd.fd, src.filename().string().c_str(), dst_fd.fd,
                      dst.filename().string().c_str()) == 0;
#endif
}

bool NativeFileStore::remove(std::string_view path)
{
    const fs::path target = resolve(path);

#ifdef _WIN32
    const std::wstring base = real_base().wstring();

    // Open the LEAF object itself (a symlink/junction entry is opened as the link, never followed),
    // verify the opened handle resolves inside the jail, then delete-by-handle (disposition) — the
    // delete applies to the verified object, not to a re-traversed path string.
    UniqueHandle h(CreateFileW(target.c_str(), DELETE | FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!h.valid())
        return false; // missing (mirrors the old is_regular_file gate on absence)

    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileInformationByHandle(h.h, &info) == 0)
        return false;
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        return false; // file-only, matching MemoryFileStore (junction/dir-symlink leaves included)
    const std::optional<FinalPath> real = final_path_of(h.h);
    if (!real.has_value() || !inside_folded_base(real->folded, base))
        return false;
    return dispose_on_close(h.h); // unlinked when the handle closes
#else
    // Pin + verify the parent, then unlinkat relative to it. A symlink leaf removes the LINK (the
    // target is untouched — unlink never follows); a directory fails (file-only contract).
    const std::string base = real_base().string();
    const fs::path parent = target.has_parent_path() ? target.parent_path() : base_;
    UniqueFd parent_fd = open_verified_dir(parent, base);
    if (!parent_fd.valid())
        return false;
    return ::unlinkat(parent_fd.fd, target.filename().string().c_str(), 0) == 0;
#endif
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
