// A long-running child process whose STDOUT is captured over a pipe (M9 e14a, the daemon lifecycle
// spine). The one native infra the editor Shell needs to SPAWN `context daemon` as a child and read
// the D20 attach token off the child's stdout — never argv/env (05 §2 / 08 threat model).
//
// This is the D10-clean home the spec mandates: `context_common` links only the warnings baseline and
// nothing from the EditorKernel's internal modules, so putting the spawn primitive here keeps the
// shell-boundary gate's FORBIDDEN list byte-identical (the Shell spawns the `context` binary as a
// SUBPROCESS — a runtime dependency, not a link one — so nothing kernel-internal enters its closure).
//
// The base spawn/kill/wait shape mirrors the CI-proven cross-process helper `src/cli/tests/process_util.h`
// (POSIX fork+execv+waitpid+SIGKILL, Windows CreateProcessW+WaitForSingleObject+TerminateProcess); the
// NEW part is the captured-stdout pipe (POSIX pipe+dup2+poll, Windows CreatePipe+PeekNamedPipe) and the
// timed line reader. The header names NO platform types (HANDLEs are opaque `void*`), so a CEF-adjacent
// consumer never transitively pulls `<windows.h>` and its `near`/`far` macros.
//
// OWNERSHIP / LIFETIME. A ChildProcess OWNS the OS process + the captured stdout handle:
//   * terminate() force-kills (SIGKILL / TerminateProcess) and reaps — the child is GONE.
//   * detach() relinquishes ownership: the child keeps running, handles are closed, and neither the
//     destructor nor terminate() will ever touch it again. This is the exit-policy "leave the daemon
//     running for the other attached clients" path.
//   * the destructor behaves like detach() — it closes handles WITHOUT killing. A spawned daemon that
//     other clients may be using must NOT die merely because the owning handle dropped; a caller that
//     wants it gone calls terminate() (or wait()s after a clean in-band shutdown).

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace context::common::process
{

// What to spawn. `executable` is argv[0]; `args` are the remaining arguments (each passed verbatim to
// the child with NO shell involved — no quoting/metacharacter hazard, unlike the std::system runner in
// subprocess.h). `capture_stdout` routes the child's stdout into a pipe read_line() drains; stderr and
// stdin stay the parent's inherited handles (so daemon logs remain visible and never fill the pipe).
struct SpawnOptions
{
    std::filesystem::path executable;
    std::vector<std::string> args;
    bool capture_stdout = true;
};

class ChildProcess
{
public:
    ChildProcess() = default;
    ~ChildProcess();

    ChildProcess(ChildProcess&& other) noexcept;
    ChildProcess& operator=(ChildProcess&& other) noexcept;
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    // Spawn `options.executable` with `options.args`. On failure returns an invalid ChildProcess
    // (valid() == false) and sets `error`; never throws.
    [[nodiscard]] static ChildProcess spawn(const SpawnOptions& options, std::string& error);

    // A live (spawned, not-yet-detached) handle.
    [[nodiscard]] bool valid() const noexcept;
    // The child's OS process id (0 when invalid). Informational — a sibling can kill by pid.
    [[nodiscard]] std::int64_t pid() const noexcept { return pid_; }

    // Read ONE '\n'-terminated line from the captured stdout, waiting up to `timeout_ms`. Returns true
    // and sets `line` (WITHOUT the trailing '\n'/'\r') when a whole line arrived; false on timeout, on
    // end-of-stream (the child closed stdout / exited), or when stdout was not captured. Bytes read
    // past the newline are retained for the next call, so a single read that delivered several lines
    // is drained line by line.
    [[nodiscard]] bool read_line(std::string& line, int timeout_ms);

    // A non-blocking liveness probe. true while the child is still running; false once it has exited
    // (POSIX reaps it here so it does not linger as a zombie).
    [[nodiscard]] bool running();

    // Wait up to `timeout_ms` for the child to exit. true (+ `exit_code`) once it has exited (and it is
    // reaped); false on timeout (the child is still running).
    [[nodiscard]] bool wait(int timeout_ms, int& exit_code);

    // Force-kill (SIGKILL / TerminateProcess) and reap. Idempotent; a no-op on an invalid/detached
    // handle.
    void terminate();

    // Relinquish ownership: the child keeps running, its captured stdout handle (and, on Windows, the
    // process handle) are closed, and the destructor/terminate() will not touch it. Intended for the
    // exit-policy path where the daemon must SURVIVE this process because other clients are attached.
    void detach();

private:
    void close_stdout();
    void close_process_handle();
    void reset() noexcept;

    std::int64_t pid_ = 0;
    bool exited_ = false;
    std::string pending_; // bytes read past the last newline, awaiting the next read_line()

#if defined(_WIN32)
    void* process_handle_ = nullptr; // HANDLE (opaque so the header never includes <windows.h>)
    void* stdout_read_ = nullptr;    // HANDLE, the read end of the stdout pipe
#else
    int stdout_fd_ = -1; // the read end of the stdout pipe
#endif
};

} // namespace context::common::process
