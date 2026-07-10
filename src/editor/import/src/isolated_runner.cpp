// The v1 reference isolated runner, the unprivileged-subprocess runner (issue #72), the portable
// result-codec used across the subprocess pipe, and the R-ASSET-001 determinism double-run gate.

#include "context/editor/import/isolated_runner.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#if defined(__linux__) || defined(__APPLE__)
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace context::editor::import
{
namespace
{

// --- portable little-endian wire helpers (the subprocess result frame) --------------------------

void put_u8(std::string& out, std::uint8_t v)
{
    out.push_back(static_cast<char>(v));
}

void put_u32(std::string& out, std::uint32_t v)
{
    for (int shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<char>((v >> shift) & 0xFFU));
}

void put_u64(std::string& out, std::uint64_t v)
{
    for (int shift = 0; shift < 64; shift += 8)
        out.push_back(static_cast<char>((v >> shift) & 0xFFU));
}

void put_bytes(std::string& out, std::string_view b)
{
    put_u64(out, b.size());
    out.append(b.data(), b.size());
}

// A bounds-checked cursor over the frame — every read validates against the remaining bytes and
// returns false rather than reading past the end (a child that died mid-write yields a short frame).
class Reader
{
public:
    explicit Reader(std::string_view buf) : buf_(buf) {}

    [[nodiscard]] bool u8(std::uint8_t& v)
    {
        if (remaining() < 1)
            return false;
        v = static_cast<std::uint8_t>(buf_[pos_++]);
        return true;
    }

    [[nodiscard]] bool u32(std::uint32_t& v)
    {
        if (remaining() < 4)
            return false;
        v = 0;
        for (int i = 0; i < 4; ++i)
            v |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(buf_[pos_++])) << (i * 8);
        return true;
    }

    [[nodiscard]] bool u64(std::uint64_t& v)
    {
        if (remaining() < 8)
            return false;
        v = 0;
        for (int i = 0; i < 8; ++i)
            v |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(buf_[pos_++])) << (i * 8);
        return true;
    }

    [[nodiscard]] bool bytes(std::string& v)
    {
        std::uint64_t len = 0;
        if (!u64(len))
            return false;
        if (len > static_cast<std::uint64_t>(remaining()))
            return false;
        const auto n = static_cast<std::size_t>(len);
        v.assign(buf_.data() + pos_, n);
        pos_ += n;
        return true;
    }

    [[nodiscard]] bool at_end() const { return pos_ == buf_.size(); }

private:
    [[nodiscard]] std::size_t remaining() const { return buf_.size() - pos_; }

    std::string_view buf_;
    std::size_t pos_ = 0;
};

// --- shared policy contract (both runners enforce it identically) -------------------------------

// Seed the audit with the granted envelope. os_primitive_enforced starts false; only the enforced
// subprocess path (Linux run_subprocess) flips it true once the seccomp filter is actually applied —
// the in-process run_isolated never claims a primitive it did not install (honest staging).
IsolatedImport make_audit(const SandboxPolicy& policy)
{
    IsolatedImport out;
    const OsSandboxSupport os = os_sandbox_support();
    out.audit.input_path = policy.input_path;
    out.audit.output_key = policy.output_key;
    out.audit.network_allowed = false; // v1 never grants network (R-SEC-010)
    out.audit.os_primitive = os.primitive;
    out.audit.os_primitive_enforced = false;
    return out;
}

// The two isolation refusals every runner applies BEFORE touching the importer. Returns true (and
// fills `out` with an ok=false import.jail_escape) when the run must be refused; false to proceed.
bool policy_refused(const SandboxPolicy& policy, const ImportInput& input, IsolatedImport& out)
{
    // v1 never grants network (R-SEC-010).
    if (policy.allow_network)
    {
        out.result.ok = false;
        out.result.diagnostics.push_back(
            {"import.jail_escape", "network capability requested but denied (R-SEC-010)"});
        return true;
    }
    // The source the run reads must be inside the narrowed read scope (R-SEC-006/008). Skipped for a
    // purely in-memory import (empty source_path) — the bytes are already the input.
    if (!input.source_path.empty() && !read_permitted(policy, input.source_path))
    {
        out.result.ok = false;
        out.result.diagnostics.push_back(
            {"import.jail_escape", "source path escapes the importer read scope (R-SEC-006/008)"});
        return true;
    }
    return false;
}

} // namespace

// --- the portable result codec (public: unit-tested directly) ------------------------------------

std::string encode_import_result(const ImportResult& result)
{
    std::string out;
    put_u8(out, result.ok ? 1U : 0U);
    put_u32(out, static_cast<std::uint32_t>(result.artifacts.size()));
    for (const DerivedArtifact& a : result.artifacts)
    {
        put_u8(out, static_cast<std::uint8_t>(a.kind));
        put_u32(out, a.derived_format_version);
        put_bytes(out, a.name);
        put_bytes(out, a.bytes);
    }
    put_u32(out, static_cast<std::uint32_t>(result.diagnostics.size()));
    for (const ImportDiagnostic& d : result.diagnostics)
    {
        put_bytes(out, d.code);
        put_bytes(out, d.message);
    }
    return out;
}

