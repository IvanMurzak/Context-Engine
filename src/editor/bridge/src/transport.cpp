// Real IPC transport implementation (see transport.h). POSIX = Unix domain stream socket; Windows =
// named pipe (byte mode). The #ifdef split mirrors lock.cpp: one small, portable surface over two OS
// primitives, warnings-clean under GCC/libstdc++, MSVC, and Apple Clang/libc++.

#include "context/editor/bridge/transport.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// <aclapi.h> (pulls <accctrl.h>): the owner-SID DACL primitives for the D20 named-pipe hardening
// (SetEntriesInAclW / EXPLICIT_ACCESS_W / TRUSTEE_W) plus the ACE-walk used to assert owner-only.
#include <aclapi.h>
#else
#include <cerrno>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace context::editor::bridge
{

namespace
{
// FNV-1a 64-bit: a tiny, stdlib-only, stable string hash so the endpoint name is a short fixed-width
// token derived from the (arbitrarily long) canonical project path. Not security-sensitive — just a
// collision-resistant-enough per-project discriminator for the socket/pipe name.
std::uint64_t fnv1a(const std::string& s)
{
    std::uint64_t h = 1469598103934665603ULL;
    for (const char c : s)
    {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        h *= 1099511628211ULL;
    }
    return h;
}

std::string hex16(std::uint64_t v)
{
    static const char* kHex = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i)
    {
        out[static_cast<std::size_t>(i)] = kHex[v & 0xFULL];
        v >>= 4;
    }
    return out;
}

void sleep_ms(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// Pack a 4-byte big-endian length prefix.
void pack_u32(std::uint32_t v, std::array<unsigned char, 4>& out)
{
    out[0] = static_cast<unsigned char>((v >> 24) & 0xFFu);
    out[1] = static_cast<unsigned char>((v >> 16) & 0xFFu);
    out[2] = static_cast<unsigned char>((v >> 8) & 0xFFu);
    out[3] = static_cast<unsigned char>(v & 0xFFu);
}

std::uint32_t unpack_u32(const std::array<unsigned char, 4>& in)
{
    return (static_cast<std::uint32_t>(in[0]) << 24) | (static_cast<std::uint32_t>(in[1]) << 16) |
           (static_cast<std::uint32_t>(in[2]) << 8) | static_cast<std::uint32_t>(in[3]);
}

#if defined(_WIN32)
constexpr bool kIsWindows = true;

std::wstring widen_ascii(const std::string& s)
{
    std::wstring out;
    out.reserve(s.size());
    for (const char c : s)
        out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    return out;
}

bool handle_valid(void* h) noexcept
{
    return h != nullptr && h != INVALID_HANDLE_VALUE;
}

// --- D20 owner-SID DACL for the named pipe (closing the documented gap; POSIX uses 0600) ----------
// Copy the current process user's SID into `out`. false on any token / SID failure.
bool current_user_sid(std::vector<unsigned char>& out)
{
    HANDLE token = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE)
        return false;
    DWORD needed = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &needed); // sizing call
    if (needed == 0)
    {
        ::CloseHandle(token);
        return false;
    }
    std::vector<unsigned char> buf(needed);
    const BOOL got = ::GetTokenInformation(token, TokenUser, buf.data(), needed, &needed);
    ::CloseHandle(token);
    if (got == FALSE)
        return false;
    const auto* tu = reinterpret_cast<const TOKEN_USER*>(buf.data());
    if (tu->User.Sid == nullptr || ::IsValidSid(tu->User.Sid) == FALSE)
        return false;
    const DWORD len = ::GetLengthSid(tu->User.Sid);
    out.resize(len);
    return ::CopySid(len, out.data(), tu->User.Sid) != FALSE;
}

// An owner-only SECURITY_ATTRIBUTES: a DACL whose SOLE ACE grants FILE_ALL_ACCESS to the current
// process user's SID. No other ACE, so every other principal is implicitly denied (the M1 loopback
// ambient guard, tightened from the previous default-DACL nullptr). Built once per server and applied
// to EVERY pipe instance (listen()'s first + each accept()-minted successor).
struct PipeSecurity
{
    SECURITY_ATTRIBUTES sa{};
    SECURITY_DESCRIPTOR sd{};
    PACL acl = nullptr;             // SetEntriesInAclW allocation (LocalFree)
    std::vector<unsigned char> sid; // an owned copy of the token-user SID bytes

