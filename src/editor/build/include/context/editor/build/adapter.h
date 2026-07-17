// The platform export-adapter plan (R-BUILD-001 / R-BUILD-005, M8 tasks a06 + a10 + a13): the pure,
// deterministic description of the RUNNABLE artifact the build adapter produces for a (target, flavor) —
// the shipped RuntimeKernel binary + the v1 pack + a launcher + a build manifest, laid out in a
// documented tarball (R-BUILD-005 minimal packaging). a05 ended its pipeline at an honest adapter STUB
// (BuildSummary::adapter.supported=false); a06 replaced that stub for the first two real adapters —
// Linux DESKTOP (render subsystem present) and Linux SERVER/headless (render absent, L-5 DCE'd) — and
// a10 adds the WINDOWS desktop + server/headless adapters (the same flavor axis), whose shipped binaries
// carry a `.exe` suffix, use a `launch.cmd` batch launcher (cmd.exe cannot run the POSIX launch.sh), and
// require Authenticode code-signing to ship (R-SEC-003; the launcher/manifest are still unsigned text —
// only the runtime `.exe` is signed, by the signtool-compatible signing hook, see signing.h).
// a13 adds the MACOS desktop + server/headless adapters — POSIX like Linux (no `.exe` suffix, a
// `launch.sh` launcher), but like Windows they REQUIRE code-signing to ship: a Developer-ID signature +
// Apple notarization + stapled ticket (R-SEC-003; macOS 15+ Gatekeeper hard-blocks an un-notarized
// build). Only the runtime binary is signed/notarized; the launcher/manifest stay unsigned text.
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
    std::string role;         // one of the kRole* tokens: "runtime" | "runtime-loader" (web) |
                              // "pack" | "launcher" | "manifest"
};

// Which launcher dialect a target uses. POSIX targets (linux) ship a `launch.sh`; Windows ships a
// `launch.cmd` batch file (cmd.exe cannot run a POSIX sh script); the web target (a11) ships an
// `index.html` shell that boots the Emscripten/emdawnwebgpu RuntimeKernel over the browser's WebGPU
// and streams the v1 pack by HTTP range requests (a static-file "launcher", not an executable script).
enum class LauncherKind
{
    None,       // an unsupported target — no launcher
    PosixSh,    // launch.sh (linux)
    WindowsCmd, // launch.cmd (windows)
    WebHtml     // index.html (web — the Emscripten export shell)
};

// The export-adapter plan for a (target, flavor). supported=false is the honest stub for any target the
// a06/a10/a11/a13 adapter set does not yet cover (linux + windows + macos + web are all covered now) —
// the CLI then reports the stub exactly as a05 did, never a faked artifact (R-BUILD-007). The web target
// is covered only in the desktop flavor (a headless/server web build is nonsensical — the browser is
// inherently render-present); web + server plans the honest stub.
struct AdapterPlan
{
    bool supported = false;
    std::string target;              // the requested build target
    std::string flavor;              // "desktop" | "server" (the a06 flavor axis)
    bool render_present = false;     // desktop links the render subsystem; server omits it (L-5 DCE)
    bool requires_signing = false;   // windows (Authenticode) + macos (Developer ID + notarization)
                                     // require code-signing to ship (R-SEC-003); linux/web do not (a06).
                                     // See signing.h for the per-platform signing hook.
    std::string runtime_binary;      // the shipped host binary name inside bin/ (flavor + OS specific;
                                     // Windows carries a `.exe` suffix; web ships the `.wasm` module)
    std::string runtime_loader;      // the web target's Emscripten JS glue inside bin/ (empty for the
                                     // native linux/windows targets, whose runtime is a single binary)
    std::string pack_name;           // the pack file name inside content/
    LauncherKind launcher_kind = LauncherKind::None;
    std::string launcher_name;       // "launch.sh" (posix) | "launch.cmd" (windows)
    std::string manifest_name;       // "context.build.json"
    std::vector<ArtifactEntry> layout; // the full tarball layout (documented order)
};

// The a06 flavor axis. A desktop build ships the render subsystem; a server/headless build omits it.
inline constexpr const char* kFlavorDesktop = "desktop";
inline constexpr const char* kFlavorServer = "server";

