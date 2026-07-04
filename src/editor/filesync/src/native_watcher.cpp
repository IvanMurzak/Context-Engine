// Native OS watcher backends — ReadDirectoryChangesW / inotify / FSEvents behind the Watcher seam.
//
// Everything here produces HINTS only (R-FILE-002 / L-22): a delivered path is re-hashed by the
// reconciler and the content hash decides truth; a lost event is caught by the low-frequency full
// re-hash crawl. So every backend is written to prefer LOSING events over blocking, throwing, or
// crashing — loss is a latency cost surfaced via the latched degraded() flag, never a correctness
// bug. See native_watcher.h for the seam contract and the per-backend degraded signals.

#include "context/editor/filesync/native_watcher.h"

#include "context/editor/filesync/path_jail.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601 // Windows 7+ — CancelIoEx (guards against an older toolchain default)
#endif
#include <windows.h>

#include <cstring>
#include <thread>
#elif defined(__linux__)
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <cerrno>
#include <thread>
#include <unordered_map>
#elif defined(__APPLE__)
#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#endif

namespace fs = std::filesystem;

namespace context::editor::filesync
{

namespace
{
// Userspace pending-hint cap. Deduplication bounds the queue by DISTINCT paths, so this only trips
// on a genuinely enormous burst (e.g. a >64k-file storm between two polls); beyond it hints are
// dropped (the crawl converges them) and the loss is latched as degraded — never silent.
constexpr std::size_t kMaxPendingHints = 1u << 16;
} // namespace

// ---------------------------------------------------------------------------------------------
// Shared core: the mutex-guarded pending queue + logical path mapping (all backends feed this)
// ---------------------------------------------------------------------------------------------

struct NativeWatcher::Impl
{
    fs::path real_root; // canonicalized watch dir — the stable base for real -> relative mapping
    std::string prefix; // logical prefix prepended to every relative path ("" = none)

    std::mutex mutex;
    std::vector<WatchEvent> pending;
    std::unordered_set<std::string> pending_paths; // dedup: kind is advisory, one hint per path
    std::atomic<bool> degraded_flag{false};

    void mark_degraded() noexcept { degraded_flag.store(true, std::memory_order_relaxed); }

    [[nodiscard]] bool is_degraded() const noexcept
    {
        return degraded_flag.load(std::memory_order_relaxed);
    }

    std::vector<WatchEvent> drain()
    {
        std::vector<WatchEvent> out;
        const std::lock_guard<std::mutex> lock(mutex);
        out.swap(pending);
        pending_paths.clear();
        return out;
    }

    void enqueue_logical(std::string logical, ChangeKind kind)
    {
        if (logical.empty())
            return;
        const std::lock_guard<std::mutex> lock(mutex);
        if (pending.size() >= kMaxPendingHints)
        {
            // Storm overflow: drop the hint (the crawl converges it) and latch the loss visibly.
            mark_degraded();
            return;
        }
        if (!pending_paths.insert(logical).second)
            return; // already pending — a second hint for the same path adds nothing.
        pending.push_back(WatchEvent{std::move(logical), kind});
    }

    // `rel` is a path RELATIVE to the watch root, in whatever separator style the OS delivered
    // (normalize_path unifies '\' to '/'). Emits `<prefix>/<rel>` as the logical seam path.
    void enqueue_relative(std::string_view rel, ChangeKind kind)
    {
        std::string composed =
            prefix.empty() ? std::string(rel) : prefix + "/" + std::string(rel);
        std::string norm = normalize_path(composed);
        if (norm.empty() || norm == prefix)
            return; // an event on the watch root itself is not a file hint.
        enqueue_logical(std::move(norm), kind);
    }

    // Map a REAL filesystem path (built from / delivered under real_root) to its logical seam path.
    void enqueue_real(const fs::path& real, ChangeKind kind)
    {
        const std::string rel = real.lexically_relative(real_root).generic_string();
        if (rel.empty() || rel == "." || rel.rfind("..", 0) == 0)
            return; // outside (or exactly) the watch root — never surface an escaping path.
        enqueue_relative(rel, kind);
    }

