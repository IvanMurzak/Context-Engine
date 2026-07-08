// SHA-512 (FIPS 180-4) — dependency-free, mirroring editor/filesync's sha256.h. The R-SEC-005
// engine-driven install path verifies npm Subresource-Integrity (SRI) hashes, whose default
// algorithm is `sha512-<base64>`; a third-party crypto library would trip the deny-by-default
// license gate (the same trade filesync made for its intent-log HMAC), so the install tier ships
// this small stdlib-only digest instead.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace context::editor::pkg
{

using Sha512Digest = std::array<std::uint8_t, 64>;

// One-shot SHA-512 over raw bytes.
[[nodiscard]] Sha512Digest sha512(std::string_view data) noexcept;

// Lowercase-hex rendering of a digest (128 chars).
[[nodiscard]] std::string to_hex(const Sha512Digest& digest);

} // namespace context::editor::pkg
