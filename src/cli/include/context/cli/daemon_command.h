// `context daemon` — the real EditorKernel daemon PROCESS entrypoint (R-BRIDGE-001 / R-ARCH-005 /
// L-26). A CLI-local operational command (like `context editor smoke`), NOT a contract-registry verb.
//
//   context daemon --project <dir> [--out <file>] [--launch-scopes <spec>]
//
// boots a composed EditorKernel over the NATIVE on-disk FileStore rooted at <dir>, takes the atomic
// single-instance lock (a try-lock FAILURE is the "an instance is already live → attach" signal,
// R-BRIDGE-001), records `.editor/instance.json` (the R-ARCH-005 discovery hint: endpoint + pid), and
// serves the IPC transport until a `shutdown` verb (or the process is killed). A separate `context
// attach` process drives it over the wire.

#pragma once

#include <string>
#include <vector>

namespace context::cli
{

// Process exit code returned when a second write-capable launch detects the single-instance lock and
// declines to boot (the R-BRIDGE-001 attach signal) — distinct from a clean shutdown (0) and from a
// boot/listen error (the failure envelope's exit code).
inline constexpr int kDaemonAttachSignalExit = 3;

// Boot + serve the daemon. Returns the process exit code: 0 on a clean shutdown, kDaemonAttachSignalExit
// on the attach signal, else the boot/listen failure envelope's exit code. Manages its own stdout (a
// long-running server does not fit the one-shot envelope-print path).
[[nodiscard]] int run_daemon(const std::vector<std::string>& args);

} // namespace context::cli
