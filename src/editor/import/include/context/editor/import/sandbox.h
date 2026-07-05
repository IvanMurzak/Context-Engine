// Importer isolation — the v1 slice (R-SEC-006 / R-SEC-008 / R-SEC-010). An importer is untrusted
// same-user code (the trust boundary is the OS user, R-SEC-010); it MUST run confined to seeing the
// input bytes and its own cache-output key only, with:
//   * a TOCTOU-safe project-root path jail (R-SEC-008) — reused from filesync's jail primitive,
//   * a scrubbed environment (R-SEC-010) — no ambient secrets/tokens inherited,
//   * no ambient network (R-SEC-010) — network is a granted capability, never a default,
// all inside an unprivileged subprocess. This header pins the PORTABLE, enforceable half of that
// slice (the policy + the checks the reference runner applies) as real, tested code. The per-OS
// sandbox-primitive lockdown (seccomp-bpf / AppContainer / sandbox-exec) is STAGED Linux-first and
// surfaced by os_sandbox_support() below — never silently assumed to be active (R-SEC-006).

#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace context::editor::import
{

// The capability envelope an importer run is confined to (R-SEC-006). Everything outside it is
// denied by construction: the importer converts `input_path`'s bytes and may write ONLY under its
// own `output_key` beneath `jail_root`; network is never allowed in v1. Read scope: in v1 the
// importer is a PURE function of the source bytes (importer.h) — it performs no reads of its own, so
// "input-bytes-only" is the effective guarantee; `read_permitted` enforces the broader R-SEC-008
// *structural* jail (reads anywhere under `jail_root`) that the future unprivileged-subprocess
// runner's syscall read-filter operates within. Writes stay narrowed to `output_key` (write_permitted).
struct SandboxPolicy
{
    std::string jail_root;  // the project/cache root the importer is jailed under (R-SEC-008)
    std::string input_path; // the source the importer converts (its bytes arrive via ImportInput)
    std::string output_key; // the importer's own cache-output key — its only writable target
    bool allow_network = false; // ALWAYS false in v1 (no ambient network — R-SEC-010)
};

// R-SEC-010: the scrubbed environment an importer child process inherits — deliberately minimal, so
// no ambient secret/token from the parent leaks in. Returns only explicitly-safe, non-secret
// variables (a fixed locale set), NOT the parent environment. The daemon passes exactly this map to
// the subprocess; nothing else crosses.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> scrubbed_environment();

// R-SEC-008: is `path` inside the policy's jail root? Delegates to filesync's structural jail
// (normalize + is_inside_jail) — the ONE jail primitive, shared with the file-write path. Reads are
// permitted anywhere inside the jail; the writable set is narrower (write_permitted below).
[[nodiscard]] bool read_permitted(const SandboxPolicy& policy, std::string_view path);

// R-SEC-006: may the importer WRITE `path`? Only its own cache-output key (and paths beneath it, for
// a multi-artifact key dir) — never elsewhere in the jail, never outside it. This is what makes "own
// cache-output key only" enforceable rather than merely documented.
[[nodiscard]] bool write_permitted(const SandboxPolicy& policy, std::string_view path);

// The per-OS unprivileged-subprocess sandbox primitive and whether v1 ENFORCES it on this platform.
// Honest staging (R-SEC-006): Linux (seccomp-bpf) is the wedge server platform and lands first;
// Windows (AppContainer / restricted Job Object) and macOS (sandbox-exec) are TRACKED de-risk items,
// reported `enforced=false` so a caller never assumes a lockdown that is not there.
struct OsSandboxSupport
{
    std::string primitive;  // "seccomp-bpf" / "windows-appcontainer" / "macos-sandbox-exec" / "none"
    bool enforced = false;  // true only where the primitive is actually applied in v1 (Linux)
    std::string follow_up;  // the tracked note for a not-yet-enforced platform ("" when enforced)
};
[[nodiscard]] OsSandboxSupport os_sandbox_support();

} // namespace context::editor::import
