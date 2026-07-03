// Single-instance project lock implementation (see lock.h).

#include "context/editor/bridge/lock.h"

#include <map>
#include <mutex>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace context::editor::bridge
{

namespace
{
// ---- in-process guard, keyed by canonical path -------------------------------------------------
// Tracks how many shared / exclusive holders exist in THIS process per canonical project path so a
// second instantiation in the same process detects the conflict deterministically (independent of
// POSIX fcntl's same-process re-lock semantics). The OS lock still handles cross-process exclusion.
struct PathHolders
{
    int shared = 0;
    bool exclusive = false;
};

std::mutex& guard_mutex()
{
    static std::mutex m;
    return m;
}

std::map<std::string, PathHolders>& guard_table()
{
    static std::map<std::string, PathHolders> table;
    return table;
}

// Try to reserve an in-process slot. false => an incompatible holder already exists in this process.
bool guard_acquire(const std::string& key, LockMode mode)
{
    std::lock_guard<std::mutex> lk(guard_mutex());
    PathHolders& h = guard_table()[key];
    if (mode == LockMode::exclusive)
    {
        if (h.exclusive || h.shared > 0)
            return false;
        h.exclusive = true;
    }
    else
    {
        if (h.exclusive)
            return false;
        ++h.shared;
    }
    return true;
}

void guard_release(const std::string& key, LockMode mode)
{
    std::lock_guard<std::mutex> lk(guard_mutex());
    auto it = guard_table().find(key);
    if (it == guard_table().end())
        return;
    if (mode == LockMode::exclusive)
        it->second.exclusive = false;
    else if (it->second.shared > 0)
        --it->second.shared;
    if (!it->second.exclusive && it->second.shared == 0)
        guard_table().erase(it);
}

// Resolve to a stable identity key. weakly_canonical folds junction / subst / UNC / "." spellings so
// two spellings of the same directory produce the same key even before `.editor/` exists.
std::string canonical_key_for(const std::filesystem::path& project_root)
{
    std::error_code ec;
    std::filesystem::path canon = std::filesystem::weakly_canonical(project_root, ec);
    if (ec)
        canon = project_root;
    return canon.generic_string();
}
} // namespace

ProjectLock::ProjectLock(const std::filesystem::path& project_root)
    : canonical_key_(canonical_key_for(project_root))
{
    lock_path_ = std::filesystem::path(canonical_key_) / ".editor" / "lock";
#if defined(_WIN32)
    handle_ = INVALID_HANDLE_VALUE;
#endif
}

ProjectLock::~ProjectLock()
{
    release();
}

ProjectLock::ProjectLock(ProjectLock&& other) noexcept
    : lock_path_(std::move(other.lock_path_)), canonical_key_(std::move(other.canonical_key_)),
      held_(other.held_), mode_(other.mode_), error_(std::move(other.error_))
{
#if defined(_WIN32)
    handle_ = other.handle_;
    other.handle_ = INVALID_HANDLE_VALUE;
#else
    fd_ = other.fd_;
    other.fd_ = -1;
#endif
    other.held_ = false;
}

ProjectLock& ProjectLock::operator=(ProjectLock&& other) noexcept
{
    if (this != &other)
    {
        release();
        lock_path_ = std::move(other.lock_path_);
        canonical_key_ = std::move(other.canonical_key_);
        held_ = other.held_;
        mode_ = other.mode_;
        error_ = std::move(other.error_);
#if defined(_WIN32)
        handle_ = other.handle_;
        other.handle_ = INVALID_HANDLE_VALUE;
#else
        fd_ = other.fd_;
        other.fd_ = -1;
#endif
        other.held_ = false;
    }
    return *this;
}

LockOutcome ProjectLock::try_acquire(LockMode mode)
{
    if (held_)
        return LockOutcome::acquired;

    error_.clear();

    // Layer 1: the in-process guard. A same-process conflict is the attach signal without ever
    // touching the OS lock (so POSIX fcntl same-process re-lock never masks it).
    if (!guard_acquire(canonical_key_, mode))
        return LockOutcome::already_held;

    // Ensure <project>/.editor/ exists before opening the lock file.
    std::error_code ec;
    std::filesystem::create_directories(lock_path_.parent_path(), ec);
    if (ec)
    {
        error_ = "could not create .editor directory: " + ec.message();
        guard_release(canonical_key_, mode);
        return LockOutcome::error;
    }

#if defined(_WIN32)
    HANDLE h = ::CreateFileW(lock_path_.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        error_ = "CreateFileW failed on the lock file";
        guard_release(canonical_key_, mode);
        return LockOutcome::error;
    }
    // Byte-range lock on the first byte. LOCKFILE_FAIL_IMMEDIATELY => non-blocking try-lock; add
    // LOCKFILE_EXCLUSIVE_LOCK for exclusive, omit for a shared lock.
    OVERLAPPED ov{};
    DWORD flags = LOCKFILE_FAIL_IMMEDIATELY;
    if (mode == LockMode::exclusive)
        flags |= LOCKFILE_EXCLUSIVE_LOCK;
    if (!::LockFileEx(h, flags, 0, 1, 0, &ov))
    {
        ::CloseHandle(h);
        guard_release(canonical_key_, mode);
        return LockOutcome::already_held; // cross-process conflict — the attach signal
    }
    handle_ = h;
#else
    int fd = ::open(lock_path_.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd < 0)
    {
        error_ = "open failed on the lock file";
        guard_release(canonical_key_, mode);
        return LockOutcome::error;
    }
    // POSIX advisory byte-range lock on the first byte via fcntl. F_SETLK is non-blocking; a
    // conflicting lock returns EACCES/EAGAIN — the cross-process attach signal.
    struct flock fl{};
    fl.l_type = (mode == LockMode::exclusive) ? F_WRLCK : F_RDLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 1;
    if (::fcntl(fd, F_SETLK, &fl) < 0)
    {
        ::close(fd);
        guard_release(canonical_key_, mode);
        return LockOutcome::already_held; // cross-process conflict — the attach signal
    }
    fd_ = fd;
#endif

    held_ = true;
    mode_ = mode;
    return LockOutcome::acquired;
}

void ProjectLock::os_release()
{
#if defined(_WIN32)
    if (handle_ != INVALID_HANDLE_VALUE)
    {
        OVERLAPPED ov{};
        ::UnlockFileEx(handle_, 0, 1, 0, &ov);
        ::CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
#else
    if (fd_ >= 0)
    {
        struct flock fl{};
        fl.l_type = F_UNLCK;
        fl.l_whence = SEEK_SET;
        fl.l_start = 0;
        fl.l_len = 1;
        ::fcntl(fd_, F_SETLK, &fl);
        ::close(fd_);
        fd_ = -1;
    }
#endif
}

void ProjectLock::release()
{
    if (!held_)
    {
        os_release(); // close any dangling handle even if we never fully acquired
        return;
    }
    os_release();
    guard_release(canonical_key_, mode_);
    held_ = false;
}

} // namespace context::editor::bridge
