// Unicode NFC normalization with an ASCII quick-check fast path (R-FILE-001).

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace context::editor::serializer
{

// True when every byte is ASCII (< 0x80). ASCII text is NFC by construction, so this is THE fast
// path R-FILE-001 mandates: the overwhelming majority of authored content (keys, ids, refs, most
// values) never pays a table lookup. The cost split is measured by the serializer-nfc-bench ctest
// and published in this module's README.
[[nodiscard]] bool is_ascii(std::string_view text) noexcept;

// UAX #15 quick check, conservative: true means `text` is DEFINITELY NFC (every codepoint has
// quick-check YES and combining classes are canonically ordered); false means it needs the full
// normalization pass (which is a byte-identical no-op if the text was NFC after all — the
// conservative answer costs time, never correctness). Precondition: valid UTF-8.
[[nodiscard]] bool is_nfc_quick(std::string_view text) noexcept;

// Normalize `text` to NFC. Returns std::nullopt when the text is already definitively NFC (the
// common case — no allocation, callers keep the original), otherwise the normalized string.
// Precondition: valid UTF-8 (the JSON parser validates before calling).
[[nodiscard]] std::optional<std::string> normalize_nfc(std::string_view text);

// The Unicode version the composition/decomposition tables were generated from (nfc_tables.inc).
[[nodiscard]] const char* nfc_unicode_version() noexcept;

} // namespace context::editor::serializer
