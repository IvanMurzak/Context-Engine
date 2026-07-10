// Running an importer under the R-SEC-006 isolation slice + the R-ASSET-001 determinism gate.
//
// Two runners share the SandboxPolicy contract:
//   * run_isolated() — the IN-PROCESS reference runner. Enforces the portable half of the slice (path
//     jail R-SEC-008, scrubbed env R-SEC-010, input-bytes-only read scope, no network) and produces a
//     SandboxAudit of exactly what the run was permitted to touch. It underpins the determinism gate +
//     the fuzz replay, and is the honest fallback where no OS primitive is enforced yet.
//   * run_subprocess() — the UNPRIVILEGED-SUBPROCESS runner (issue #72). On Linux + macOS it fork()s a
//     child, locks it down with the OS primitive (sandbox.h::apply_importer_sandbox — seccomp-bpf on
//     Linux, a deny-default Seatbelt profile on macOS), runs the pure importer there, and pipes the
//     ImportResult back — a real OS-enforced sandbox (os_primitive_enforced=true). Where the primitive
//     is not yet enforced (Windows) it falls back to run_isolated. Importers are pure over source_bytes
//     (importer.h), so the result is identical across the swap and NO importer changes.
//
// Read scope (owner ruling, issue #72 — RESOLVED 2026-07-09): input-bytes-only by default, with a
// declared-read-paths escape hatch, all ⊆ the R-SEC-008 jail (see sandbox.h::SandboxPolicy /
// read_permitted). The seccomp filter enforces it structurally: the child gets no `open*`, so it
// cannot open any path the policy did not grant. (In v1 the source bytes reach the pure importer
// in-memory; a daemon fork+exec host will pre-open exactly the granted set — a tracked follow-up, see
// run_subprocess.)

#pragma once

#include "context/editor/import/importer.h"
#include "context/editor/import/sandbox.h"

#include <string>
#include <string_view>

namespace context::editor::import
{

// What an isolated run was permitted to see — provenance for tests + audit (R-SEC-006). Records that
// the run observed ONLY the input bytes and could write ONLY its own output key, and whether an OS
// sandbox primitive was actually enforced under it (honest: false on non-Linux in v1).
struct SandboxAudit
{
    std::string input_path;      // the single readable source the run saw
    std::string output_key;      // the single writable key the run was confined to
    bool network_allowed = false;
    bool os_primitive_enforced = false; // mirrors os_sandbox_support().enforced
    std::string os_primitive;           // the primitive name (or "none")
};

struct IsolatedImport
{
    ImportResult result;
    SandboxAudit audit;
};

// Run `importer` on `input` confined to `policy`. Enforces read/write jailing over the policy before
// and after the import (the importer itself touches no paths in v1 — it is pure over source_bytes —
// so this validates the CONTRACT the subprocess runner will enforce at the syscall layer). Total:
// never throws; a jail/policy violation surfaces as an `import.jail_escape` diagnostic with ok=false.
[[nodiscard]] IsolatedImport run_isolated(const Importer& importer, const ImportInput& input,
                                         const SandboxPolicy& policy);

// Run `importer` on `input` in an UNPRIVILEGED SUBPROCESS confined to `policy` (R-SEC-006, issue #72).
// On Linux + macOS the child is fork()ed, locked down with the OS primitive
// (sandbox.h::apply_importer_sandbox — seccomp-bpf on Linux, a deny-default Seatbelt profile on macOS),
// runs the pure importer, and pipes its ImportResult back to the parent — os_primitive_enforced=true in
// the audit. Where the OS primitive is not enforced yet (Windows in v1) it transparently delegates to
// run_isolated (os_primitive_enforced=false, honest staging). The policy pre-checks (network /
// jail-escape refusals) run in the PARENT, so a bad policy never even forks. Total: never throws; a
// spawn/pipe/decode failure of the isolated child surfaces as an `import.subprocess_failed` diagnostic
// with ok=false.
[[nodiscard]] IsolatedImport run_subprocess(const Importer& importer, const ImportInput& input,
                                            const SandboxPolicy& policy);

// Encode/decode an ImportResult across the importer-subprocess pipe — PORTABLE (no platform code), so
// the wire framing is unit-tested directly on every host. The child serializes its ImportResult to a
// length-prefixed byte frame written to the result pipe; the parent decodes it back. `decode` is total:
// it returns false on any truncation / length-overrun instead of throwing (a short read from a child
// that died mid-write is a subprocess failure, not a crash).
[[nodiscard]] std::string encode_import_result(const ImportResult& result);
[[nodiscard]] bool decode_import_result(std::string_view frame, ImportResult& out);

// The R-ASSET-001 determinism gate: import twice and byte-compare EVERY artifact (kind, name, and
// bytes). `deterministic` is false on any divergence, with `divergence` naming the first mismatch
// (the CI double-run byte-compare gate runs this over every importer x corpus entry).
struct DeterminismReport
{
    bool deterministic = false;
    std::string divergence; // "" when deterministic; else a human-readable first-mismatch note
};
[[nodiscard]] DeterminismReport check_deterministic(const Importer& importer,
                                                   const ImportInput& input);

} // namespace context::editor::import
