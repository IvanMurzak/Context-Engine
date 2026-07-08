// Source Map v3 parser + resolver (R-OBS-005 / L-61). STL-only, no V8 — a LOCAL gate. See
// source_map.h for the scope + deferred seams.
//
// Two self-contained pieces live in the anonymous namespace: a minimal recursive-descent JSON
// reader (the source map is a flat JSON object; runtime/ts must NOT link the editor/contract JSON
// layer — that would invert the runtime<-editor layering, exactly as error_catalog.cpp keeps the
// ts.* code STRINGS local rather than linking back), and a base64-VLQ decoder for the `mappings`
// grid. Neither throws; both fail-closed with a diagnostic.

#include "context/runtime/ts/source_map.h"

#include <cstddef>
#include <limits>

namespace context::runtime::ts
{
namespace
{

// --- minimal JSON reader (object/array/string/number/bool/null) --------------------------------
//
// Purpose-built for a Source Map v3 document: it need only reach `version` (number), `sources`
// (string array), `names` (string array), and `mappings` (string), while structurally skipping any
// other field (`sourcesContent`, `sourceRoot`, `file`, ...). It validates structure so malformed
// JSON fails parse() rather than silently mis-reading. Values are materialised lazily: strings and
// numbers are captured, arrays/objects are skipped unless the caller descends into them.

class JsonReader
{
public:
    explicit JsonReader(std::string_view s) : s_(s) {}

    [[nodiscard]] bool ok() const { return ok_; }

    // Skip insignificant whitespace.
    void skipWs()
    {
        while (pos_ < s_.size())
        {
            const char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            {
                ++pos_;
            }
            else
            {
                break;
            }
        }
    }

    [[nodiscard]] bool atEnd()
    {
        skipWs();
        return pos_ >= s_.size();
    }

    [[nodiscard]] char peek()
    {
        skipWs();
        return pos_ < s_.size() ? s_[pos_] : '\0';
    }

    // Consume a specific delimiter char; records failure if it is not next.
    bool expect(char c)
    {
        if (peek() != c)
        {
            return fail();
        }
        ++pos_;
        return true;
    }

    // Parse a JSON string (leading '"' must be next). Handles the standard escapes incl. \uXXXX.
    bool parseString(std::string& out)
    {
        if (peek() != '"')
        {
            return fail();
        }
        ++pos_; // opening quote
        out.clear();
        while (pos_ < s_.size())
        {
            const char c = s_[pos_++];
            if (c == '"')
            {
                return true;
            }
            if (c == '\\')
            {
                if (pos_ >= s_.size())
                {
                    return fail();
                }
                const char e = s_[pos_++];
                switch (e)
                {
                case '"':
                    out.push_back('"');
                    break;
                case '\\':
                    out.push_back('\\');
                    break;
                case '/':
                    out.push_back('/');
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u':
                    if (!parseUnicodeEscape(out))
                    {
                        return fail();
                    }
                    break;
                default:
                    return fail();
                }
            }
            else
            {
                out.push_back(c);
            }
        }
        return fail(); // unterminated string
    }

    // Parse a JSON number into a signed integer (the source map's `version` is an int). Fractions
    // and exponents are consumed but truncated toward zero — no source-map integer field needs them.
    bool parseInt(long long& out)
    {
        skipWs();
        const std::size_t start = pos_;
        bool neg = false;
        if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+'))
        {
            neg = s_[pos_] == '-';
            ++pos_;
        }
        long long value = 0;
        bool anyDigit = false;
        while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9')
        {
            const int digit = s_[pos_] - '0';
            // Fail closed on a malformed/adversarial overlong integer rather than invoking
            // signed-overflow UB. parseInt is reachable on `version` AND — via skipValue's default
            // branch — on any extraneous numeric field, so an oversized literal must not overflow
            // `value * 10 + digit`. Mirrors decodeVlq's pre-add headroom guard (the reader's
            // "neither throws; both fail-closed" contract).
            if (value > (std::numeric_limits<long long>::max() - digit) / 10)
            {
                pos_ = start;
                return fail();
            }
            value = value * 10 + digit;
            anyDigit = true;
            ++pos_;
        }
        // Consume (and ignore) any fractional / exponent tail so the reader stays in sync.
        while (pos_ < s_.size())
        {
            const char c = s_[pos_];
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-')
            {
                ++pos_;
            }
            else
            {
                break;
            }
        }
        if (!anyDigit)
        {
            pos_ = start;
            return fail();
        }
        out = neg ? -value : value;
        return true;
    }

