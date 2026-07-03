// Dispatcher unit tests: the capability-negotiation attach (R-CLI-010), scope enforcement in the
// dispatcher (R-SEC-007 — read/query token REJECTED for install/build), and the JSON-RPC 2.0 wire
// framing over both.

#include "context/editor/bridge/dispatcher.h"

#include "context/editor/bridge/event_stream.h"
#include "context/editor/bridge/scope.h"
#include "context/editor/contract/handshake.h"
#include "context/editor/contract/json.h"

#include "bridge_test.h"

#include <string>
#include <variant>

using namespace context::editor::bridge;
using context::editor::contract::ClientHandshake;
using context::editor::contract::Envelope;
using context::editor::contract::Json;

int main()
{
    // --- attach(): capability negotiation (JSON-value level) ------------------------------------
    {
        Dispatcher d;
        ClientHandshake client;
        client.protocol_major = 0;
        client.capabilities = {"describe", "dry-run", "not-a-real-cap"};
        auto result = d.attach(client, ScopeSet::read_query());
        CHECK(std::holds_alternative<Session>(result));
        const Session& s = std::get<Session>(result);
        CHECK(s.attached);
        CHECK(s.protocol_major == 0);
        // Negotiated subset = client ∩ daemon; the bogus capability is dropped.
        bool has_describe = false, has_bogus = false;
        for (const std::string& c : s.capabilities)
        {
            has_describe = has_describe || c == "describe";
            has_bogus = has_bogus || c == "not-a-real-cap";
        }
        CHECK(has_describe);
        CHECK(!has_bogus);
    }

    // --- attach(): FAILURE PATH — out-of-window protocol hard-fails ------------------------------
    {
        Dispatcher d;
        ClientHandshake client;
        client.protocol_major = 1; // outside the {0} window at v1
        auto result = d.attach(client, ScopeSet::read_query());
        CHECK(std::holds_alternative<Envelope>(result));
        const Envelope& env = std::get<Envelope>(result);
        CHECK(!env.ok());
        CHECK(env.error().has_value());
        CHECK(env.error()->code == "handshake.incompatible_protocol");
    }

    // --- dispatch(): R-SEC-007 scope enforcement ------------------------------------------------
    {
        Dispatcher d;
        Session read;
        read.attached = true;
        read.scopes = ScopeSet::read_query();

        // Install rejected for a read/query token (the DoD gate — at the dispatcher, not an adapter).
        const Envelope install = d.dispatch("package.add", Json::object(), read);
        CHECK(!install.ok());
        CHECK(install.error()->code == kScopeDeniedCode);

        // Build rejected for a read/query token.
        const Envelope build = d.dispatch("build", Json::object(), read);
        CHECK(!build.ok());
        CHECK(build.error()->code == kScopeDeniedCode);

        // A read verb is allowed and returns the self-description.
        const Envelope describe = d.dispatch("describe", Json::object(), read);
        CHECK(describe.ok());
        CHECK(describe.data().contains("contract"));

        // A build+install token passes the scope gate — it reaches the (reserved) backing, proving
        // the rejection above was the SCOPE gate and not a blanket denial.
        Session privileged;
        privileged.attached = true;
        privileged.scopes = ScopeSet::all();
        const Envelope allowed = d.dispatch("package.add", Json::object(), privileged);
        CHECK(!allowed.ok());
        CHECK(allowed.error()->code == "contract.unimplemented"); // NOT scope.denied
    }

    // --- handle(): JSON-RPC 2.0 attach mutates the session --------------------------------------
    {
        EventStream stream("inc-test");
        Subscriber clients({"clients"}, 8);
        stream.add_subscriber(&clients);
        Dispatcher d(&stream);

        Session session;
        const std::string req =
            R"({"jsonrpc":"2.0","id":1,"method":"attach","params":{"protocolMajor":0,)"
            R"("capabilities":["describe"],"scope":"read"}})";
        const std::string resp = d.handle(req, session);
        const Json parsed = Json::parse(resp);
        CHECK(parsed.at("jsonrpc").as_string() == "2.0");
        CHECK(parsed.at("id").as_int() == 1);
        CHECK(parsed.contains("result"));
        CHECK(parsed.at("result").at("protocolMajor").as_int() == 0);
        CHECK(session.attached); // the session was mutated in place
        // A `clients` "attached" event was emitted on the stream.
        auto ev = clients.drain();
        CHECK(ev.size() == 1);
        CHECK(ev[0].payload.at("event").as_string() == "attached");
    }

    // --- handle(): FAILURE PATH — read token REJECTED for install over JSON-RPC (DoD) ------------
    {
        Dispatcher d;
        Session session;
        (void)d.handle(R"({"jsonrpc":"2.0","id":1,"method":"attach","params":{"protocolMajor":0,)"
                       R"("capabilities":[],"scope":"read"}})",
                       session);
        CHECK(session.attached);

        const std::string resp =
            d.handle(R"({"jsonrpc":"2.0","id":2,"method":"package.add","params":{"name":"physics"}})",
                     session);
        const Json parsed = Json::parse(resp);
        CHECK(parsed.contains("error"));
        // The transport-level error data carries the SAME R-CLI-008 code (scope.denied).
        CHECK(parsed.at("error").at("data").at("code").as_string() == kScopeDeniedCode);
    }

    // --- handle(): a scoped token is allowed to describe over JSON-RPC --------------------------
    {
        Dispatcher d;
        Session session;
        (void)d.handle(R"({"jsonrpc":"2.0","id":1,"method":"attach","params":{"protocolMajor":0,)"
                       R"("capabilities":["describe"],"scope":"read"}})",
                       session);
        const std::string resp =
            d.handle(R"({"jsonrpc":"2.0","id":3,"method":"describe","params":{}})", session);
        const Json parsed = Json::parse(resp);
        CHECK(parsed.contains("result"));
        CHECK(parsed.at("result").at("ok").as_bool());
    }

    // --- handle(): a method before attach is refused --------------------------------------------
    {
        Dispatcher d;
        Session session; // not attached
        const std::string resp =
            d.handle(R"({"jsonrpc":"2.0","id":9,"method":"describe","params":{}})", session);
        const Json parsed = Json::parse(resp);
        CHECK(parsed.contains("error"));
        CHECK(parsed.at("error").at("data").at("code").as_string() == "usage.invalid");
    }

    // --- handle(): out-of-window attach fails over JSON-RPC -------------------------------------
    {
        Dispatcher d;
        Session session;
        const std::string resp =
            d.handle(R"({"jsonrpc":"2.0","id":4,"method":"attach","params":{"protocolMajor":9,)"
                     R"("capabilities":[]}})",
                     session);
        const Json parsed = Json::parse(resp);
        CHECK(parsed.contains("error"));
        CHECK(parsed.at("error").at("data").at("code").as_string() == "handshake.incompatible_protocol");
        CHECK(!session.attached);
    }

    // --- handle(): malformed JSON -> JSON-RPC parse error ---------------------------------------
    {
        Dispatcher d;
        Session session;
        const std::string resp = d.handle("{ this is not json", session);
        const Json parsed = Json::parse(resp);
        CHECK(parsed.at("error").at("code").as_int() == -32700);
    }

    // --- handle(): a notification (no id) yields no response ------------------------------------
    {
        Dispatcher d;
        Session session;
        const std::string resp =
            d.handle(R"({"jsonrpc":"2.0","method":"attach","params":{"protocolMajor":0,)"
                     R"("capabilities":[]}})",
                     session);
        CHECK(resp.empty());
        CHECK(session.attached); // the attach still took effect
    }

    BRIDGE_TEST_MAIN_END();
}
