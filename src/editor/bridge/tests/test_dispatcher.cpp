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
using context::editor::contract::kProtocolMajor;

int main()
{
    // --- attach(): capability negotiation (JSON-value level) ------------------------------------
    {
        Dispatcher d;
        ClientHandshake client;
        client.protocol_major = kProtocolMajor; // in-window (the frozen major)
        client.capabilities = {"describe", "dry-run", "not-a-real-cap"};
        auto result = d.attach(client, ScopeSet::read_query());
        CHECK(std::holds_alternative<Session>(result));
        const Session& s = std::get<Session>(result);
        CHECK(s.attached);
        CHECK(s.protocol_major == kProtocolMajor);
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
        client.protocol_major = kProtocolMajor + 1; // outside the {1} window at the freeze
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
            R"({"jsonrpc":"2.0","id":1,"method":"attach","params":{"protocolMajor":1,)"
            R"("capabilities":["describe"],"scope":"read"}})";
        const std::string resp = d.handle(req, session);
        const Json parsed = Json::parse(resp);
        CHECK(parsed.at("jsonrpc").as_string() == "2.0");
        CHECK(parsed.at("id").as_int() == 1);
        CHECK(parsed.contains("result"));
        CHECK(parsed.at("result").at("protocolMajor").as_int() == 1);
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
        (void)d.handle(R"({"jsonrpc":"2.0","id":1,"method":"attach","params":{"protocolMajor":1,)"
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
        (void)d.handle(R"({"jsonrpc":"2.0","id":1,"method":"attach","params":{"protocolMajor":1,)"
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
            d.handle(R"({"jsonrpc":"2.0","method":"attach","params":{"protocolMajor":1,)"
                     R"("capabilities":[]}})",
                     session);
        CHECK(resp.empty());
        CHECK(session.attached); // the attach still took effect
    }

    // --- dispatch(): R-CLI-015 subscription protocol served against the live stream --------------
    {
        EventStream stream("inc-disp");
        Dispatcher d(&stream);
        Session sess;
        sess.attached = true;
        sess.scopes = ScopeSet::read_query(); // subscribe/ack/unsubscribe are read-baseline methods

        stream.publish("files", Json::object()); // seq 1

        // subscribe -> {subId, snapshot} (snapshot-then-delta).
        Json sub_params = Json::object();
        Json topics = Json::array();
        topics.push_back(Json(std::string("files")));
        sub_params.set("topics", std::move(topics));
        const Envelope sub = d.dispatch("subscribe", sub_params, sess);
        CHECK(sub.ok());
        CHECK(!sub.data().at("subId").as_string().empty());
        CHECK(sub.data().at("snapshot").at("lastSeq").as_int() == 1);
        const std::string sub_id = sub.data().at("subId").as_string();
        CHECK(stream.subscription_count() == 1);

        // ack -> advances the cursor; the daemon echoes the slowest-acked retention floor.
        Json ack_params = Json::object();
        ack_params.set("subId", Json(sub_id));
        ack_params.set("seq", Json(1));
        const Envelope ack = d.dispatch("ack", ack_params, sess);
        CHECK(ack.ok());
        CHECK(ack.data().at("slowestAckedSeq").as_int() == 1);

        // FAILURE PATH: ack an unknown subId -> subscription.unknown_sub.
        Json bad = Json::object();
        bad.set("subId", Json(std::string("sub-nope")));
        bad.set("seq", Json(1));
        const Envelope bad_ack = d.dispatch("ack", bad, sess);
        CHECK(!bad_ack.ok());
        CHECK(bad_ack.error()->code == "subscription.unknown_sub");

        // FAILURE PATH: unsubscribe with no subId -> usage.missing_argument.
        const Envelope no_id = d.dispatch("unsubscribe", Json::object(), sess);
        CHECK(!no_id.ok());
        CHECK(no_id.error()->code == "usage.missing_argument");

        // unsubscribe -> removed; the subscription is gone.
        Json unsub = Json::object();
        unsub.set("subId", Json(sub_id));
        const Envelope removed = d.dispatch("unsubscribe", unsub, sess);
        CHECK(removed.ok());
        CHECK(removed.data().at("removed").as_bool());
        CHECK(stream.subscription_count() == 0);
    }

    // --- handle(): subscribe over the JSON-RPC 2.0 wire -----------------------------------------
    {
        EventStream stream("inc-wire");
        Dispatcher d(&stream);
        Session session;
        (void)d.handle(R"({"jsonrpc":"2.0","id":1,"method":"attach","params":{"protocolMajor":1,)"
                       R"("capabilities":[],"scope":"read"}})",
                       session);
        const std::string resp = d.handle(
            R"({"jsonrpc":"2.0","id":2,"method":"subscribe","params":{"topics":["files"]}})",
            session);
        const Json parsed = Json::parse(resp);
        CHECK(parsed.contains("result"));
        CHECK(parsed.at("result").at("ok").as_bool());
        CHECK(!parsed.at("result").at("data").at("subId").as_string().empty());
    }

    // --- attach(): D20 attach-token enforcement — DEFAULT OFF, then ON (accept + deny) -----------
    {
        // DEFAULT OFF (e01, C-F1): the token is carried but never gated on — a client attaches with
        // any token or none. This is what keeps e01 additive for the tokenless CLI.
        Dispatcher off;
        CHECK(!off.attach_auth_required());
        ClientHandshake anon; // no token
        CHECK(std::holds_alternative<Session>(off.attach(anon, ScopeSet::read_query())));
        ClientHandshake stray;
        stray.token = "whatever";
        CHECK(std::holds_alternative<Session>(off.attach(stray, ScopeSet::read_query())));

        // ENFORCEMENT ON: the correct token attaches; a wrong OR missing one is attach.denied.
        Dispatcher on;
        on.configure_attach_auth("s3cr3t-token", true);
        CHECK(on.attach_auth_required());

        ClientHandshake good;
        good.token = "s3cr3t-token";
        CHECK(std::holds_alternative<Session>(on.attach(good, ScopeSet::read_query())));

        ClientHandshake wrong;
        wrong.token = "nope";
        const auto denied = on.attach(wrong, ScopeSet::read_query());
        CHECK(std::holds_alternative<Envelope>(denied));
        CHECK(std::get<Envelope>(denied).error()->code == kAttachDeniedCode);
        CHECK(std::get<Envelope>(denied).exit_code() == 6); // permission class

        ClientHandshake missing; // empty token == "no token supplied"
        const auto denied2 = on.attach(missing, ScopeSet::read_query());
        CHECK(std::holds_alternative<Envelope>(denied2));
        CHECK(std::get<Envelope>(denied2).error()->code == kAttachDeniedCode);

        // The auth check PRECEDES protocol negotiation: an out-of-window client with a wrong token is
        // attach.denied (never leaking the protocol-window failure to an unauthenticated caller).
        ClientHandshake bad_proto;
        bad_proto.protocol_major = kProtocolMajor + 1;
        bad_proto.token = "nope";
        const auto denied3 = on.attach(bad_proto, ScopeSet::read_query());
        CHECK(std::holds_alternative<Envelope>(denied3));
        CHECK(std::get<Envelope>(denied3).error()->code == kAttachDeniedCode);
    }

    // --- handle(): D20 token over the JSON-RPC 2.0 wire (flag ON) --------------------------------
    {
        Dispatcher d;
        d.configure_attach_auth("tok-abc", true);

        Session denied_sess;
        const std::string denied =
            d.handle(R"({"jsonrpc":"2.0","id":1,"method":"attach","params":{"protocolMajor":1,)"
                     R"("capabilities":[],"token":"wrong"}})",
                     denied_sess);
        const Json dp = Json::parse(denied);
        CHECK(dp.contains("error"));
        CHECK(dp.at("error").at("data").at("code").as_string() == kAttachDeniedCode);
        CHECK(!denied_sess.attached);

        Session ok_sess;
        const std::string ok =
            d.handle(R"({"jsonrpc":"2.0","id":2,"method":"attach","params":{"protocolMajor":1,)"
                     R"("capabilities":[],"token":"tok-abc"}})",
                     ok_sess);
        CHECK(Json::parse(ok).contains("result"));
        CHECK(ok_sess.attached);
    }

    // --- handle(): D19 per-connection subscription tracking on the session ----------------------
    // The serve loop's fan-out + disconnect cleanup key off session.subscriptions, so handle() must
    // record a subId on subscribe and drop it on unsubscribe.
    {
        EventStream stream("inc-subtrack");
        Dispatcher d(&stream);
        Session session;
        (void)d.handle(R"({"jsonrpc":"2.0","id":1,"method":"attach","params":{"protocolMajor":1,)"
                       R"("capabilities":[],"scope":"read"}})",
                       session);
        CHECK(session.subscriptions.empty());

        const std::string s1 = d.handle(
            R"({"jsonrpc":"2.0","id":2,"method":"subscribe","params":{"topics":["files"]}})",
            session);
        const std::string sub_id = Json::parse(s1).at("result").at("data").at("subId").as_string();
        CHECK(session.subscriptions.size() == 1);
        CHECK(session.subscriptions[0] == sub_id);

        // A second subscribe tracks a second id.
        (void)d.handle(
            R"({"jsonrpc":"2.0","id":3,"method":"subscribe","params":{"topics":["derivation"]}})",
            session);
        CHECK(session.subscriptions.size() == 2);

        // unsubscribe removes exactly that id from the session's tracked set.
        const std::string ureq =
            std::string(R"({"jsonrpc":"2.0","id":4,"method":"unsubscribe","params":{"subId":")") +
            sub_id + R"("}})";
        (void)d.handle(ureq, session);
        CHECK(session.subscriptions.size() == 1);
        CHECK(session.subscriptions[0] != sub_id);
    }

    BRIDGE_TEST_MAIN_END();
}
