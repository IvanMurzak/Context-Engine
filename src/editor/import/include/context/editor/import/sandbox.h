// Importer isolation — the v1 slice (R-SEC-006 / R-SEC-008 / R-SEC-010). An importer is untrusted
// same-user code (the trust boundary is the OS user, R-SEC-010); it MUST run confined to seeing the
// input bytes and its own cache-output key only, with:
//   * a TOCTOU-safe project-root path jail (R-SEC-008) — reused from filesync's jail primitive,
//   * a scrubbed environment (R-SEC-010) — no ambient secrets/tokens inherited,
//   * no ambient network (R-SEC-010) — network is a granted capability, never a default,
// all inside an unprivileged subprocess. This header pins the PORTABLE, enforceable half of that
// slice (the policy + the checks the reference runner applies) as real, tested code. The per-OS
// sandbox-primitive lockdown (seccomp-bpf on Linux / a deny-default Seatbelt profile on macOS /
// AppContainer on Windows) is ENFORCED on Linux + macOS and still STAGED on Windows, surfaced by
// os_sandbox_support() below — never silently assumed to be active (R-SEC-006).

#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace context::editor::import
{

// The capability envelope an importer run is confined to (R-SEC-006). Everything outside it is
// denied by construction: the importer converts `input_path`'s bytes and may write ONLY under its
// own `output_key` beneath `jail_root`; network is never allowed.
//
// Read scope (OWNER RULING, issue #72 — RESOLVED 2026-07-09): the enforced read set is
// **input-bytes-only by default, with a declared-read-paths escape hatch**. The importer subprocess
// may read ONLY the source bytes it converts (`input_path`) plus any paths it EXPLICITLY declares in
// `declared_read_paths` (e.g. a glTF's sibling `.bin`/textures, a `.tmx`'s tileset) — NOT the whole
// jail. Every declared path still stays inside the R-SEC-008 structural jail (`jail_root`): the escape
// hatch narrows FROM jail-wide, it can never widen past the jail. Effective read set =
// (input-bytes ∪ declared-paths) ⊆ jail. `read_permitted` enforces exactly this narrowed set; the
// per-OS syscall sandbox enforces it STRUCTURALLY — the child process is granted no `open*` capability
// at all, so it physically cannot open anything the policy did not grant. (In v1 the source bytes
// reach the pure importer in-memory, and a daemon fork+exec host will pre-open exactly the granted
// set — a tracked follow-up, see run_subprocess.) Writes stay narrowed to `output_key`
// (write_permitted).
struct SandboxPolicy
{
    std::string jail_root;  // the project/cache root the importer is jailed under (R-SEC-008)
    std::string input_path; // the source the importer converts (its bytes arrive via ImportInput)
    std::string output_key; // the importer's own cache-output key — its only writable target
    // The escape-hatch read set (owner ruling, issue #72): additional paths the importer DECLARES it
    // needs (sibling assets), granted explicitly per-importer. Empty by default — the pure v1
    // importers declare none, so the effective read set stays input-bytes-only. Each entry must be
    // inside `jail_root` or `read_permitted` refuses it (the escape hatch never widens past the jail).
    std::vector<std::string> declared_read_paths;
    bool allow_network = false; // ALWAYS false (no ambient network — R-SEC-010)
};

// R-SEC-010: the scrubbed environment an importer child process inherits — deliberately minimal, so
// no ambient secret/token from the parent leaks in. Returns only explicitly-safe, non-secret
// variables (a fixed locale set), NOT the parent environment. The daemon passes exactly this map to
// the subprocess; nothing else crosses.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> scrubbed_environment();

// May the importer READ `path`? (Owner ruling, issue #72.) True iff `path` is the source `input_path`
// OR at/under one of `declared_read_paths`, AND stays inside the R-SEC-008 jail (`jail_root`) — the
// narrowed input-bytes ∪ declared-paths set, NOT the whole jail. Delegates to filesync's structural
// jail primitive (normalize + is_inside_jail) for containment. The parent consults this to decide
// which files the importer may read; the per-OS syscall sandbox then denies the child any `open*`
// (v1: the source reaches the importer in-memory; a daemon fork+exec host will pre-open the granted
// set — a tracked follow-up), so the grant is structurally enforced. The writable set is narrower
// still (write_permitted).
[[nodiscard]] bool read_permitted(const SandboxPolicy& policy, std::string_view path);

// R-SEC-006: may the importer WRITE `path`? Only its own cache-output key (and paths beneath it, for
// a multi-artifact key dir) — never elsewhere in the jail, never outside it. This is what makes "own
// cache-output key only" enforceable rather than merely documented.
[[nodiscard]] bool write_permitted(const SandboxPolicy& policy, std::string_view path);

// The per-OS unprivileged-subprocess sandbox primitive and whether it is ENFORCED on this platform.
// Honest staging (R-SEC-006): Linux (seccomp-bpf) and macOS (a deny-default Seatbelt profile applied
// via sandbox_init) are BOTH enforced now; Windows (AppContainer / restricted Job Object) is a TRACKED
// de-risk item, still reported `enforced=false` so a caller never assumes a lockdown that is not there.
struct OsSandboxSupport
{
    std::string primitive;  // "seccomp-bpf" / "windows-appcontainer" / "macos-sandbox-exec" / "none"
    bool enforced = false;  // true where the primitive is actually applied (Linux + macOS today)
    std::string follow_up;  // the tracked note for a not-yet-enforced platform ("" when enforced)
};
[[nodiscard]] OsSandboxSupport os_sandbox_support();

// Apply the per-OS sandbox primitive to the CURRENT process — called inside the unprivileged importer
// subprocess (after fork, before the importer runs) to lock it down to a pure-computation syscall set:
// no `open*` (input-bytes-only — with no `open*` the child cannot reach any path the policy did not
// grant; in v1 the source bytes reach it in-memory), no network (`socket`/`connect`), no process
// creation (`execve`/`clone`/`fork`), no `ptrace`. On Linux this installs a seccomp-bpf filter
// (PR_SET_NO_NEW_PRIVS + a hand-written classic BPF syscall program — no libseccomp dependency, so the
// deny-by-default license gate stays clean); a denied syscall fails closed with EPERM (only a
// wrong-arch/ABI syscall is killed outright — the x86_64 guard closes the i386/x32 ABI-confusion
// hole). On macOS it installs a deny-by-default Seatbelt profile via sandbox_init (no third-party
// dependency either): fresh file opens, sockets, and process creation are all refused, mirroring the
// seccomp intent. On Windows it is a no-op reporting `applied=false` (its primitive is a tracked
// follow-up). Irreversible for the process, so call it only in a child that runs nothing but the pure
// importer. Never throws.
struct SandboxApplyResult
{
    bool applied = false;   // true iff the OS primitive was actually installed (Linux + macOS today)
    std::string primitive;  // mirrors os_sandbox_support().primitive
    std::string error;      // "" on success; else why the primitive could not be installed
};
[[nodiscard]] SandboxApplyResult apply_importer_sandbox();

} // namespace context::editor::import
