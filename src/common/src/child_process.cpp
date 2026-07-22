// The long-running child-process primitive with captured stdout (see context/common/child_process.h).
//
// POSIX = pipe + fork + dup2 + execv, drained with poll(); Windows = CreatePipe + CreateProcessW,
// drained with PeekNamedPipe + ReadFile. The base spawn/kill/wait shape mirrors the CI-proven
// src/cli/tests/process_util.h; the captured-stdout pipe + the timed line reader are the net-new part.

#include "context/common/child_process.h"

#include <chrono>
#include <cstring>
#include <utility>

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
#include <csignal>
#include <ctime>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace context::common::process
{
namespace
{

// Pull the next '\n'-terminated line out of `pending`, dropping a trailing '\r' so a CRLF child (the
// Windows `context daemon` writes '\n' via fputc, but a shell wrapper might CRLF) reads cleanly.
bool take_line(std::string& pending, std::string& line)
{
    const std::string::size_type nl = pending.find('\n');
    if (nl == std::string::npos)
        return false;
    std::string::size_type end = nl;
    if (end > 0 && pending[end - 1] == '\r')
        --end;
    line.assign(pending, 0, end);
    pending.erase(0, nl + 1);
    return true;
}

[[maybe_unused]] int clamp_nonneg(long long ms) // used only on the POSIX poll() path
{
    if (ms < 0)
        return 0;
    if (ms > 0x7fffffffLL)
        return 0x7fffffff;
    return static_cast<int>(ms);
}

} // namespace

ChildProcess::~ChildProcess()
{
    // The destructor behaves like detach(): close handles WITHOUT killing, so a spawned daemon that
    // other clients may be using survives the owning handle dropping.
    close_stdout();
    close_process_handle();
    pid_ = 0;
}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
{
    *this = std::move(other);
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept
{
    if (this == &other)
        return *this;
    // Release anything we currently hold (detach semantics — never kill on a move).
    close_stdout();
    close_process_handle();
#if defined(_WIN32)
    process_handle_ = other.process_handle_;
    stdout_read_ = other.stdout_read_;
#else
    stdout_fd_ = other.stdout_fd_;
#endif
    pid_ = other.pid_;
    exited_ = other.exited_;
    pending_ = std::move(other.pending_);
    other.reset();
    return *this;
}

void ChildProcess::reset() noexcept
{
    pid_ = 0;
    exited_ = false;
    pending_.clear();
#if defined(_WIN32)
    process_handle_ = nullptr;
    stdout_read_ = nullptr;
#else
    stdout_fd_ = -1;
#endif
}

bool ChildProcess::valid() const noexcept
{
    return pid_ != 0;
}

void ChildProcess::close_stdout()
{
#if defined(_WIN32)
    if (stdout_read_ != nullptr)
    {
        ::CloseHandle(static_cast<HANDLE>(stdout_read_));
        stdout_read_ = nullptr;
    }
#else
    if (stdout_fd_ >= 0)
    {
        ::close(stdout_fd_);
        stdout_fd_ = -1;
    }
#endif
}

void ChildProcess::close_process_handle()
{
#if defined(_WIN32)
    if (process_handle_ != nullptr)
    {
        ::CloseHandle(static_cast<HANDLE>(process_handle_));
        process_handle_ = nullptr;
    }
#endif
    // POSIX keeps no separate process handle — pid_ is the identity and reaping is waitpid's job.
}

void ChildProcess::detach()
{
    close_stdout();
    close_process_handle();
    // On POSIX we intentionally do NOT waitpid here: reaping would block on the still-running daemon.
    // detach() is used as this process exits, so the orphaned child reparents to init and is reaped
    // there; a zombie would only accrue if the parent lived on indefinitely after detaching, which the
    // exit-policy caller never does.
    pid_ = 0;
}

#if defined(_WIN32)

ChildProcess ChildProcess::spawn(const SpawnOptions& options, std::string& error)
{
    ChildProcess child;

    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE; // the write end must be inheritable so the child can write to it
    sa.lpSecurityDescriptor = nullptr;

    if (options.capture_stdout)
    {
        if (::CreatePipe(&read_end, &write_end, &sa, 0) == FALSE)
        {
            error = "CreatePipe failed (" + std::to_string(::GetLastError()) + ")";
            return child;
        }
        // The parent's READ end must NOT be inherited by the child, or the child would keep the pipe
        // open and read_line() would never see EOF after the daemon exits.
        ::SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    }

    // Build the command line: "exe" "arg" "arg" ... in wide chars (paths come from std::filesystem in
    // the ACP narrow form). No metacharacter risk — our args are flags + filesystem paths.
    std::wstring cmd = L"\"" + options.executable.wstring() + L"\"";
    for (const std::string& a : options.args)
    {
        std::wstring wide;
        if (!a.empty())
        {
            const int need =
                ::MultiByteToWideChar(CP_ACP, 0, a.data(), static_cast<int>(a.size()), nullptr, 0);
            if (need > 0)
            {
                wide.resize(static_cast<std::size_t>(need));
                ::MultiByteToWideChar(CP_ACP, 0, a.data(), static_cast<int>(a.size()), wide.data(),
                                      need);
            }
        }
        cmd += L" \"" + wide + L"\"";
    }
    std::vector<wchar_t> mutable_cmd(cmd.begin(), cmd.end());
    mutable_cmd.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    if (options.capture_stdout)
    {
        // Only stdout is captured; stdin/stderr stay the parent's handles so daemon logs remain
        // visible and never fill the (unread-after-handshake) stdout pipe.
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = write_end;
        si.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);
    }

    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE, 0, nullptr,
                                     nullptr, &si, &pi);
    if (write_end != nullptr)
        ::CloseHandle(write_end); // the parent never writes; the child holds its inherited copy

    if (ok == FALSE)
    {
        error = "CreateProcessW failed for '" + options.executable.string() + "' (" +
                std::to_string(::GetLastError()) + ")";
        if (read_end != nullptr)
            ::CloseHandle(read_end);
        return child;
    }
    ::CloseHandle(pi.hThread);
    child.process_handle_ = pi.hProcess;
    child.stdout_read_ = read_end;
    child.pid_ = static_cast<std::int64_t>(pi.dwProcessId);
    return child;
}

