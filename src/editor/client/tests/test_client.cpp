// context_client core: instance discovery, JSON-RPC framing/classification, and the Client's
// response/event demux + attach handshake. Happy, edge, and failure paths (R-QA-013).

#include "context/editor/client/client.h"
#include "context/editor/client/instance.h"
#include "context/editor/client/wire.h"

#include "client_test.h"
#include "mock_channel.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace fs = std::filesystem;

using context::editor::client::AttachOptions;
using context::editor::client::Client;
using context::editor::client::discover_instance;
using context::editor::client::FrameKind;
using context::editor::client::InboundFrame;
using context::editor::client::InstanceInfo;
using context::editor::client::build_request;
using context::editor::client::parse_frame;
using context::editor::client::parse_instance_document;
using context::editor::contract::Json;
using clientmock::MockChannel;

namespace
{
// --- instance discovery ---------------------------------------------------------------------------
void test_parse_instance_document()
{
    const std::optional<InstanceInfo> ok = parse_instance_document(
        R"({"endpoint":"\\\\.\\pipe\\context-abc","pid":4242,"protocolMajor":1,"token":"deadbeef"})");
    CHECK(ok.has_value());
    CHECK(ok->endpoint == "\\\\.\\pipe\\context-abc");
    CHECK(ok->token == "deadbeef");
    CHECK(ok->protocol_major == 1);
    CHECK(ok->pid == 4242);

    // A pre-D20 document (no token) is still usable — the token simply comes back empty.
    const std::optional<InstanceInfo> no_token =
        parse_instance_document(R"({"endpoint":"/tmp/ctx.sock"})");
    CHECK(no_token.has_value());
    CHECK(no_token->token.empty());

    // Failure/edge: torn or unusable documents read as "not ready", never as a usable instance.
    CHECK(!parse_instance_document("").has_value());
    CHECK(!parse_instance_document("{\"endpoint\":").has_value());     // torn mid-write
    CHECK(!parse_instance_document("[]").has_value());                 // not an object
    CHECK(!parse_instance_document("{}").has_value());                 // no endpoint
    CHECK(!parse_instance_document(R"({"endpoint":""})").has_value()); // empty endpoint
    CHECK(!parse_instance_document(R"({"endpoint":7})").has_value());  // wrong type
}

void test_discover_instance_times_out_without_a_daemon()
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path project =
        fs::temp_directory_path() / ("ctx-client-nodaemon-" + std::to_string(stamp));
    std::error_code ec;
    fs::create_directories(project, ec);

    const auto started = std::chrono::steady_clock::now();
    CHECK(!discover_instance(project, 60).has_value());
    // It waited rather than returning instantly — the boot-race retry is real.
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() >= 50);

    fs::remove_all(project, ec);
}

void test_discover_instance_reads_a_published_document()
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path project =
        fs::temp_directory_path() / ("ctx-client-discover-" + std::to_string(stamp));
    std::error_code ec;
    fs::create_directories(project / ".editor", ec);
    {
        std::ofstream f(project / ".editor" / "instance.json", std::ios::binary | std::ios::trunc);
        f << R"({"endpoint":"/tmp/ctx-test.sock","pid":11,"protocolMajor":1,"token":"abc123"})";
    }
    const std::optional<InstanceInfo> info = discover_instance(project, 1000);
    CHECK(info.has_value());
    CHECK(info->endpoint == "/tmp/ctx-test.sock");
    CHECK(info->token == "abc123");

    fs::remove_all(project, ec);
}

// --- framing ----------------------------------------------------------------------------------------
void test_build_request()
{
    Json params = Json::object();
    params.set("path", Json(std::string("proj/a.scene")));
    const std::string raw = build_request(7, "query", std::move(params));
    const Json doc = Json::parse(raw);
    CHECK(doc.at("jsonrpc").as_string() == "2.0");
    CHECK(doc.at("id").as_int() == 7);
    CHECK(doc.at("method").as_string() == "query");
    CHECK(doc.at("params").at("path").as_string() == "proj/a.scene");
}

