// The platform export-adapter plan (R-BUILD-001 / R-BUILD-005, M8 task a06): the pure, deterministic
// description of the RUNNABLE artifact the build adapter produces for a (target, flavor) — the shipped
// RuntimeKernel binary + the v1 pack + a launcher + a build manifest, laid out in a documented tarball
// (R-BUILD-005 minimal packaging: NO code-signing on Linux). a05 ended its pipeline at an honest
// adapter STUB (BuildSummary::adapter.supported=false); a06 replaces that stub for the first two real adapters —
// Linux DESKTOP (render subsystem present) and Linux SERVER/headless (render absent, L-5 DCE'd).
//
// This is the PURE half (no filesystem, no clock): plan_adapter describes the artifact, and the
// launcher/manifest are pure text functions of the plan. The CLI (src/cli/build_command.cpp) owns the
// on-disk tarball assembly (via pkg::tar_write) and the R-BUILD-009 smoke launch — mirroring how a05
// keeps the pure orchestrator IO-free and lets the CLI own the pack write.
//
// DETERMINISM (DoD 3): everything the adapter itself produces — the layout, launch.sh, context.build.json
// — is byte-deterministic. The ONE non-reproducible input is the shipped runtime BINARY, whose bytes
// vary with the LTO/DCE final link (non-deterministic linker section ordering). The artifact is
// therefore deterministic MODULO the LTO link: every adapter-produced input is bit-reproducible.

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

// The export-adapter plan for a (target, flavor). supported=false is the honest stub for any target the
// a06 adapter set does not yet cover (only "linux" is covered) — the CLI then reports the stub exactly
// as a05 did, never a faked artifact (R-BUILD-007).
struct AdapterPlan
{
    bool supported = false;
    std::string target;              // the requested build target
    std::string flavor;              // "desktop" | "server" (the a06 flavor axis)
    bool render_present = false;     // desktop links the render subsystem; server omits it (L-5 DCE)
    std::string runtime_binary;      // the shipped host binary name inside bin/ (flavor-specific)
    std::string pack_name;           // the pack file name inside content/
    std::string launcher_name;       // "launch.sh"
    std::string manifest_name;       // "context.build.json"
    std::vector<ArtifactEntry> layout; // the full tarball layout (documented order)
};

// The a06 flavor axis. A desktop build ships the render subsystem; a server/headless build omits it.
inline constexpr const char* kFlavorDesktop = "desktop";
inline constexpr const char* kFlavorServer = "server";

// The shipped host binary name for each flavor (matches the CMake OUTPUT_NAME of the two executables
// src/runtime/host/ builds). Kept in one place so the adapter plan, the launcher, and the manifest
// agree with the actual artifact names.
inline constexpr const char* kRuntimeBinaryDesktop = "context-runtime";
inline constexpr const char* kRuntimeBinaryServer = "context-runtime-server";

// True when `flavor` is one of the two a06 flavors (desktop | server).
[[nodiscard]] bool is_known_flavor(const std::string& flavor);

// Plan the export adapter for (target, flavor). Total + deterministic. supported=true only for the a06
// adapter set (target "linux", flavor desktop|server); any other target/flavor yields supported=false
// (the honest stub). `pack_name` names the content pack inside the tarball (defaults to "game.pack"
// when empty).
[[nodiscard]] AdapterPlan plan_adapter(const std::string& target, const std::string& flavor,
                                       const std::string& pack_name);

// Render the launcher script (launch.sh) — a pure function of the plan. POSIX sh that boots the
// artifact RELATIVE TO ITS OWN DIRECTORY (so it runs from anywhere a tarball is extracted): it execs
// bin/<runtime> --pack content/<pack> and forwards any extra args (e.g. --ticks N).
[[nodiscard]] std::string render_launcher(const AdapterPlan& plan);

// Render the build manifest (context.build.json) — a machine-readable record of the artifact. Pure +
// deterministic (hand-rolled canonical JSON; 64-bit values as decimal strings, the envelope precision
// rule). Empty when the plan is unsupported (no artifact is produced).
[[nodiscard]] std::string render_manifest(const AdapterPlan& plan, std::uint64_t engine_version,
                                          std::uint64_t generation, std::uint64_t pack_hash);

} // namespace context::editor::build