    // Hint every regular file under a directory that just APPEARED (created or moved in): its
    // contents raced the watch registration (inotify) or produced no per-file events at all (a
    // moved-in tree under RDCW), so scan once and hint what is already there. Best-effort — any
    // iteration error just ends the scan (the crawl is the safety net).
    void enqueue_tree(const fs::path& real_dir)
    {
        std::error_code ec;
        for (fs::recursive_directory_iterator
                 it(real_dir, fs::directory_options::skip_permission_denied, ec),
             end;
             it != end; it.increment(ec))
        {
            if (ec)
                break;
            std::error_code fec;
            if (it->is_regular_file(fec) && !fec)
                enqueue_real(it->path(), ChangeKind::created);
        }
    }

    // --- platform backend (exactly one of the sections below defines these) --------------------
    bool start();
    void stop();

#ifdef _WIN32
    HANDLE dir_handle = INVALID_HANDLE_VALUE;
    HANDLE stop_event = nullptr;
    HANDLE io_event = nullptr;
    OVERLAPPED overlapped{};
    // 64 KiB is the documented ReadDirectoryChangesW ceiling for network directories; DWORD-aligned
    // as FILE_NOTIFY_INFORMATION parsing requires.
    alignas(DWORD) unsigned char buffer[64 * 1024] = {};
    std::thread thread;

    bool issue_read();
    void run();
    void parse_and_enqueue(DWORD bytes);
#elif defined(__linux__)
    int inotify_fd = -1;
    int wake_fd = -1;
    std::unordered_map<int, fs::path> wd_to_dir; // ctor-populated; thread-only after start()
    std::thread thread;

    void add_watch(const fs::path& dir);
    void watch_new_tree(const fs::path& dir);
    void run();
    void handle_event(const struct inotify_event* event);
#elif defined(__APPLE__)
    FSEventStreamRef stream = nullptr;
    dispatch_queue_t queue = nullptr;

    // Static member (not a free function) so it can name the private Impl through `info`.
    static void fsevents_trampoline(ConstFSEventStreamRef stream_ref, void* info,
                                    size_t num_events, void* event_paths,
                                    const FSEventStreamEventFlags event_flags[],
                                    const FSEventStreamEventId event_ids[]);
    void on_fsevents(size_t num_events, void* event_paths,
                     const FSEventStreamEventFlags* event_flags);
#endif
};

// ---------------------------------------------------------------------------------------------
// Windows — ReadDirectoryChangesW (native recursive subtree watch, overlapped IO)
// ---------------------------------------------------------------------------------------------

#ifdef _WIN32

namespace
{
std::string wide_to_utf8(const wchar_t* data, int wide_chars)
{
    if (wide_chars <= 0)
        return {};
    const int needed =
        WideCharToMultiByte(CP_UTF8, 0, data, wide_chars, nullptr, 0, nullptr, nullptr);
    if (needed <= 0)
        return {};
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, data, wide_chars, out.data(), needed, nullptr, nullptr);
    return out;
}
} // namespace

bool NativeWatcher::Impl::start()
{
    // FILE_FLAG_BACKUP_SEMANTICS is required to open a directory handle; OVERLAPPED for the async
    // read loop. Generous sharing so the watch never blocks other processes touching the tree.
    dir_handle = CreateFileW(real_root.c_str(), FILE_LIST_DIRECTORY,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                             nullptr);
    if (dir_handle == INVALID_HANDLE_VALUE)
    {
        mark_degraded(); // registration failure (missing dir, network FS refusing the watch, ...)
        return false;
    }
    stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    io_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stop_event == nullptr || io_event == nullptr || !issue_read())
    {
        // issue_read() failing here means the OS refused the very first watch — registration failed.
        mark_degraded();
        if (stop_event != nullptr)
            CloseHandle(stop_event);
        if (io_event != nullptr)
            CloseHandle(io_event);
        stop_event = nullptr;
        io_event = nullptr;
        CloseHandle(dir_handle);
        dir_handle = INVALID_HANDLE_VALUE;
        return false;
    }
    // The first ReadDirectoryChangesW is issued BEFORE the thread spawns, so changes made
    // immediately after construction are already being captured (no startup race).
    thread = std::thread([this] { run(); });
    return true;
}