bool ChildProcess::read_line(std::string& line, int timeout_ms)
{
    if (take_line(pending_, line))
        return true;
    if (stdout_read_ == nullptr)
        return false;

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
    char buf[4096];
    for (;;)
    {
        DWORD avail = 0;
        if (::PeekNamedPipe(static_cast<HANDLE>(stdout_read_), nullptr, 0, nullptr, &avail, nullptr) ==
            FALSE)
            return false; // the write end is closed (child exited) — end of stream
        if (avail > 0)
        {
            DWORD to_read = avail < sizeof(buf) ? avail : static_cast<DWORD>(sizeof(buf));
            DWORD got = 0;
            if (::ReadFile(static_cast<HANDLE>(stdout_read_), buf, to_read, &got, nullptr) == FALSE ||
                got == 0)
                return false;
            pending_.append(buf, got);
            if (take_line(pending_, line))
                return true;
            continue; // more may be buffered; loop without sleeping
        }
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        ::Sleep(5);
    }
}

bool ChildProcess::running()
{
    if (process_handle_ == nullptr)
        return false;
    return ::WaitForSingleObject(static_cast<HANDLE>(process_handle_), 0) == WAIT_TIMEOUT;
}

bool ChildProcess::wait(int timeout_ms, int& exit_code)
{
    if (process_handle_ == nullptr)
    {
        exit_code = exited_ ? 0 : -1;
        return true;
    }
    const DWORD r =
        ::WaitForSingleObject(static_cast<HANDLE>(process_handle_), static_cast<DWORD>(timeout_ms < 0 ? 0 : timeout_ms));
    if (r != WAIT_OBJECT_0)
        return false;
    DWORD code = 0;
    ::GetExitCodeProcess(static_cast<HANDLE>(process_handle_), &code);
    exit_code = static_cast<int>(code);
    exited_ = true;
    close_process_handle();
    close_stdout();
    pid_ = 0;
    return true;
}

void ChildProcess::terminate()
{
    if (process_handle_ != nullptr)
    {
        ::TerminateProcess(static_cast<HANDLE>(process_handle_), 1);
        ::WaitForSingleObject(static_cast<HANDLE>(process_handle_), 5000);
        close_process_handle();
    }
    close_stdout();
    exited_ = true;
    pid_ = 0;
}

