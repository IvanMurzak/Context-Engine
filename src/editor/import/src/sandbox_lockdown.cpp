// The per-OS unprivileged-subprocess sandbox PRIMITIVE (R-SEC-006) — apply_importer_sandbox().
//
// Linux (the wedge server platform) is the enforced path: a hand-written seccomp-bpf classic-BPF
// filter locks the importer child down to a pure-computation syscall set. It is HAND-WRITTEN on
// purpose — pulling libseccomp would add a third-party dependency and trip the deny-by-default license
// gate for zero benefit (the filter is tiny + fixed). Windows AppContainer / restricted Job Object and
// macOS sandbox-exec are tracked de-risk items (honest staging, R-SEC-006): this returns applied=false
// there and the runner falls back to the portable in-process slice.
//
// NOTE: the Linux body is compiled ONLY on the `build (ubuntu-latest)` CI leg — the local Windows
// Strawberry-GCC gate and the macOS leg preprocess it out (the platform-#if blind spot documented in
// test.md). Keep it self-contained and heavily reviewed; its permit/deny behaviour is proven by the
// syscall assertions in tests/test_sandbox.cpp (Linux-only section).

#include "context/editor/import/sandbox.h"

#if defined(__linux__)

#include <cerrno>
#include <cstddef>

#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

// Older kernel headers only define SECCOMP_RET_KILL (== kill-thread); prefer kill-PROCESS when present.
#ifndef SECCOMP_RET_KILL_PROCESS
#define SECCOMP_RET_KILL_PROCESS SECCOMP_RET_KILL
#endif

// AddressSanitizer detection (GCC defines __SANITIZE_ADDRESS__; Clang signals via __has_feature).
// Consumed below to widen the allow-set by ONE benign syscall in ASan-instrumented builds only —
// see the CONTEXT_IMPORT_ASAN_RUNTIME block inside the filter for the full rationale.
#if defined(__SANITIZE_ADDRESS__)
#define CONTEXT_IMPORT_ASAN_RUNTIME 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define CONTEXT_IMPORT_ASAN_RUNTIME 1
#endif
#endif
#ifndef CONTEXT_IMPORT_ASAN_RUNTIME
#define CONTEXT_IMPORT_ASAN_RUNTIME 0
#endif

namespace context::editor::import
{
namespace
{

// Fall through to the next instruction (RET ALLOW) when nr == the syscall; else skip that RET. This is
// the standard self-contained ALLOW idiom — no manual jump-offset arithmetic to get wrong.
#define CONTEXT_ALLOW_SYSCALL(name)                                                                \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_##name, 0, 1),                                         \
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)

// Allow `name` ONLY when its `prot` argument (args[2] for both mmap and mprotect on x86_64) does NOT
// request PROT_EXEC. A pure importer never needs to create executable memory after fork — the dynamic
// linker mapped every code segment before fork — so refusing PROT_EXEC denies a memory-corruption bug
// (e.g. in a hand-written PNG/WAV/glTF parser) the W^X pivot of mmap/mprotect-ing RWX memory and
// jumping to it, even though the syscall number itself is on the allow-set. Self-contained like
// CONTEXT_ALLOW_SYSCALL: on a nr MISMATCH A is left untouched (still the syscall number) so the
// following dispatch entries keep matching; on a MATCH A is reloaded with prot and the block returns
// ALLOW / EPERM inline. Only tiny fixed in-block offsets — no deny-terminal distance to get wrong.
#define CONTEXT_ALLOW_SYSCALL_NO_EXEC(name)                                                        \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_##name, 0, 4),                                         \
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,                                                          \
                 static_cast<__u32>(offsetof(struct seccomp_data, args[2]))),                       \
        BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K, PROT_EXEC, 1, 0),                                       \
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),                                               \
        BPF_STMT(BPF_RET | BPF_K,                                                                    \
                 static_cast<__u32>(SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)))

} // namespace

