// Query-language implementation (see query_language.h): the R-CLI-012 grammar, its recursive-descent
// parser + AST, the total-ordering comparator, the unified cursor codec, and the string semantics.

#include "context/editor/contract/query_language.h"

#include <array>
#include <cctype>
#include <cstddef>
#include <exception>
#include <limits>
#include <string>

namespace context::editor::contract
{

// ==================================================================================================
// operator metadata
// ==================================================================================================
std::string_view compare_op_token(CompareOp op)
{
    switch (op)
    {
    case CompareOp::eq:
        return "==";
    case CompareOp::ne:
        return "!=";
    case CompareOp::lt:
        return "<";
    case CompareOp::le:
        return "<=";
    case CompareOp::gt:
        return ">";
    case CompareOp::ge:
        return ">=";
    case CompareOp::has:
        return "has";
    case CompareOp::contains:
        return "contains";
    case CompareOp::starts_with:
        return "startswith";
    case CompareOp::ends_with:
        return "endswith";
    case CompareOp::matches:
        return "matches";
    }
    return ""; // unreachable: the switch is total over CompareOp
}

std::string_view compare_op_class(CompareOp op)
{
    switch (op)
    {
    case CompareOp::eq:
    case CompareOp::ne:
        return "equality";
    case CompareOp::lt:
    case CompareOp::le:
    case CompareOp::gt:
    case CompareOp::ge:
        return "range";
    case CompareOp::has:
        return "existence";
    case CompareOp::contains:
    case CompareOp::starts_with:
    case CompareOp::ends_with:
    case CompareOp::matches:
        return "string-match";
    }
    return ""; // unreachable
}

// ==================================================================================================
// string semantics (R-CLI-012: NFC normalization + explicit case handling)
// ==================================================================================================
namespace
{
// Continuation byte of a UTF-8 multi-byte sequence: 10xxxxxx.
bool is_utf8_cont(unsigned char c)
{
    return (c & 0xC0u) == 0x80u;
}
} // namespace

bool is_valid_utf8(std::string_view text)
{
    std::size_t i = 0;
    const std::size_t n = text.size();
    while (i < n)
    {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 0x80u)
        {
            i += 1; // ASCII
            continue;
        }
        std::size_t len = 0;
        std::uint32_t cp = 0;
        std::uint32_t lo = 0;
        if ((c & 0xE0u) == 0xC0u)
        {
            len = 2;
            cp = c & 0x1Fu;
            lo = 0x80u; // reject overlong 2-byte forms
        }
        else if ((c & 0xF0u) == 0xE0u)
        {
            len = 3;
            cp = c & 0x0Fu;
            lo = 0x800u;
        }
        else if ((c & 0xF8u) == 0xF0u)
        {
            len = 4;
            cp = c & 0x07u;
            lo = 0x10000u;
        }
        else
        {
            return false; // a stray continuation byte or a 5/6-byte lead
        }
        if (i + len > n)
            return false; // truncated
        for (std::size_t k = 1; k < len; ++k)
        {
            const auto cc = static_cast<unsigned char>(text[i + k]);
            if (!is_utf8_cont(cc))
                return false;
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (cp < lo)
            return false; // overlong encoding
        if (cp > 0x10FFFFu)
            return false; // beyond Unicode
        if (cp >= 0xD800u && cp <= 0xDFFFu)
            return false; // UTF-16 surrogate half, illegal in UTF-8
        i += len;
    }
    return true;
}

std::string ascii_case_fold(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (const char ch : text)
    {
        const auto c = static_cast<unsigned char>(ch);
        out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : ch);
    }
    return out;
}

std::optional<std::string> normalize_nfc(std::string_view text)
{
    if (!is_valid_utf8(text))
        return std::nullopt;
    // v1 scope (published in the descriptor): ASCII is invariant under NFC; a non-ASCII input is
    // validated and assumed already NFC. The full canonical decomposition+composition table is the
    // tracked follow-up — the SEMANTICS are contract now so activating it later is non-breaking.
    return std::string(text);
}