bool NativeWatcher::Impl::issue_read()
{
    constexpr DWORD kFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                              FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE |
                              FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION;
    ResetEvent(io_event);
    std::memset(&overlapped, 0, sizeof(overlapped));
    overlapped.hEvent = io_event;
    return ReadDirectoryChangesW(dir_handle, buffer, static_cast<DWORD>(sizeof(buffer)),
                                 /*bWatchSubtree=*/TRUE, kFilter, nullptr, &overlapped,
                                 nullptr) != 0;
}

void NativeWatcher::Impl::parse_and_enqueue(DWORD bytes)
{
    std::size_t offset = 0;
    for (;;)
    {
        if (offset + sizeof(FILE_NOTIFY_INFORMATION) > bytes)
            break; // defensive: never read past what the kernel filled.
        const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer + offset);
        const std::string rel = wide_to_utf8(
            info->FileName, static_cast<int>(info->FileNameLength / sizeof(WCHAR)));

        ChangeKind kind = ChangeKind::modified;
        if (info->Action == FILE_ACTION_ADDED || info->Action == FILE_ACTION_RENAMED_NEW_NAME)
            kind = ChangeKind::created;
        else if (info->Action == FILE_ACTION_REMOVED || info->Action == FILE_ACTION_RENAMED_OLD_NAME)
            kind = ChangeKind::removed;

        if (!rel.empty())
        {
            if (kind == ChangeKind::created)
            {
                // A directory that appeared (created or moved in): its files produce no per-file
                // events of their own — hint them by scanning once. RDCW names are relative.
                const fs::path real = real_root / fs::path(rel);
                std::error_code ec;
                if (fs::is_directory(real, ec) && !ec)
                    enqueue_tree(real);
                else
                    enqueue_relative(rel, kind);
            }
            else
            {
                enqueue_relative(rel, kind);
            }
        }

        if (info->NextEntryOffset == 0)
            break;
        offset += info->NextEntryOffset;
    }
}

void NativeWatcher::Impl::run()
{
    for (;;)
    {
        HANDLE handles[2] = {stop_event, io_event};
        const DWORD waited = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (waited == WAIT_OBJECT_0)
        {
            // stop(): cancel the in-flight read and reap it so the OVERLAPPED is quiesced.
            CancelIoEx(dir_handle, &overlapped);
            DWORD bytes = 0;
            GetOverlappedResult(dir_handle, &overlapped, &bytes, TRUE);
            return;
        }
        if (waited != WAIT_OBJECT_0 + 1)
        {
            mark_degraded(); // wait failure — the watch thread cannot continue.
            return;
        }

        DWORD bytes = 0;
        if (!GetOverlappedResult(dir_handle, &overlapped, &bytes, FALSE))
        {
            const DWORD err = GetLastError();
            if (err == ERROR_OPERATION_ABORTED)
                return; // cancelled by stop()
            mark_degraded();
            if (err != ERROR_NOTIFY_ENUM_DIR)
                return; // watch is dead
            // ERROR_NOTIFY_ENUM_DIR: the kernel buffer overflowed — events were LOST but the
            // watch survives. Degraded is latched above; keep delivering what we still can.
        }
        else if (bytes == 0)
        {
            mark_degraded(); // too many changes for the buffer — events lost, watch alive.
        }
        else
        {
            parse_and_enqueue(bytes);
        }

        if (!issue_read())
        {
            mark_degraded(); // could not re-arm the watch — no further hints will arrive.
            return;
        }
    }
}

void NativeWatcher::Impl::stop()
{
    if (thread.joinable())
    {
        SetEvent(stop_event);
        thread.join();
    }
    if (io_event != nullptr)
        CloseHandle(io_event);
    if (stop_event != nullptr)
        CloseHandle(stop_event);
    if (dir_handle != INVALID_HANDLE_VALUE)
        CloseHandle(dir_handle);
    io_event = nullptr;
    stop_event = nullptr;
    dir_handle = INVALID_HANDLE_VALUE;
}

