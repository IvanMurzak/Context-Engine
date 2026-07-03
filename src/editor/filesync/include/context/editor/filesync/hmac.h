// HMAC-SHA256 (RFC 2104) over the dependency-free SHA-256, for intent-log entry integrity (R-FILE-004).

#pragma once

#include <string>
#include <string_view>

namespace context::editor::filesync
{

// HMAC-SHA256(key, message) rendered as lowercase hex. Used to integrity-stamp each crash-recovery
// intent-log entry: the MAC detects corruption and cross-project / foreign-log replay. It does NOT
// defend against same-user tampering — the key is a per-project secret stored on the same filesystem
// the user already controls (R-FILE-004 / R-SEC-010: the trust boundary is the OS user).
[[nodiscard]] std::string hmac_sha256_hex(std::string_view key, std::string_view message);

} // namespace context::editor::filesync