void test_parse_frame_classifies_every_shape()
{
    const std::optional<InboundFrame> response =
        parse_frame(R"({"jsonrpc":"2.0","id":3,"result":{"ok":true,"data":{"n":1}}})");
    CHECK(response.has_value());
    CHECK(response->kind == FrameKind::response);
    CHECK(response->id == 3);
    CHECK(!response->has_error);
    CHECK(response->result.at("data").at("n").as_int() == 1);

    const std::optional<InboundFrame> error = parse_frame(
        R"({"jsonrpc":"2.0","id":4,"error":{"code":-32000,"message":"attach denied"}})");
    CHECK(error.has_value());
    CHECK(error->kind == FrameKind::response);
    CHECK(error->has_error);
    CHECK(error->error_message == "attach denied");

    const std::optional<InboundFrame> event = parse_frame(
        R"({"jsonrpc":"2.0","method":"event","params":{"subId":"sub-1","event":{"seq":9}}})");
    CHECK(event.has_value());
    CHECK(event->kind == FrameKind::event);
    CHECK(event->sub_id == "sub-1");
    CHECK(event->event.at("seq").as_int() == 9);

    const std::optional<InboundFrame> gap =
        parse_frame(R"({"jsonrpc":"2.0","method":"event.gap","params":{}})");
    CHECK(gap.has_value());
    CHECK(gap->kind == FrameKind::gap);

    // An additive notification a NEWER daemon pushes is ignorable, not fatal — forward compatibility.
    const std::optional<InboundFrame> future =
        parse_frame(R"({"jsonrpc":"2.0","method":"something.new","params":{}})");
    CHECK(future.has_value());
    CHECK(future->kind == FrameKind::unknown);

    // A response with neither result nor error is a protocol fault surfaced as an error.
    const std::optional<InboundFrame> empty = parse_frame(R"({"jsonrpc":"2.0","id":5})");
    CHECK(empty.has_value());
    CHECK(empty->has_error);

    // Genuinely unparseable input is nullopt (a framing fault).
    CHECK(!parse_frame("not json").has_value());
    CHECK(!parse_frame("[1,2,3]").has_value());
}

// --- Client -----------------------------------------------------------------------------------------
void test_attach_carries_token_scopes_and_protocol()
{
    auto owned = std::make_unique<MockChannel>();
    MockChannel* mock = owned.get();
    mock->on("attach",
             [](const clientmock::Request&)
             {
                 Json data = Json::object();
                 Json scopes = Json::array();
                 scopes.push_back(Json(std::string("read_query")));
                 scopes.push_back(Json(std::string("file_write")));
                 data.set("scopes", std::move(scopes));
                 return MockChannel::ok_envelope(std::move(data));
             });
    Client client(std::move(owned));

    AttachOptions options;
    options.scope = "write,session";
    options.token = "tok-abcdef";
    std::string error;
    CHECK(client.attach(options, error));
    CHECK(error.empty());
    CHECK(client.attached());
    CHECK(client.granted_scopes().size() == 2);
    CHECK(client.granted_scopes()[0] == "read_query");

    const std::vector<clientmock::Request> attaches = mock->requests_for("attach");
    CHECK(attaches.size() == 1);
    CHECK(attaches[0].params.at("token").as_string() == "tok-abcdef");
    CHECK(attaches[0].params.at("scope").as_string() == "write,session");
    CHECK(attaches[0].params.contains("protocolMajor"));
    CHECK(attaches[0].params.at("capabilities").size() == 1);
}

// The attach reply is the ONE response the daemon returns UN-ENVELOPED: `Dispatcher::handle`'s
// handshake branch puts {protocolMajor, clientId, capabilities, scopes} straight into JSON-RPC
// `result`, while every OTHER verb answers with an R-CLI-008 envelope whose payload sits under
// `result.data`. The mock above scripts the ENVELOPED shape, which is exactly why a reader that
// only looked under `result.data` left granted_scopes() silently EMPTY on every real attach from
// e02 until e08a found it. This test scripts the FLAT shape the real daemon emits, so the
// regression cannot come back without a red unit test (no daemon, no sockets).
void test_attach_reads_the_flat_daemon_handshake_reply()
{
    auto owned = std::make_unique<MockChannel>();
    MockChannel* mock = owned.get();
    mock->on("attach",
             [](const clientmock::Request&)
             {
                 // Byte-for-byte the dispatcher's handshake `result` — NOT ok_envelope().
                 Json result = Json::object();
                 result.set("protocolMajor", Json(static_cast<std::uint64_t>(1)));
                 result.set("clientId", Json(static_cast<std::uint64_t>(7)));
                 Json caps = Json::array();
                 caps.push_back(Json(std::string("events")));
                 result.set("capabilities", std::move(caps));
                 Json scopes = Json::array();
                 scopes.push_back(Json(std::string("read_query")));
                 scopes.push_back(Json(std::string("session_control")));
                 result.set("scopes", std::move(scopes));
                 return result;
             });
    Client client(std::move(owned));

    AttachOptions options;
    options.scope = "session";
    std::string error;
    CHECK(client.attach(options, error));
    CHECK(client.granted_scopes().size() == 2);
    CHECK(client.granted_scopes()[1] == "session_control");
    // The e08a echo-suppression identity, learned from the same flat reply.
    CHECK(client.client_id() == 7);
    CHECK(mock->requests_for("attach").size() == 1);
}

