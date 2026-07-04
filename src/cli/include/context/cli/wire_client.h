// Shared plumbing for CLI-local operational commands that drive a RUNNING daemon over the IPC wire
// (`context attach`, `context fetch`): flag parsing over raw operational-arg vectors, endpoint
// discovery via `<project>/.editor/instance.json` (R-ARCH-005), and one-shot JSON-RPC 2.0 calls over
// a connected bridge::TransportClient. Extracted from attach_command.cpp so the wire mechanics stay
// single-sourced across the operational client commands.

#pragma once

#include "context/editor/bridge/transport.h"
#include "context/editor/contract/json.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace context::cli
{

// `--name value` / `--name=value` lookup over an operational command's raw arg vector.
[[nodiscard]] std::optional<std::string> flag_value(const std::vector<std::string>& args,
                                                    const std::string& name);

// Presence of a bare `--name` flag.
[[nodiscard]] bool has_flag(const std::vector<std::string>& args, const std::string& name);

// Discover the daemon's endpoint from `<project>/.editor/instance.json`, retrying for the boot race
// (the daemon may still be publishing the file). nullopt if none appears within `timeout_ms`.
[[nodiscard]] std::optional<std::string> discover_endpoint(const std::filesystem::path& project,
                                                           int timeout_ms);

// Build one compact JSON-RPC 2.0 request string.
[[nodiscard]] std::string build_request(std::int64_t id, const std::string& method,
                                        editor::contract::Json params);

// Send one JSON-RPC request and return its `result` object. On a transport failure, a JSON-RPC
// error response, or a malformed reply, returns nullopt and sets `error`. When the failure is a
// daemon-side JSON-RPC error response (as opposed to a transport/parse failure) and
// `rejected_by_daemon` is non-null, sets `*rejected_by_daemon` — letting a handshake call site
// distinguish a genuine protocol rejection from a mere transport hiccup.
[[nodiscard]] std::optional<editor::contract::Json>
call(editor::bridge::TransportClient& client, std::int64_t id, const std::string& method,
     editor::contract::Json params, std::string& error, bool* rejected_by_daemon = nullptr);

} // namespace context::cli