    PipeSecurity() = default;
    PipeSecurity(const PipeSecurity&) = delete;
    PipeSecurity& operator=(const PipeSecurity&) = delete;
    ~PipeSecurity()
    {
        if (acl != nullptr)
            ::LocalFree(acl);
    }
};

// Build the owner-only security holder. nullptr on any failure — the caller then falls back to the
// default DACL (never LESS secure than today's nullptr SA, just not the tightened owner-only ACL).
std::unique_ptr<PipeSecurity> make_owner_only_security()
{
    auto holder = std::make_unique<PipeSecurity>();
    if (!current_user_sid(holder->sid))
        return nullptr;

    EXPLICIT_ACCESS_W ea{};
    ea.grfAccessPermissions = FILE_ALL_ACCESS;
    ea.grfAccessMode = SET_ACCESS;
    ea.grfInheritance = NO_INHERITANCE;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_USER;
    ea.Trustee.ptstrName = reinterpret_cast<LPWSTR>(holder->sid.data());

    if (::SetEntriesInAclW(1, &ea, nullptr, &holder->acl) != ERROR_SUCCESS)
        return nullptr;
    if (::InitializeSecurityDescriptor(&holder->sd, SECURITY_DESCRIPTOR_REVISION) == FALSE)
        return nullptr;
    // Explicit, present, non-defaulted DACL: an owner-only ACL (NOT a NULL DACL, which would grant
    // everyone). This is exactly what closes the documented named-pipe gap.
    if (::SetSecurityDescriptorDacl(&holder->sd, TRUE, holder->acl, FALSE) == FALSE)
        return nullptr;

    holder->sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    holder->sa.lpSecurityDescriptor = &holder->sd;
    holder->sa.bInheritHandle = FALSE;
    return holder;
}

// True iff every ACE of `acl` is an ACCESS_ALLOWED ACE for `owner_sid`, and at least one such ACE
// exists — i.e. an owner-only DACL (no world/authenticated-users grant, no deny games).
bool acl_is_owner_only(PACL acl, PSID owner_sid)
{
    if (acl == nullptr)
        return false; // a NULL DACL grants everyone — not owner-only
    bool any_allow = false;
    for (WORD i = 0; i < acl->AceCount; ++i)
    {
        void* ace = nullptr;
        if (::GetAce(acl, i, &ace) == FALSE)
            return false;
        const auto* header = static_cast<const ACE_HEADER*>(ace);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE)
            return false; // a deny/audit/other ACE is not the clean owner-only shape we build
        auto* allow = static_cast<ACCESS_ALLOWED_ACE*>(ace);
        if (::EqualSid(reinterpret_cast<PSID>(&allow->SidStart), owner_sid) == FALSE)
            return false; // an ACE for some OTHER principal — world-reachable
        any_allow = true;
    }
    return any_allow;
}
#else
constexpr bool kIsWindows = false;

// send() flag that suppresses SIGPIPE on a broken connection where the platform supports it (Linux);
// on platforms without it we fall back to SO_NOSIGPIPE at socket-creation time (Apple).
#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

void set_nosigpipe(int fd)
{
#if defined(SO_NOSIGPIPE)
    int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#else
    (void)fd;
#endif
}
#endif
} // namespace

std::string endpoint_for(const std::string& canonical_project_key)
{
    const std::string tag = hex16(fnv1a(canonical_project_key));
    if (kIsWindows)
        return "\\\\.\\pipe\\context-" + tag;
    return "/tmp/context-" + tag + ".sock";
}

// ------------------------------------------------------------------------------------------------
// TransportConnection
// ------------------------------------------------------------------------------------------------

TransportConnection::~TransportConnection()
{
    close();
}

TransportConnection::TransportConnection(TransportConnection&& other) noexcept
{
    error_ = std::move(other.error_);
#if defined(_WIN32)
    handle_ = other.handle_;
    other.handle_ = nullptr;
#else
    fd_ = other.fd_;
    other.fd_ = -1;
#endif
}

