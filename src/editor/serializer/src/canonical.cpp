// Canonical-JSON writer + byte-level canonicalization + document header reader — see canonical.h.

#include "context/editor/serializer/canonical.h"

#include "context/editor/serializer/json_parse.h"
#include "context/editor/serializer/nfc.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <vector>

namespace context::editor::serializer
{

void ecma_number(double v, std::string& out)
{
    if (v == 0.0) // covers -0: serialized as "0" (R-FILE-001)
    {
        out.push_back('0');
        return;
    }
    bool neg = v < 0.0;
    if (neg)
        v = -v;

    // Shortest round-trip digits via to_chars in scientific form: d[.ddddd]e±dd. (Floating-point
    // to_chars is portable across all three CI toolchains; from_chars<double> is not — the parser
    // uses strtod for that reason.)
    char buf[64];
    const auto res = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::scientific);
    const char* p = buf;
    const char* end = res.ptr;

    char digits[32];
    int k = 0;           // significant digit count
    digits[k++] = *p++;  // leading digit
    if (p < end && *p == '.')
    {
        ++p;
        while (p < end && *p != 'e')
            digits[k++] = *p++;
    }
    int e10 = 0; // decimal exponent from the scientific form
    if (p < end && *p == 'e')
    {
        ++p;
        const bool eneg = (*p == '-');
        if (*p == '+' || *p == '-')
            ++p;
        while (p < end)
            e10 = e10 * 10 + (*p++ - '0');
        if (eneg)
            e10 = -e10;
    }
    // Strip trailing zero digits (to_chars shortest form should not produce any, but be safe);
    // value = 0.digits * 10^n with n = e10 + 1 (the ECMA-262 §6.1.6.1.20 "n" and "k").
    while (k > 1 && digits[k - 1] == '0')
        --k;
    const int n = e10 + 1;

    if (neg)
        out.push_back('-');
    if (k <= n && n <= 21) // integral value, no exponent: digits then zero-padding
    {
        out.append(digits, static_cast<std::size_t>(k));
        out.append(static_cast<std::size_t>(n - k), '0');
    }
    else if (0 < n && n <= 21) // fixed notation with a decimal point inside the digits
    {
        out.append(digits, static_cast<std::size_t>(n));
        out.push_back('.');
        out.append(digits + n, static_cast<std::size_t>(k - n));
    }
    else if (-6 < n && n <= 0) // fixed notation with leading "0.000…"
    {
        out.append("0.");
        out.append(static_cast<std::size_t>(-n), '0');
        out.append(digits, static_cast<std::size_t>(k));
    }
    else // exponent notation
    {
        const int e = n - 1;
        out.push_back(digits[0]);
        if (k > 1)
        {
            out.push_back('.');
            out.append(digits + 1, static_cast<std::size_t>(k - 1));
        }
        out.push_back('e');
        out.push_back(e >= 0 ? '+' : '-');
        char ebuf[16];
        const auto er = std::to_chars(ebuf, ebuf + sizeof(ebuf), e >= 0 ? e : -e);
        out.append(ebuf, static_cast<std::size_t>(er.ptr - ebuf));
    }
}

std::uint64_t canonical_hash_of(std::string_view bytes) noexcept
{
    // FNV-1a 64-bit. The derivation layer's canonical-content hash function (see canonical.h for
    // the role split vs filesync's raw-byte content_hash).
    std::uint64_t hash = 1469598103934665603ULL; // FNV offset basis
    for (unsigned char byte : bytes)
    {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= 1099511628211ULL; // FNV prime
    }
    return hash;
}

