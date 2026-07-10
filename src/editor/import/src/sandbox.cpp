// Importer isolation v1 slice — the portable, enforceable half (jail + scrubbed env + no network)
// plus the HONEST per-OS sandbox-primitive support report (R-SEC-006/008/010).

#include "context/editor/import/sandbox.h"

#include "context/editor/filesync/path_jail.h"

namespace context::editor::import
{

std::vector<std::pair<std::string, std::string>> scrubbed_environment()
{
    // Deliberately NOT the parent environment (R-SEC-010): no ambient secret/token crosses to an
    // importer child. Only an explicitly-safe, non-secret, DETERMINISTIC set — the C locale, which
    // also pins locale-dependent parsing off (a determinism aid, R-ASSET-001). A future importer
    // that legitimately needs another variable adds it here explicitly, never by inheritance.
    return {
        {"LANG", "C"},
        {"LC_ALL", "C"},
    };
}

bool read_permitted(const SandboxPolicy& policy, std::string_view path)
{
    // OWNER RULING (issue #72 — RESOLVED 2026-07-09): the enforced read set is input-bytes-only by
    // default, with a declared-read-paths escape hatch, all inside the R-SEC-008 jail. This predicate
    // NARROWS from the former jail-wide grant: a read is permitted iff `path` is the source
    // `input_path`, OR is at/under one of the importer's `declared_read_paths` — AND (outer bound) it
    // stays inside `jail_root`. The escape hatch can never widen past the jail. Fails closed on an
    // empty/malformed policy. Containment uses the ONE shared jail primitive (filesync).
    if (policy.jail_root.empty())
        return false;
    if (!filesync::is_inside_jail(policy.jail_root, path))
        return false; // outer bound: never past the R-SEC-008 structural jail
    // Input bytes: the source the importer converts (its sole default read).
    if (!policy.input_path.empty() && filesync::is_inside_jail(policy.input_path, path))
        return true;
    // Escape hatch: an explicitly-declared sibling-asset path (each already ⊆ jail, re-checked here).
    for (const std::string& declared : policy.declared_read_paths)
        if (!declared.empty() && filesync::is_inside_jail(declared, path))
            return true;
    return false; // inside the jail but not in the narrowed input-bytes ∪ declared-paths set
}

bool write_permitted(const SandboxPolicy& policy, std::string_view path)
{
    // R-SEC-006: writes are confined to the importer's OWN cache-output key (and paths beneath it),
    // AND that key must itself be inside the jail — defense in depth, so a malformed output_key can
    // never widen the writable set past the jail.
    if (policy.output_key.empty() || policy.jail_root.empty())
        return false;
    if (!filesync::is_inside_jail(policy.jail_root, path))
        return false;
    return filesync::is_inside_jail(policy.output_key, path);
}

OsSandboxSupport os_sandbox_support()
{
    // HONEST staging (R-SEC-006): report the primitive this OS is locked down with, and truthfully
    // whether it is enforced. Linux (seccomp-bpf) is the wedge server platform and IS enforced now
    // (apply_importer_sandbox installs the filter in the importer subprocess). Windows / macOS still
    // report `enforced=false` — their primitives are tracked de-risk items and the runner falls back
    // to the portable in-process slice (jail + scrubbed env + no network + input-bytes-only) there.
    // Never claim a lockdown that is not there.
#if defined(__linux__)
    return {"seccomp-bpf", true, ""};
#elif defined(_WIN32)
    return {"windows-appcontainer", false,
            "Windows AppContainer / restricted Job Object lockdown is a tracked de-risk item; the "
            "runner falls back to the portable in-process slice (path jail + scrubbed env + "
            "no-ambient-network + input-bytes-only) here."};
#elif defined(__APPLE__)
    return {"macos-sandbox-exec", false,
            "macOS sandbox-exec lockdown is a tracked de-risk item; the runner falls back to the "
            "portable in-process slice (path jail + scrubbed env + no-ambient-network + "
            "input-bytes-only) here."};
#else
    return {"none", false,
            "No per-OS sandbox primitive is mapped for this platform; the runner falls back to the "
            "portable in-process slice (path jail + scrubbed env + no-ambient-network + "
            "input-bytes-only) here."};
#endif
}

} // namespace context::editor::import
