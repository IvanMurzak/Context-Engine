// Real IPC transport implementation (see transport.h). POSIX = Unix domain stream socket; Windows =
// named pipe (byte mode). The #ifdef split mirrors lock.cpp: one small, portable surface over two OS
// primitives, warnings-clean under GCC/libstdc++, MSVC, and Apple Clang/libc++.

#include "context/editor/bridge/transport.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
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
    const std::wstring name = widen_ascii(endpoint_);
    // Create the first pipe instance; subsequent instances are created per-accept so a fresh client
    // always finds a listening instance (PIPE_UNLIMITED_INSTANCES).
    HANDLE h = ::CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX,
                                  PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                  PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, nullptr);
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

    // Pre-create the next instance so the next accept() has a listening instance immediately.
    const std::wstring name = widen_ascii(endpoint_);
    HANDLE next = ::CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX,
                                     PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                     PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, nullptr);
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

bool TransportClient::notify(std::string_view request_json)
{
    if (!conn_.write_frame(request_json))
    {
        error_ = conn_.error().empty() ? "write failed" : conn_.error();
        return false;
    }
    return true;
}

} // namespace context::editor::bridge
