// The derivation graph's canonical parse node — the REAL canonical-JSON serializer (M2, R-FILE-001).

#pragma once

#include "context/editor/serializer/json_tree.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace context::editor::derivation
{

// The output of the parse node: a source file's authored bytes reduced to THE canonical form plus
// the canonical-content hash the rest of the derivation graph keys on. This is deliberately
// distinct from filesync's raw-byte content hash (R-FILE-001 two-hash split, L-22): two files that
// differ only in insignificant formatting — whitespace, key order, number notation, non-NFC
// encodings, \u escapes — derive to the SAME canonical form, so downstream nodes memoize past a
// no-op edit.
//
// M2: the body delegates to src/editor/serializer/ (issue #42) — the real canonical-JSON
// serializer that replaced the M1 whitespace-normalizing placeholder BEHIND THIS SAME SEAM (the
// graph mechanics never changed). JSON input canonicalizes fully; non-JSON input (binary sidecars,
// TS/shader text — the L-32 carve-outs) has NO canonicalization pass, so its canonical form is the
// raw bytes and the canonical hash equals the raw-byte hash by construction.
struct CanonicalForm
{
    std::string bytes;                // canonical file bytes (JSON) or the raw bytes (non-JSON)
    std::uint64_t canonical_hash = 0; // canonical-content hash over `bytes` (FNV-1a 64)
    bool is_json = false;             // true when the input parsed + canonicalized as JSON
    // Machine-readable findings from the parse (R-FILE-003 shape). For JSON these are the
    // NON-FATAL encoding heals (encoding.bom / encoding.crlf — fixed in `bytes` already); for
    // non-JSON they carry the parse failure a JSON-expecting caller may surface. The graph itself
    // ignores them in M2 wave 1 (the per-node diagnostics topic is the schema-model task).
    std::vector<serializer::Diagnostic> diagnostics;
};

// Parse + canonicalize `source_bytes`. Deterministic and total (never throws); empty input yields
// an empty non-JSON canonical form with the empty-string hash.
[[nodiscard]] CanonicalForm canonical_parse(std::string_view source_bytes);

// FNV-1a 64-bit over bytes — the canonical-content hash function. Exposed so tests (and the graph)
// can predict a canonical hash without re-deriving. NOT filesync's content_hash (that one is
// raw-byte and lives behind the file-sync layer's own seam); this is the derivation layer's
// canonical hash, computed over CANONICAL bytes.
[[nodiscard]] std::uint64_t canonical_hash_of(std::string_view bytes) noexcept;

} // namespace context::editor::derivation
