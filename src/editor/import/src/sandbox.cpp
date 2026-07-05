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
    // R-SEC-008: reads are confined to the jail root (the project/cache root). The ONE jail
    // primitive, shared with the file-write path (filesync).
    if (policy.jail_root.empty())
        return false;
    return filesync::is_inside_jail(policy.jail_root, path);
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
    // HONEST staging (R-SEC-006): report the primitive this OS WILL be locked down with, and that v1
    // does NOT yet apply it in-process. `enforced` is false across the board here because the
    // unprivileged-subprocess + sandbox-primitive lockdown is the staged rollout — this PR ships the
    // portable slice (jail + scrubbed env + no network + input-bytes-only). Never claim a lockdown
    // that is not there.
#if defined(__linux__)
    return {"seccomp-bpf", false,
            "Linux-first subprocess lockdown (seccomp-bpf) is the next importer-isolation milestone; "
            "v1 enforces the path jail + scrubbed env + no-ambient-network in-process."};
#elif defined(_WIN32)
    return {"windows-appcontainer", false,
            "Windows AppContainer / restricted Job Object lockdown is a tracked de-risk item; v1 "
            "enforces the path jail + scrubbed env + no-ambient-network in-process."};
#elif defined(__APPLE__)
    return {"macos-sandbox-exec", false,
            "macOS sandbox-exec lockdown is a tracked de-risk item; v1 enforces the path jail + "
            "scrubbed env + no-ambient-network in-process."};
#else
    return {"none", false,
            "No per-OS sandbox primitive is mapped for this platform; v1 enforces the path jail + "
            "scrubbed env + no-ambient-network in-process."};
#endif
}

} // namespace context::editor::import