SandboxApplyResult apply_importer_sandbox()
{
    SandboxApplyResult out;
    out.primitive = "seccomp-bpf";

    // No-new-privs is a hard precondition for an unprivileged seccomp filter (else the kernel refuses
    // to install one without CAP_SYS_ADMIN). It also blocks any set-uid re-privilege inside the child.
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
    {
        out.error = "prctl(PR_SET_NO_NEW_PRIVS) failed";
        return out;
    }

    // The pure-computation allow-set: memory management (a malloc-heavy importer), pipe read/write of
    // already-open descriptors (the result pipe + any parent-granted input — never a fresh open), and
    // a clean exit. Everything NOT listed falls through to the deny-by-default terminal below. Reads
    // are input-bytes-only structurally: there is no `open`/`openat` in the set, so the child cannot
    // reach any path the policy did not grant (owner ruling, issue #72; in v1 the source bytes arrive
    // in-memory — see run_subprocess). No `socket`/`connect` => no network (R-SEC-010); no
    // `execve`/`clone`/`fork` => no process creation; no `ptrace`. `mmap`/`mprotect` are on the set but
    // argument-gated to REFUSE PROT_EXEC (W^X): the child cannot mint executable memory post-fork, so a
    // parser memory-corruption bug has no mmap/mprotect pivot to native code. The arch is gated first (x86_64, the
    // Linux-first server target) to close the i386/x32 syscall-ABI-confusion hole; a wrong-arch call is
    // KILLED, not merely denied. NOTE: signal-raising syscalls (`tgkill`/`kill`/`rt_sigaction`) are
    // deliberately omitted — an importer that hits abort()/assert() is denied its SIGABRT path (EPERM)
    // and dies via glibc's illegal-instruction fallback instead; still fail-closed, just a coarser
    // diagnostic (grant `tgkill` if a clean SIGABRT is ever wanted).
    struct sock_filter filter[] = {
        // Guard: only trust syscall numbers under the expected ABI.
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                 static_cast<__u32>(offsetof(struct seccomp_data, arch))),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),

        // Dispatch on the syscall number.
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, static_cast<__u32>(offsetof(struct seccomp_data, nr))),

        // Pipe / descriptor I/O (already-open FDs only).
        CONTEXT_ALLOW_SYSCALL(read),
        CONTEXT_ALLOW_SYSCALL(write),
        CONTEXT_ALLOW_SYSCALL(readv),
        CONTEXT_ALLOW_SYSCALL(writev),
        CONTEXT_ALLOW_SYSCALL(close),
        CONTEXT_ALLOW_SYSCALL(lseek),
        CONTEXT_ALLOW_SYSCALL(fstat),
        // Memory management for the allocator. mmap/mprotect are additionally gated to REFUSE PROT_EXEC
        // (no executable-memory creation post-fork — W^X for the pure-computation child); mremap carries
        // no prot argument (it preserves the existing protection) so it cannot add PROT_EXEC.
        CONTEXT_ALLOW_SYSCALL(brk),
        CONTEXT_ALLOW_SYSCALL_NO_EXEC(mmap),
        CONTEXT_ALLOW_SYSCALL(munmap),
        CONTEXT_ALLOW_SYSCALL(mremap),
        CONTEXT_ALLOW_SYSCALL_NO_EXEC(mprotect),
        CONTEXT_ALLOW_SYSCALL(madvise),
        // Threading/signal machinery libstdc++/glibc may touch on the pure path.
        CONTEXT_ALLOW_SYSCALL(futex),
        CONTEXT_ALLOW_SYSCALL(rt_sigprocmask),
        CONTEXT_ALLOW_SYSCALL(rt_sigreturn),
#if CONTEXT_IMPORT_ASAN_RUNTIME
        // ASan-instrumented builds ONLY (never the production filter): the compiler emits
        // __asan_handle_no_return before every noreturn call — including the child's _exit — and the
        // ASan runtime then QUERIES the alternate signal stack (sigaltstack(nullptr, ...) in
        // asan_posix.cpp PlatformUnpoisonStacks) to unpoison it. Denying it fails the runtime's
        // internal CHECK and aborts the child, so the sanitize CI leg could never run the REAL
        // filter end-to-end (observed: `sanitize (ASan+UBSan, ubuntu)`, run 29081682083).
        // sigaltstack is pure in-process signal-stack bookkeeping — no filesystem / network / exec /
        // memory-permission capability — and gating it on the sanitizer build keeps the production
        // allow-set unchanged while the sanitizers exercise this exact filter logic.
        CONTEXT_ALLOW_SYSCALL(sigaltstack),
#endif
        // Clean termination.
        CONTEXT_ALLOW_SYSCALL(exit),
        CONTEXT_ALLOW_SYSCALL(exit_group),

        // Deny-by-default: everything else fails closed with EPERM (observable + non-fatal, so an
        // unlisted benign syscall degrades gracefully rather than crashing a pure importer, while
        // open*/socket/connect/execve/ptrace/clone are all refused).
        BPF_STMT(BPF_RET | BPF_K,
                 static_cast<__u32>(SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA))),
    };

    struct sock_fprog prog;
    prog.len = static_cast<unsigned short>(sizeof(filter) / sizeof(filter[0]));
    prog.filter = filter;

    if (::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog, 0, 0) != 0)
    {
        out.error = "prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER) failed";
        return out;
    }

    out.applied = true;
    return out;
}

#undef CONTEXT_ALLOW_SYSCALL
#undef CONTEXT_ALLOW_SYSCALL_NO_EXEC
#undef CONTEXT_IMPORT_ASAN_RUNTIME

} // namespace context::editor::import

#else // non-Linux: the OS primitive is a tracked follow-up (honest staging, R-SEC-006).

namespace context::editor::import
{

SandboxApplyResult apply_importer_sandbox()
{
    // No enforced primitive here yet — the caller falls back to the portable in-process slice and
    // reports os_primitive_enforced=false. Mirror os_sandbox_support()'s primitive name so an audit
    // still names the intended lockdown.
    SandboxApplyResult out;
    out.primitive = os_sandbox_support().primitive;
    out.error = "per-OS sandbox primitive not yet enforced on this platform (tracked follow-up)";
    return out;
}

} // namespace context::editor::import

#endif
