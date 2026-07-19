// Operational-argument parsing shared across the CLI's commands: `--name value` / `--name=value`
// lookup, bare-flag presence, and the strict decimal-u64 parse operational knobs use.
//
// This is ARGUMENT parsing only. The wire itself — endpoint discovery, the attach handshake,
// JSON-RPC calls, the subscription consumer — lives in context_client (src/editor/client/), the ONE
// client-side implementation the CLI, the editor shell, and every out-of-tree consumer share. This
// header was `wire_client.h` until M9 e02 moved that plumbing out; what remained is CLI-local
// argv handling, which no client SDK has any business carrying.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace context::cli
{

// `--name value` / `--name=value` lookup over an operational command's raw arg vector.
[[nodiscard]] std::optional<std::string> flag_value(const std::vector<std::string>& args,
                                                    const std::string& name);

// Presence of a bare `--name` flag.
[[nodiscard]] bool has_flag(const std::vector<std::string>& args, const std::string& name);

// Strict non-negative decimal parse for operational flag VALUES: all digits, overflow-checked.
// nullopt on anything else. Deliberately NOT std::stoull, which accepts a leading '-' (silently
// wrapping "-1" to a huge value) and ignores trailing junk ("600abc" -> 600) — an operational knob
// that misparses silently can turn a safety net off with no error.
[[nodiscard]] std::optional<std::uint64_t> parse_u64(std::string_view text);

} // namespace context::cli
