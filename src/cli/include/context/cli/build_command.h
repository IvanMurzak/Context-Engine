// `context build --target <t>` — the headless per-agent build verb (M8 task a05, issue #257). Wraps
// the pure context_build orchestrator (src/editor/build/) with the disk IO: read the project manifest
// + scenes, drive verify → toolchain → aot → transcode → pack → link, write the packed artifact, and
// report the R-CLI-008 envelope with the build's generation + artifact pointers. Non-interactive
// (R-CLI-003). Registered in the ONE contract registry so CLI ≡ RPC ≡ MCP ≡ introspection holds by
// construction (R-CLI-009); this header exposes only the contract Envelope, so no build type leaks
// into the CLI's public surface.

#pragma once

#include "context/editor/contract/envelope.h"

#include <map>
#include <string>

namespace context::cli
{

// Run `context build`. Reads --target (required: windows|linux|macos|web), --project (default "."),
// and --out (default <project>/build/<target>.pack). --dry-run (core flag) plans without writing.
[[nodiscard]] editor::contract::Envelope run_build(const std::map<std::string, std::string>& flags);

} // namespace context::cli