namespace
{
// Glob match with '*' (any run, incl. empty) and '?' (exactly one byte). Iterative backtracking so a
// pathological pattern cannot recurse without bound. Operates on already-normalized/folded bytes.
bool glob_match(std::string_view pat, std::string_view text)
{
    std::size_t p = 0;
    std::size_t t = 0;
    std::size_t star = std::string_view::npos;
    std::size_t mark = 0;
    while (t < text.size())
    {
        if (p < pat.size() && (pat[p] == '?' || pat[p] == text[t]))
        {
            ++p;
            ++t;
        }
        else if (p < pat.size() && pat[p] == '*')
        {
            star = p;
            mark = t;
            ++p;
        }
        else if (star != std::string_view::npos)
        {
            p = star + 1;
            ++mark;
            t = mark;
        }
        else
        {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*')
        ++p;
    return p == pat.size();
}
} // namespace

bool string_match(CompareOp op, std::string_view field_value, std::string_view pattern,
                  bool case_insensitive)
{
    const std::optional<std::string> nv = normalize_nfc(field_value);
    const std::optional<std::string> np = normalize_nfc(pattern);
    if (!nv.has_value() || !np.has_value())
        return false; // invalid UTF-8 never matches
    std::string value = *nv;
    std::string pat = *np;
    if (case_insensitive)
    {
        value = ascii_case_fold(value);
        pat = ascii_case_fold(pat);
    }
    switch (op)
    {
    case CompareOp::contains:
        return value.find(pat) != std::string::npos;
    case CompareOp::starts_with:
        return value.size() >= pat.size() && value.compare(0, pat.size(), pat) == 0;
    case CompareOp::ends_with:
        return value.size() >= pat.size() &&
               value.compare(value.size() - pat.size(), pat.size(), pat) == 0;
    case CompareOp::matches:
        return glob_match(pat, value);
    case CompareOp::eq:
    case CompareOp::ne:
    case CompareOp::lt:
    case CompareOp::le:
    case CompareOp::gt:
    case CompareOp::ge:
    case CompareOp::has:
        return false; // not a string-match operator
    }
    return false; // unreachable
}

// ==================================================================================================
// predicate parser: tokenizer + recursive descent over the R-CLI-012 grammar
// ==================================================================================================
namespace
{
// A field-path character: identifiers are [A-Za-z_][A-Za-z0-9_-]*, joined by '.' or "[<index>]".
bool is_ident_start(unsigned char c)
{
    return std::isalpha(c) != 0 || c == '_';
}
bool is_ident_char(unsigned char c)
{
    return std::isalnum(c) != 0 || c == '_' || c == '-';
}

// The recursive-descent parser. Byte-offset cursor over the input; on the FIRST error it records the
// code+message+offset and every production short-circuits (fail() is sticky).
class Parser
{
public:
    explicit Parser(std::string_view input) : src_(input) {}

    PredicateParse run()
    {
        skip_ws();
        if (at_end())
        {
            // Empty query == match-everything: an AND with no children (the identity predicate).
            PredicateParse ok;
            ok.ok = true;
            ok.predicate.kind = NodeKind::logical_and;
            return ok;
        }
        Predicate root = parse_or();
        skip_ws();
        if (!failed_ && !at_end())
            fail("query.syntax_error", "unexpected trailing input");
        PredicateParse out;
        if (failed_)
        {
            out.ok = false;
            out.error_code = code_;
            out.message = msg_;
            out.error_offset = err_pos_;
            return out;
        }
        out.ok = true;
        out.predicate = std::move(root);
        return out;
    }

private:
    // disjunction = conjunction , { "or" , conjunction }
    Predicate parse_or()
    {
        Predicate left = parse_and();
        std::vector<Predicate> terms;
        while (!failed_)
        {
            skip_ws();
            if (!match_keyword("or"))
                break;
            if (terms.empty())
                terms.push_back(std::move(left));
            terms.push_back(parse_and());
        }
        if (terms.empty())
            return left;
        Predicate node;
        node.kind = NodeKind::logical_or;
        node.children = std::move(terms);
        return node;
    }

    // conjunction = negation , { "and" , negation }
    Predicate parse_and()
    {
        Predicate left = parse_not();
        std::vector<Predicate> terms;
        while (!failed_)
        {
            skip_ws();
            if (!match_keyword("and"))
                break;
            if (terms.empty())
                terms.push_back(std::move(left));
            terms.push_back(parse_not());
        }
        if (terms.empty())
            return left;
        Predicate node;
        node.kind = NodeKind::logical_and;
        node.children = std::move(terms);
        return node;
    }

    // negation = [ "not" ] , primary
    Predicate parse_not()
    {
        skip_ws();
        if (match_keyword("not"))
        {
            // A repeated "not" prefix recurses one stack frame per token; bound it so a crafted
            // deeply-nested untrusted query fails cleanly instead of overflowing the call stack.
            if (!enter_depth())
                return {};
            Predicate child = parse_not();
            leave_depth();
            Predicate node;
            node.kind = NodeKind::logical_not;
            node.children.push_back(std::move(child));
            return node;
        }
        return parse_primary();
    }

    // primary = "(" , disjunction , ")" | predicate
    Predicate parse_primary()
    {
        skip_ws();
        if (peek() == '(')
        {
            ++pos_;
            // A "(" recurses back into the disjunction grammar; bound the nesting depth (same
            // untrusted-input rationale as parse_not) so unbalanced/deep parens cannot blow the stack.
            if (!enter_depth())
                return {};
            Predicate inner = parse_or();
            leave_depth();
            skip_ws();
            if (!consume(')'))
                fail("query.syntax_error", "expected ')'");
            return inner;
        }
        return parse_predicate_leaf();
    }

    // predicate = existence | stringmatch | comparison
    Predicate parse_predicate_leaf()
    {
        skip_ws();
        // A leading identifier may open a string-match/existence function call OR a field path.
        const std::size_t save = pos_;
        const std::string word = read_ident_run();
        if (word.empty())
        {
            fail("query.syntax_error", "expected a field path or predicate function");
            return {};
        }
        // Function-call forms: <fn> "(" field ["," string] ")".
        if (peek() == '(')
        {
            const FnKind fn = classify_fn(word);
            if (fn.kind == FnKind::none)
            {
                fail("query.unknown_operator", "unknown predicate function '" + word + "'");
                return {};
            }
            return parse_fn_call(fn);
        }
        // Otherwise `word` was the start of a field path for a comparison; rewind and read the path.
        pos_ = save;
        return parse_comparison();
    }

    struct FnKind
    {
        enum Kind
        {
            none,
            has,
            str
        } kind = none;
        CompareOp op = CompareOp::has;
        bool ci = false;
    };

    static FnKind classify_fn(const std::string& w)
    {
        FnKind f;
        if (w == "has")
        {
            f.kind = FnKind::has;
            return f;
        }
        struct Row
        {
            const char* name;
            CompareOp op;
            bool ci;
        };
        static const std::array<Row, 8> table = {{
            {"contains", CompareOp::contains, false},
            {"icontains", CompareOp::contains, true},
            {"startswith", CompareOp::starts_with, false},
            {"istartswith", CompareOp::starts_with, true},
            {"endswith", CompareOp::ends_with, false},
            {"iendswith", CompareOp::ends_with, true},
            {"matches", CompareOp::matches, false},
            {"imatches", CompareOp::matches, true},
        }};
        for (const Row& r : table)
        {
            if (w == r.name)
            {
                f.kind = FnKind::str;
                f.op = r.op;
                f.ci = r.ci;
                return f;
            }
        }
        return f; // none
    }

    Predicate parse_fn_call(const FnKind& fn)
    {
        if (!consume('('))
        {
            fail("query.syntax_error", "expected '('");
            return {};
        }
        skip_ws();
        const std::string field = read_field_path();
        if (field.empty() && !failed_)
            fail("query.syntax_error", "expected a field path inside the function call");
        Predicate node;
        node.kind = NodeKind::comparison;
        node.field = field;
        if (fn.kind == FnKind::has)
        {
            node.op = CompareOp::has;
            skip_ws();
            if (!consume(')'))
                fail("query.syntax_error", "has(field) takes exactly one argument");
            return node;
        }
        // string-match: a second string argument.
        node.op = fn.op;
        node.case_insensitive = fn.ci;
        skip_ws();
        if (!consume(','))
            fail("query.syntax_error", "expected ',' before the match string");
        skip_ws();
        node.value = read_string_literal();
        skip_ws();
        if (!consume(')'))
            fail("query.syntax_error", "expected ')'");
        return node;
    }

    // comparison = field , cmp-op , literal
    Predicate parse_comparison()
    {
        Predicate node;
        node.kind = NodeKind::comparison;
        node.field = read_field_path();
        if (node.field.empty())
        {
            if (!failed_)
                fail("query.syntax_error", "expected a field path");
            return node;
        }
        skip_ws();
        node.op = read_cmp_op();
        if (failed_)
            return node;
        skip_ws();
        node.value = read_literal();
        return node;
    }

    CompareOp read_cmp_op()
    {
        // Two-char operators first so "<=" is not read as "<".
        if (starts_with_at("=="))
        {
            pos_ += 2;
            return CompareOp::eq;
        }
        if (starts_with_at("!="))
        {
            pos_ += 2;
            return CompareOp::ne;
        }
        if (starts_with_at("<="))
        {
            pos_ += 2;
            return CompareOp::le;
        }
        if (starts_with_at(">="))
        {
            pos_ += 2;
            return CompareOp::ge;
        }
        if (peek() == '<')
        {
            ++pos_;
            return CompareOp::lt;
        }
        if (peek() == '>')
        {
            ++pos_;
            return CompareOp::gt;
        }
        fail("query.unknown_operator", "expected a comparison operator (== != < <= > >=)");
        return CompareOp::eq;
    }

    // literal = number | string | "true" | "false" | "null"
    Json read_literal()
    {
        const char c = peek();
        if (c == '"')
            return read_string_literal();
        if (c == '-' || (std::isdigit(static_cast<unsigned char>(c)) != 0))
            return read_number_literal();
        const std::string word = read_ident_run();
        if (word == "true")
            return Json(true);
        if (word == "false")
            return Json(false);
        if (word == "null")
            return Json(nullptr);
        fail("query.syntax_error", "expected a literal (number, string, true, false, or null)");
        return Json(nullptr);
    }

    Json read_string_literal()
    {
        if (peek() != '"')
        {
            fail("query.syntax_error", "expected a double-quoted string");
            return Json(std::string());
        }
        ++pos_; // opening quote
        std::string out;
        while (!at_end())
        {
            const char ch = src_[pos_];
            if (ch == '"')
            {
                ++pos_;
                if (!is_valid_utf8(out))
                {
                    fail("query.syntax_error", "string literal is not valid UTF-8");
                    return Json(std::string());
                }
                return Json(std::move(out));
            }
            if (ch == '\\')
            {
                ++pos_;
                if (at_end())
                    break;
                const char esc = src_[pos_];
                switch (esc)
                {
                case '"':
                    out.push_back('"');
                    break;
                case '\\':
                    out.push_back('\\');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                default:
                    fail("query.syntax_error", "invalid string escape");
                    return Json(std::string());
                }
                ++pos_;
                continue;
            }
            out.push_back(ch);
            ++pos_;
        }
        fail("query.syntax_error", "unterminated string literal");
        return Json(std::string());
    }

    Json read_number_literal()
    {
        const std::size_t start = pos_;
        if (peek() == '-')
            ++pos_;
        bool any_digit = false;
        while (!at_end() && std::isdigit(static_cast<unsigned char>(src_[pos_])) != 0)
        {
            ++pos_;
            any_digit = true;
        }
        bool is_float = false;
        if (!at_end() && src_[pos_] == '.')
        {
            is_float = true;
            ++pos_;
            while (!at_end() && std::isdigit(static_cast<unsigned char>(src_[pos_])) != 0)
            {
                ++pos_;
                any_digit = true;
            }
        }
        if (!any_digit)
        {
            fail("query.syntax_error", "malformed number literal");
            return Json(nullptr);
        }
        const std::string text(src_.substr(start, pos_ - start));
        // A client-supplied literal is untrusted: an out-of-range value throws from stod/stoll — fail
        // it as a syntax error rather than letting the exception escape the parser.
        try
        {
            if (is_float)
                return Json(std::stod(text));
            return Json(static_cast<std::int64_t>(std::stoll(text)));
        }
        catch (const std::exception&)
        {
            fail("query.syntax_error", "number literal is out of range");
            return Json(nullptr);
        }
    }

    // field = ident , { ("." , ident) | ("[" , index , "]") }
    std::string read_field_path()
    {
        skip_ws();
        std::string out;
        if (at_end() || !is_ident_start(static_cast<unsigned char>(src_[pos_])))
            return out;
        out += read_ident_run();
        while (!at_end())
        {
            const char c = src_[pos_];
            if (c == '.')
            {
                out.push_back('.');
                ++pos_;
                const std::string seg = read_ident_run();
                if (seg.empty())
                {
                    fail("query.syntax_error", "expected a field segment after '.'");
                    return out;
                }
                out += seg;
            }
            else if (c == '[')
            {
                out.push_back('[');
                ++pos_;
                bool any = false;
                while (!at_end() && std::isdigit(static_cast<unsigned char>(src_[pos_])) != 0)
                {
                    out.push_back(src_[pos_]);
                    ++pos_;
                    any = true;
                }
                if (!any || at_end() || src_[pos_] != ']')
                {
                    fail("query.syntax_error", "expected '[<index>]'");
                    return out;
                }
                out.push_back(']');
                ++pos_;
            }
            else
            {
                break;
            }
        }
        return out;
    }

    std::string read_ident_run()
    {
        std::string out;
        if (at_end() || !is_ident_start(static_cast<unsigned char>(src_[pos_])))
            return out;
        out.push_back(src_[pos_]);
        ++pos_;
        while (!at_end() && is_ident_char(static_cast<unsigned char>(src_[pos_])))
        {
            out.push_back(src_[pos_]);
            ++pos_;
        }
        return out;
    }

    // Match a bare keyword (not followed by an ident char) at the cursor; advance on success.
    bool match_keyword(std::string_view kw)
    {
        if (!starts_with_at(kw))
            return false;
        const std::size_t after = pos_ + kw.size();
        if (after < src_.size() && is_ident_char(static_cast<unsigned char>(src_[after])))
            return false; // "andy" is not the keyword "and"
        pos_ = after;
        return true;
    }

    bool starts_with_at(std::string_view s) const
    {
        return src_.substr(pos_).rfind(s, 0) == 0;
    }

    // helpers
    char peek() const { return at_end() ? '\0' : src_[pos_]; }
    bool at_end() const { return pos_ >= src_.size(); }
    void skip_ws()
    {
        while (!at_end() && std::isspace(static_cast<unsigned char>(src_[pos_])) != 0)
            ++pos_;
    }
    bool consume(char c)
    {
        if (peek() != c)
            return false;
        ++pos_;
        return true;
    }
    void fail(std::string code, std::string message)
    {
        if (failed_)
            return; // sticky: keep the first error
        failed_ = true;
        code_ = std::move(code);
        msg_ = std::move(message);
        err_pos_ = pos_;
    }

    // Bounded-recursion guard. A client query is untrusted (see the header contract), so a crafted
    // deeply-nested predicate (many '(' or repeated "not") must fail with a code rather than recurse
    // until the call stack overflows. enter_depth() fails the parse past the cap; callers pair it with
    // leave_depth() on the success path.
    bool enter_depth()
    {
        if (++depth_ > kMaxDepth)
        {
            fail("query.syntax_error", "predicate nesting too deep");
            return false;
        }
        return true;
    }
    void leave_depth()
    {
        if (depth_ > 0)
            --depth_;
    }

    static constexpr std::size_t kMaxDepth = 128;

    std::string_view src_;
    std::size_t pos_ = 0;
    bool failed_ = false;
    std::string code_;
    std::string msg_;
    std::size_t err_pos_ = 0;
    std::size_t depth_ = 0;
};
} // namespace

PredicateParse parse_predicate(std::string_view input)
{
    Parser parser(input);
    return parser.run();
}

// ==================================================================================================
// total ordering (R-CLI-012)
// ==================================================================================================
OrderParse parse_order(std::string_view input)
{
    OrderParse out;
    std::vector<OrderTerm> terms;
    std::size_t i = 0;
    const std::size_t n = input.size();
    auto skip_ws = [&] {
        while (i < n && std::isspace(static_cast<unsigned char>(input[i])) != 0)
            ++i;
    };
    skip_ws();
    while (i < n)
    {
        // key = "@id" | field-path
        std::string key;
        if (input[i] == '@')
        {
            key.push_back('@');
            ++i;
            while (i < n && is_ident_char(static_cast<unsigned char>(input[i])))
            {
                key.push_back(input[i]);
                ++i;
            }
        }
        else
        {
            while (i < n)
            {
                const char c = input[i];
                if (is_ident_char(static_cast<unsigned char>(c)) || c == '.' || c == '[' || c == ']')
                {
                    key.push_back(c);
                    ++i;
                }
                else
                {
                    break;
                }
            }
        }
        if (key.empty())
        {
            out.error_code = "query.syntax_error";
            out.message = "expected an order-by key";
            out.error_offset = i;
            return out;
        }
        skip_ws();
        bool descending = false;
        // optional asc | desc
        auto read_dir_word = [&](std::string_view w) {
            if (input.substr(i).rfind(w, 0) != 0)
                return false;
            const std::size_t after = i + w.size();
            if (after < n && is_ident_char(static_cast<unsigned char>(input[after])))
                return false;
            i = after;
            return true;
        };
        if (read_dir_word("desc"))
            descending = true;
        else if (read_dir_word("asc"))
            descending = false;
        terms.push_back(OrderTerm{key, descending});
        skip_ws();
        if (i < n)
        {
            if (input[i] != ',')
            {
                out.error_code = "query.syntax_error";
                out.message = "expected ',' between order-by terms";
                out.error_offset = i;
                return out;
            }
            ++i;
            skip_ws();
            if (i >= n) // a trailing comma with no following term
            {
                out.error_code = "query.syntax_error";
                out.message = "trailing ',' with no order-by term";
                out.error_offset = i;
                return out;
            }
        }
    }
    // Guarantee a TOTAL order: append an "@id asc" tiebreaker unless the caller already sorts by @id.
    bool has_id = false;
    for (const OrderTerm& t : terms)
        has_id = has_id || t.key == std::string(kEntityIdKey);
    if (!has_id)
        terms.push_back(OrderTerm{std::string(kEntityIdKey), false});
    out.ok = true;
    out.terms = std::move(terms);
    return out;
}

namespace
{
// Resolve a dotted/indexed field path against a JSON row. Returns nullptr when any segment is
// absent. Only object-member and array-index steps are honored (the query surface's addressing).
const Json* resolve_path(const Json& row, const std::string& path)
{
    const Json* cur = &row;
    std::size_t i = 0;
    const std::size_t n = path.size();
    while (i < n)
    {
        if (path[i] == '[')
        {
            ++i;
            std::size_t idx = 0;
            bool any = false;
            while (i < n && path[i] >= '0' && path[i] <= '9')
            {
                const auto digit = static_cast<std::size_t>(path[i] - '0');
                // Guard the accumulation against overflow (mirrors parse_u64's untrusted-input
                // posture): an absurdly long index wraps std::size_t otherwise. Overflow => no match.
                if (idx > (std::numeric_limits<std::size_t>::max() - digit) / 10u)
                    return nullptr;
                idx = idx * 10u + digit;
                ++i;
                any = true;
            }
            if (!any || i >= n || path[i] != ']')
                return nullptr;
            ++i;
            if (!cur->is_array() || idx >= cur->size())
                return nullptr;
            cur = &cur->at(idx);
        }
        else
        {
            if (path[i] == '.')
                ++i;
            std::string seg;
            while (i < n && path[i] != '.' && path[i] != '[')
            {
                seg.push_back(path[i]);
                ++i;
            }
            if (seg.empty() || !cur->is_object() || !cur->contains(seg))
                return nullptr;
            cur = &cur->at(seg);
        }
    }
    return cur;
}

// Order two scalar JSON values of a single term. Numbers compare numerically; strings by NFC
// code-point (byte) order; booleans false<true; a type mismatch orders by a stable type rank so the
// comparison is total. Returns <0 / 0 / >0.
int compare_scalar(const Json& a, const Json& b)
{
    auto rank = [](const Json& j) -> int {
        switch (j.type())
        {
        case Json::Type::null:
            return 0;
        case Json::Type::boolean:
            return 1;
        case Json::Type::number:
            return 2;
        case Json::Type::string:
            return 3;
        case Json::Type::array:
            return 4;
        case Json::Type::object:
            return 5;
        }
        return 6;
    };
    const int ra = rank(a);
    const int rb = rank(b);
    if (ra != rb)
        return ra < rb ? -1 : 1;
    if (a.is_number())
    {
        const double da = a.as_number();
        const double db = b.as_number();
        if (da < db)
            return -1;
        if (da > db)
            return 1;
        return 0;
    }
    if (a.is_bool())
    {
        const int ba = a.as_bool() ? 1 : 0;
        const int bb = b.as_bool() ? 1 : 0;
        return ba - bb;
    }
    if (a.is_string())
    {
        const std::string na = normalize_nfc(a.as_string()).value_or(a.as_string());
        const std::string nb = normalize_nfc(b.as_string()).value_or(b.as_string());
        return na.compare(nb) < 0 ? -1 : (na.compare(nb) > 0 ? 1 : 0);
    }
    return 0; // null == null; composite ranks already separated above
}
} // namespace

int compare_rows(const Json& lhs, const Json& rhs, const std::vector<OrderTerm>& order)
{
    for (const OrderTerm& term : order)
    {
        const Json* a = resolve_path(lhs, term.key);
        const Json* b = resolve_path(rhs, term.key);
        // Missing keys sort LAST (defined, stable) regardless of the term's direction.
        if (a == nullptr && b == nullptr)
            continue;
        if (a == nullptr)
            return 1;
        if (b == nullptr)
            return -1;
        int c = compare_scalar(*a, *b);
        if (c != 0)
            return term.descending ? -c : c;
    }
    return 0;
}

// ==================================================================================================
// unified cursor codec (R-CLI-012 unified with R-BRIDGE-008)
// ==================================================================================================
namespace
{
bool valid_incarnation_char(char ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
           ch == '.' || ch == '_' || ch == '-';
}
bool valid_incarnation(std::string_view id)
{
    if (id.empty())
        return false;
    for (const char ch : id)
        if (!valid_incarnation_char(ch))
            return false;
    return true;
}
std::optional<std::uint64_t> parse_u64(std::string_view text)
{
    if (text.empty() || text.size() > 20)
        return std::nullopt;
    std::uint64_t value = 0;
    for (const char ch : text)
    {
        if (ch < '0' || ch > '9')
            return std::nullopt;
        const auto digit = static_cast<std::uint64_t>(ch - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u)
            return std::nullopt;
        value = value * 10u + digit;
    }
    return value;
}
std::string to_hex(std::string_view bytes)
{
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const char ch : bytes)
    {
        const auto c = static_cast<unsigned char>(ch);
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 0x0Fu]);
    }
    return out;
}
std::optional<std::string> from_hex(std::string_view hex)
{
    if (hex.size() % 2 != 0)
        return std::nullopt;
    auto nibble = [](char ch) -> int {
        if (ch >= '0' && ch <= '9')
            return ch - '0';
        if (ch >= 'a' && ch <= 'f')
            return ch - 'a' + 10;
        return -1;
    };
    std::string out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2)
    {
        const int hi = nibble(hex[i]);
        const int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0)
            return std::nullopt;
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return out;
}
} // namespace

