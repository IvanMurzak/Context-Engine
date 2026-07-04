// The parsed-JSON document tree the canonical serializer round-trips through (R-FILE-001 / L-32).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace context::editor::serializer
{

struct JsonMember;

// One JSON value. A deliberately plain, dependency-free DOM: the canonical serializer is the
// foundation layer every other M2 data-model component builds on, so it must not drag a third-party
// JSON library (vcpkg dep + license-allowlist entry) into the default build. Distinct from
// contract::Json (the R-CLI-008 envelope DOM, insertion-ordered for `describe` golden stability):
// THIS tree pins the exact number domain the canonical form needs — integer literals are preserved
// losslessly as i64/u64 (the spike-ratified behavior the M0 fixpoint proof binds, R-FILE-001) while
// everything else is an ECMAScript double.
struct JsonValue
{
    enum class Type
    {
        null_value,
        boolean,
        integer,          // literal without fraction/exponent, in i64 range (lossless)
        unsigned_integer, // literal without fraction/exponent, in (i64max, u64max] (lossless)
        number,           // everything else — an ECMAScript double (shortest-round-trip formatted)
        string,
        array,
        object,
    };

    Type type = Type::null_value;
    bool boolean_value = false;
    std::int64_t int_value = 0;
    std::uint64_t uint_value = 0;
    double number_value = 0.0;
    std::string string_value;        // NFC-normalized by the parser (R-FILE-001)
    std::vector<JsonValue> elements; // array elements, authored order (stable array ordering)
    std::vector<JsonMember> members; // object members, authored order (the WRITER sorts by key)
};

// One object member. Keys are NFC-normalized by the parser; the canonical writer sorts members
// lexicographically by UTF-8 byte order (== code-point order for valid UTF-8).
struct JsonMember
{
    std::string key;
    JsonValue value;
};

// One machine-readable diagnostic (R-FILE-003 shape: code + message + position). `code` is a stable
// dotted identifier (e.g. "encoding.crlf", "json.duplicate_key"); line/column are 1-based and refer
// to the SOURCE bytes handed to the parser.
struct Diagnostic
{
    std::string code;
    std::string message;
    std::size_t line = 1;
    std::size_t column = 1;
};

} // namespace context::editor::serializer
