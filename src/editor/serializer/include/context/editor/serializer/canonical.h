// The canonical-JSON serializer — every authored file kind's ONE canonical form (R-FILE-001, L-32).

#pragma once

#include "context/editor/serializer/json_tree.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace context::editor::serializer
{

// ECMAScript Number::toString for a double (ECMA-262 §6.1.6.1.20 notation rules over the shortest
// round-trip digits): fixed notation for decimal exponents in (-6, 21], exponent notation outside;
// -0 serializes as "0" (R-FILE-001). Appends to `out`. Precondition: `v` is finite (NaN/Infinity
// are banned in schemas — R-FILE-001); serialize_canonical enforces this for whole trees.
// Ported from the spike-ratified writer the M0 fixpoint proof ran (spikes/parse-bench, 3,810 files,
// 0 mismatches vs the Python reference writer in bench/gen_corpus.py).
void ecma_number(double v, std::string& out);

// Serialize `root` in the canonical form and append to `out`: UTF-8, LF, 2-space indent, ": "
// separator, object keys sorted lexicographically by UTF-8 bytes, authored array order preserved
// (stable array ordering), minimal string escaping (control chars, quote, backslash; non-ASCII as
// raw NFC UTF-8), ECMAScript number formatting, and EXACTLY ONE trailing newline — the emitted
// bytes ARE the canonical file bytes. Strings that are not definitively NFC are normalized on the
// way out, so the writer's output is NFC even for programmatically built trees.
// Returns false (leaving `out` untouched) when the tree contains a non-finite double — the ONE
// unrepresentable input (R-FILE-001 bans NaN/Infinity).
[[nodiscard]] bool serialize_canonical(const JsonValue& root, std::string& out);

// FNV-1a 64-bit over bytes — the canonical-content hash function (over canonical bytes). The same
// bit-width and family as filesync's raw-byte content_hash but a DIFFERENT ROLE: this one keys
// derivation/cache identity (R-FILE-001 two-hash split); filesync's keys watch/reconcile + CAS.
[[nodiscard]] std::uint64_t canonical_hash_of(std::string_view bytes) noexcept;

// The byte-level canonicalization every writer/parse-node goes through.
struct CanonicalizeResult
{
    // True when `source` parsed as JSON and `bytes` holds its canonical serialization. False for
    // any non-JSON input (binary sidecars, TS/shader text — the L-32 carve-outs): those have NO
    // canonicalization pass, so `bytes` is the raw input verbatim and the canonical hash EQUALS
    // the raw-byte hash by construction (R-FILE-001: for binaries, raw ≡ canonical).
    bool is_json = false;
    std::string bytes;                   // canonical file bytes (JSON) or the raw input (non-JSON)
    std::uint64_t canonical_hash = 0;    // canonical_hash_of(bytes)
    std::vector<Diagnostic> diagnostics; // non-fatal encoding findings (JSON) or the parse failure
};

// Parse + canonicalize `source`. Deterministic and total (never throws). JSON input yields the
// canonical form (a FIXPOINT: canonicalizing canonical output is byte-identical — property-tested
// per R-FILE-001); non-JSON input passes through raw with the parse diagnostics attached so a
// caller that KNOWS the path should be JSON can surface them (R-FILE-003).
[[nodiscard]] CanonicalizeResult canonicalize(std::string_view source);

// The authored-file header block (L-32): "$schema" + "version" + the per-component-payload
// "componentVersions" map ({"<ns>:<type>": <schemaVersion>}). M2 wave 1 reads and shape-checks
// the header; per-kind schema VALIDATION (and required-header strictness per file kind) is the
// M2 schema-model task layered on top.
struct DocumentHeader
{
    bool has_schema = false;
    std::string schema; // "$schema" value
    bool has_version = false;
    std::int64_t version = 0; // "version" value
    bool has_component_versions = false;
    std::vector<std::pair<std::string, std::int64_t>> component_versions; // authored order
};

// Extract the header fields from a parsed document root. A missing field is NOT a diagnostic
// (header strictness is per-kind, decided by the schema model); a PRESENT field of the wrong
// shape appends one ("header.schema_not_string", "header.version_not_integer",
// "header.component_versions_not_object", "header.component_version_not_integer").
[[nodiscard]] DocumentHeader read_document_header(const JsonValue& root,
                                                  std::vector<Diagnostic>& diagnostics);

} // namespace context::editor::serializer
