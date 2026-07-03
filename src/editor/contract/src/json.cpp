// Json parse + dump implementation (see json.h).

#include "context/editor/contract/json.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace context::editor::contract
{

const std::string& Json::empty_string() noexcept
{
    static const std::string empty;
    return empty;
}

const Json& Json::null_ref() noexcept
{
    static const Json null_value;
    return null_value;
}

bool Json::contains(const std::string& key) const noexcept
{
    if (type_ != Type::object)
        return false;
    for (const auto& [k, v] : obj_)
        if (k == key)
            return true;
    return false;
}

const Json& Json::at(const std::string& key) const noexcept
{
    if (type_ == Type::object)
        for (const auto& [k, v] : obj_)
            if (k == key)
                return v;
    return null_ref();
}

std::size_t Json::size() const noexcept
{
    if (type_ == Type::array)
        return arr_.size();
    if (type_ == Type::object)
        return obj_.size();
    return 0;
}

const Json& Json::at(std::size_t index) const noexcept
{
    if (type_ == Type::array && index < arr_.size())
        return arr_[index];
    return null_ref();
}

void Json::set(std::string key, Json value)
{
    if (type_ != Type::object)
    {
        type_ = Type::object;
        obj_.clear();
    }
    for (auto& [k, v] : obj_)
    {
        if (k == key)
        {
            v = std::move(value);
            return;
        }
    }
    obj_.emplace_back(std::move(key), std::move(value));
}

void Json::push_back(Json value)
{
    if (type_ != Type::array)
    {
        type_ = Type::array;
        arr_.clear();
    }
    arr_.push_back(std::move(value));
}

// --- serialization ---------------------------------------------------------------------------

namespace
{
void append_escaped(std::string& out, const std::string& s)
{
    out.push_back('"');
    for (const char c : s)
    {
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                std::array<char, 8> buf{};
                std::snprintf(buf.data(), buf.size(), "\\u%04x", static_cast<unsigned>(c) & 0xff);
                out += buf.data();
            }
            else
            {
                out.push_back(c);
            }
            break;
        }
    }
    out.push_back('"');
}

void append_number(std::string& out, double value, bool integral)
{
    if (integral && std::isfinite(value))
    {
        const auto as_ll = static_cast<long long>(value);
        if (static_cast<double>(as_ll) == value)
        {
            out += std::to_string(as_ll);
            return;
        }
    }
    // Shortest representation that round-trips: try increasing precision until strtod recovers the
    // exact double (so 0.1 prints as "0.1", not "0.10000000000000001") — cleaner user-facing JSON
    // with no loss of fidelity.
    std::array<char, 32> buf{};
    for (const int precision : {15, 16, 17})
    {
        std::snprintf(buf.data(), buf.size(), "%.*g", precision, value);
        if (std::strtod(buf.data(), nullptr) == value)
            break;
    }
    out += buf.data();
}
} // namespace

std::string Json::dump(int indent) const
{
    std::string out;
    dump_to(out, indent, 0);
    return out;
}

void Json::dump_to(std::string& out, int indent, int depth) const
{
    const bool pretty = indent > 0;
    const auto newline_indent = [&](int d)
    {
        if (pretty)
        {
            out.push_back('\n');
            out.append(static_cast<std::size_t>(indent) * static_cast<std::size_t>(d), ' ');
        }
    };

    switch (type_)
    {
    case Type::null:
        out += "null";
        break;
    case Type::boolean:
        out += bool_ ? "true" : "false";
        break;
    case Type::number:
        append_number(out, num_, integral_);
        break;
    case Type::string:
        append_escaped(out, str_);
        break;
    case Type::array:
        if (arr_.empty())
        {
            out += "[]";
            break;
        }
        out.push_back('[');
        for (std::size_t i = 0; i < arr_.size(); ++i)
        {
            if (i != 0)
                out.push_back(',');
            newline_indent(depth + 1);
            arr_[i].dump_to(out, indent, depth + 1);
        }
        newline_indent(depth);
        out.push_back(']');
        break;
    case Type::object:
        if (obj_.empty())
        {
            out += "{}";
            break;
        }
        out.push_back('{');
        for (std::size_t i = 0; i < obj_.size(); ++i)
        {
            if (i != 0)
                out.push_back(',');
            newline_indent(depth + 1);
            append_escaped(out, obj_[i].first);
            out.push_back(':');
            if (pretty)
                out.push_back(' ');
            obj_[i].second.dump_to(out, indent, depth + 1);
        }
        newline_indent(depth);
        out.push_back('}');
        break;
    }
}

// --- parsing ---------------------------------------------------------------------------------

namespace
{
class Parser
{
public:
    explicit Parser(const std::string& text) : text_(text) {}

    Json parse_document()
    {
        skip_ws();
        Json value = parse_value();
        skip_ws();
        if (pos_ != text_.size())
            fail("trailing characters after JSON document");
        return value;
    }

private:
    [[noreturn]] void fail(const char* what) const
    {
        throw std::runtime_error("JSON parse error at byte " + std::to_string(pos_) + ": " + what);
    }

