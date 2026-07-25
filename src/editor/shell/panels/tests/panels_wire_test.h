// A REAL `client::Client` over the scripted wire, shared by the context_editor_panels suites that
// need one (e09b-2's gateway, e09c's composition root).
//
// Deliberately NOT in `panels_test.h`: that header declares itself a "tiny zero-dependency" harness
// and is included by every suite in this directory, including the two that link no client at all.
// This one costs `client/client.h` + `mock_channel.h`, so it is opt-in.
//
// THE MOCK IS AT THE WIRE, NOT AT THE CLIENT (mock_channel.h's standing warning): a double standing
// in for `client::Client` would let a suite pass over frames the daemon never sends. Here the frames
// are dispatcher.cpp's and a REAL client parses them. Only `attach` is scripted — a caller scripts
// whatever else it needs, and an UNSCRIPTED method is refused, which is itself a useful posture.
//
// The attach reply is FLAT in `result`, NOT an envelope. A mock that envelopes it is MORE forgiving
// than the daemon and hides a real reader bug (it hid an empty granted_scopes() from e02 to e08a),
// which is exactly why this shape is written down ONCE here rather than re-typed per suite.

#pragma once

#include "context/editor/client/client.h"
#include "context/editor/contract/handshake.h" // kProtocolMajor
#include "context/editor/contract/json.h"

#include "mock_channel.h"
#include "panels_test.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace panelstest
{

namespace client = context::editor::client;
using context::editor::contract::Json;

struct Wired
{
    clientmock::MockChannel* channel = nullptr;
    std::unique_ptr<client::Client> client;
};

// `granted_scope` is the second scope the daemon grants (beside `read`) — `file_write` for a suite
// driving writes, `session_control` for one driving the session verbs. `attach_scope` is what the
// client ASKS for.
[[nodiscard]] inline Wired make_wired_client(std::uint64_t client_id = 9,
                                             const char* granted_scope = "file_write",
                                             const char* attach_scope = "read,write")
{
    auto channel = std::make_unique<clientmock::MockChannel>();
    clientmock::MockChannel* raw = channel.get();
    raw->on("attach",
            [client_id, granted_scope](const clientmock::Request&)
            {
                Json result = Json::object();
                result.set("protocolMajor",
                           Json(static_cast<std::uint64_t>(context::editor::contract::kProtocolMajor)));
                result.set("clientId", Json(client_id));
                Json caps = Json::array();
                caps.push_back(Json(std::string("describe")));
                result.set("capabilities", std::move(caps));
                Json scopes = Json::array();
                scopes.push_back(Json(std::string("read")));
                scopes.push_back(Json(std::string(granted_scope)));
                result.set("scopes", std::move(scopes));
                return result;
            });

    Wired out;
    out.channel = raw;
    out.client = std::make_unique<client::Client>(std::move(channel));
    client::AttachOptions options;
    options.scope = attach_scope;
    options.token = "t";
    std::string error;
    CHECK(out.client->attach(options, error));
    return out;
}

} // namespace panelstest