// ---------------------------------------------------------------------------------------------
// Linux — inotify (per-directory watches, recursively maintained)
// ---------------------------------------------------------------------------------------------

#elif defined(__linux__)

namespace
{
constexpr std::uint32_t kInotifyMask = IN_CREATE | IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_FROM |
                                       IN_MOVED_TO | IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF;
} // namespace

void NativeWatcher::Impl::add_watch(const fs::path& dir)
{
    const int wd = inotify_add_watch(inotify_fd, dir.c_str(), kInotifyMask);
    if (wd < 0)
    {
        // ENOSPC = per-user watch limit exhausted; EMFILE/ENOMEM similar resource exhaustion. The
        // subtree under `dir` is now blind — exactly the truncation R-FILE-002 demands we surface.
        mark_degraded();
        return;
    }
    wd_to_dir[wd] = dir;
}

// Watch a directory that just appeared, then hint the files already inside it: they may have been
// created before our watch landed (the inotify add race) or moved in with the directory.
void NativeWatcher::Impl::watch_new_tree(const fs::path& dir)
{
    add_watch(dir);
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec),
         end;
         it != end; it.increment(ec))
    {
        if (ec)
            break;
        std::error_code fec;
        if (it->is_directory(fec) && !fec)
            add_watch(it->path());
    }
    enqueue_tree(dir);
}

bool NativeWatcher::Impl::start()
{
    inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_fd < 0)
    {
        mark_degraded(); // inotify instance limit exhausted (or unsupported) — registration failed.
        return false;
    }
    wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wake_fd < 0)
    {
        mark_degraded();
        ::close(inotify_fd);
        inotify_fd = -1;
        return false;
    }

    // Register the root and every existing subdirectory BEFORE the thread spawns, so changes made
    // immediately after construction are already being captured.
    add_watch(real_root);
    std::error_code ec;
    for (fs::recursive_directory_iterator
             it(real_root, fs::directory_options::skip_permission_denied, ec),
         end;
         it != end; it.increment(ec))
    {
        if (ec)
            break;
        std::error_code fec;
        if (it->is_directory(fec) && !fec)
            add_watch(it->path());
    }

    thread = std::thread([this] { run(); });
    return true;
}

void NativeWatcher::Impl::handle_event(const struct inotify_event* event)
{
    if ((event->mask & IN_Q_OVERFLOW) != 0)
    {
        mark_degraded(); // the kernel queue overflowed — events were lost.
        return;
    }
    if ((event->mask & IN_IGNORED) != 0)
    {
        wd_to_dir.erase(event->wd); // watch removed (dir deleted/moved) — drop the mapping.
        return;
    }
    const auto it = wd_to_dir.find(event->wd);
    if (it == wd_to_dir.end())
        return;
    if ((event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) != 0)
        return; // the directory itself went away; IN_IGNORED follows and drops the mapping.
    if (event->len == 0)
        return;

    const fs::path full = it->second / event->name;
    if ((event->mask & IN_ISDIR) != 0)
    {
        // New directories must be watched as they appear (inotify is not recursive); their
        // pre-existing contents are hinted by the scan. Directory events are not file hints.
        if ((event->mask & (IN_CREATE | IN_MOVED_TO)) != 0)
            watch_new_tree(full);
        return;
    }

    ChangeKind kind = ChangeKind::modified;
    if ((event->mask & (IN_CREATE | IN_MOVED_TO)) != 0)
        kind = ChangeKind::created;
    else if ((event->mask & (IN_DELETE | IN_MOVED_FROM)) != 0)
        kind = ChangeKind::removed;
    enqueue_real(full, kind);
}

