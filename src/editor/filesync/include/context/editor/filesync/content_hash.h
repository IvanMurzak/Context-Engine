// Raw-byte content hash (FNV-1a, 64-bit) — the change-detector for the watch-hash-reconcile pipeline.

#pragma once

#include <cstdint>
#include <string_view>

namespace context::editor::filesync
{

// FNV-1a 64-bit over raw bytes. This is the *raw-byte* content hash the reconcile pipeline
// (R-FILE-002) uses to decide "did this file's bytes change on disk". It is deliberately NOT the
// canonical-content hash the derivation graph keys on (R-FILE-001): canonicalization requires
// parsing each authored file and is the derivation layer's concern, out of scope for file-sync.
[[nodiscard]] std::uint64_t content_hash(std::string_view bytes) noexcept;

} // namespace context::editor::filesync