// A pre-e08a daemon answers the handshake with no `clientId` at all. That must degrade to 0 —
// which never matches a real wire client's id (they start at 1) — so echo suppression falls back to
// "apply everything" rather than silently DROPPING another client's fact as if it were our own.
void test_attach_against_a_daemon_without_a_client_id_degrades_to_zero()
{
    auto owned = std::make_unique<MockChannel>();
    owned->on("attach",
              [](const clientmock::Request&)
              {
                  Json result = Json::object();
                  result.set("protocolMajor", Json(static_cast<std::uint64_t>(1)));
                  Json scopes = Json::array();
                  scopes.push_back(Json(std::string("read_query")));
                  result.set("scopes", std::move(scopes));
                  return result;
              });
    Client client(std::move(owned));

    AttachOptions options;
    std::string error;
    CHECK(client.attach(options, error));
    CHECK(client.client_id() == 0);
    CHECK(client.granted_scopes().size() == 1);
    CHECK(client.granted_scopes()[0] == "read_query");
}

void test_attach_refusal_is_reported_as_a_daemon_rejection()
{
    auto owned = std::make_unique<MockChannel>();
    MockChannel* mock = owned.get();
    // The D20 refusal shape: a daemon-side JSON-RPC ERROR response, NOT a transport failure. The
    // caller must be able to tell the two apart — a refused token is permanent, a transport hiccup
    // is retriable.
    mock->fail_method("attach", "attach denied: missing or invalid attach token");
    Client client(std::move(owned));

    AttachOptions options;
    options.token = "wrong-token";
    std::string error;
    bool rejected = false;
    CHECK(!client.attach(options, error, &rejected));
    CHECK(rejected);
    CHECK(!client.attached());
    CHECK(error.find("attach denied") != std::string::npos);
}

void test_attach_omits_an_empty_token()
{
    auto owned = std::make_unique<MockChannel>();
    MockChannel* mock = owned.get();
    Client client(std::move(owned));

    AttachOptions options;
    options.token.clear();
    std::string error;
    CHECK(client.attach(options, error));
    // No token to carry => the field is absent rather than an empty string, so the daemon's
    // "missing token" branch is what fires (not a "token mismatch" on "").
    CHECK(!mock->requests_for("attach")[0].params.contains("token"));
}

void test_call_demuxes_events_arriving_before_the_response()
{
    auto owned = std::make_unique<MockChannel>();
    MockChannel* mock = owned.get();
    mock->on("query",
             [](const clientmock::Request&)
             {
                 Json data = Json::object();
                 data.set("present", Json(true));
                 return MockChannel::ok_envelope(std::move(data));
             });
    Client client(std::move(owned));

    // A subscribing client's response is preceded by pushed event frames — call() must skip past
    // them (parking them) rather than mistaking the first inbound frame for its response.
    mock->push_event("sub-1", clientmock::make_event(5, "inc-a", "files"));
    mock->push_gap();

    std::string error;
    const std::optional<Json> result = client.call("query", Json::object(), error);
    CHECK(result.has_value());
    CHECK(result->at("data").at("present").as_bool());

    // Both pushed frames were parked, in order, for the consumer.
    bool disconnected = false;
    const std::optional<InboundFrame> first = client.poll_event(0, disconnected);
    CHECK(first.has_value());
    CHECK(first->kind == FrameKind::event);
    CHECK(first->sub_id == "sub-1");
    const std::optional<InboundFrame> second = client.poll_event(0, disconnected);
    CHECK(second.has_value());
    CHECK(second->kind == FrameKind::gap);
    CHECK(!client.poll_event(0, disconnected).has_value());
    CHECK(!disconnected);
}

void test_poll_event_reports_disconnect()
{
    auto owned = std::make_unique<MockChannel>();
    MockChannel* mock = owned.get();
    Client client(std::move(owned));
    mock->break_connection();

    bool disconnected = false;
    CHECK(!client.poll_event(0, disconnected).has_value());
    CHECK(disconnected);
}

void test_call_on_a_dead_wire_fails()
{
    auto owned = std::make_unique<MockChannel>();
    MockChannel* mock = owned.get();
    Client client(std::move(owned));
    mock->break_connection();

    std::string error;
    CHECK(!client.call("query", Json::object(), error).has_value());
    CHECK(!error.empty());
}
} // namespace

int main()
{
    test_parse_instance_document();
    test_discover_instance_times_out_without_a_daemon();
    test_discover_instance_reads_a_published_document();
    test_build_request();
    test_parse_frame_classifies_every_shape();
    test_attach_carries_token_scopes_and_protocol();
    test_attach_reads_the_flat_daemon_handshake_reply();
    test_attach_against_a_daemon_without_a_client_id_degrades_to_zero();
    test_attach_refusal_is_reported_as_a_daemon_rejection();
    test_attach_omits_an_empty_token();
    test_call_demuxes_events_arriving_before_the_response();
    test_poll_event_reports_disconnect();
    test_call_on_a_dead_wire_fails();
    CLIENT_TEST_MAIN_END();
}
