// Binary-sidecar reference extraction over the parsed document tree (L-33, R-FILE-001).

#pragma once

#include "context/editor/serializer/json_tree.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace context::editor::serializer
{

// One WELL-FORMED binary-sidecar reference as authored inside a JSON document (L-33):
//
//   {"$sidecar": "<relpath>", "hash": "<raw-byte hash>"}
//
// `relpath` is owner-relative and `hash` is the sidecar file's whole-file raw-byte hash carried as
// a DECIMAL STRING (a full-range 64-bit hash exceeds 2^53, so a JSON number cannot round-trip it —
// the same convention the RPC surface uses for rawHash/canonicalHash). Path resolution, the
// R-SEC-008 jail, and on-disk verification are the file-sync layer's job (filesync/sidecar.h);
// THIS layer only reads the authored shape out of the tree.
struct SidecarRef
{
    std::string relpath;      // the "$sidecar" value, exactly as authored (owner-relative)
    std::uint64_t hash = 0;   // the declared raw-byte hash (parsed from the decimal string)
    std::string json_pointer; // RFC 6901 pointer to the reference object in the owner document
};

// Strict canonical-decimal u64 parse for the "hash" member: digits only, no sign, no leading zero
// (except "0" itself), no overflow. Exactly the strings std::to_string(std::uint64_t) produces —
// one canonical encoding per value, so tool saves converge (R-FILE-001 spirit). nullopt otherwise.
[[nodiscard]] std::optional<std::uint64_t> parse_hash_string(std::string_view text);

// True iff `value` is a WELL-FORMED sidecar reference object: "$sidecar" is a non-empty string AND
// "hash" is a string parse_hash_string accepts. Additional members are tolerated (forward-compatible
// with schema-blessed extensions); the two required members must be exactly right.
[[nodiscard]] bool is_sidecar_ref(const JsonValue& value);

// Walk `root` and collect every well-formed sidecar reference, depth-first in authored order.
// An object carrying a "$sidecar" member declares REFERENCE INTENT: it is classified as either a
// well-formed ref (collected) or a malformed one (a "sidecar.ref_malformed" diagnostic naming the
// JSON pointer and the reason — R-FILE-003 shape), and is treated as a LEAF either way (its members
// are the reference's payload, never authored content to descend into). Deterministic and total.
[[nodiscard]] std::vector<SidecarRef> collect_sidecar_refs(const JsonValue& root,
                                                           std::vector<Diagnostic>& diagnostics);

} // namespace context::editor::serializer
