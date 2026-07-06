// Query language: the ONE specified query surface (R-CLI-012), part of the versioned contract.
//
// R-CLI-006 pins the query surface's existence + provenance/generation semantics; THIS module pins
// the *language*: a published EBNF grammar, an enumerated operator set (equality / range / boolean /
// existence / string-match), mandatory total result ordering (a stable sort key defaulting to the
// entity id, so paginated results never reorder), a cursor-pagination contract UNIFIED with the
// R-BRIDGE-008 event cursor (one cursor model across event catch-up and query paging — never a
// second cursor shape), and defined string semantics (NFC normalization + explicit case handling).
//
// The SAME predicate/projection language is used across the three query surfaces — the derived world,
// live-sim state, and schema introspection — so an agent reuses one grammar everywhere (R-CLI-012:
// "not three dialects"). This module is the language SPEC + parser/AST + comparators + cursor codec;
// EXECUTING a parsed query over a live derived world is the (query, executor) split (R-LANG-009),
// which lands with the ECS/spatial-index it runs against. Because the language is a pure projection
// of the ONE registry, `context describe --json` publishes it (query_language_descriptor()) and the
// conformance test locks the published grammar to the parser the engine actually runs.
//
// protocolMajor stays 0 (the contract is UNSTABLE until the M3 freeze); every addition here is
// additive-only.

#pragma once