std::string QueryCursor::to_token() const
{
    return std::string(kCursorUriScheme) + "://v0/" + incarnation_id + "/" +
           std::to_string(generation) + "/" + std::to_string(seq) + "?after=" + to_hex(after_id);
}

std::optional<QueryCursor> QueryCursor::parse(std::string_view token)
{
    const std::string prefix = std::string(kCursorUriScheme) + "://v0/";
    if (token.rfind(prefix, 0) != 0)
        return std::nullopt;
    std::string_view rest = token.substr(prefix.size());

    const std::size_t s1 = rest.find('/');
    if (s1 == std::string_view::npos)
        return std::nullopt;
    const std::string_view incarnation = rest.substr(0, s1);
    if (!valid_incarnation(incarnation))
        return std::nullopt;
    rest = rest.substr(s1 + 1);

    const std::size_t s2 = rest.find('/');
    if (s2 == std::string_view::npos)
        return std::nullopt;
    const std::optional<std::uint64_t> generation = parse_u64(rest.substr(0, s2));
    rest = rest.substr(s2 + 1);

    const std::size_t q = rest.find("?after=");
    if (q == std::string_view::npos)
        return std::nullopt;
    const std::optional<std::uint64_t> seq = parse_u64(rest.substr(0, q));
    const std::optional<std::string> after = from_hex(rest.substr(q + 7));
    if (!generation.has_value() || !seq.has_value() || !after.has_value())
        return std::nullopt;

    QueryCursor cursor;
    cursor.incarnation_id = std::string(incarnation);
    cursor.generation = *generation;
    cursor.seq = *seq;
    cursor.after_id = *after;
    return cursor;
}

