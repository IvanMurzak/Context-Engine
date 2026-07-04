// Strict RFC 8259 JSON parser with encoding diagnostics — see json_parse.h.

#include "context/editor/serializer/json_parse.h"

#include "context/editor/serializer/nfc.h"

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace context::editor::serializer
{

namespace
{

class Parser
{
public:
    explicit Parser(std::string_view source, ParseResult& result)
        : src_(source), result_(result)
    {
    }

    void run()
    {
        // A UTF-8 BOM is stripped and flagged (non-fatal): the canonical rewrite heals it on this
        // save (R-FILE-003 eventually-valid; authored files are BOM-less UTF-8, R-FILE-001).
        if (src_.size() >= 3 && std::memcmp(src_.data(), "\xEF\xBB\xBF", 3) == 0)
        {
            pos_ = 3;
            column_ = 4;
            diag("encoding.bom", "UTF-8 BOM found; authored files are BOM-less (healed on save)");
        }
        // CR anywhere means CRLF/CR line endings (or a raw CR inside a string, which is itself a
        // fatal grammar error below). Non-fatal: the canonical form is LF-only (R-FILE-001).
        if (src_.find('\r') != std::string_view::npos)
            diag("encoding.crlf",
                 "CR/CRLF line endings found; authored files are LF-only (healed on save)");

        skip_whitespace();
        if (at_end())
        {
            fail("json.unexpected_end", "empty document: expected one JSON value");
            return;
        }
        JsonValue root;
        if (!parse_value(root, 0))
            return;
        skip_whitespace();
        if (!at_end())
        {
            fail("json.trailing_content", "unexpected content after the root value");
            return;
        }
        result_.root = std::move(root);
        result_.ok = true;
    }

private:
    [[nodiscard]] bool at_end() const noexcept { return pos_ >= src_.size(); }
    [[nodiscard]] char peek() const noexcept { return src_[pos_]; }

    void advance() noexcept
    {
        if (src_[pos_] == '\n')
        {
            ++line_;
            column_ = 1;
        }
        else
        {
            ++column_;
        }
        ++pos_;
    }

    void skip_whitespace() noexcept
    {
        while (!at_end())
        {
            const char c = peek();
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
                break;
            advance();
        }
    }

    void diag(const char* code, const char* message)
    {
        result_.diagnostics.push_back(Diagnostic{code, message, line_, column_});
    }

    bool fail(const char* code, const char* message)
    {
        diag(code, message);
        result_.ok = false;
        return false;
    }

    bool parse_value(JsonValue& out, std::size_t depth)
    {
        if (depth > kMaxParseDepth)
            return fail("json.depth_exceeded", "nesting exceeds the maximum parse depth");
        skip_whitespace();
        if (at_end())
            return fail("json.unexpected_end", "unexpected end of input: expected a value");

        switch (peek())
        {
        case '{':
            return parse_object(out, depth);
        case '[':
            return parse_array(out, depth);
        case '"':
        {
            out.type = JsonValue::Type::string;
            return parse_string(out.string_value);
        }
        case 't':
            return parse_literal("true", [&] {
                out.type = JsonValue::Type::boolean;
                out.boolean_value = true;
            });
        case 'f':
            return parse_literal("false", [&] {
                out.type = JsonValue::Type::boolean;
                out.boolean_value = false;
            });
        case 'n':
            return parse_literal("null", [&] { out.type = JsonValue::Type::null_value; });
        default:
            if (peek() == '-' || (peek() >= '0' && peek() <= '9'))
                return parse_number(out);
            return fail("json.unexpected_token", "unexpected character: expected a JSON value");
        }
    }

    template <typename Apply>
    bool parse_literal(const char* word, Apply apply)
    {
        const std::size_t len = std::strlen(word);
        if (src_.size() - pos_ < len || src_.compare(pos_, len, word) != 0)
            return fail("json.unexpected_token", "malformed literal (expected true/false/null)");
        for (std::size_t i = 0; i < len; ++i)
            advance();
        apply();
        return true;
    }

    bool parse_object(JsonValue& out, std::size_t depth)
    {
        out.type = JsonValue::Type::object;
        advance(); // '{'
        skip_whitespace();
        if (!at_end() && peek() == '}')
        {
            advance();
            return true;
        }
        while (true)
        {
            skip_whitespace();
            if (at_end() || peek() != '"')
                return fail("json.expected_key", "expected a string object key");
            JsonMember member;
            if (!parse_string(member.key))
                return false;
            // Canonical JSON requires unique keys: duplicates have no well-defined canonical
            // order and break merge identity (R-FILE-012).
            for (const JsonMember& existing : out.members)
            {
                if (existing.key == member.key)
                    return fail("json.duplicate_key", "duplicate object key");
            }
            skip_whitespace();
            if (at_end() || peek() != ':')
                return fail("json.expected_colon", "expected ':' after object key");
            advance();
            if (!parse_value(member.value, depth + 1))
                return false;
            out.members.push_back(std::move(member));
            skip_whitespace();
            if (at_end())
                return fail("json.unexpected_end", "unterminated object");
            if (peek() == ',')
            {
                advance();
                continue;
            }
            if (peek() == '}')
            {
                advance();
                return true;
            }
            return fail("json.expected_comma", "expected ',' or '}' in object");
        }
    }

    bool parse_array(JsonValue& out, std::size_t depth)
    {
        out.type = JsonValue::Type::array;
        advance(); // '['
        skip_whitespace();
        if (!at_end() && peek() == ']')
        {
            advance();
            return true;
        }
        while (true)
        {
            JsonValue element;
            if (!parse_value(element, depth + 1))
                return false;
            out.elements.push_back(std::move(element));
            skip_whitespace();
            if (at_end())
                return fail("json.unexpected_end", "unterminated array");
            if (peek() == ',')
            {
                advance();
                continue;
            }
            if (peek() == ']')
            {
                advance();
                return true;
            }
            return fail("json.expected_comma", "expected ',' or ']' in array");
        }
    }

    // Parse a string token into `out`, unescaping and NFC-normalizing (R-FILE-001).
    bool parse_string(std::string& out)
    {
        advance(); // opening '"'
        std::string raw;
        while (true)
        {
            if (at_end())
                return fail("json.unexpected_end", "unterminated string");
            const unsigned char c = static_cast<unsigned char>(peek());
            if (c == '"')
            {
                advance();
                break;
            }
            if (c == '\\')
            {
                advance();
                if (!parse_escape(raw))
                    return false;
                continue;
            }
            if (c < 0x20)
                return fail("json.invalid_char",
                            "raw control character in string (must be \\u-escaped)");
            if (c < 0x80)
            {
                raw.push_back(static_cast<char>(c));
                advance();
                continue;
            }
            if (!consume_utf8_sequence(raw))
                return false;
        }
        if (auto normalized = normalize_nfc(raw))
            out = std::move(*normalized);
        else
            out = std::move(raw);
        return true;
    }

    bool parse_escape(std::string& raw)
    {
        if (at_end())
            return fail("json.unexpected_end", "unterminated escape sequence");
        const char c = peek();
        advance();
        switch (c)
        {
        case '"':
            raw.push_back('"');
            return true;
        case '\\':
            raw.push_back('\\');
            return true;
        case '/':
            raw.push_back('/');
            return true;
        case 'b':
            raw.push_back('\b');
            return true;
        case 'f':
            raw.push_back('\f');
            return true;
        case 'n':
            raw.push_back('\n');
            return true;
        case 'r':
            raw.push_back('\r');
            return true;
        case 't':
            raw.push_back('\t');
            return true;
        case 'u':
        {
            unsigned int unit = 0;
            if (!parse_hex4(unit))
                return false;
            char32_t cp = unit;
            if (unit >= 0xD800 && unit <= 0xDBFF) // high surrogate: a low one MUST follow
            {
                if (at_end() || peek() != '\\')
                    return fail("json.invalid_escape", "unpaired high surrogate in \\u escape");
                advance();
                if (at_end() || peek() != 'u')
                    return fail("json.invalid_escape", "unpaired high surrogate in \\u escape");
                advance();
                unsigned int low = 0;
                if (!parse_hex4(low))
                    return false;
                if (low < 0xDC00 || low > 0xDFFF)
                    return fail("json.invalid_escape", "invalid low surrogate in \\u escape");
                cp = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
            }
            else if (unit >= 0xDC00 && unit <= 0xDFFF)
            {
                return fail("json.invalid_escape", "unpaired low surrogate in \\u escape");
            }
            append_utf8(cp, raw);
            return true;
        }
        default:
            return fail("json.invalid_escape", "unknown escape sequence");
        }
    }

    bool parse_hex4(unsigned int& out)
    {
        out = 0;
        for (int i = 0; i < 4; ++i)
        {
            if (at_end())
                return fail("json.unexpected_end", "unterminated \\u escape");
            const char c = peek();
            unsigned int digit = 0;
            if (c >= '0' && c <= '9')
                digit = static_cast<unsigned int>(c - '0');
            else if (c >= 'a' && c <= 'f')
                digit = static_cast<unsigned int>(c - 'a') + 10;
            else if (c >= 'A' && c <= 'F')
                digit = static_cast<unsigned int>(c - 'A') + 10;
            else
                return fail("json.invalid_escape", "non-hex digit in \\u escape");
            out = (out << 4) | digit;
            advance();
        }
        return true;
    }

    static void append_utf8(char32_t cp, std::string& out)
    {
        if (cp < 0x80)
        {
            out.push_back(static_cast<char>(cp));
        }
        else if (cp < 0x800)
        {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else if (cp < 0x10000)
        {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else
        {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    // Validate + copy one multi-byte UTF-8 sequence (strict: no overlongs, no surrogates, <= U+10FFFF).
    bool consume_utf8_sequence(std::string& raw)
    {
        const unsigned char b0 = static_cast<unsigned char>(peek());
        std::size_t len = 0;
        char32_t cp = 0;
        char32_t min = 0;
        if ((b0 & 0xE0) == 0xC0)
        {
            len = 2;
            cp = b0 & 0x1Fu;
            min = 0x80;
        }
        else if ((b0 & 0xF0) == 0xE0)
        {
            len = 3;
            cp = b0 & 0x0Fu;
            min = 0x800;
        }
        else if ((b0 & 0xF8) == 0xF0)
        {
            len = 4;
            cp = b0 & 0x07u;
            min = 0x10000;
        }
        else
        {
            return fail("encoding.invalid_utf8", "invalid UTF-8 lead byte in string");
        }
        if (src_.size() - pos_ < len)
            return fail("encoding.invalid_utf8", "truncated UTF-8 sequence in string");
        for (std::size_t i = 1; i < len; ++i)
        {
            const unsigned char b = static_cast<unsigned char>(src_[pos_ + i]);
            if ((b & 0xC0) != 0x80)
                return fail("encoding.invalid_utf8", "invalid UTF-8 continuation byte in string");
            cp = (cp << 6) | (b & 0x3Fu);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return fail("encoding.invalid_utf8", "invalid UTF-8 scalar value in string");
        raw.append(src_.substr(pos_, len));
        for (std::size_t i = 0; i < len; ++i)
            advance();
        return true;
    }

    bool parse_number(JsonValue& out)
    {
        const std::size_t start = pos_;
        bool is_integer_literal = true;

        if (peek() == '-')
            advance();
        // Integer part: a single 0, or [1-9][0-9]* (leading zeros are not JSON).
        if (at_end() || peek() < '0' || peek() > '9')
            return fail("json.invalid_number", "malformed number: missing integer digits");
        if (peek() == '0')
        {
            advance();
            if (!at_end() && peek() >= '0' && peek() <= '9')
                return fail("json.invalid_number", "malformed number: leading zero");
        }
        else
        {
            while (!at_end() && peek() >= '0' && peek() <= '9')
                advance();
        }
        if (!at_end() && peek() == '.')
        {
            is_integer_literal = false;
            advance();
            if (at_end() || peek() < '0' || peek() > '9')
                return fail("json.invalid_number", "malformed number: missing fraction digits");
            while (!at_end() && peek() >= '0' && peek() <= '9')
                advance();
        }
        if (!at_end() && (peek() == 'e' || peek() == 'E'))
        {
            is_integer_literal = false;
            advance();
            if (!at_end() && (peek() == '+' || peek() == '-'))
                advance();
            if (at_end() || peek() < '0' || peek() > '9')
                return fail("json.invalid_number", "malformed number: missing exponent digits");
            while (!at_end() && peek() >= '0' && peek() <= '9')
                advance();
        }

        const std::string_view token = src_.substr(start, pos_ - start);
        if (is_integer_literal)
        {
            // Lossless integer preservation inside i64/u64 (the spike-ratified rule the corpus
            // pins); beyond u64 an integer literal falls through to the ECMAScript double domain.
            std::int64_t i64 = 0;
            auto ri = std::from_chars(token.data(), token.data() + token.size(), i64);
            if (ri.ec == std::errc() && ri.ptr == token.data() + token.size())
            {
                out.type = JsonValue::Type::integer;
                out.int_value = i64;
                return true;
            }
            std::uint64_t u64 = 0;
            auto ru = std::from_chars(token.data(), token.data() + token.size(), u64);
            if (ru.ec == std::errc() && ru.ptr == token.data() + token.size())
            {
                out.type = JsonValue::Type::unsigned_integer;
                out.uint_value = u64;
                return true;
            }
        }
        // strtod, not std::from_chars<double>: libc++ (the macOS CI leg) shipped floating-point
        // from_chars far later than integer from_chars. strtod under the engine's default "C"
        // locale is correctly rounded on all three toolchains and the grammar scan above already
        // guarantees the whole token is consumed.
        const std::string token_z(token);
        errno = 0;
        char* parse_end = nullptr;
        const double d = std::strtod(token_z.c_str(), &parse_end);
        if (parse_end != token_z.c_str() + token_z.size())
            return fail("json.invalid_number", "malformed number");
        if (errno == ERANGE && (d == HUGE_VAL || d == -HUGE_VAL))
        {
            // ECMAScript overflow would be Infinity — unrepresentable in the canonical form
            // (NaN/Infinity are banned, R-FILE-001). Underflow (ERANGE with a (sub)normal/zero
            // result) is fine: strtod already produced the correctly rounded value.
            return fail("json.number_out_of_range", "number magnitude overflows a double");
        }
        out.type = JsonValue::Type::number;
        out.number_value = d;
        return true;
    }

    std::string_view src_;
    ParseResult& result_;
    std::size_t pos_ = 0;
    std::size_t line_ = 1;
    std::size_t column_ = 1;
};

} // namespace

ParseResult parse_json(std::string_view source)
{
    ParseResult result;
    Parser parser(source, result);
    parser.run();
    return result;
}

} // namespace context::editor::serializer
