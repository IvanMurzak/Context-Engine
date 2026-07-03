// Single-instance project lock (R-BRIDGE-001 / R-ARCH-005): the atomic first action of any
// write-capable EditorKernel instantiation.
//
// At most one EditorKernel operates on a Project at a time, enforced by an OS ADVISORY byte-range
// lock on `<project>/.editor/lock` (not merely a lockfile's presence). Acquisition is the atomic
// first action of a write-capable instantiation, and a try-lock FAILURE is itself the
// "instance already live → attach" signal — one gate, no separate liveness probe to race
// (R-ARCH-005). Instance identity is keyed by the CANONICAL project path, so Windows junctions /
// subst / UNC spellings that alias one directory to many paths still exclude (L-26: each git
// worktree is its own Project instance). Read-only use may take the lock in SHARED mode and escalate
// to exclusive per write.
//
// Two enforcement layers, both keyed on the canonical path: (1) an in-process guard so a second
// instantiation in the SAME process detects the conflict deterministically (and so POSIX fcntl's
// same-process re-lock semantics never mask it); (2) the OS byte-range lock (Windows LockFileEx /
// POSIX fcntl) that excludes a second OS PROCESS. The OS lock is the cross-process authority; the
// in-process guard makes single-process behavior deterministic and unit-testable.

#pragma once

#include <filesystem>
#include <string>

namespace context::editor::bridge
{

enum class LockMode
{
    shared,    // read-only in-process use; coexists with other shared holders
    exclusive, // write-capable instantiation; excludes all other holders
};

enum class LockOutcome
{
    acquired,     // the lock is now held in the requested mode
    already_held, // an incompatible instance already holds it — THIS is the "attach" signal
    error,        // an unexpected OS/filesystem error (see error_message())
};

class ProjectLock
{
public:
    // Resolves `project_root` to its canonical form and targets `<canonical>/.editor/lock`. Does NOT
    // acquire — call try_acquire(). The `.editor/` directory + lock file are created on first
    // acquire.
    explicit ProjectLock(const std::filesystem::path& project_root);
    ~ProjectLock();

    ProjectLock(const ProjectLock&) = delete;
    ProjectLock& operator=(const ProjectLock&) = delete;
    ProjectLock(ProjectLock&& other) noexcept;
    ProjectLock& operator=(ProjectLock&& other) noexcept;

    // Atomic, NON-BLOCKING try-lock. exclusive conflicts with any existing holder; shared conflicts
    // only with an existing exclusive holder. Returns already_held (the attach signal) on conflict.
    [[nodiscard]] LockOutcome try_acquire(LockMode mode);

    // Release the lock if held (also invoked by the destructor). Safe to call when not held.
    void release();

    [[nodiscard]] bool held() const noexcept { return held_; }
    [[nodiscard]] LockMode mode() const noexcept { return mode_; }
    [[nodiscard]] const std::filesystem::path& lock_path() const noexcept { return lock_path_; }
    [[nodiscard]] const std::string& canonical_key() const noexcept { return canonical_key_; }
    [[nodiscard]] const std::string& error_message() const noexcept { return error_; }

private:
    void os_release();

    std::filesystem::path lock_path_;
    std::string canonical_key_; // the in-process-guard + identity key
    bool held_ = false;
    LockMode mode_ = LockMode::exclusive;
    std::string error_;

#if defined(_WIN32)
    void* handle_ = nullptr; // HANDLE; INVALID_HANDLE_VALUE when closed
#else
    int fd_ = -1;
#endif
};

} // namespace context::editor::bridge