// ==================================================================================================
// EBNF grammar + describe descriptor (R-CLI-013)
// ==================================================================================================
std::string_view query_ebnf()
{
    // The published grammar (also written verbatim to docs/query-language.md). ISO/IEC 14977 EBNF.
    static const std::string ebnf =
        "query        = disjunction ;\n"
        "disjunction  = conjunction , { \"or\" , conjunction } ;\n"
        "conjunction  = negation , { \"and\" , negation } ;\n"
        "negation     = [ \"not\" ] , primary ;\n"
        "primary      = \"(\" , disjunction , \")\" | predicate ;\n"
        "predicate    = existence | string-match | comparison ;\n"
        "existence    = \"has\" , \"(\" , field , \")\" ;\n"
        "string-match = str-fn , \"(\" , field , \",\" , string , \")\" ;\n"
        "str-fn       = \"contains\" | \"startswith\" | \"endswith\" | \"matches\"\n"
        "             | \"icontains\" | \"istartswith\" | \"iendswith\" | \"imatches\" ;\n"
        "comparison   = field , cmp-op , literal ;\n"
        "cmp-op       = \"==\" | \"!=\" | \"<=\" | \">=\" | \"<\" | \">\" ;\n"
        "field        = ident , { (\".\" , ident) | (\"[\" , index , \"]\") } ;\n"
        "literal      = number | string | \"true\" | \"false\" | \"null\" ;\n"
        "ident        = ( letter | \"_\" ) , { letter | digit | \"_\" | \"-\" } ;\n"
        "string       = '\"' , { char | escape } , '\"' ;\n"
        "escape       = \"\\\\\" , ( '\"' | \"\\\\\" | \"n\" | \"t\" ) ;\n"
        "number       = [ \"-\" ] , digit , { digit } , [ \".\" , { digit } ] ;\n"
        "index        = digit , { digit } ;\n"
        "order-by     = order-term , { \",\" , order-term } ;\n"
        "order-term   = ( field | \"@id\" ) , [ \"asc\" | \"desc\" ] ;\n";
    return ebnf;
}

