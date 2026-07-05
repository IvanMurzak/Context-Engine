// Running an importer under the R-SEC-006 isolation slice + the R-ASSET-001 determinism gate.
//
// The v1 reference runner executes the importer IN-PROCESS behind the SandboxPolicy — enforcing the
// portable half of the slice (path jail R-SEC-008, scrubbed env R-SEC-010, input-bytes-only, no
// network) and producing a SandboxAudit of exactly what the run was permitted to touch. That is
// enough for the determinism gate and to make the isolation CONTRACT real and tested. The
// unprivileged-subprocess + per-OS sandbox-primitive lockdown (seccomp-bpf Linux-first) is the
// staged rollout (sandbox.h::os_sandbox_support); the daemon swaps this in-process runner for the
// subprocess one without any importer change, since importers are already pure (importer.h).

#pragma once

#include "context/editor/import/importer.h"
#include "context/editor/import/sandbox.h"

#include <string>

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