    // Structurally skip the next JSON value (object/array/string/number/bool/null), keeping the
    // reader in sync so following fields still parse. `depth` bounds the mutual recursion with
    // skipContainer so a pathologically nested extraneous value fails closed (per the reader's
    // "neither throws; both fail-closed" contract) instead of overflowing the native stack.
    bool skipValue(int depth = 0)
    {
        if (depth > kMaxSkipDepth)
        {
            return fail();
        }
        const char c = peek();
        switch (c)
        {
        case '"':
        {
            std::string scratch;
            return parseString(scratch);
        }
        case '{':
            return skipContainer('{', '}', depth);
        case '[':
            return skipContainer('[', ']', depth);
        case 't':
            return skipLiteral("true");
        case 'f':
            return skipLiteral("false");
        case 'n':
            return skipLiteral("null");
        default:
        {
            long long scratch = 0;
            return parseInt(scratch);
        }
        }
    }

private:
    // Bounds skipValue/skipContainer mutual recursion. A real Source Map v3 document nests its
    // skipped fields only a few levels deep; 64 is far above any legitimate map yet well below the
    // native-stack limit, so a pathological value fails closed instead of overflowing the stack.
    static constexpr int kMaxSkipDepth = 64;

    bool fail()
    {
        ok_ = false;
        return false;
    }

