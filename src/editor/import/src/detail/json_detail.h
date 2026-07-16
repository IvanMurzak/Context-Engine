// Internal JSON helpers shared by the importers (build a canonical descriptor tree; read glTF).
// Not a public header — included via a relative path from the importer .cpp files only. Wraps the
// engine's serializer::JsonValue so the importers never hand-format JSON (their descriptors go out
// as canonical bytes for byte-determinism, R-ASSET-001).

#pragma once

#include "context/editor/serializer/json_tree.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace context::editor::import::detail
{
using serializer::JsonMember;
using serializer::JsonValue;

// --- builders (construct a JsonValue tree an importer serializes canonically) -------------------

[[nodiscard]] inline JsonValue jstr(std::string s)
{
    JsonValue v;
    v.type = JsonValue::Type::string;
    v.string_value = std::move(s);
    return v;
}

[[nodiscard]] inline JsonValue jint(std::int64_t n)
{
    JsonValue v;
    v.type = JsonValue::Type::integer;
    v.int_value = n;
    return v;
}

[[nodiscard]] inline JsonValue juint(std::uint64_t n)
{
    JsonValue v;
    v.type = JsonValue::Type::unsigned_integer;
    v.uint_value = n;
    return v;
}

[[nodiscard]] inline JsonValue jbool(bool b)
{
    JsonValue v;
    v.type = JsonValue::Type::boolean;
    v.boolean_value = b;
    return v;
}

[[nodiscard]] inline JsonValue jobject()
{
    JsonValue v;
    v.type = JsonValue::Type::object;
    return v;
}

[[nodiscard]] inline JsonValue jarray()
{
    JsonValue v;
    v.type = JsonValue::Type::array;
    return v;
}

// Append a member to an object value. The canonical writer sorts keys, so authoring order here is
// irrelevant to the output bytes — but it stays deterministic regardless.
inline void put(JsonValue& obj, std::string key, JsonValue value)
{
    obj.members.push_back(JsonMember{std::move(key), std::move(value)});
}

inline void append(JsonValue& arr, JsonValue value)
{
    arr.elements.push_back(std::move(value));
}

// --- readers (navigate a parsed glTF tree) ------------------------------------------------------

// The object member named `key`, or nullptr (non-object or absent).
[[nodiscard]] inline const JsonValue* member(const JsonValue& obj, std::string_view key)
{
    if (obj.type != JsonValue::Type::object)
        return nullptr;
    for (const JsonMember& m : obj.members)
        if (m.key == key)
            return &m.value;
    return nullptr;
}

// A signed-integer read from a value that may be integer or unsigned_integer; `fallback` otherwise.
[[nodiscard]] inline std::int64_t as_int64(const JsonValue* v, std::int64_t fallback)
{
    if (v == nullptr)
        return fallback;
    if (v->type == JsonValue::Type::integer)
        return v->int_value;
    if (v->type == JsonValue::Type::unsigned_integer)
        return static_cast<std::int64_t>(v->uint_value);
    return fallback;
}

// A boolean read; `fallback` for a null / non-boolean value. The sibling of as_int64 — same
// pointer-taking shape, so `as_bool(member(obj, key), true)` reads like its integer counterpart.
[[nodiscard]] inline bool as_bool(const JsonValue* v, bool fallback)
{
    if (v == nullptr || v->type != JsonValue::Type::boolean)
        return fallback;
    return v->boolean_value;
}

// --- diagnostics hygiene ------------------------------------------------------------------------

// Render `raw` for a human diagnostic message: returned verbatim when every byte is printable ASCII
// (0x20-0x7E), else a deterministic hex form ("0xAB 0xCD ..."). Importer diagnostics splice bytes
// taken straight from an untrusted source (a PNG chunk type, a glTF version string), so a fuzzed or
// malicious asset could otherwise inject terminal control/escape sequences into a log; hex-encoding
// the non-printable case neutralizes that while staying byte-deterministic (R-ASSET-001: a bad
// source must fail identically twice, so this must be a pure function of `raw`).
[[nodiscard]] inline std::string ascii_or_hex(std::string_view raw)
{
    const auto is_printable = [](unsigned char b) { return b >= 0x20 && b <= 0x7E; };
    bool all_printable = true;
    for (const char ch : raw)
        if (!is_printable(static_cast<unsigned char>(ch)))
        {
            all_printable = false;
            break;
        }
    if (all_printable)
        return std::string(raw);

    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(raw.size() * 5);
    for (const char ch : raw)
    {
        const auto b = static_cast<unsigned char>(ch);
        if (!out.empty())
            out.push_back(' ');
        out.push_back('0');
        out.push_back('x');
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0FU]);
    }
    return out;
}

// --- byte writers (little-endian scalar fields into a binary container) --------------------------

// Append a little-endian u32. The write counterpart of read_u32le below — shared for the same
// reason: a fixed byte order is what makes a container's bytes identical on every host, and each
// producer re-hand-rolling the shift/mask is how the two directions drift apart.
inline void put_u32le(std::string& out, std::uint32_t v)
{
    out.push_back(static_cast<char>(v & 0xFFU));
    out.push_back(static_cast<char>((v >> 8) & 0xFFU));
    out.push_back(static_cast<char>((v >> 16) & 0xFFU));
    out.push_back(static_cast<char>((v >> 24) & 0xFFU));
}

// --- byte readers (little-endian scalar fields in a binary container: GLB header, WAV chunks) ----

// A little-endian u32/u16 at byte offset `at`. Callers bounds-check `at` against the buffer before
// calling (the importers validate every offset up-front). Shared so the glTF/GLB + WAV parsers do
// not each re-hand-roll the same shift/mask.
[[nodiscard]] inline std::uint32_t read_u32le(std::string_view b, std::size_t at) noexcept
{
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at])) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at + 1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at + 2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at + 3])) << 24);
}

[[nodiscard]] inline std::uint16_t read_u16le(std::string_view b, std::size_t at) noexcept
{
    return static_cast<std::uint16_t>(static_cast<std::uint8_t>(b[at]) |
                                      (static_cast<std::uint8_t>(b[at + 1]) << 8));
}

} // namespace context::editor::import::detail