#include "context/editor/contract/json.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace context::editor::contract
{

// --- the enumerated operator set (R-CLI-012) ------------------------------------------------------
// Grouped into the five requirement classes. Boolean composition (and / or / not) is STRUCTURAL in
// the AST (LogicalAnd / LogicalOr / LogicalNot nodes), not a CompareOp — it composes predicates
// rather than comparing a field to a literal.
enum class CompareOp
{
    // equality
    eq, // ==
    ne, // !=
    // range (ordered comparison; numbers compare numerically, strings by NFC code-point order)
    lt, // <
    le, // <=
    gt, // >
    ge, // >=
    // existence — has(field): the field path resolves to a present (non-absent) value
    has,
    // string-match (the RHS is always a string literal; each has a case-insensitive i-variant)
    contains,    // contains(field, "s")     / icontains
    starts_with, // startswith(field, "s")   / istartswith
    ends_with,   // endswith(field, "s")     / iendswith
    matches,     // matches(field, "glob")   / imatches  — '*' and '?' glob wildcards
};

// The operator's stable contract token (the symbol/function name published in the grammar). Total
// over the enum (no default arm) so a new operator forces a token here — the parser and the
// descriptor read the SAME spelling.
[[nodiscard]] std::string_view compare_op_token(CompareOp op);

// The requirement class an operator belongs to (for the enumerated-set introspection). One of
// "equality" | "range" | "existence" | "string-match".
[[nodiscard]] std::string_view compare_op_class(CompareOp op);

// --- the predicate AST ----------------------------------------------------------------------------
enum class NodeKind
{
    comparison,  // a leaf: field <op> literal (or has(field))
    logical_and, // children all hold (>= 1 child)
    logical_or,  // any child holds (>= 1 child)
    logical_not, // exactly one child; negates it
};

// One node of a parsed predicate tree. A comparison leaf carries {field, op, value, ci}; a composite
// carries `children`. Value-copyable (no owning pointers) so tests compare trees by value.
struct Predicate
{
    NodeKind kind = NodeKind::comparison;

    // comparison-leaf payload (valid when kind == comparison):
    std::string field;            // the dotted/indexed field path, e.g. "transform.position.x"
    CompareOp op = CompareOp::eq; // the operator
    Json value;                   // the RHS literal (null for `has`)
    bool case_insensitive = false; // set by the i-variant string-match forms

    // composite children (and/or: >= 1; not: exactly 1):
    std::vector<Predicate> children;
};

// The outcome of parse_predicate(): on success `ok` and `predicate`; on failure a catalog error code
// + human message + the byte offset in the input where parsing stopped (for a pointer-style
// diagnostic). error_code is one of query.syntax_error / query.unknown_operator (R-CLI-012).
struct PredicateParse
{
    bool ok = false;
    Predicate predicate;
    std::string error_code;
    std::string message;
    std::size_t error_offset = 0;
};

// Parse a where-clause predicate expression against the R-CLI-012 grammar (see query_language.cpp for
// the productions; query-language.md + query_language_descriptor() publish the EBNF). Strict: a
// client-supplied query is untrusted input, so anything off-grammar fails with a code, never a
// best-effort partial parse. An empty/whitespace-only input parses to the always-true predicate
// (LogicalAnd with no children) — "match everything", the natural identity.
[[nodiscard]] PredicateParse parse_predicate(std::string_view input);

// --- mandatory total result ordering (R-CLI-012) --------------------------------------------------
// A single order term: a sort key (a field path, or the reserved entity-id token) + direction.
struct OrderTerm
{
    std::string key;         // a field path, or kEntityIdKey ("@id")
    bool descending = false; // ascending by default
};

// The reserved total-order key: the entity id. Ordering ALWAYS ends with an "@id asc" tiebreaker
// (appended by parse_order when absent) so the sort is TOTAL — two rows can never compare equal, so a
// paginated sequence never reorders across pages (the R-CLI-012 guarantee).
inline constexpr std::string_view kEntityIdKey = "@id";

struct OrderParse
{
    bool ok = false;
    std::vector<OrderTerm> terms; // guaranteed non-empty + guaranteed to end with the @id tiebreaker
    std::string error_code;
    std::string message;
    std::size_t error_offset = 0;
};

// Parse a comma-separated order-by clause ("transform.position.x desc, name asc"). An empty clause
// yields the default total order [ {@id, asc} ]. Always guarantees a total order by appending an
// "@id asc" final tiebreaker when the caller did not already sort by @id.
[[nodiscard]] OrderParse parse_order(std::string_view input);

// Compare two result rows under an order spec. Each row is a JSON object; a term's key is looked up
// as a field path (kEntityIdKey reads the row's "@id" member). Returns <0 / 0 / >0. Because a total
// order ends in @id and @id is unique, a well-formed row pair never returns 0 — the property the
// pagination-stability test asserts. Missing keys sort LAST (stable, defined) regardless of direction.
[[nodiscard]] int compare_rows(const Json& lhs, const Json& rhs, const std::vector<OrderTerm>& order);

// --- unified cursor (R-CLI-012 unified with R-BRIDGE-008) -----------------------------------------
// ONE cursor model spans event catch-up and query paging. The {incarnation_id, seq} pair is the
// SAME identity the R-BRIDGE-008 event stream resumes on ("since seq N", scoped to a daemon
// incarnation — a restart invalidates it by construction). For QUERY paging the cursor additionally
// stamps the `generation` the page is consistent to (so all pages of one paged query read a single
// derived-world generation) and carries `after_id` — the last-returned entity id under the total
// order — for keyset pagination (stable under concurrent mutation: the next page starts strictly
// after the last id, so inserts/deletes never shift the window). An event cursor is exactly a query
// cursor with an empty after_id.
struct QueryCursor
{
    std::string incarnation_id;     // the daemon incarnation (epoch identity; restart => re-snapshot)
    std::uint64_t seq = 0;          // the R-BRIDGE-008 monotonic, totally-ordered event seq
    std::uint64_t generation = 0;   // the derived-world generation this page is consistent to
    std::string after_id;           // keyset position: the last entity id returned (empty for events)

    // Serialize as the opaque cursor URI: "context-cur://v0/<incarnation>/<generation>/<seq>?after=<hex>".
    // Deterministic; parse() round-trips it exactly. after_id is lowercase-hex-encoded so the token
    // stays in a URI-clean charset for any entity-id bytes (treat the whole string as opaque).
    [[nodiscard]] std::string to_token() const;

    // Parse an opaque cursor token. nullopt on anything malformed (wrong scheme/version, a bad
    // incarnation charset, non-numeric seq/generation, or a non-hex after) — a client-supplied cursor
    // is untrusted, so parsing is strict rather than forgiving (mirrors ResourceHandle::parse).
    [[nodiscard]] static std::optional<QueryCursor> parse(std::string_view token);
};

// The URI scheme every cursor token uses (sibling of kResourceUriScheme; "one large-result contract"
// — a handle for one oversized payload, a cursor for a paged/streamed sequence, R-CLI-017).
inline constexpr std::string_view kCursorUriScheme = "context-cur";

// --- defined string semantics (R-CLI-012: NFC normalization + explicit case handling) -------------
// Strict UTF-8 validation: true iff `text` is well-formed UTF-8 (no overlong forms, no surrogates, no
// truncated sequences). Untrusted query literals are validated before any string operation.
[[nodiscard]] bool is_valid_utf8(std::string_view text);

// ASCII case-fold: lowercases A–Z, leaves every other byte untouched. This is the v1 case-handling
// primitive backing the i-variant string-match operators. Non-ASCII case-folding (full Unicode) is a
// bounded, DEFINED limitation published in the descriptor — see query_language_descriptor().
[[nodiscard]] std::string ascii_case_fold(std::string_view text);

// NFC normalization (R-CLI-012 string semantics). CONTRACT: comparisons operate on NFC-normalized
// values so canonically-equivalent spellings compare equal. v1 SCOPE (published honestly in the
// descriptor): ASCII is invariant under NFC and passes through unchanged; a non-ASCII input is
// validated as UTF-8 and assumed already NFC (returned unchanged). The full canonical
// decomposition+composition pass over non-ASCII is the tracked follow-up; the SEMANTICS are contract
// now, so activating the full table later is non-breaking. Returns nullopt on invalid UTF-8.
[[nodiscard]] std::optional<std::string> normalize_nfc(std::string_view text);

// Apply the R-CLI-012 string-match semantics: normalize both operands (NFC), optionally ASCII
// case-fold (when case_insensitive), then run the operator. `op` MUST be a string-match operator
// (contains/starts_with/ends_with/matches); false for any other op. Invalid-UTF-8 operands never
// match (they are rejected upstream at parse, but this stays defensive).
[[nodiscard]] bool string_match(CompareOp op, std::string_view field_value, std::string_view pattern,
                                bool case_insensitive);

// --- describe integration (R-CLI-013) -------------------------------------------------------------
// The whole-language self-description embedded by Registry::describe() as contract.queryLanguage:
// the EBNF grammar, the enumerated operator set (token + class per op), the ordering rule, the
// unified-cursor shape + its R-BRIDGE-008 reconciliation, the string semantics + their v1 scope, and
// the three surfaces the one language spans. Pure — a projection of this module's own constants.
[[nodiscard]] Json query_language_descriptor();

// The published EBNF grammar string (also written to docs/query-language.md). Exposed so the
// descriptor and the conformance test read the ONE spelling.
[[nodiscard]] std::string_view query_ebnf();

} // namespace context::editor::contract
