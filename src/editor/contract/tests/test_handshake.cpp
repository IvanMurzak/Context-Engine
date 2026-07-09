// Handshake tests: the attach exchange CARRIES {protocolMajor, capabilities[]} and HARD-FAILS
// through the R-CLI-008 envelope outside the compatibility window, with capability-subset
// degradation inside it (R-CLI-004/010; happy + failure paths, R-QA-013). Post-M3-freeze the
// protocol major is FROZEN at 1 and the window is exactly {1}.

#include "context/editor/contract/envelope.h"
#include "context/editor/contract/handshake.h"
#include "contract_test.h"

#include <variant>

using namespace context::editor::contract;

int main()
{
    // --- the protocol descriptor carries {protocolMajor=1, capabilities[]} + STABLE flag --------
    {
        CHECK(kProtocolMajor == 1); // FROZEN at the M3 contract freeze (R-CLI-004)
        const Json d = protocol_descriptor();
        CHECK(d.at("protocolMajor").as_int() == 1);
        CHECK(d.at("stable").as_bool() == true); // STABLE / FROZEN at major 1 (R-CLI-004)
        CHECK(d.at("minClientProtocol").as_int() == 1);
        CHECK(d.at("capabilities").is_array());
        CHECK(d.at("capabilities").size() >= 1);
        CHECK(!d.at("note").as_string().empty());
        CHECK(!daemon_capabilities().empty());
    }

    // --- in-window negotiation degrades to the client ∩ daemon capability subset ----------------
    {
        ClientHandshake client;
        client.protocol_major = kProtocolMajor; // in-window (the frozen major)
        client.capabilities = {"describe", "if-match", "capability-the-daemon-lacks"};
        HandshakeResult result = negotiate(client);
        CHECK(std::holds_alternative<Negotiated>(result));
        const Negotiated& agreed = std::get<Negotiated>(result);
        CHECK(agreed.protocol_major == kProtocolMajor);
        // The unknown client capability is dropped; the two shared ones survive, daemon-order.
        CHECK(agreed.capabilities.size() == 2);
        CHECK(agreed.capabilities[0] == "describe");
        CHECK(agreed.capabilities[1] == "if-match");
    }

    // --- an in-window client with NO overlapping capabilities negotiates an empty subset --------
    {
        ClientHandshake client;
        client.protocol_major = kProtocolMajor; // in-window (the frozen major)
        client.capabilities = {"nothing-in-common"};
        HandshakeResult result = negotiate(client);
        CHECK(std::holds_alternative<Negotiated>(result));
        CHECK(std::get<Negotiated>(result).capabilities.empty());
    }

    // --- an out-of-window client HARD-FAILS through the R-CLI-008 envelope ----------------------
    {
        ClientHandshake client;
        client.protocol_major = kProtocolMajor + 1; // only the frozen major exists => outside window
        client.capabilities = {"describe"};
        HandshakeResult result = negotiate(client);
        CHECK(std::holds_alternative<Envelope>(result));
        const Envelope& env = std::get<Envelope>(result);
        CHECK(!env.ok());
        CHECK(env.error()->code == "handshake.incompatible_protocol");
        CHECK(env.exit_code() == 7); // protocol/version class
    }

    CONTRACT_TEST_MAIN_END();
}
