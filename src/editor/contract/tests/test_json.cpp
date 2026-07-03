// Json tests: build/dump/parse round-trips, escapes, integer-vs-double preservation, nesting,
// insertion-order stability, and malformed-input failure paths (R-QA-013, R-CLI-008/013).

#include "context/editor/contract/json.h"
#include "contract_test.h"

#include <stdexcept>
#include <string>

using namespace context::editor::contract;

int main()
{
    // --- scalars + integer/double distinction --------------------------------------------------
    {
        CHECK(Json(nullptr).dump() == "null");
        CHECK(Json(true).dump() == "true");
        CHECK(Json(false).dump() == "false");
        CHECK(Json(0).dump() == "0");           // integer, not "0.0"
        CHECK(Json(42).dump() == "42");
        CHECK(Json(std::uint64_t{7}).dump() == "7");
        CHECK(Json(-5).dump() == "-5");
        CHECK(Json(1.5).dump() == "1.5");       // real number keeps its fraction
        CHECK(Json("hi").dump() == "\"hi\"");
    }

    // --- object insertion order is preserved ---------------------------------------------------
    {
        Json obj = Json::object();
        obj.set("b", Json(1));
        obj.set("a", Json(2));
        obj.set("c", Json(3));
        CHECK(obj.dump() == "{\"b\":1,\"a\":2,\"c\":3}");
        // Re-setting overwrites in place (keeps position).
        obj.set("a", Json(9));
        CHECK(obj.dump() == "{\"b\":1,\"a\":9,\"c\":3}");
        CHECK(obj.size() == 3);
        CHECK(obj.contains("a"));
        CHECK(!obj.contains("z"));
        CHECK(obj.at("a").as_int() == 9);
        CHECK(obj.at("missing").is_null()); // missing key => shared null, no crash
    }

    // --- arrays --------------------------------------------------------------------------------
    {
        Json arr = Json::array();
        arr.push_back(Json(1));
        arr.push_back(Json("two"));
        arr.push_back(Json(true));
        CHECK(arr.size() == 3);
        CHECK(arr.dump() == "[1,\"two\",true]");
        CHECK(arr.at(1).as_string() == "two");
        CHECK(arr.at(99).is_null()); // out of range => null
        CHECK(Json::array().dump() == "[]");
        CHECK(Json::object().dump() == "{}");
    }

    // --- string escaping round-trips -----------------------------------------------------------
    {
        const std::string raw = "quote:\" back:\\ tab:\t newline:\n control:\x01 slash:/";
        const std::string dumped = Json(raw).dump();
        const Json reparsed = Json::parse(dumped);
        CHECK(reparsed.is_string());
        CHECK(reparsed.as_string() == raw);
    }

    // --- nested round-trip through parse -------------------------------------------------------
    {
        const std::string text =
            "{ \"a\": [1, 2, {\"deep\": true}], \"b\": null, \"n\": -3.25e2, \"s\": \"x\" }";
        const Json v = Json::parse(text);
        CHECK(v.is_object());
        CHECK(v.at("a").is_array());
        CHECK(v.at("a").size() == 3);
        CHECK(v.at("a").at(0).as_int() == 1);
        CHECK(v.at("a").at(2).at("deep").as_bool() == true);
        CHECK(v.at("b").is_null());
        CHECK(v.at("n").as_number() == -325.0);
        CHECK(v.at("s").as_string() == "x");
    }

    // --- unicode escape decodes to UTF-8 -------------------------------------------------------
    {
        const Json v = Json::parse("\"\\u00e9\""); // é
        CHECK(v.as_string().size() == 2);          // two UTF-8 bytes
    }

    // --- dump(indent) is valid + re-parses to an equal document --------------------------------
    {
        Json obj = Json::object();
        obj.set("list", Json::array());
        obj.set("k", Json("v"));
        const std::string pretty = obj.dump(2);
        CHECK(pretty.find('\n') != std::string::npos); // actually pretty-printed
        const Json round = Json::parse(pretty);
        CHECK(round.at("k").as_string() == "v");
        CHECK(round.at("list").is_array());
    }

    // --- malformed input throws (failure paths) ------------------------------------------------
    {
        const char* bad[] = {"", "{", "[1,2", "{\"a\":}", "tru", "\"unterminated", "{\"a\" 1}",
                             "[1 2]", "nul"};
        for (const char* b : bad)
        {
            bool threw = false;
            try
            {
                (void)Json::parse(b); // discard the [[nodiscard]] result; we only want the throw
            }
            catch (const std::runtime_error&)
            {
                threw = true;
            }
            CHECK(threw);
        }
    }

    CONTRACT_TEST_MAIN_END();
}