TransportConnection& TransportConnection::operator=(TransportConnection&& other) noexcept
{
    if (this != &other)
    {
        close();
        error_ = std::move(other.error_);
#if defined(_WIN32)
        handle_ = other.handle_;
        other.handle_ = nullptr;
#else
        fd_ = other.fd_;
        other.fd_ = -1;
#endif
    }
    return *this;
}

bool TransportConnection::valid() const noexcept
{
#if defined(_WIN32)
    return handle_valid(handle_);
#else
    return fd_ >= 0;
#endif
}

void TransportConnection::close() noexcept
{
#if defined(_WIN32)
    if (handle_valid(handle_))
    {
        ::FlushFileBuffers(handle_);
        ::CloseHandle(handle_);
    }
    handle_ = nullptr;
#else
    if (fd_ >= 0)
        ::close(fd_);
    fd_ = -1;
#endif
}

bool TransportConnection::read_exact(void* buf, std::size_t n)
{
    auto* p = static_cast<unsigned char*>(buf);
    std::size_t got = 0;
    while (got < n)
    {
#if defined(_WIN32)
        DWORD read_now = 0;
        const DWORD want = static_cast<DWORD>(n - got);
        if (::ReadFile(handle_, p + got, want, &read_now, nullptr) == FALSE)
        {
            const DWORD e = ::GetLastError();
            if (e != ERROR_BROKEN_PIPE && e != ERROR_PIPE_NOT_CONNECTED)
                error_ = "ReadFile failed (" + std::to_string(e) + ")";
            return false; // broken pipe == clean EOF (empty error_)
        }
        if (read_now == 0)
            return false; // EOF
        got += static_cast<std::size_t>(read_now);
#else
        const ssize_t r = ::recv(fd_, p + got, n - got, 0);
        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            error_ = std::string("recv failed: ") + std::strerror(errno);
            return false;
        }
        if (r == 0)
            return false; // EOF
        got += static_cast<std::size_t>(r);
#endif
    }
    return true;
}

bool TransportConnection::write_all(const void* buf, std::size_t n)
{
    const auto* p = static_cast<const unsigned char*>(buf);
    std::size_t sent = 0;
    while (sent < n)
    {
#if defined(_WIN32)
        DWORD wrote_now = 0;
        const DWORD want = static_cast<DWORD>(n - sent);
        if (::WriteFile(handle_, p + sent, want, &wrote_now, nullptr) == FALSE)
        {
            error_ = "WriteFile failed (" + std::to_string(::GetLastError()) + ")";
            return false;
        }
        sent += static_cast<std::size_t>(wrote_now);
#else
        const ssize_t w = ::send(fd_, p + sent, n - sent, kSendFlags);
        if (w < 0)
        {
            if (errno == EINTR)
                continue;
            error_ = std::string("send failed: ") + std::strerror(errno);
            return false;
        }
        sent += static_cast<std::size_t>(w);
#endif
    }
    return true;
}

std::optional<std::string> TransportConnection::read_frame()
{
    error_.clear();
    if (!valid())
    {
        error_ = "connection is not open";
        return std::nullopt;
    }
    std::array<unsigned char, 4> header{};
    if (!read_exact(header.data(), header.size()))
        return std::nullopt; // EOF or error (error_ set on the latter)

    const std::uint32_t len = unpack_u32(header);
    if (len > kMaxFrameBytes)
    {
        error_ = "frame length " + std::to_string(len) + " exceeds the maximum";
        return std::nullopt;
    }
    std::string payload;
    payload.resize(len);
    if (len > 0 && !read_exact(payload.data(), payload.size()))
    {
        if (error_.empty())
            error_ = "peer disconnected mid-frame";
        return std::nullopt;
    }
    return payload;
}

bool TransportConnection::write_frame(std::string_view payload)
{
    error_.clear();
    if (!valid())
    {
        error_ = "connection is not open";
        return false;
    }
    if (payload.size() > kMaxFrameBytes)
    {
        error_ = "payload exceeds the maximum frame size";
        return false;
    }
    std::array<unsigned char, 4> header{};
    pack_u32(static_cast<std::uint32_t>(payload.size()), header);
    if (!write_all(header.data(), header.size()))
        return false;
    if (!payload.empty() && !write_all(payload.data(), payload.size()))
        return false;
    return true;
}

