// The platform export-adapter plan (R-BUILD-001 / R-BUILD-005, M8 tasks a06 + a10): the pure,
// deterministic description of the RUNNABLE artifact the build adapter produces for a (target, flavor) —
// the shipped RuntimeKernel binary + the v1 pack + a launcher + a build manifest, laid out in a
// documented tarball (R-BUILD-005 minimal packaging). a05 ended its pipeline at an honest adapter STUB
// (BuildSummary::adapter.supported=false); a06 replaced that stub for the first two real adapters —
// Linux DESKTOP (render subsystem present) and Linux SERVER/headless (render absent, L-5 DCE'd) — and
// a10 adds the WINDOWS desktop + server/headless adapters (the same flavor axis), whose shipped binaries
// carry a `.exe` suffix, use a `launch.cmd` batch launcher (cmd.exe cannot run the POSIX launch.sh), and
// require Authenticode code-signing to ship (R-SEC-003; the launcher/manifest are still unsigned text —
// only the runtime `.exe` is signed, by the signtool-compatible signing hook, see signing.h).
//
// This is the PURE half (no filesystem, no clock): plan_adapter describes the artifact, and the
// launcher/manifest are pure text functions of the plan. The CLI (src/cli/build_command.cpp) owns the
// on-disk tarball assembly (via pkg::tar_write) and the R-BUILD-009 smoke launch — mirroring how a05
// keeps the pure orchestrator IO-free and lets the CLI own the pack write.
//
// DETERMINISM: everything the adapter itself produces — the layout, the launcher, context.build.json —
// is byte-deterministic. The ONE non-reproducible input is the shipped runtime BINARY, whose bytes vary
// with the LTO/DCE final link (non-deterministic linker section ordering) and, on Windows, the embedded
// Authenticode signature + its RFC-3161 timestamp. The artifact is therefore deterministic MODULO the
// LTO link (+ the signature on Windows): every adapter-produced input is bit-reproducible.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace context::editor::build
{

// One entry in the export artifact's tarball layout (documented order).
struct ArtifactEntry
{
    std::string archive_path; // path inside the tarball, e.g. "bin/context-runtime"
    std::string role;         // "runtime" | "pack" | "launcher" | "manifest"
};

// Which launcher dialect a target uses. POSIX targets (linux) ship a `launch.sh`; Windows ships a
// `launch.cmd` batch file (cmd.exe cannot run a POSIX sh script).
enum class LauncherKind
{
    None,      // an unsupported target — no launcher
    PosixSh,   // launch.sh (linux)
    WindowsCmd // launch.cmd (windows)
};

// The export-adapter plan for a (target, flavor). supported=false is the honest stub for any target the
// a06/a10 adapter set does not yet cover (linux + windows are covered) — the CLI then reports the stub
// exactly as a05 did, never a faked artifact (R-BUILD-007).
struct AdapterPlan
{
    bool supported = false;
    std::string target;              // the requested build target
    std::string flavor;              // "desktop" | "server" (the a06 flavor axis)
    bool render_present = false;     // desktop links the render subsystem; server omits it (L-5 DCE)
    bool requires_signing = false;   // windows requires Authenticode code-signing to ship (R-SEC-003);
                                     // linux does not (a06). See signing.h for the signing hook.
    std::string runtime_binary;      // the shipped host binary name inside bin/ (flavor + OS specific;
                                     // Windows carries a `.exe` suffix)
    std::string pack_name;           // the pack file name inside content/
    LauncherKind launcher_kind = LauncherKind::None;
    std::string launcher_name;       // "launch.sh" (posix) | "launch.cmd" (windows)
    std::string manifest_name;       // "context.build.json"
    std::vector<ArtifactEntry> layout; // the full tarball layout (documented order)
};

// The a06 flavor axis. A desktop build ships the render subsystem; a server/headless build omits it.
inline constexpr const char* kFlavorDesktop = "desktop";
inline constexpr const char* kFlavorServer = "server";

// The a06/a10 export targets that plan a real adapter (any other target is the honest stub).
inline constexpr const char* kTargetLinux = "linux";
inline constexpr const char* kTargetWindows = "windows";

// The shipped host binary BASE name for each flavor (matches the CMake OUTPUT_NAME of the two
// executables src/runtime/host/ builds). Kept in one place so the adapter plan, the launcher, and the
// manifest agree with the actual artifact names. Windows appends `.exe` (kExeSuffixWindows) to these.
inline constexpr const char* kRuntimeBinaryDesktop = "context-runtime";
inline constexpr const char* kRuntimeBinaryServer = "context-runtime-server";
inline constexpr const char* kExeSuffixWindows = ".exe";

// True when `flavor` is one of the two a06 flavors (desktop | server).
[[nodiscard]] bool is_known_flavor(const std::string& flavor);

// Plan the export adapter for (target, flavor). Total + deterministic. supported=true only for the
// a06/a10 adapter set (target "linux" | "windows", flavor desktop|server); any other target/flavor
// yields supported=false (the honest stub). `pack_name` names the content pack inside the tarball
// (defaults to "game.pack" when empty).
[[nodiscard]] AdapterPlan plan_adapter(const std::string& target, const std::string& flavor,
                                       const std::string& pack_name);

// Render the launcher script — a pure function of the plan. For a POSIX target a `#!/bin/sh` launch.sh;
// for a Windows target a `@echo off` launch.cmd batch file. Both boot the artifact RELATIVE TO THE
// LAUNCHER'S OWN DIRECTORY (so it runs from anywhere a tarball is extracted): they exec
// bin/<runtime> --pack content/<pack> and forward any extra args (e.g. --ticks N). Empty for an
// unsupported plan.
[[nodiscard]] std::string render_launcher(const AdapterPlan& plan);

// Render the build manifest (context.build.json) — a machine-readable record of the artifact. Pure +
// deterministic (hand-rolled canonical JSON; 64-bit values as decimal strings, the envelope precision
// rule). Empty when the plan is unsupported (no artifact is produced).
[[nodiscard]] std::string render_manifest(const AdapterPlan& plan, std::uint64_t engine_version,
                                          std::uint64_t generation, std::uint64_t pack_hash);

} // namespace context::editor::build
