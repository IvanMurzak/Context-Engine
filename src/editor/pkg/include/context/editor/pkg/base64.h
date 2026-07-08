// Standard (RFC 4648) base64 — dependency-free. Subresource-Integrity digests (R-SEC-005) are
// carried as `<alg>-<base64>`, so the install path decodes the base64 payload to compare raw digest
// bytes. Kept stdlib-only for the same deny-by-default-license reason as sha512.h.

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace context::editor::pkg
{

// Encode raw bytes to standard base64 (with `=` padding).
[[nodiscard]] std::string base64_encode(std::string_view bytes);

// STRICT standard-base64 decode: returns std::nullopt on any invalid character, bad padding, or a
// length that is not a multiple of 4 (fail-closed — a malformed integrity string must never
// silently decode to attacker-chosen bytes). Whitespace is NOT tolerated.
[[nodiscard]] std::optional<std::string> base64_decode(std::string_view text);

} // namespace context::editor::pkg
