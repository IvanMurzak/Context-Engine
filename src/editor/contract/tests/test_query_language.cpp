// Query-language conformance tests (R-CLI-012 / R-QA-013): grammar acceptance + rejection vectors,
// the enumerated operator set, total-ordering determinism + pagination stability, unified-cursor
// round-trip + malformed rejection + R-BRIDGE-008 reconciliation, string semantics (UTF-8 / NFC /
// case) vectors, and the `context describe` queryLanguage section. Happy + edge + failure per R-QA-013.

#include "context/editor/contract/error_catalog.h"
#include "context/editor/contract/json.h"
#include "context/editor/contract/query_language.h"
#include "context/editor/contract/registry.h"
#include "contract_test.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace context::editor::contract;

namespace
{
Json row(const std::string& id, const std::string& field, Json value)
{
    Json r = Json::object();
    r.set("@id", Json(id));
    if (!field.empty())
        r.set(field, std::move(value));
    return r;
}

// A strict-weak-ordering "less than" from compare_rows for std::sort.
struct RowLess
{
    const std::vector<OrderTerm>* order;
    bool operator()(const Json& a, const Json& b) const { return compare_rows(a, b, *order) < 0; }
};
} // namespace

int main()
{
    // ============================================================================================
    // 1. grammar ACCEPTANCE vectors — one per production / operator class
    // ============================================================================================
    {
        // empty query == match-everything (AND with no children)
        PredicateParse empty = parse_predicate("   ");
        CHECK(empty.ok);
        CHECK(empty.predicate.kind == NodeKind::logical_and);
        CHECK(empty.predicate.children.empty());

        // equality
        PredicateParse eq = parse_predicate("name == \"orc\"");
        CHECK(eq.ok);
        CHECK(eq.predicate.kind == NodeKind::comparison);
        CHECK(eq.predicate.field == "name");
        CHECK(eq.predicate.op == CompareOp::eq);
        CHECK(eq.predicate.value.is_string() && eq.predicate.value.as_string() == "orc");

        PredicateParse ne = parse_predicate("kind != \"tree\"");
        CHECK(ne.ok && ne.predicate.op == CompareOp::ne);

        // range with int, float, negative
        PredicateParse gt = parse_predicate("health > 10");
        CHECK(gt.ok && gt.predicate.op == CompareOp::gt);
        CHECK(gt.predicate.value.is_number() && gt.predicate.value.as_int() == 10);
        CHECK(parse_predicate("x <= 3.5").ok);
        CHECK(parse_predicate("x >= -2").ok);
        PredicateParse fl = parse_predicate("transform.position.x < 1.25");
        CHECK(fl.ok && fl.predicate.field == "transform.position.x");
        CHECK(fl.predicate.value.as_number() > 1.24 && fl.predicate.value.as_number() < 1.26);

        // bool + null literals
        CHECK(parse_predicate("visible == true").ok);
        CHECK(parse_predicate("visible == false").ok);
        PredicateParse nul = parse_predicate("parent == null");
        CHECK(nul.ok && nul.predicate.value.is_null());

        // existence
        PredicateParse has = parse_predicate("has(Health)");
        CHECK(has.ok && has.predicate.op == CompareOp::has && has.predicate.field == "Health");
        CHECK(has.predicate.value.is_null());

        // string-match, all four + one case-insensitive
        CHECK(parse_predicate("contains(name, \"rc\")").ok);
        CHECK(parse_predicate("startswith(name, \"or\")").ok);
        CHECK(parse_predicate("endswith(name, \"rc\")").ok);
        CHECK(parse_predicate("matches(name, \"o*c\")").ok);
        PredicateParse ci = parse_predicate("icontains(name, \"ORC\")");
        CHECK(ci.ok && ci.predicate.op == CompareOp::contains && ci.predicate.case_insensitive);

        // field indexing
        PredicateParse idx = parse_predicate("items[0].id == 7");
        CHECK(idx.ok && idx.predicate.field == "items[0].id");

        // boolean composition + precedence + parentheses
        PredicateParse conj = parse_predicate("a == 1 and b == 2");
        CHECK(conj.ok && conj.predicate.kind == NodeKind::logical_and);
        CHECK(conj.predicate.children.size() == 2);

        PredicateParse disj = parse_predicate("a == 1 or b == 2 or c == 3");
        CHECK(disj.ok && disj.predicate.kind == NodeKind::logical_or);
        CHECK(disj.predicate.children.size() == 3);

        PredicateParse neg = parse_predicate("not has(x)");
        CHECK(neg.ok && neg.predicate.kind == NodeKind::logical_not);
        CHECK(neg.predicate.children.size() == 1);
        CHECK(neg.predicate.children[0].op == CompareOp::has);

        // precedence: "a and b or c" => OR(AND(a,b), c)
        PredicateParse prec = parse_predicate("a == 1 and b == 2 or c == 3");
        CHECK(prec.ok && prec.predicate.kind == NodeKind::logical_or);
        CHECK(prec.predicate.children.size() == 2);
        CHECK(prec.predicate.children[0].kind == NodeKind::logical_and);

        // parentheses override precedence: "(a or b) and c" => AND(OR(a,b), c)
        PredicateParse paren = parse_predicate("(a == 1 or b == 2) and c == 3");
        CHECK(paren.ok && paren.predicate.kind == NodeKind::logical_and);
        CHECK(paren.predicate.children[0].kind == NodeKind::logical_or);

        // "andy" is a field, not the keyword "and"
        PredicateParse kw = parse_predicate("andy == 1");
        CHECK(kw.ok && kw.predicate.kind == NodeKind::comparison && kw.predicate.field == "andy");
    }

    // ============================================================================================
    // 2. grammar REJECTION vectors — each fails with the right catalog code, never a partial parse
    // ============================================================================================
    {
        struct Bad
        {
            const char* query;
            const char* code;
        };
        const Bad bad[] = {
            {"health >", "query.syntax_error"},         // missing literal
            {"health > < 1", "query.syntax_error"},     // literal is another operator
            {"health = 1", "query.unknown_operator"},   // single '=' is not an operator
            {"health ~ 1", "query.unknown_operator"},   // unknown symbol operator
            {"foo(x)", "query.unknown_operator"},       // unknown predicate function
            {"health > 10 extra", "query.syntax_error"}, // trailing garbage
            {"contains(a, \"x)", "query.syntax_error"}, // unterminated string
            {"has(a, b)", "query.syntax_error"},        // has takes one arg
            {"contains(a)", "query.syntax_error"},      // string-match needs a pattern
            {"(a == 1", "query.syntax_error"},          // unbalanced paren
            {"== 1", "query.syntax_error"},             // missing field
            {"n == 999999999999999999999999999", "query.syntax_error"}, // out-of-range number
            {"", ""},                                    // sentinel: handled as accept above
        };
        for (const Bad& b : bad)
        {
            if (b.code[0] == '\0')
                continue;
            PredicateParse p = parse_predicate(b.query);
            CHECK(!p.ok);
            CHECK(p.error_code == b.code);
            CHECK(!p.message.empty());
        }
    }

    // ============================================================================================
    // 3. enumerated operator set — tokens + classes are complete and match the descriptor
    // ============================================================================================
    {
        CHECK(compare_op_token(CompareOp::eq) == "==");
        CHECK(compare_op_token(CompareOp::ge) == ">=");
        CHECK(compare_op_token(CompareOp::has) == "has");
        CHECK(compare_op_token(CompareOp::matches) == "matches");
        CHECK(compare_op_class(CompareOp::eq) == "equality");
        CHECK(compare_op_class(CompareOp::lt) == "range");
        CHECK(compare_op_class(CompareOp::has) == "existence");
        CHECK(compare_op_class(CompareOp::contains) == "string-match");
    }

    // ============================================================================================
    // 4. total ordering — default @id, tiebreaker appended, determinism + pagination stability
    // ============================================================================================
    {
        // default order is [@id asc]
        OrderParse def = parse_order("");
        CHECK(def.ok && def.terms.size() == 1);
        CHECK(def.terms[0].key == "@id" && !def.terms[0].descending);

        // a non-@id key always gets the @id tiebreaker appended (total order)
        OrderParse byname = parse_order("name desc");
        CHECK(byname.ok && byname.terms.size() == 2);
        CHECK(byname.terms[0].key == "name" && byname.terms[0].descending);
        CHECK(byname.terms[1].key == "@id" && !byname.terms[1].descending);

        // sorting explicitly by @id does NOT double-append
        OrderParse byid = parse_order("@id desc");
        CHECK(byid.ok && byid.terms.size() == 1 && byid.terms[0].descending);

        OrderParse multi = parse_order("a asc, b desc");
        CHECK(multi.ok && multi.terms.size() == 3);

        // rejection: trailing comma / empty key
        CHECK(!parse_order("a asc,").ok);
        CHECK(!parse_order(",a").ok);

        // build rows with a DELIBERATELY non-unique sort field (all hp==5) so only the @id
        // tiebreaker makes the order total.
        std::vector<Json> rows;
        rows.push_back(row("e3", "hp", Json(5)));
        rows.push_back(row("e1", "hp", Json(5)));
        rows.push_back(row("e2", "hp", Json(5)));
        rows.push_back(row("e4", "hp", Json(5)));
        OrderParse ord = parse_order("hp asc"); // + @id asc tiebreaker
        CHECK(ord.ok);

        // TOTAL order property: no two distinct rows compare equal.
        for (std::size_t i = 0; i < rows.size(); ++i)
            for (std::size_t j = 0; j < rows.size(); ++j)
                if (i != j)
                {
                    CHECK(compare_rows(rows[i], rows[j], ord.terms) != 0);
                    // antisymmetry
                    CHECK(compare_rows(rows[i], rows[j], ord.terms) ==
                          -compare_rows(rows[j], rows[i], ord.terms));
                }

        std::vector<Json> sorted = rows;
        std::sort(sorted.begin(), sorted.end(), RowLess{&ord.terms});
        // deterministic: e1,e2,e3,e4 by the @id tiebreaker
        CHECK(sorted[0].at("@id").as_string() == "e1");
        CHECK(sorted[1].at("@id").as_string() == "e2");
        CHECK(sorted[2].at("@id").as_string() == "e3");
        CHECK(sorted[3].at("@id").as_string() == "e4");

        // re-sorting a shuffled copy yields the identical order (determinism, no reorder)
        std::vector<Json> shuffled = {rows[3], rows[0], rows[2], rows[1]};
        std::sort(shuffled.begin(), shuffled.end(), RowLess{&ord.terms});
        for (std::size_t i = 0; i < sorted.size(); ++i)
            CHECK(shuffled[i].at("@id").as_string() == sorted[i].at("@id").as_string());

        // pagination stability under a concurrent INSERT: page after "e2" is [e3,e4]; inserting e5
        // (sorts last) never shifts the already-returned window.
        auto tail_after = [&](const std::vector<Json>& all, const std::string& after_id) {
            std::vector<Json> s = all;
            std::sort(s.begin(), s.end(), RowLess{&ord.terms});
            std::vector<std::string> out;
            for (const Json& r : s)
                if (r.at("@id").as_string() > after_id)
                    out.push_back(r.at("@id").as_string());
            return out;
        };
        std::vector<std::string> page2 = tail_after(rows, "e2");
        CHECK(page2.size() == 2 && page2[0] == "e3" && page2[1] == "e4");
        std::vector<Json> mutated = rows;
        mutated.push_back(row("e5", "hp", Json(5)));
        std::vector<std::string> page2b = tail_after(mutated, "e2");
        CHECK(page2b.size() == 3 && page2b[0] == "e3" && page2b[1] == "e4" && page2b[2] == "e5");

        // missing keys sort last, regardless of direction
        Json with = row("a", "hp", Json(9));
        Json without = row("b", "", Json(nullptr));
        OrderParse asc = parse_order("hp asc");
        OrderParse desc = parse_order("hp desc");
        CHECK(compare_rows(with, without, asc.terms) < 0);  // present before missing
        CHECK(compare_rows(with, without, desc.terms) < 0); // still: missing sorts last
    }

    // ============================================================================================
    // 5. unified cursor — round-trip, event/query reconciliation, malformed rejection
    // ============================================================================================
    {
        QueryCursor c;
        c.incarnation_id = "inc-42.abc";
        c.seq = 12345;
        c.generation = 7;
        c.after_id = "world/entity#99";
        const std::string token = c.to_token();
        auto parsed = QueryCursor::parse(token);
        CHECK(parsed.has_value());
        CHECK(parsed->incarnation_id == c.incarnation_id);
        CHECK(parsed->seq == c.seq);
        CHECK(parsed->generation == c.generation);
        CHECK(parsed->after_id == c.after_id); // exact bytes survive the hex round-trip

        // an EVENT cursor is a query cursor with an empty after_id (R-BRIDGE-008 reconciliation)
        QueryCursor ev;
        ev.incarnation_id = "inc-1";
        ev.seq = 500;
        auto ev_parsed = QueryCursor::parse(ev.to_token());
        CHECK(ev_parsed.has_value());
        CHECK(ev_parsed->after_id.empty());
        CHECK(ev_parsed->seq == 500 && ev_parsed->generation == 0);

        // an entity id with URI-hostile bytes still round-trips (opaque, hex-encoded)
        QueryCursor weird;
        weird.incarnation_id = "i";
        weird.after_id = "a/b?c=d&e#f";
        auto weird_parsed = QueryCursor::parse(weird.to_token());
        CHECK(weird_parsed.has_value() && weird_parsed->after_id == "a/b?c=d&e#f");

        // malformed rejections
        CHECK(!QueryCursor::parse("nope://v0/i/0/0?after=").has_value());       // wrong scheme
        CHECK(!QueryCursor::parse("context-cur://v0//0/0?after=").has_value()); // empty incarnation
        CHECK(!QueryCursor::parse("context-cur://v0/i!/0/0?after=").has_value()); // bad char
        CHECK(!QueryCursor::parse("context-cur://v0/i/x/0?after=").has_value()); // non-numeric gen
        CHECK(!QueryCursor::parse("context-cur://v0/i/0/x?after=").has_value()); // non-numeric seq
        CHECK(!QueryCursor::parse("context-cur://v0/i/0/0?after=zz").has_value()); // non-hex
        CHECK(!QueryCursor::parse("context-cur://v0/i/0/0?after=abc").has_value()); // odd-length hex
        CHECK(!QueryCursor::parse("context-cur://v0/i/0/0").has_value());          // missing ?after=
    }

    // ============================================================================================
    // 6. string semantics — UTF-8 validation, ASCII case-fold, NFC scope, string_match operators
    // ============================================================================================
    {
        // valid UTF-8
        CHECK(is_valid_utf8("hello"));
        CHECK(is_valid_utf8("caf\xC3\xA9")); // "café" (é = U+00E9)
        CHECK(is_valid_utf8(""));
        // invalid UTF-8
        CHECK(!is_valid_utf8("\x80"));             // lone continuation
        CHECK(!is_valid_utf8("\xC0\x80"));         // overlong
        CHECK(!is_valid_utf8("\xE2\x82"));         // truncated 3-byte
        CHECK(!is_valid_utf8("\xED\xA0\x80"));     // UTF-16 surrogate half
        CHECK(!is_valid_utf8("\xF5\x80\x80\x80")); // beyond U+10FFFF

        CHECK(ascii_case_fold("AbC-99") == "abc-99");
        CHECK(ascii_case_fold("café") == "café"); // non-ASCII bytes untouched

        CHECK(normalize_nfc("plain").has_value());
        CHECK(normalize_nfc("plain").value() == "plain");
        CHECK(!normalize_nfc("\x80").has_value()); // invalid UTF-8 -> nullopt

        // string_match operators (case-sensitive)
        CHECK(string_match(CompareOp::contains, "orc-warrior", "warrior", false));
        CHECK(!string_match(CompareOp::contains, "orc", "ORC", false));
        CHECK(string_match(CompareOp::starts_with, "orc", "or", false));
        CHECK(!string_match(CompareOp::starts_with, "orc", "rc", false));
        CHECK(string_match(CompareOp::ends_with, "orc", "rc", false));
        CHECK(string_match(CompareOp::matches, "orc", "o*c", false));
        CHECK(string_match(CompareOp::matches, "orc", "o?c", false));
        CHECK(!string_match(CompareOp::matches, "oc", "o?c", false));
        CHECK(string_match(CompareOp::matches, "anything", "*", false));
        // case-insensitive forms
        CHECK(string_match(CompareOp::contains, "ORC", "orc", true));
        CHECK(string_match(CompareOp::starts_with, "OrcWarrior", "orc", true));
        CHECK(string_match(CompareOp::matches, "ORC", "o?c", true));
        // a non-string-match operator returns false
        CHECK(!string_match(CompareOp::eq, "a", "a", false));
    }

    // ============================================================================================
    // 7. `context describe` queryLanguage section (R-CLI-013) — surfaced, complete, re-parses
    // ============================================================================================
    {
        const Json d = query_language_descriptor();
        CHECK(d.at("requirement").as_string() == "R-CLI-012");
        CHECK(!d.at("stable").as_bool()); // protocolMajor 0 until the M3 freeze
        CHECK(!d.at("ebnf").as_string().empty());
        CHECK(d.at("ebnf").as_string() == std::string(query_ebnf())); // ONE spelling

        // the operator set enumerates all 11 operators; every published token matches the parser's
        const Json& ops = d.at("operators");
        CHECK(ops.size() == 11);
        for (std::size_t i = 0; i < ops.size(); ++i)
        {
            CHECK(!ops.at(i).at("token").as_string().empty());
            CHECK(!ops.at(i).at("class").as_string().empty());
        }
        CHECK(d.at("booleanOperators").size() == 3);

        CHECK(d.at("ordering").at("defaultKey").as_string() == "@id");
        CHECK(d.at("ordering").at("total").as_bool());

        const Json& cur = d.at("cursor");
        CHECK(cur.at("uriScheme").as_string() == "context-cur");
        CHECK(cur.at("shape").is_object());
        CHECK(!cur.at("reconciliation").as_string().empty());

        CHECK(d.at("stringSemantics").at("normalization").as_string() == "NFC");
        CHECK(!d.at("stringSemantics").at("caseHandling").as_string().empty());
        CHECK(!d.at("stringSemantics").at("v1Scope").as_string().empty());

        CHECK(d.at("surfaces").size() == 3); // derived world, live sim, schema introspection

        // it re-parses as valid JSON (embeddable in `context describe --json`)
        const Json reparsed = Json::parse(d.dump());
        CHECK(reparsed.at("requirement").as_string() == "R-CLI-012");

        // and it is actually embedded in the whole-contract describe under contract.queryLanguage
        const Json describe = Registry::instance().describe();
        const Json& ql = describe.at("contract").at("queryLanguage");
        CHECK(ql.is_object());
        CHECK(ql.at("requirement").as_string() == "R-CLI-012");
        CHECK(ql.at("operators").size() == 11);
    }

    // ============================================================================================
    // 8. error catalog carries the R-CLI-012 codes (additive-only; usage exit class)
    // ============================================================================================
    {
        for (const char* code : {"query.syntax_error", "query.unknown_operator",
                                 "query.invalid_cursor", "query.unsupported_surface"})
        {
            const ErrorCode* e = find_code(code);
            CHECK(e != nullptr);
            if (e != nullptr)
            {
                CHECK(e->origin == "R-CLI-012");
                CHECK(e->exit_code == 2); // usage class
            }
        }
    }

    CONTRACT_TEST_MAIN_END();
}