// The a06/a10/a11/a13 export targets that plan a real adapter (any other target is the honest stub).
inline constexpr const char* kTargetLinux = "linux";
inline constexpr const char* kTargetWindows = "windows";
inline constexpr const char* kTargetMacos = "macos";
inline constexpr const char* kTargetWeb = "web";

// The shipped host binary BASE name for each flavor (matches the CMake OUTPUT_NAME of the two
// executables src/runtime/host/ builds). Kept in one place so the adapter plan, the launcher, and the
// manifest agree with the actual artifact names. Windows appends `.exe` (kExeSuffixWindows) to these.
inline constexpr const char* kRuntimeBinaryDesktop = "context-runtime";
inline constexpr const char* kRuntimeBinaryServer = "context-runtime-server";
inline constexpr const char* kExeSuffixWindows = ".exe";

// The web (a11) artifact names. The runtime is the Emscripten pair: a `.wasm` module + its `.js` glue
// (both under bin/); the launcher is an `index.html` shell that boots the module over the browser's
// WebGPU and streams content/<pack> by HTTP range requests. Web is WebGPU-only (render always present)
// and never desktop/server-flavored — a headless web build is nonsensical.
inline constexpr const char* kRuntimeBinaryWeb = "context-runtime.wasm";
inline constexpr const char* kRuntimeLoaderWeb = "context-runtime.js";
inline constexpr const char* kWebLauncherName = "index.html";

// The artifact-layout ROLE tokens — the contract between the plan producer (plan_adapter, which tags
// each ArtifactEntry with a role) and the CLI artifact assembler (build_command.cpp emit_artifact,
// which resolves each entry's bytes by matching its role). Centralized here — beside the flavor/target/
// name tokens — so a rename or typo can't silently drift the two modules (an unknown role is a
// fail-closed emit refusal at runtime, not a compile error). "runtime-loader" is web-only (the
// Emscripten .js glue paired with the .wasm runtime).
inline constexpr const char* kRoleRuntime = "runtime";
inline constexpr const char* kRoleRuntimeLoader = "runtime-loader";
inline constexpr const char* kRolePack = "pack";
inline constexpr const char* kRoleLauncher = "launcher";
inline constexpr const char* kRoleManifest = "manifest";

// True when `flavor` is one of the two a06 flavors (desktop | server).
[[nodiscard]] bool is_known_flavor(const std::string& flavor);

// Plan the export adapter for (target, flavor). Total + deterministic. supported=true only for the
// a06/a10/a11/a13 adapter set (target "linux" | "windows" | "macos" in flavor desktop|server; target
// "web" in the desktop flavor only); any other target/flavor yields supported=false (the honest stub).
// `pack_name` names the content pack inside the artifact (defaults to "game.pack" when empty).
[[nodiscard]] AdapterPlan plan_adapter(const std::string& target, const std::string& flavor,
                                       const std::string& pack_name);

// Render the launcher — a pure function of the plan. For a POSIX target a `#!/bin/sh` launch.sh; for a
// Windows target a `@echo off` launch.cmd batch file; for the web target an `index.html` shell. The
// native launchers boot the artifact RELATIVE TO THE LAUNCHER'S OWN DIRECTORY (so it runs from anywhere
// a tarball is extracted): they exec bin/<runtime> --pack content/<pack> and forward any extra args
// (e.g. --ticks N). The web `index.html` loads bin/<runtime-loader> (the Emscripten JS glue) with the
// canvas + the `--pack content/<pack>` argument, all page-relative so it serves from any static host.
// Empty for an unsupported plan.
[[nodiscard]] std::string render_launcher(const AdapterPlan& plan);

// Render the build manifest (context.build.json) — a machine-readable record of the artifact. Pure +
// deterministic (hand-rolled canonical JSON; 64-bit values as decimal strings, the envelope precision
// rule). Empty when the plan is unsupported (no artifact is produced).
[[nodiscard]] std::string render_manifest(const AdapterPlan& plan, std::uint64_t engine_version,
                                          std::uint64_t generation, std::uint64_t pack_hash);

} // namespace context::editor::build
