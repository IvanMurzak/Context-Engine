// Minimal cross-platform child-process helper for the cross-process e2e test (spawn / wait-with-
// timeout / kill / reap). Header-only, test-only. POSIX = fork+execv+waitpid; Windows = CreateProcessW
// + WaitForSingleObject. The timeout + guaranteed reap make the socket/process e2e deterministic:
// nothing hangs CI, no zombie is left behind.

#pragma once

#include <chrono>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <ctime>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace ctest_proc
{

#if defined(_WIN32)

struct Process
{
    HANDLE handle = nullptr;
};

inline Process spawn(const std::string& exe, const std::vector<std::string>& args)
{
    std::string cmd = "\"" + exe + "\"";
    for (const std::string& a : args)
        cmd += " \"" + a + "\"";

    std::vector<wchar_t> buf;
    buf.reserve(cmd.size() + 1);
    for (const char c : cmd)
        buf.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    buf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    Process p;
    if (::CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si,
                         &pi) != FALSE)
    {
        ::CloseHandle(pi.hThread);
        p.handle = pi.hProcess;
    }
    return p;
}

inline bool valid(const Process& p) noexcept
{
    return p.handle != nullptr;
}

// Wait up to timeout_ms. Returns true (and sets exit_code) if the process exited; false on timeout.
inline bool wait_for(Process& p, int timeout_ms, int& exit_code)
{
    const DWORD r = ::WaitForSingleObject(p.handle, static_cast<DWORD>(timeout_ms));
    if (r == WAIT_OBJECT_0)
    {
        DWORD code = 0;
        ::GetExitCodeProcess(p.handle, &code);
        exit_code = static_cast<int>(code);
        return true;
    }
    return false;
}

inline void kill(Process& p)
{
    if (p.handle != nullptr)
        ::TerminateProcess(p.handle, 1);
}

inline void release(Process& p)
{
    if (p.handle != nullptr)
    {
        ::CloseHandle(p.handle);
        p.handle = nullptr;
    }
}

#else

struct Process
{
    pid_t pid = -1;
};

inline Process spawn(const std::string& exe, const std::vector<std::string>& args)
{
    // Build argv BEFORE fork() so the child (which must only call async-signal-safe execv) does no
    // allocation. The c_str() pointers live in the parent's args and survive into the COW child.
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(exe.c_str()));
    for (const std::string& a : args)
        argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    Process p;
    const pid_t pid = ::fork();
    if (pid < 0)
        return p;
    if (pid == 0)
    {
        ::execv(exe.c_str(), argv.data());
        ::_exit(127); // exec failed
    }
    p.pid = pid;
    return p;
}

inline bool valid(const Process& p) noexcept
{
    return p.pid > 0;
}

inline bool wait_for(Process& p, int timeout_ms, int& exit_code)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;)
    {
        int status = 0;
        const pid_t r = ::waitpid(p.pid, &status, WNOHANG);
        if (r == p.pid)
        {
            if (WIFEXITED(status))
                exit_code = WEXITSTATUS(status);
            else
                exit_code = 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
            p.pid = -1; // reaped
            return true;
        }
        if (r < 0)
        {
            exit_code = -1;
            p.pid = -1;
            return true; // already reaped / error — treat as exited
        }
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        timespec ts{};
        ts.tv_sec = 0;
        ts.tv_nsec = 10 * 1000 * 1000; // 10 ms
        ::nanosleep(&ts, nullptr);
    }
}

inline void kill(Process& p)
{
    if (p.pid > 0)
        ::kill(p.pid, SIGKILL);
}

inline void release(Process& p)
{
    if (p.pid > 0)
    {
        int status = 0;
        ::waitpid(p.pid, &status, 0);
        p.pid = -1;
    }
}

#endif

} // namespace ctest_proc