Json query_language_descriptor()
{
    Json out = Json::object();
    out.set("requirement", Json(std::string("R-CLI-012")));
    out.set("stable", Json(false)); // protocolMajor 0 until the M3 freeze
    out.set("ebnf", Json(std::string(query_ebnf())));

    // The enumerated operator set: one entry per operator with its published token + class + the
    // case-insensitive form where one exists.
    Json operators = Json::array();
    struct OpRow
    {
        CompareOp op;
        const char* ci_form; // the i-variant spelling, or "" when the op has none
    };
    static const std::array<OpRow, 11> op_rows = {{
        {CompareOp::eq, ""},
        {CompareOp::ne, ""},
        {CompareOp::lt, ""},
        {CompareOp::le, ""},
        {CompareOp::gt, ""},
        {CompareOp::ge, ""},
        {CompareOp::has, ""},
        {CompareOp::contains, "icontains"},
        {CompareOp::starts_with, "istartswith"},
        {CompareOp::ends_with, "iendswith"},
        {CompareOp::matches, "imatches"},
    }};
    for (const OpRow& r : op_rows)
    {
        Json e = Json::object();
        e.set("token", Json(std::string(compare_op_token(r.op))));
        e.set("class", Json(std::string(compare_op_class(r.op))));
        if (r.ci_form[0] != '\0')
            e.set("caseInsensitiveForm", Json(std::string(r.ci_form)));
        operators.push_back(std::move(e));
    }
    out.set("operators", std::move(operators));

    // Boolean composition (structural in the AST, not a CompareOp).
    Json booleans = Json::array();
    booleans.push_back(Json(std::string("and")));
    booleans.push_back(Json(std::string("or")));
    booleans.push_back(Json(std::string("not")));
    out.set("booleanOperators", std::move(booleans));

    Json ordering = Json::object();
    ordering.set("defaultKey", Json(std::string(kEntityIdKey)));
    ordering.set("total", Json(true));
    ordering.set("rule", Json(std::string(
                                 "results carry a mandatory TOTAL order; the sort always ends with an "
                                 "'@id asc' tiebreaker (the entity id is unique), so paginated results "
                                 "never reorder across pages. Missing keys sort last.")));
    out.set("ordering", std::move(ordering));

    // The unified cursor (R-CLI-012 unified with R-BRIDGE-008): ONE shape across event catch-up and
    // query paging.
    Json cursor = Json::object();
    cursor.set("uriScheme", Json(std::string(kCursorUriScheme)));
    cursor.set("tokenForm", Json(std::string("context-cur://v0/<incarnationId>/<generation>/<seq>"
                                             "?after=<hex(entityId)>")));
    Json cursor_shape = Json::object();
    cursor_shape.set("incarnationId",
                     Json(std::string("string — the daemon incarnation; a restart invalidates the "
                                      "cursor (forces a fresh snapshot), shared with R-BRIDGE-008")));
    cursor_shape.set("seq", Json(std::string("number — the R-BRIDGE-008 monotonic, totally-ordered "
                                             "event seq (event catch-up position)")));
    cursor_shape.set("generation",
                     Json(std::string("number — the derived-world generation this page is consistent "
                                      "to (every page of one paged query reads one generation)")));
    cursor_shape.set("after", Json(std::string("string — keyset position: the last entity id "
                                               "returned under the total order; empty for an event "
                                               "cursor")));
    cursor.set("shape", std::move(cursor_shape));
    cursor.set("reconciliation",
               Json(std::string("ONE cursor model, not two: {incarnationId, seq} is the R-BRIDGE-008 "
                                "event cursor verbatim; an event cursor is a query cursor with an "
                                "empty 'after'. Query paging adds generation + after for "
                                "mutation-stable keyset paging (R-CLI-017: a handle carries one "
                                "oversized payload, a cursor a paged sequence).")));
    out.set("cursor", std::move(cursor));

    Json strings = Json::object();
    strings.set("normalization", Json(std::string("NFC")));
    strings.set("caseHandling",
                Json(std::string("explicit per-operator: comparisons are case-SENSITIVE by default; "
                                 "the i-prefixed string-match forms (icontains/istartswith/"
                                 "iendswith/imatches) are case-INSENSITIVE")));
    strings.set("v1Scope",
                Json(std::string("strict UTF-8 validation on every literal; ASCII is invariant under "
                                 "NFC and passes through; ASCII case-folding backs the i-forms. The "
                                 "full Unicode canonical decomposition+composition table and "
                                 "non-ASCII case-folding are the tracked follow-up — the SEMANTICS "
                                 "are contract now, so activating the full table is non-breaking.")));
    out.set("stringSemantics", std::move(strings));

    // The ONE language spans three surfaces — not three dialects (R-CLI-012).
    Json surfaces = Json::array();
    surfaces.push_back(Json(std::string("derived-world")));
    surfaces.push_back(Json(std::string("live-sim-state")));
    surfaces.push_back(Json(std::string("schema-introspection")));
    out.set("surfaces", std::move(surfaces));

    out.set("composesWith",
            Json(std::string("mutation verbs: `context query … | context set …` (the query result's "
                             "entity ids + provenance feed a following mutation), R-CLI-006")));
    return out;
}

} // namespace context::editor::contract
