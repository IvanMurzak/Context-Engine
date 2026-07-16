// Per-target toolchain manifest consumption (R-PKG-002 / L-42, task a05). The build orchestrator
// resolves the requested build target to its pinned toolchain before it drives a per-target build.
//
// The manifest table here MIRRORS the L-42 source-of-truth `cmake/toolchain-versions.json` (the
// declared, versioned per-target toolchain artifact — its `targets` keys + `compiler` ids), the same
// way pack::PlatformVariant mirrors import::platform_profiles(): two frozen tables that must agree,
// bound by a drift-guard test (test_build_orchestrator parses the real cmake/toolchain-versions.json
// and asserts this embedded table tracks it). The engine binary carries this compiled-in table so a
// headless `context build` needs no engine-source checkout at build time.
//
// A build target id (the CLI `--target` value: windows/linux/macos/web — the import::platform_profiles
// ids) maps to a manifest TRIPLE key (windows-x86_64/linux-x86_64/macos-arm64/web-emscripten). A
// target with no manifest entry cannot have its toolchain fetched (build.toolchain_fetch_failed).

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace context::editor::build
{

// One resolved per-target toolchain (the fields the build consumes from the L-42 manifest).
struct ToolchainEntry
{
    std::string target;     // the manifest triple key, e.g. "linux-x86_64"
    std::string compiler;   // the compiler id, e.g. "clang" / "apple-clang" / "msvc" / "emscripten-clang"
    std::string pin;        // the exact major.minor pin, or empty when the manifest records none
    std::string enforcement;// "strict" | "advisory" | "documented" (L-42 drift-alarm level)
};

// The embedded per-target toolchain manifest (R-PKG-002), mirroring cmake/toolchain-versions.json.
[[nodiscard]] const std::vector<ToolchainEntry>& toolchain_manifest();

// Map a build target id (the CLI --target value: windows/linux/macos/web) to its manifest triple key
// (windows-x86_64/linux-x86_64/macos-arm64/web-emscripten). Empty for an unknown build target id.
[[nodiscard]] std::string toolchain_target_key(std::string_view build_target);

// Resolve a build target id against a toolchain manifest; nullptr when the target maps to no manifest
// entry (an unknown target id, or a triple absent from the manifest). Taking the manifest as a
// parameter keeps the resolve pure + injectable (the R-QA-011 corpus drives an incomplete manifest to
// reach build.toolchain_fetch_failed); the CLI passes the embedded toolchain_manifest().
[[nodiscard]] const ToolchainEntry* resolve_toolchain(const std::vector<ToolchainEntry>& manifest,
                                                      std::string_view build_target);

} // namespace context::editor::build