std::optional<std::string> TransportConnection::read_frame_timed(int timeout_ms, bool& timed_out)
{
    timed_out = false;
    error_.clear();
    if (!valid())
    {
        error_ = "connection is not open";
        return std::nullopt;
    }
#if defined(_WIN32)
    // No cross-frame wait primitive for a synchronous byte-mode pipe: poll for available bytes, then
    // read the whole frame (read_frame blocks briefly for the rest of an in-flight frame). A short
    // sleep bounds the idle poll — the perf-critical bench runs on POSIX, not here.
    DWORD avail = 0;
    if (::PeekNamedPipe(handle_, nullptr, 0, nullptr, &avail, nullptr) == FALSE)
    {
        const DWORD e = ::GetLastError();
        if (e != ERROR_BROKEN_PIPE && e != ERROR_PIPE_NOT_CONNECTED)
        {
            error_ = "PeekNamedPipe failed (" + std::to_string(e) + ")";
            return std::nullopt; // a real error (timed_out stays false)
        }
        // The peer closed — but bytes it wrote BEFORE closing may still be buffered on our side, and
        // PeekNamedPipe reports the broken pipe rather than those bytes. Dropping them here would
        // discard an already-delivered frame: exactly what a request/response client sees as "the
        // daemon never answered" when the daemon replies and immediately exits (`shutdown`). ReadFile
        // still drains the buffer first and only then fails, so let read_frame() decide — it returns
        // the frame if one is pending and reports EOF only when there is genuinely nothing left.
        // POSIX needs no equivalent: poll() reports POLLIN for buffered data after a peer close.
        return read_frame();
    }
    if (avail == 0)
    {
        if (timeout_ms > 0)
            ::Sleep(static_cast<DWORD>(timeout_ms));
        timed_out = true;
        return std::nullopt;
    }
    return read_frame(); // data is available -> read the framed message
#else
    struct pollfd pfd;
    pfd.fd = fd_;
    pfd.events = POLLIN;
    pfd.revents = 0;
    const int pr = ::poll(&pfd, 1, timeout_ms);
    if (pr == 0)
    {
        timed_out = true; // the wait elapsed with no data
        return std::nullopt;
    }
    if (pr < 0)
    {
        if (errno == EINTR)
        {
            timed_out = true; // treat an interrupted wait as a timeout — the caller re-polls
            return std::nullopt;
        }
        error_ = std::string("poll failed: ") + std::strerror(errno);
        return std::nullopt;
    }
    if (pfd.revents & (POLLIN | POLLHUP | POLLERR))
        return read_frame(); // readable (or EOF/error, which read_frame reports as nullopt)
    timed_out = true;
    return std::nullopt;
#endif
}

void TransportConnection::unblock() noexcept
{
#if defined(_WIN32)
    if (handle_valid(handle_))
    {
        ::CancelIoEx(handle_, nullptr);  // cancel any pending read (overlapped path)
        ::DisconnectNamedPipe(handle_);  // force the server end closed -> a blocked ReadFile returns
    }
#else
    if (fd_ >= 0)
        ::shutdown(fd_, SHUT_RDWR); // half-close both directions -> a blocked recv() returns EOF
#endif
}

// ------------------------------------------------------------------------------------------------
// TransportServer
// ------------------------------------------------------------------------------------------------

TransportServer::TransportServer(std::string endpoint) : endpoint_(std::move(endpoint)) {}

TransportServer::~TransportServer()
{
    stop();
}

#if defined(_WIN32)

bool TransportServer::listen()
{
    if (listening_)
        return true;
    // D20: build the owner-only DACL once and apply it to every pipe instance. A build failure yields
    // a nullptr SA (the default-DACL behavior of before) — degraded, never bricked.
    if (security_ == nullptr)
        security_ = make_owner_only_security().release();
    LPSECURITY_ATTRIBUTES psa =
        (security_ != nullptr) ? &reinterpret_cast<PipeSecurity*>(security_)->sa : nullptr;
    const std::wstring name = widen_ascii(endpoint_);
    // Create the first pipe instance; subsequent instances are created per-accept so a fresh client
    // always finds a listening instance (PIPE_UNLIMITED_INSTANCES).
    HANDLE h = ::CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX,
                                  PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                  PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, psa);
    if (h == INVALID_HANDLE_VALUE)
    {
        error_ = "CreateNamedPipeW failed (" + std::to_string(::GetLastError()) + ")";
        return false;
    }
    pending_ = h;
    listening_ = true;
    return true;
}