bool decode_import_result(std::string_view frame, ImportResult& out)
{
    Reader rd(frame);
    ImportResult r;

    std::uint8_t ok = 0;
    if (!rd.u8(ok))
        return false;
    r.ok = ok != 0;

    std::uint32_t artifact_count = 0;
    if (!rd.u32(artifact_count))
        return false;
    for (std::uint32_t i = 0; i < artifact_count; ++i)
    {
        std::uint8_t kind = 0;
        std::uint32_t dfv = 0;
        DerivedArtifact a;
        if (!rd.u8(kind) || !rd.u32(dfv) || !rd.bytes(a.name) || !rd.bytes(a.bytes))
            return false;
        if (kind > static_cast<std::uint8_t>(ArtifactKind::audio))
            return false; // an out-of-range kind is a corrupt frame, never a silent coercion
        a.kind = static_cast<ArtifactKind>(kind);
        a.derived_format_version = dfv;
        r.artifacts.push_back(std::move(a));
    }

    std::uint32_t diag_count = 0;
    if (!rd.u32(diag_count))
        return false;
    for (std::uint32_t i = 0; i < diag_count; ++i)
    {
        ImportDiagnostic d;
        if (!rd.bytes(d.code) || !rd.bytes(d.message))
            return false;
        r.diagnostics.push_back(std::move(d));
    }

    if (!rd.at_end())
        return false; // trailing bytes => a malformed / over-long frame

    out = std::move(r);
    return true;
}

// --- the in-process reference runner -------------------------------------------------------------

IsolatedImport run_isolated(const Importer& importer, const ImportInput& input,
                            const SandboxPolicy& policy)
{
    IsolatedImport out = make_audit(policy);
    if (policy_refused(policy, input, out))
        return out;

    // The importer is pure over source_bytes (importer.h) — it touches no path/env/clock, so running
    // it here is already confined to "input bytes + own output key". run_subprocess swaps in the real
    // OS-enforced sandbox with no importer change (os_sandbox_support staging).
    out.result = importer.import(input);
    return out;
}

// --- the unprivileged-subprocess runner (issue #72) ----------------------------------------------

// The POSIX fork()+seccomp/Seatbelt path — shared by Linux (seccomp-bpf) and macOS (a deny-by-default
// Seatbelt profile via sandbox_init). Both apply their OS primitive in the child via
// apply_importer_sandbox() (sandbox.h), so this fork/pipe/wait plumbing is identical; only the
// primitive installed inside apply_importer_sandbox() differs per platform.
#if defined(__linux__) || defined(__APPLE__)

namespace
{

// Child-side _exit codes (POSIX: Linux + macOS). The parent only distinguishes 0 vs non-zero, so exact
// values are diagnostic-only, but naming them keeps the fail-closed intent legible.
constexpr int kExitSandboxApplyFailed = 42; // apply_importer_sandbox() failed => run NOTHING unsandboxed
constexpr int kExitResultWriteFailed = 43;  // the result frame could not be written to the parent

bool write_all(int fd, std::string_view data)
{
    std::size_t off = 0;
    while (off < data.size())
    {
        const ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false;
        off += static_cast<std::size_t>(n);
    }
    return true;
}

std::string read_all(int fd)
{
    std::string out;
    char buf[4096];
    for (;;)
    {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            break; // a read error ends the frame; the parent's decode will reject a short frame
        }
        if (n == 0)
            break; // EOF: the child closed the write end
        out.append(buf, static_cast<std::size_t>(n));
    }
    return out;
}

void mark_subprocess_failed(IsolatedImport& out)
{
    out.result = ImportResult{};
    out.result.ok = false;
    out.result.diagnostics.push_back(
        {"import.subprocess_failed", "the isolated importer subprocess failed to spawn, was killed by "
                                     "its OS sandbox, or exited without returning a result (R-SEC-006)"});
}

} // namespace