    [[nodiscard]] char peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }

    char next()
    {
        if (pos_ >= text_.size())
            fail("unexpected end of input");
        return text_[pos_++];
    }

    void skip_ws()
    {
        while (pos_ < text_.size())
        {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                ++pos_;
            else
                break;
        }
    }

    void expect(char c)
    {
        if (next() != c)
            fail("expected a specific character");
    }

    Json parse_value()
    {
        skip_ws();
        const char c = peek();
        switch (c)
        {
        case '{':
            return parse_object();
        case '[':
            return parse_array();
        case '"':
            return Json(parse_string());
        case 't':
        case 'f':
            return parse_bool();
        case 'n':
            return parse_null();
        default:
            if (c == '-' || (c >= '0' && c <= '9'))
                return parse_number();
            fail("unexpected token");
        }
    }

    Json parse_object()
    {
        expect('{');
        Json obj = Json::object();
        skip_ws();
        if (peek() == '}')
        {
            ++pos_;
            return obj;
        }
        while (true)
        {
            skip_ws();
            if (peek() != '"')
                fail("expected string key in object");
            std::string key = parse_string();
            skip_ws();
            expect(':');
            obj.set(std::move(key), parse_value());
            skip_ws();
            const char c = next();
            if (c == '}')
                break;
            if (c != ',')
                fail("expected ',' or '}' in object");
        }
        return obj;
    }

    Json parse_array()
    {
        expect('[');
        Json arr = Json::array();
        skip_ws();
        if (peek() == ']')
        {
            ++pos_;
            return arr;
        }
        while (true)
        {
            arr.push_back(parse_value());
            skip_ws();
            const char c = next();
            if (c == ']')
                break;
            if (c != ',')
                fail("expected ',' or ']' in array");
        }
        return arr;
    }

    std::string parse_string()
    {
        expect('"');
        std::string out;
        while (true)
        {
            const char c = next();
            if (c == '"')
                break;
            if (c == '\\')
            {
                const char esc = next();
                switch (esc)
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
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'u':
                    out += parse_unicode_escape();
                    break;
                default:
                    fail("invalid escape sequence");
                }
            }
            else
            {
                out.push_back(c);
            }
        }
        return out;
    }

    // Decode a \uXXXX escape into UTF-8. Surrogate pairs (𐀀) are combined.
    std::string parse_unicode_escape()
    {
        unsigned cp = read_hex4();
        if (cp >= 0xD800 && cp <= 0xDBFF)
        {
            if (next() != '\\' || next() != 'u')
                fail("expected low surrogate after high surrogate");
            const unsigned low = read_hex4();
            if (low < 0xDC00 || low > 0xDFFF)
                fail("invalid low surrogate");
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
        }
        return encode_utf8(cp);
    }

    unsigned read_hex4()
    {
        unsigned value = 0;
        for (int i = 0; i < 4; ++i)
        {
            const char c = next();
            value <<= 4;
            if (c >= '0' && c <= '9')
                value |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f')
                value |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                value |= static_cast<unsigned>(c - 'A' + 10);
            else
                fail("invalid hex digit in \\u escape");
        }
        return value;
    }

    static std::string encode_utf8(unsigned cp)
    {
        std::string out;
        if (cp <= 0x7F)
        {
            out.push_back(static_cast<char>(cp));
        }
        else if (cp <= 0x7FF)
        {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else if (cp <= 0xFFFF)
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
        return out;
    }

    Json parse_bool()
    {
        if (text_.compare(pos_, 4, "true") == 0)
        {
            pos_ += 4;
            return Json(true);
        }
        if (text_.compare(pos_, 5, "false") == 0)
        {
            pos_ += 5;
            return Json(false);
        }
        fail("invalid literal");
    }

    Json parse_null()
    {
        if (text_.compare(pos_, 4, "null") == 0)
        {
            pos_ += 4;
            return Json(nullptr);
        }
        fail("invalid literal");
    }

    Json parse_number()
    {
        const std::size_t start = pos_;
        bool is_integral = true;
        if (peek() == '-')
            ++pos_;
        while (pos_ < text_.size())
        {
            const char c = text_[pos_];
            if (c >= '0' && c <= '9')
            {
                ++pos_;
            }
            else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-')
            {
                is_integral = false;
                ++pos_;
            }
            else
            {
                break;
            }
        }
        const std::string token = text_.substr(start, pos_ - start);
        try
        {
            if (is_integral)
                return Json(static_cast<std::int64_t>(std::stoll(token)));
            return Json(std::stod(token));
        }
        catch (const std::exception&)
        {
            fail("malformed number");
        }
    }

    const std::string& text_;
    std::size_t pos_ = 0;
};
} // namespace

Json Json::parse(const std::string& text)
{
    Parser parser(text);
    return parser.parse_document();
}

} // namespace context::editor::contract