std::optional<TransportConnection> TransportServer::accept()
{
    if (stopped_ || !listening_ || !handle_valid(pending_))
        return std::nullopt;

    HANDLE current = pending_;
    const BOOL connected = ::ConnectNamedPipe(current, nullptr);
    if (connected == FALSE && ::GetLastError() != ERROR_PIPE_CONNECTED)
    {
        if (stopped_)
            return std::nullopt;
        error_ = "ConnectNamedPipe failed (" + std::to_string(::GetLastError()) + ")";
        return std::nullopt;
    }

    // Pre-create the next instance so the next accept() has a listening instance immediately. Every
    // instance carries the SAME owner-only DACL (D20) as the first.
    LPSECURITY_ATTRIBUTES psa =
        (security_ != nullptr) ? &reinterpret_cast<PipeSecurity*>(security_)->sa : nullptr;
    const std::wstring name = widen_ascii(endpoint_);
    HANDLE next = ::CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX,
                                     PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                     PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, psa);
    pending_ = (next == INVALID_HANDLE_VALUE) ? nullptr : next;
    return TransportConnection(static_cast<void*>(current));
}

void TransportServer::stop() noexcept
{
    stopped_ = true;
    if (handle_valid(pending_))
    {
        ::DisconnectNamedPipe(pending_);
        ::CloseHandle(pending_);
    }
    pending_ = nullptr;
    listening_ = false;
    if (security_ != nullptr)
    {
        delete reinterpret_cast<PipeSecurity*>(security_);
        security_ = nullptr;
    }
}

bool TransportServer::endpoint_owner_restricted() const
{
    // Inspect the exact DACL we built and passed to every CreateNamedPipeW — a reliable assertion of
    // the applied security with no live-handle READ_CONTROL concerns. Owner-only iff the ACL's every
    // ACE is an allow-ACE for the current-user SID (and at least one exists).
    if (!listening_ || security_ == nullptr)
        return false;
    const auto* sec = reinterpret_cast<const PipeSecurity*>(security_);
    std::vector<unsigned char> me;
    if (!current_user_sid(me))
        return false;
    return acl_is_owner_only(sec->acl, reinterpret_cast<PSID>(me.data()));
}

#else

bool TransportServer::listen()
{
    if (listening_)
        return true;
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
        error_ = std::string("socket() failed: ") + std::strerror(errno);
        return false;
    }
    set_nosigpipe(fd);

    sockaddr_un addr{};
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (endpoint_.size() + 1 > sizeof(addr.sun_path))
    {
        error_ = "endpoint path is too long for AF_UNIX";
        ::close(fd);
        return false;
    }
    std::memcpy(addr.sun_path, endpoint_.c_str(), endpoint_.size());

    ::unlink(endpoint_.c_str()); // remove a stale socket file from a prior (crashed) daemon

    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        error_ = std::string("bind() failed: ") + std::strerror(errno);
        ::close(fd);
        return false;
    }
    // Owner-only access on the socket file — the M1 loopback ambient guard (R-BRIDGE-007). The
    // owner-SID DACL / 0700-dir hardening is a documented follow-up.
    ::chmod(endpoint_.c_str(), S_IRUSR | S_IWUSR);

    if (::listen(fd, 8) != 0)
    {
        error_ = std::string("listen() failed: ") + std::strerror(errno);
        ::close(fd);
        return false;
    }
    listen_fd_ = fd;
    listening_ = true;
    return true;
}

std::optional<TransportConnection> TransportServer::accept()
{
    if (stopped_ || listen_fd_ < 0)
        return std::nullopt;
    for (;;)
    {
        const int cfd = ::accept(listen_fd_, nullptr, nullptr);
        if (cfd >= 0)
        {
            set_nosigpipe(cfd);
            return TransportConnection(cfd);
        }
        if (errno == EINTR)
        {
            if (stopped_)
                return std::nullopt;
            continue;
        }
        if (stopped_)
            return std::nullopt; // listen_fd_ closed under us by stop()
        error_ = std::string("accept() failed: ") + std::strerror(errno);
        return std::nullopt;
    }
}