namespace
{

// Escape + quote one string in the canonical form: minimal escaping (control chars, quote,
// backslash), non-ASCII as raw UTF-8. `s` must be NFC (the parser guarantees it; the writer's
// value path re-checks via quick check before calling).
void write_string(const std::string& s, std::string& out)
{
    out.push_back('"');
    for (unsigned char c : s)
    {
        switch (c)
        {
        case '"':
            out.append("\\\"");
            break;
        case '\\':
            out.append("\\\\");
            break;
        case '\b':
            out.append("\\b");
            break;
        case '\f':
            out.append("\\f");
            break;
        case '\n':
            out.append("\\n");
            break;
        case '\r':
            out.append("\\r");
            break;
        case '\t':
            out.append("\\t");
            break;
        default:
            if (c < 0x20)
            {
                char ubuf[8];
                std::snprintf(ubuf, sizeof(ubuf), "\\u%04x", c);
                out.append(ubuf);
            }
            else
            {
                out.push_back(static_cast<char>(c)); // raw UTF-8 pass-through
            }
        }
    }
    out.push_back('"');
}

// Write `s` as canonical NFC output: already-NFC strings (the common case, and everything the
// parser produced) pass through; a programmatically built non-NFC string is normalized here so the
// writer's output is unconditionally NFC (R-FILE-001).
void write_string_nfc(const std::string& s, std::string& out)
{
    if (auto normalized = normalize_nfc(s))
        write_string(*normalized, out);
    else
        write_string(s, out);
}

[[nodiscard]] bool write_value(const JsonValue& v, int indent, std::string& out)
{
    switch (v.type)
    {
    case JsonValue::Type::null_value:
        out.append("null");
        return true;
    case JsonValue::Type::boolean:
        out.append(v.boolean_value ? "true" : "false");
        return true;
    case JsonValue::Type::integer:
    {
        char b[24];
        const auto r = std::to_chars(b, b + sizeof(b), v.int_value);
        out.append(b, static_cast<std::size_t>(r.ptr - b));
        return true;
    }
    case JsonValue::Type::unsigned_integer:
    {
        char b[24];
        const auto r = std::to_chars(b, b + sizeof(b), v.uint_value);
        out.append(b, static_cast<std::size_t>(r.ptr - b));
        return true;
    }
    case JsonValue::Type::number:
        if (!std::isfinite(v.number_value))
            return false; // NaN/Infinity are banned in the canonical form (R-FILE-001)
        ecma_number(v.number_value, out);
        return true;
    case JsonValue::Type::string:
        write_string_nfc(v.string_value, out);
        return true;
    case JsonValue::Type::array:
    {
        if (v.elements.empty())
        {
            out.append("[]");
            return true;
        }
        out.push_back('[');
        for (std::size_t i = 0; i < v.elements.size(); ++i)
        {
            out.push_back('\n');
            out.append(static_cast<std::size_t>(indent + 1) * 2, ' ');
            if (!write_value(v.elements[i], indent + 1, out))
                return false;
            if (i + 1 < v.elements.size())
                out.push_back(',');
        }
        out.push_back('\n');
        out.append(static_cast<std::size_t>(indent) * 2, ' ');
        out.push_back(']');
        return true;
    }
    case JsonValue::Type::object:
    {
        if (v.members.empty())
        {
            out.append("{}");
            return true;
        }
        // Keys are emitted NFC (like every string), so the sort MUST run over the normalized
        // keys — sorting raw bytes and emitting normalized ones could produce output that is not
        // sorted by its own bytes, breaking the fixpoint. Lexicographic UTF-8 byte order
        // (== code-point order for valid UTF-8); keys are unique (the parser enforces it — a
        // programmatic tree must keep them unique after NFC too).
        std::vector<std::string> keys(v.members.size());
        for (std::size_t i = 0; i < v.members.size(); ++i)
        {
            if (auto normalized = normalize_nfc(v.members[i].key))
                keys[i] = std::move(*normalized);
            else
                keys[i] = v.members[i].key;
        }
        std::vector<std::uint32_t> idx(v.members.size());
        std::iota(idx.begin(), idx.end(), 0u);
        std::sort(idx.begin(), idx.end(),
                  [&](std::uint32_t a, std::uint32_t b) { return keys[a] < keys[b]; });
        out.push_back('{');
        for (std::size_t i = 0; i < idx.size(); ++i)
        {
            const JsonMember& m = v.members[idx[i]];
            out.push_back('\n');
            out.append(static_cast<std::size_t>(indent + 1) * 2, ' ');
            write_string(keys[idx[i]], out);
            out.append(": ");
            if (!write_value(m.value, indent + 1, out))
                return false;
            if (i + 1 < idx.size())
                out.push_back(',');
        }
        out.push_back('\n');
        out.append(static_cast<std::size_t>(indent) * 2, ' ');
        out.push_back('}');
        return true;
    }
    }
    return false; // unreachable; keeps -Werror switch-coverage happy across compilers
}

} // namespace

bool serialize_canonical(const JsonValue& root, std::string& out)
{
    std::string bytes;
    if (!write_value(root, 0, bytes))
        return false;
    bytes.push_back('\n'); // canonical files end with EXACTLY one newline
    out.append(bytes);
    return true;
}

CanonicalizeResult canonicalize(std::string_view source)
{
    CanonicalizeResult result;
    ParseResult parsed = parse_json(source);
    result.diagnostics = std::move(parsed.diagnostics);
    if (parsed.ok)
    {
        std::string bytes;
        if (serialize_canonical(parsed.root, bytes))
        {
            result.is_json = true;
            result.bytes = std::move(bytes);
            result.canonical_hash = canonical_hash_of(result.bytes);
            return result;
        }
        // Unreachable from a real parse (the grammar cannot produce NaN/Infinity); fall through
        // to the raw path so the function stays total.
    }
    // Non-JSON content (binary sidecars, TS/shader text — the L-32 carve-outs): NO
    // canonicalization pass, so canonical bytes ARE the raw bytes and the canonical hash equals
    // the raw-byte hash by construction (R-FILE-001).
    result.is_json = false;
    result.bytes.assign(source.data(), source.size());
    result.canonical_hash = canonical_hash_of(result.bytes);
    return result;
}

DocumentHeader read_document_header(const JsonValue& root, std::vector<Diagnostic>& diagnostics)
{
    DocumentHeader header;
    if (root.type != JsonValue::Type::object)
        return header;

    for (const JsonMember& m : root.members)
    {
        if (m.key == "$schema")
        {
            if (m.value.type == JsonValue::Type::string)
            {
                header.has_schema = true;
                header.schema = m.value.string_value;
            }
            else
            {
                diagnostics.push_back(
                    Diagnostic{"header.schema_not_string", "\"$schema\" must be a string"});
            }
        }
        else if (m.key == "version")
        {
            if (m.value.type == JsonValue::Type::integer)
            {
                header.has_version = true;
                header.version = m.value.int_value;
            }
            else
            {
                diagnostics.push_back(
                    Diagnostic{"header.version_not_integer", "\"version\" must be an integer"});
            }
        }
        else if (m.key == "componentVersions")
        {
            if (m.value.type != JsonValue::Type::object)
            {
                diagnostics.push_back(Diagnostic{"header.component_versions_not_object",
                                                 "\"componentVersions\" must be an object map"});
                continue;
            }
            header.has_component_versions = true;
            for (const JsonMember& cv : m.value.members)
            {
                if (cv.value.type == JsonValue::Type::integer)
                {
                    header.component_versions.emplace_back(cv.key, cv.value.int_value);
                }
                else
                {
                    diagnostics.push_back(Diagnostic{
                        "header.component_version_not_integer",
                        "componentVersions[\"" + cv.key + "\"] must be an integer schema version"});
                }
            }
        }
    }
    return header;
}

} // namespace context::editor::serializer