#else // ---------------------------------------------------------------------------------- POSIX

ChildProcess ChildProcess::spawn(const SpawnOptions& options, std::string& error)
{
    ChildProcess child;

    int fds[2] = {-1, -1};
    if (options.capture_stdout)
    {
        if (::pipe(fds) != 0)
        {
            error = "pipe() failed: " + std::string(std::strerror(errno));
            return child;
        }
    }

    // Build argv BEFORE fork(): the child must call only async-signal-safe functions (dup2/close/
    // execv/_exit), never allocate. The c_str() pointers live in the parent's strings and survive into
    // the copy-on-write child.
    const std::string exe = options.executable.string();
    std::vector<char*> argv;
    argv.reserve(options.args.size() + 2);
    argv.push_back(const_cast<char*>(exe.c_str()));
    for (const std::string& a : options.args)
        argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0)
    {
        error = "fork() failed: " + std::string(std::strerror(errno));
        if (fds[0] >= 0)
            ::close(fds[0]);
        if (fds[1] >= 0)
            ::close(fds[1]);
        return child;
    }
    if (pid == 0)
    {
        // --- child --- (async-signal-safe only)
        if (options.capture_stdout)
        {
            ::dup2(fds[1], STDOUT_FILENO); // route stdout into the pipe; stderr/stdin stay inherited
            ::close(fds[0]);
            ::close(fds[1]);
        }
        ::execv(exe.c_str(), argv.data());
        ::_exit(127); // exec failed
    }

    // --- parent ---
    if (options.capture_stdout)
    {
        ::close(fds[1]); // the parent never writes; only the child holds the write end now
        child.stdout_fd_ = fds[0];
    }
    child.pid_ = static_cast<std::int64_t>(pid);
    return child;
}

bool ChildProcess::read_line(std::string& line, int timeout_ms)
{
    if (take_line(pending_, line))
        return true;
    if (stdout_fd_ < 0)
        return false;

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
    char buf[4096];
    for (;;)
    {
        const auto now = std::chrono::steady_clock::now();
        const long long remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        struct pollfd pfd;
        pfd.fd = stdout_fd_;
        pfd.events = POLLIN;
        pfd.revents = 0;
        const int pr = ::poll(&pfd, 1, clamp_nonneg(remaining));
        if (pr < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (pr == 0)
            return false; // timed out with no full line
        const ssize_t n = ::read(stdout_fd_, buf, sizeof(buf));
        if (n > 0)
        {
            pending_.append(buf, static_cast<std::size_t>(n));
            if (take_line(pending_, line))
                return true;
            continue;
        }
        if (n == 0)
            return false; // EOF — the child closed stdout / exited
        if (errno == EINTR || errno == EAGAIN)
            continue;
        return false;
    }
}

bool ChildProcess::running()
{
    if (pid_ <= 0)
        return false;
    int status = 0;
    const pid_t r = ::waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
    if (r == 0)
        return true; // still running
    // Exited (r == pid) or already gone (r < 0): reap and remember.
    exited_ = true;
    pid_ = 0;
    return false;
}

bool ChildProcess::wait(int timeout_ms, int& exit_code)
{
    if (pid_ <= 0)
    {
        exit_code = exited_ ? 0 : -1;
        return true;
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
    for (;;)
    {
        int status = 0;
        const pid_t r = ::waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
        if (r == static_cast<pid_t>(pid_))
        {
            if (WIFEXITED(status))
                exit_code = WEXITSTATUS(status);
            else
                exit_code = 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
            exited_ = true;
            pid_ = 0;
            return true;
        }
        if (r < 0)
        {
            exit_code = -1;
            exited_ = true;
            pid_ = 0;
            return true; // already reaped / gone — treat as exited
        }
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        timespec ts{};
        ts.tv_sec = 0;
        ts.tv_nsec = 10 * 1000 * 1000; // 10 ms
        ::nanosleep(&ts, nullptr);
    }
}

void ChildProcess::terminate()
{
    if (pid_ > 0)
    {
        ::kill(static_cast<pid_t>(pid_), SIGKILL);
        int status = 0;
        ::waitpid(static_cast<pid_t>(pid_), &status, 0);
        pid_ = 0;
    }
    close_stdout();
    exited_ = true;
}

#endif

} // namespace context::common::process
