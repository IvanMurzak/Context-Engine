// Envelope tests: the uniform R-CLI-008 shape `{ ok, data|error, generationAfter, warnings[] }`,
// the error object shape, exit-code mapping, and JSON round-trip (happy + failure paths, R-QA-013).

#include "context/editor/contract/envelope.h"
#include "context/editor/contract/json.h"
#include "contract_test.h"

using namespace context::editor::contract;

int main()
{
    // --- success envelope ----------------------------------------------------------------------
    {
        Json data = Json::object();
        data.set("value", Json(7));
        Envelope env = Envelope::success(data, /*generation_after=*/42);
        env.add_warning("advisory: overrides diverged");
        CHECK(env.ok());
        CHECK(env.exit_code() == 0);
        CHECK(env.generation_after() == 42);

        const Json j = env.to_json();
        CHECK(j.at("ok").as_bool() == true);
        CHECK(j.at("data").at("value").as_int() == 7);
        CHECK(j.at("generationAfter").as_int() == 42);
        CHECK(j.at("warnings").size() == 1);
        CHECK(!j.contains("error")); // success carries no error object
    }

    // --- failure envelope defaults message/retriable/exit from the catalog ---------------------
    {
        Envelope env = Envelope::failure("cas.mismatch");
        CHECK(!env.ok());
        CHECK(env.exit_code() == 4); // conflict class from the catalog
        CHECK(env.error().has_value());
        CHECK(env.error()->retriable == true); // pulled from the catalog entry

        const Json j = env.to_json();
        CHECK(j.at("ok").as_bool() == false);
        CHECK(j.at("error").at("code").as_string() == "cas.mismatch");
        CHECK(!j.at("error").at("message").as_string().empty());
        CHECK(j.at("error").at("retriable").as_bool() == true);
        CHECK(!j.contains("data")); // failure carries no data object
        CHECK(j.at("warnings").is_array());
    }

    // --- failure with an overridden message + a JSON pointer -----------------------------------
    {
        Envelope env = Envelope::failure("path.jail_violation", "escaped project root",
                                         "/entities/0/mesh");
        CHECK(env.exit_code() == 6); // permission class
        const Json j = env.to_json();
        CHECK(j.at("error").at("message").as_string() == "escaped project root");
        CHECK(j.at("error").at("pointer").as_string() == "/entities/0/mesh");
    }

    // --- an unknown code still produces a well-formed envelope (exit 1) -------------------------
    {
        Envelope env = Envelope::failure("totally.unknown.code");
        CHECK(env.exit_code() == 1);
        CHECK(!env.ok());
        const Json j = env.to_json();
        CHECK(j.at("error").at("code").as_string() == "totally.unknown.code");
    }

    // --- the envelope JSON re-parses (it is valid JSON) ----------------------------------------
    {
        Envelope env = Envelope::success(Json("payload"));
        const Json round = Json::parse(env.dump());
        CHECK(round.at("ok").as_bool() == true);
        CHECK(round.at("data").as_string() == "payload");
    }

    CONTRACT_TEST_MAIN_END();
}
