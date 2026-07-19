// Daemon discovery for a client-SDK consumer (R-ARCH-005): reading the `<project>/.editor/instance.json`
// hint a running EditorKernel daemon publishes — the endpoint to connect to, the per-instance attach
// token (D20), and the daemon's protocol major.
//
// A client NEVER recomputes the endpoint from the project path: the daemon records the exact bound
// endpoint verbatim, so discovery reads it rather than re-deriving a string that must agree.

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace context::editor::client
{

// The published discovery hint. `token` is the D20 per-instance attach token — carried on attach so
// the daemon can gate the handshake (enforcement default ON since e02); empty when the daemon wrote
// no token (a pre-D20 instance file).
struct InstanceInfo
{
    std::string endpoint;
    std::string token;
    std::uint32_t protocol_major = 0;
    std::int64_t pid = 0;
};

// Parse one instance-document text. nullopt when the text is not valid JSON, is not an object, or
// carries no non-empty string `endpoint` (the one field without which a client cannot proceed). A
// torn/partial read therefore reads as "not ready yet" rather than as a hard failure — which is what
// lets discover_instance() retry the daemon's publish race.
[[nodiscard]] std::optional<InstanceInfo> parse_instance_document(const std::string& text);

// Discover the running daemon for `project`, retrying for the boot race (the daemon may still be
// publishing the file) until `timeout_ms` elapses. nullopt when no usable instance document appears.
[[nodiscard]] std::optional<InstanceInfo> discover_instance(const std::filesystem::path& project,
                                                            int timeout_ms);

} // namespace context::editor::client
