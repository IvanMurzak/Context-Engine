// Minimal M1 placeholder for the canonical parse step — the real canonical-JSON serializer is M2.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace context::editor::derivation
{

// The output of the parse node: a source file's authored bytes reduced to a canonical form plus the
// canonical-content hash the rest of the derivation graph keys on. This is deliberately distinct from
// filesync's raw-byte content hash (R-FILE-001, L-22): two files that differ only in insignificant
// formatting derive to the SAME canonical form, so downstream nodes memoize past a no-op edit.
//
// M1 SCOPE: the "canonicalization" here is a trivial whitespace normalization standing in for the M2
// canonical-JSON serializer + schema model. The GRAPH mechanics (dependency tracking, incremental
// re-derive, backpressure, generation counter, read-your-writes barrier) are the M1 deliverable; this
// node is intentionally a placeholder with a stable seam so M2 can replace its body without touching
// the graph.
struct CanonicalForm
{
    std::string bytes;                // normalized bytes (M2: the canonical-JSON serialization)
    std::uint64_t canonical_hash = 0; // canonical-content hash over `bytes` (FNV-1a 64, placeholder)
};

// Parse + canonicalize `source_bytes`. M1 normalization: trim leading/trailing ASCII whitespace and
// collapse every internal run of ASCII whitespace to a single space. Deterministic and total (never
// throws); empty / whitespace-only input yields an empty canonical form with the empty-string hash.
[[nodiscard]] CanonicalForm canonical_parse(std::string_view source_bytes);

// FNV-1a 64-bit over raw bytes — the placeholder canonical hash. Exposed so tests (and the graph) can
// predict a canonical hash without re-deriving. NOT filesync's content_hash (that one is raw-byte and
// lives behind the file-sync layer's own seam); this is the derivation layer's canonical hash.
[[nodiscard]] std::uint64_t canonical_hash_of(std::string_view bytes) noexcept;

} // namespace context::editor::derivation