void NativeWatcher::Impl::run()
{
    // Aligned per the inotify(7) sample; large enough for a burst of long names per read.
    alignas(struct inotify_event) unsigned char buf[64 * 1024];
    for (;;)
    {
        struct pollfd fds[2] = {{inotify_fd, POLLIN, 0}, {wake_fd, POLLIN, 0}};
        const int ready = ::poll(fds, 2, -1);
        if (ready < 0)
        {
            if (errno == EINTR)
                continue;
            mark_degraded(); // the wait loop itself failed — no further hints will arrive.
            return;
        }
        if ((fds[1].revents & POLLIN) != 0)
            return; // stop() woke us.
        if ((fds[0].revents & POLLIN) == 0)
            continue;

        for (;;)
        {
            const ssize_t n = ::read(inotify_fd, buf, sizeof(buf));
            if (n <= 0)
                break; // EAGAIN — drained.
            ssize_t offset = 0;
            while (offset < n)
            {
                const auto* event = reinterpret_cast<const struct inotify_event*>(buf + offset);
                handle_event(event);
                offset += static_cast<ssize_t>(sizeof(struct inotify_event)) +
                          static_cast<ssize_t>(event->len);
            }
        }
    }
}

void NativeWatcher::Impl::stop()
{
    if (thread.joinable())
    {
        // The only failure an eventfd write of 1 can see here is EINTR (the counter is empty and
        // ours); loop on it so the thread is guaranteed to wake before the join below. The result
        // is bound so glibc's warn_unused_result on write() is satisfied under -Werror.
        const std::uint64_t one = 1;
        ssize_t wrote = 0;
        do
        {
            wrote = ::write(wake_fd, &one, sizeof(one));
        } while (wrote < 0 && errno == EINTR);
        thread.join();
    }
    if (wake_fd >= 0)
        ::close(wake_fd);
    if (inotify_fd >= 0)
        ::close(inotify_fd);
    wake_fd = -1;
    inotify_fd = -1;
}

// ---------------------------------------------------------------------------------------------
// macOS — FSEvents (file-level events on a private serial dispatch queue)
// ---------------------------------------------------------------------------------------------

#elif defined(__APPLE__)

void NativeWatcher::Impl::fsevents_trampoline(ConstFSEventStreamRef /*stream_ref*/, void* info,
                                              size_t num_events, void* event_paths,
                                              const FSEventStreamEventFlags event_flags[],
                                              const FSEventStreamEventId /*event_ids*/[])
{
    static_cast<Impl*>(info)->on_fsevents(num_events, event_paths, event_flags);
}

void NativeWatcher::Impl::on_fsevents(size_t num_events, void* event_paths,
                                      const FSEventStreamEventFlags* event_flags)
{
    // Without kFSEventStreamCreateFlagUseCFTypes, eventPaths is a C array of UTF-8 C strings.
    const char** paths = static_cast<const char**>(event_paths);
    for (size_t i = 0; i < num_events; ++i)
    {
        const FSEventStreamEventFlags flags = event_flags[i];
        if ((flags & (kFSEventStreamEventFlagMustScanSubDirs | kFSEventStreamEventFlagKernelDropped |
                      kFSEventStreamEventFlagUserDropped | kFSEventStreamEventFlagEventIdsWrapped)) !=
            0)
        {
            mark_degraded(); // coalesced/dropped delivery — hint coverage was lost.
        }

        const fs::path real(paths[i]);
        if ((flags & kFSEventStreamEventFlagItemIsDir) != 0)
        {
            // A directory that appeared (created or renamed in): hint its files, which may not get
            // per-file events of their own. A renamed-AWAY directory no longer exists on disk, so
            // the scan no-ops. Directory events themselves are not file hints.
            if ((flags & (kFSEventStreamEventFlagItemCreated | kFSEventStreamEventFlagItemRenamed)) !=
                0)
                enqueue_tree(real);
            continue;
        }

        ChangeKind kind = ChangeKind::modified;
        if ((flags & kFSEventStreamEventFlagItemRemoved) != 0)
            kind = ChangeKind::removed;
        else if ((flags & (kFSEventStreamEventFlagItemCreated | kFSEventStreamEventFlagItemRenamed)) !=
                 0)
            kind = ChangeKind::created; // advisory — a renamed-away path re-hashes to a removal.
        enqueue_real(real, kind);
    }
}

