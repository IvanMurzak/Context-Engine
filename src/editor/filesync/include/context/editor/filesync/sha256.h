// SHA-256 (FIPS 180-4) — dependency-free, so the intent-log HMAC (R-FILE-004) needs no third-party
// crypto library (which would trip the deny-by-default license gate).

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace context::editor::filesync
{

using Sha256Digest = std::array<std::uint8_t, 32>;

// One-shot SHA-256 over raw bytes.
[[nodiscard]] Sha256Digest sha256(std::string_view data) noexcept;

// Lowercase-hex rendering of a digest (64 chars).
[[nodiscard]] std::string to_hex(const Sha256Digest& digest);

} // namespace context::editor::filesync