    bool parseUnicodeEscape(std::string& out)
    {
        if (pos_ + 4 > s_.size())
        {
            return false;
        }
        unsigned code = 0;
        for (int i = 0; i < 4; ++i)
        {
            const char h = s_[pos_++];
            code <<= 4;
            if (h >= '0' && h <= '9')
            {
                code |= static_cast<unsigned>(h - '0');
            }
            else if (h >= 'a' && h <= 'f')
            {
                code |= static_cast<unsigned>(h - 'a' + 10);
            }
            else if (h >= 'A' && h <= 'F')
            {
                code |= static_cast<unsigned>(h - 'A' + 10);
            }
            else
            {
                return false;
            }
        }
        // Encode the (BMP) code point as UTF-8. Source-map source paths / names are ASCII in
        // practice; a surrogate pair is emitted as two 3-byte sequences (lossy for astral names,
        // which never index a mapping) — acceptable for the DoD floor.
        if (code < 0x80)
        {
            out.push_back(static_cast<char>(code));
        }
        else if (code < 0x800)
        {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
        else
        {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
        return true;
    }

    bool skipLiteral(std::string_view lit)
    {
        skipWs();
        if (s_.substr(pos_, lit.size()) != lit)
        {
            return fail();
        }
        pos_ += lit.size();
        return true;
    }

    bool skipContainer(char open, char close, int depth = 0)
    {
        if (!expect(open))
        {
            return false;
        }
        if (peek() == close)
        {
            ++pos_;
            return true;
        }
        while (true)
        {
            if (open == '{')
            {
                std::string key;
                if (!parseString(key) || !expect(':'))
                {
                    return false;
                }
            }
            if (!skipValue(depth + 1))
            {
                return false;
            }
            const char n = peek();
            if (n == ',')
            {
                ++pos_;
                continue;
            }
            if (n == close)
            {
                ++pos_;
                return true;
            }
            return fail();
        }
    }

    std::string_view s_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

// Read a `"..."` string array into `out`. Positioned just after the field's ':'.
bool readStringArray(JsonReader& r, std::vector<std::string>& out)
{
    if (!r.expect('['))
    {
        return false;
    }
    if (r.peek() == ']')
    {
        r.expect(']');
        return r.ok();
    }
    while (true)
    {
        std::string s;
        if (!r.parseString(s))
        {
            return false;
        }
        out.push_back(std::move(s));
        const char n = r.peek();
        if (n == ',')
        {
            r.expect(',');
            continue;
        }
        if (n == ']')
        {
            r.expect(']');
            return r.ok();
        }
        return false;
    }
}

// --- base64-VLQ ---------------------------------------------------------------------------------

// Decode one base64 digit to its 6-bit value, or -1 if not a base64 alphabet char.
int base64Value(char c)
{
    if (c >= 'A' && c <= 'Z')
    {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z')
    {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9')
    {
        return c - '0' + 52;
    }
    if (c == '+')
    {
        return 62;
    }
    if (c == '/')
    {
        return 63;
    }
    return -1;
}

// Decode one VLQ value from `s` starting at `pos`, advancing `pos`. Returns false on a malformed or
// truncated VLQ. The value is the zig-zag-decoded signed delta.
bool decodeVlq(std::string_view s, std::size_t& pos, std::int64_t& out)
{
    std::int64_t result = 0;
    int shift = 0;
    bool continuation = true;
    bool any = false;
    while (continuation)
    {
        if (pos >= s.size())
        {
            return false;
        }
        const int digit = base64Value(s[pos]);
        if (digit < 0)
        {
            return false;
        }
        ++pos;
        any = true;
        continuation = (digit & 0x20) != 0;
        const std::int64_t chunk = static_cast<std::int64_t>(digit & 0x1F);
        // Fail closed on a malformed/adversarial overlong VLQ rather than invoking signed
        // left-shift UB: at shift 60 a non-zero 5-bit chunk shifted into (or past) the int64 sign
        // bit is undefined behaviour, and `shift >= 64` would make the headroom shift itself UB.
        // VLQ chunks occupy disjoint 5-bit windows, so bounding this chunk against the remaining
        // int64 headroom bounds the accumulated value. (Real esbuild maps never reach shift 60.)
        if (shift >= 63 || chunk > (std::numeric_limits<std::int64_t>::max() >> shift))
        {
            return false;
        }
        result += chunk << shift;
        shift += 5;
    }
    if (!any)
    {
        return false;
    }
    // zig-zag decode: LSB is the sign.
    const bool negative = (result & 1) != 0;
    out = result >> 1;
    if (negative)
    {
        out = -out;
    }
    return true;
}

} // namespace

std::optional<SourceMap> SourceMap::parse(std::string_view json, std::string* err)
{
    const auto setErr = [&](const char* m) {
        if (err != nullptr)
        {
            *err = m;
        }
        return std::nullopt;
    };

    JsonReader r(json);
    if (!r.expect('{'))
    {
        return setErr("source map is not a JSON object");
    }

    SourceMap map;
    std::string mappings;
    bool sawVersion = false;
    bool sawMappings = false;

    if (r.peek() != '}')
    {
        while (true)
        {
            std::string key;
            if (!r.parseString(key) || !r.expect(':'))
            {
                return setErr("malformed source map object member");
            }
            if (key == "version")
            {
                long long v = 0;
                if (!r.parseInt(v))
                {
                    return setErr("source map version is not a number");
                }
                if (v != 3)
                {
                    return setErr("unsupported source map version (only v3 is supported)");
                }
                sawVersion = true;
            }
            else if (key == "sources")
            {
                if (!readStringArray(r, map.sources_))
                {
                    return setErr("source map `sources` is not a string array");
                }
            }
            else if (key == "names")
            {
                if (!readStringArray(r, map.names_))
                {
                    return setErr("source map `names` is not a string array");
                }
            }
            else if (key == "mappings")
            {
                if (!r.parseString(mappings))
                {
                    return setErr("source map `mappings` is not a string");
                }
                sawMappings = true;
            }
            else if (!r.skipValue())
            {
                return setErr("malformed source map member value");
            }

            const char n = r.peek();
            if (n == ',')
            {
                r.expect(',');
                continue;
            }
            if (n == '}')
            {
                r.expect('}');
                break;
            }
            return setErr("malformed source map object (expected ',' or '}')");
        }
    }
    else
    {
        r.expect('}');
    }

    if (!r.ok())
    {
        return setErr("malformed source map JSON");
    }
    if (!sawVersion)
    {
        return setErr("source map is missing the required `version` field");
    }
    if (!sawMappings)
    {
        return setErr("source map is missing the required `mappings` field");
    }

    // --- decode the mappings grid ---------------------------------------------------------------
    // Running state persists across the whole map EXCEPT genColumn, which resets each generated
    // line. sourceIndex / origLine / origColumn / nameIndex are cumulative deltas across lines.
    std::int64_t sourceIndex = 0;
    std::int64_t origLine = 0;
    std::int64_t origColumn = 0;
    std::int64_t nameIndex = 0;

    std::vector<Segment> current;
    std::int64_t genColumn = 0;
    std::size_t i = 0;
    const std::string_view m = mappings;

    const auto flushLine = [&]() {
        map.segments_.push_back(std::move(current));
        current.clear();
        genColumn = 0;
    };

    while (i <= m.size())
    {
        if (i == m.size() || m[i] == ';' || m[i] == ',')
        {
            if (i == m.size())
            {
                flushLine();
                break;
            }
            if (m[i] == ';')
            {
                flushLine();
            }
            ++i;
            continue;
        }

        // Decode one segment: 1, 4, or 5 VLQ fields.
        std::int64_t genColDelta = 0;
        if (!decodeVlq(m, i, genColDelta))
        {
            return setErr("malformed VLQ in `mappings` (generated column)");
        }
        genColumn += genColDelta;
        if (genColumn < 0)
        {
            return setErr("negative generated column in `mappings`");
        }
        // Segments on a generated line MUST be genColumn-ascending: resolve()'s binary search and the
        // documented segments_ invariant depend on it. A cumulative genColumn that moves BACKWARD is a
        // malformed map — fail closed rather than silently mis-resolving via a broken precondition.
        if (!current.empty() && genColumn < static_cast<std::int64_t>(current.back().genColumn))
        {
            return setErr("non-ascending generated column in `mappings`");
        }

        Segment seg;
        seg.genColumn = static_cast<std::uint32_t>(genColumn);

        // Peek whether more fields follow this segment (a segment ends at ',' ';' or end-of-string).
        const auto moreFields = [&]() {
            return i < m.size() && m[i] != ',' && m[i] != ';';
        };

        if (moreFields())
        {
            std::int64_t sourceDelta = 0;
            std::int64_t lineDelta = 0;
            std::int64_t columnDelta = 0;
            if (!decodeVlq(m, i, sourceDelta) || !decodeVlq(m, i, lineDelta) ||
                !decodeVlq(m, i, columnDelta))
            {
                return setErr("malformed VLQ in `mappings` (source/line/column)");
            }
            sourceIndex += sourceDelta;
            origLine += lineDelta;
            origColumn += columnDelta;
            if (sourceIndex < 0 || origLine < 0 || origColumn < 0)
            {
                return setErr("negative source/line/column index in `mappings`");
            }
            seg.hasSource = true;
            seg.sourceIndex = static_cast<std::uint32_t>(sourceIndex);
            seg.origLine = static_cast<std::uint32_t>(origLine);
            seg.origColumn = static_cast<std::uint32_t>(origColumn);

            if (moreFields())
            {
                std::int64_t nameDelta = 0;
                if (!decodeVlq(m, i, nameDelta))
                {
                    return setErr("malformed VLQ in `mappings` (name)");
                }
                nameIndex += nameDelta;
                if (nameIndex < 0)
                {
                    return setErr("negative name index in `mappings`");
                }
                seg.hasName = true;
                seg.nameIndex = static_cast<std::uint32_t>(nameIndex);
            }
        }

        current.push_back(seg);
    }

    return map;
}

std::optional<OriginalPosition> SourceMap::resolve(std::uint32_t line, std::uint32_t column) const
{
    if (line >= segments_.size())
    {
        return std::nullopt;
    }
    const std::vector<Segment>& row = segments_[line];
    if (row.empty())
    {
        return std::nullopt;
    }

    // Greatest genColumn <= column (segments are genColumn-ascending). Binary search.
    std::size_t lo = 0;
    std::size_t hi = row.size();
    bool found = false;
    std::size_t best = 0;
    while (lo < hi)
    {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (row[mid].genColumn <= column)
        {
            best = mid;
            found = true;
            lo = mid + 1;
        }
        else
        {
            hi = mid;
        }
    }
    if (!found)
    {
        return std::nullopt; // column precedes the first segment on the line
    }

    const Segment& seg = row[best];
    if (!seg.hasSource || seg.sourceIndex >= sources_.size())
    {
        return std::nullopt; // a generated-column-only segment maps to no source
    }

    OriginalPosition out;
    out.source = sources_[seg.sourceIndex];
    out.line = seg.origLine;
    out.column = seg.origColumn;
    if (seg.hasName && seg.nameIndex < names_.size())
    {
        out.name = names_[seg.nameIndex];
    }
    return out;
}

} // namespace context::runtime::ts