bool NativeWatcher::Impl::start()
{
    CFStringRef cf_path = CFStringCreateWithCString(kCFAllocatorDefault, real_root.c_str(),
                                                    kCFStringEncodingUTF8);
    if (cf_path == nullptr)
    {
        mark_degraded();
        return false;
    }
    const void* path_values[1] = {cf_path};
    CFArrayRef cf_paths =
        CFArrayCreate(kCFAllocatorDefault, path_values, 1, &kCFTypeArrayCallBacks);
    FSEventStreamContext context{};
    context.info = this;
    // 50 ms coalescing latency: the reconciler's own poll cadence is the real debounce; NoDefer
    // makes the first event in a burst prompt. FileEvents = per-file paths (macOS 10.7+).
    stream = FSEventStreamCreate(kCFAllocatorDefault, &Impl::fsevents_trampoline, &context,
                                 cf_paths, kFSEventStreamEventIdSinceNow, 0.05,
                                 kFSEventStreamCreateFlagFileEvents |
                                     kFSEventStreamCreateFlagNoDefer);
    if (cf_paths != nullptr)
        CFRelease(cf_paths);
    CFRelease(cf_path);
    if (stream == nullptr)
    {
        mark_degraded(); // stream creation refused — registration failed.
        return false;
    }

    queue = dispatch_queue_create("context.filesync.native-watcher", DISPATCH_QUEUE_SERIAL);
    FSEventStreamSetDispatchQueue(stream, queue);
    if (!FSEventStreamStart(stream))
    {
        mark_degraded(); // could not start delivery — registration failed.
        FSEventStreamInvalidate(stream);
        FSEventStreamRelease(stream);
        stream = nullptr;
        dispatch_release(queue);
        queue = nullptr;
        return false;
    }
    return true;
}

void NativeWatcher::Impl::stop()
{
    if (stream != nullptr)
    {
        FSEventStreamStop(stream);
        FSEventStreamInvalidate(stream); // no callbacks after this returns.
        FSEventStreamRelease(stream);
        stream = nullptr;
    }
    if (queue != nullptr)
    {
        dispatch_release(queue);
        queue = nullptr;
    }
}

// ---------------------------------------------------------------------------------------------
// Any other platform — permanently degraded (NullWatcher semantics), so callers need no #ifdef
// ---------------------------------------------------------------------------------------------

#else

bool NativeWatcher::Impl::start()
{
    mark_degraded();
    return false;
}

void NativeWatcher::Impl::stop() {}

#endif

// ---------------------------------------------------------------------------------------------
// Public surface
// ---------------------------------------------------------------------------------------------

NativeWatcher::NativeWatcher(fs::path watch_dir, std::string logical_prefix)
    : impl_(std::make_unique<Impl>())
{
    impl_->prefix = normalize_path(logical_prefix);

    // Canonicalize so real -> relative mapping is stable even when the OS reports resolved paths
    // (FSEvents resolves /var -> /private/var; junctions/symlinks similar elsewhere).
    std::error_code ec;
    fs::path real = fs::weakly_canonical(watch_dir, ec);
    if (ec)
        real = std::move(watch_dir);
    impl_->real_root = std::move(real);

    std::error_code dir_ec;
    if (!fs::is_directory(impl_->real_root, dir_ec) || dir_ec)
    {
        impl_->mark_degraded(); // registration failure: missing or non-directory watch root.
        return;
    }
    (void)impl_->start(); // marks degraded itself on any registration failure.
}

NativeWatcher::~NativeWatcher()
{
    impl_->stop();
}

std::vector<WatchEvent> NativeWatcher::poll()
{
    return impl_->drain();
}

bool NativeWatcher::degraded() const
{
    return impl_->is_degraded();
}

bool NativeWatcher::backend_available() noexcept
{
#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
    return true;
#else
    return false;
#endif
}

} // namespace context::editor::filesync