void TransportServer::stop() noexcept
{
    stopped_ = true;
    if (listen_fd_ >= 0)
    {
        ::close(listen_fd_);
        ::unlink(endpoint_.c_str());
    }
    listen_fd_ = -1;
    listening_ = false;
}

bool TransportServer::endpoint_owner_restricted() const
{
    // POSIX: the socket file is already chmod'd 0600 in listen() (owner rw only). Assert exactly that
    // — the mode bits equal S_IRUSR|S_IWUSR and nothing broader (no group/other access).
    if (!listening_)
        return false;
    struct stat st{};
    if (::stat(endpoint_.c_str(), &st) != 0)
        return false;
    const auto perms = static_cast<unsigned>(st.st_mode) &
                       static_cast<unsigned>(S_IRWXU | S_IRWXG | S_IRWXO);
    return perms == static_cast<unsigned>(S_IRUSR | S_IWUSR);
}

#endif

// ------------------------------------------------------------------------------------------------
// TransportClient
// ------------------------------------------------------------------------------------------------

TransportClient::TransportClient(std::string endpoint) : endpoint_(std::move(endpoint)) {}

void TransportClient::close() noexcept
{
    conn_.close();
}

#if defined(_WIN32)

bool TransportClient::connect(int timeout_ms)
{
    const std::wstring name = widen_ascii(endpoint_);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;)
    {
        HANDLE h = ::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                 OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            DWORD mode = PIPE_READMODE_BYTE;
            ::SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
            conn_ = TransportConnection(static_cast<void*>(h));
            return true;
        }
        const DWORD e = ::GetLastError();
        if (std::chrono::steady_clock::now() >= deadline)
        {
            error_ = "connect timed out (last error " + std::to_string(e) + ")";
            return false;
        }
        if (e == ERROR_PIPE_BUSY)
        {
            ::WaitNamedPipeW(name.c_str(), 100);
            continue;
        }
        // ERROR_FILE_NOT_FOUND: the daemon has not created the pipe yet — back off and retry.
        sleep_ms(25);
    }
}

#else

bool TransportClient::connect(int timeout_ms)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;)
    {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
        {
            error_ = std::string("socket() failed: ") + std::strerror(errno);
            return false;
        }
        set_nosigpipe(fd);

        sockaddr_un addr{};
        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        if (endpoint_.size() + 1 > sizeof(addr.sun_path))
        {
            error_ = "endpoint path is too long for AF_UNIX";
            ::close(fd);
            return false;
        }
        std::memcpy(addr.sun_path, endpoint_.c_str(), endpoint_.size());

        if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0)
        {
            conn_ = TransportConnection(fd);
            return true;
        }
        const int e = errno;
        ::close(fd);
        if (std::chrono::steady_clock::now() >= deadline)
        {
            error_ = std::string("connect timed out: ") + std::strerror(e);
            return false;
        }
        sleep_ms(25); // ECONNREFUSED / ENOENT while the daemon is still binding — back off + retry
    }
}

#endif

std::optional<std::string> TransportClient::request(std::string_view request_json)
{
    if (!conn_.write_frame(request_json))
    {
        error_ = conn_.error().empty() ? "write failed" : conn_.error();
        return std::nullopt;
    }
    std::optional<std::string> response = conn_.read_frame();
    if (!response.has_value())
        error_ = conn_.error().empty() ? "peer disconnected before responding" : conn_.error();
    return response;
}

bool TransportClient::send(std::string_view request_json)
{
    if (!conn_.write_frame(request_json))
    {
        error_ = conn_.error().empty() ? "write failed" : conn_.error();
        return false;
    }
    return true;
}

std::optional<std::string> TransportClient::receive()
{
    std::optional<std::string> frame = conn_.read_frame();
    if (!frame.has_value())
        error_ = conn_.error().empty() ? "peer disconnected" : conn_.error();
    return frame;
}

std::optional<std::string> TransportClient::receive_timed(int timeout_ms, bool& timed_out)
{
    timed_out = false;
    std::optional<std::string> frame = conn_.read_frame_timed(timeout_ms, timed_out);
    if (!frame.has_value() && !timed_out)
        error_ = conn_.error().empty() ? "peer disconnected" : conn_.error();
    return frame;
}

} // namespace context::editor::bridge