IsolatedImport run_subprocess(const Importer& importer, const ImportInput& input,
                              const SandboxPolicy& policy)
{
    IsolatedImport out = make_audit(policy);
    // The policy pre-checks run in the PARENT, so a network/jail-escape request never even forks.
    if (policy_refused(policy, input, out))
        return out;

    int fds[2];
    if (::pipe(fds) != 0)
    {
        mark_subprocess_failed(out);
        return out;
    }

    const pid_t pid = ::fork();
    if (pid < 0)
    {
        ::close(fds[0]);
        ::close(fds[1]);
        mark_subprocess_failed(out);
        return out;
    }

    if (pid == 0)
    {
        // CHILD (unprivileged, about-to-be-sandboxed). It runs NOTHING but the pure importer + the
        // pipe write, then _exit — no exec, no further spawning. NOTE (honest staging): this forks
        // WITHOUT exec, so (a) it is safe only in the single-threaded reference/CLI context — a
        // multi-threaded daemon fork snapshot can hold another thread's allocator lock — and (b) the
        // child inherits the parent's open descriptors, and the OS sandbox primitive (the Linux seccomp
        // filter / the macOS deny-default Seatbelt profile) allows read/write on ALREADY-OPEN fds
        // (blocking only fresh `open*`). The daemon integration hardens BOTH by switching to a
        // fork+exec importer-host (O_CLOEXEC scrubs the fd table so the child starts with only the
        // result pipe) or a pre-forked zygote — a tracked follow-up; the lockdown (no fresh open =>
        // input-bytes-only for the filesystem, no network) + the read-scope contract are identical
        // either way.
        ::close(fds[0]); // the child never reads the result pipe
        const SandboxApplyResult applied = apply_importer_sandbox();
        if (!applied.applied)
            ::_exit(kExitSandboxApplyFailed); // fail closed: never run an importer unsandboxed here
        const ImportResult result = importer.import(input);
        const std::string frame = encode_import_result(result);
        if (!write_all(fds[1], frame))
            ::_exit(kExitResultWriteFailed);
        ::close(fds[1]);
        ::_exit(0);
    }

    // PARENT.
    ::close(fds[1]); // the parent never writes the result pipe
    const std::string frame = read_all(fds[0]);
    ::close(fds[0]);

    int status = 0;
    pid_t waited = 0;
    while ((waited = ::waitpid(pid, &status, 0)) < 0 && errno == EINTR)
    {
    }
    if (waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        // waitpid error (e.g. ECHILD under an embedder's SIGCHLD auto-reaper), killed by seccomp/a
        // signal, or a fail-closed child _exit — treat every non-clean wait as a subprocess failure,
        // never a silent exit-0 (a real exit status was never obtained when waited < 0).
        mark_subprocess_failed(out);
        return out;
    }

    ImportResult decoded;
    if (!decode_import_result(frame, decoded))
    {
        mark_subprocess_failed(out);
        return out;
    }

    out.result = std::move(decoded);
    // The OS primitive (seccomp-bpf on Linux / the Seatbelt profile on macOS) WAS applied in the child.
    out.audit.os_primitive_enforced = true;
    return out;
}

#else // no enforced OS primitive here yet — delegate to the in-process slice (honest staging).

IsolatedImport run_subprocess(const Importer& importer, const ImportInput& input,
                              const SandboxPolicy& policy)
{
    // Windows AppContainer / restricted Job Object is a tracked follow-up; until it lands the runner is
    // the portable in-process slice (os_primitive_enforced stays false). No importer change when the
    // primitive arrives — importers are pure over source_bytes.
    return run_isolated(importer, input, policy);
}

#endif

// --- the R-ASSET-001 determinism double-run gate -------------------------------------------------

DeterminismReport check_deterministic(const Importer& importer, const ImportInput& input)
{
    const ImportResult first = importer.import(input);
    const ImportResult second = importer.import(input);
    DeterminismReport report;

    if (first.ok != second.ok)
    {
        report.divergence = "ok flag differs between runs";
        return report;
    }
    if (first.artifacts.size() != second.artifacts.size())
    {
        report.divergence = "artifact count differs (" + std::to_string(first.artifacts.size()) +
                            " vs " + std::to_string(second.artifacts.size()) + ")";
        return report;
    }
    for (std::size_t i = 0; i < first.artifacts.size(); ++i)
    {
        const DerivedArtifact& a = first.artifacts[i];
        const DerivedArtifact& b = second.artifacts[i];
        const std::string at = "artifact[" + std::to_string(i) + "]";
        if (a.kind != b.kind)
        {
            report.divergence = at + " kind differs";
            return report;
        }
        if (a.name != b.name)
        {
            report.divergence = at + " name differs ('" + a.name + "' vs '" + b.name + "')";
            return report;
        }
        if (a.derived_format_version != b.derived_format_version)
        {
            report.divergence = at + " ('" + a.name + "') derived-format version differs";
            return report;
        }
        if (a.bytes != b.bytes)
        {
            report.divergence = at + " ('" + a.name + "') bytes differ (" +
                                std::to_string(a.bytes.size()) + " vs " +
                                std::to_string(b.bytes.size()) + " bytes)";
            return report;
        }
    }
    // Failure paths must also be deterministic: a bad source fails identically twice.
    if (first.diagnostics.size() != second.diagnostics.size())
    {
        report.divergence = "diagnostic count differs";
        return report;
    }
    for (std::size_t i = 0; i < first.diagnostics.size(); ++i)
    {
        if (first.diagnostics[i].code != second.diagnostics[i].code ||
            first.diagnostics[i].message != second.diagnostics[i].message)
        {
            report.divergence = "diagnostic[" + std::to_string(i) + "] differs";
            return report;
        }
    }

    report.deterministic = true;
    return report;
}

} // namespace context::editor::import
