// Strict JSON parser for authored files — the parse half of the canonical round-trip (R-FILE-001).

#pragma once

#include "context/editor/serializer/json_tree.h"

#include <string_view>
#include <vector>

namespace context::editor::serializer
{

// The outcome of parsing one authored document.
//
// `ok == true`  — `root` holds the tree; `diagnostics` may still carry NON-FATAL findings the
//                 canonical rewrite heals on this save (R-FILE-003 eventually-valid, L-34 spirit):
//                 "encoding.bom" (a UTF-8 BOM was stripped) and "encoding.crlf" (CR/CRLF line
//                 endings found — authored files are LF, R-FILE-001).
// `ok == false` — `root` is unspecified; `diagnostics` carries at least one FATAL finding with a
//                 1-based line/column into the source bytes.
struct ParseResult
{
    bool ok = false;
    JsonValue root;
    std::vector<Diagnostic> diagnostics;
};

// Parse `source` as one strict RFC 8259 JSON document. Deterministic and total (never throws).
//
// Strictness beyond the RFC, per the canonical-form contract:
//   - object keys must be UNIQUE ("json.duplicate_key") — duplicate keys have no well-defined
//     canonical order and break R-FILE-012 merge identity;
//   - the document must be valid UTF-8 ("encoding.invalid_utf8"); \u escapes must not encode
//     unpaired surrogates ("json.invalid_escape");
//   - NaN/Infinity have no literal (R-FILE-001 bans them in schemas): they are unparseable here
//     by grammar ("json.unexpected_token");
//   - nesting depth is capped ("json.depth_exceeded") so adversarial input cannot exhaust the
//     stack (the L-33 file-granularity ceiling keeps real documents far below the cap);
//   - exactly one root value; trailing non-whitespace is "json.trailing_content".
//
// Number typing (spike-ratified, see json_tree.h): an integer literal (no fraction/exponent) in
// i64/u64 range parses losslessly; every other number parses as an ECMAScript double, including
// integer literals BEYOND u64 range (pure ECMAScript number semantics for the overflow case —
// the committed test-vector corpus pins this, R-QA-011).
//
// Strings (values AND keys) are unescaped and NFC-normalized (R-FILE-001) before landing in the
// tree, so the canonical writer emits NFC bytes without re-scanning.
[[nodiscard]] ParseResult parse_json(std::string_view source);

// Maximum nesting depth `parse_json` accepts (arrays + objects combined).
inline constexpr std::size_t kMaxParseDepth = 256;

} // namespace context::editor::serializer
