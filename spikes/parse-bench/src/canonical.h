// parse-bench spike — canonical serializer (R-FILE-001 / L-32).
//
// Canonical form: UTF-8, LF, 2-space indent, ": " separator, object keys sorted
// lexicographically by UTF-8 bytes, ECMAScript shortest-round-trip number formatting
// (-0 -> "0"; NaN/Infinity banned), strings escaped minimally (control chars, quote,
// backslash), non-ASCII passes through as raw UTF-8 (the M0 corpus is NFC by
// construction — full NFC verification is flagged in FINDINGS.md as unmeasured).
#pragma once

#include <string>

#include "json_value.h"

namespace ctx {

// ECMAScript Number::toString for a double (ECMA-262 §6.1.6.1.20 notation rules,
// shortest-round-trip digits). Appends to `out`.
void ecmaNumber(double v, std::string& out);

// Serialize `v` in canonical form (no trailing newline; callers append '\n' —
// canonical files end with exactly one).
void canonicalWrite(const JsonValue& v, std::string& out);

}  // namespace ctx
