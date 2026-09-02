// A real `client::Client` over a scripted `clientmock::MockChannel`, attached (so it has a client
// id) -- shared by every T1 in this directory that needs a REAL client rather than a stand-in:
// mocking the WIRE (not the client) means the frames exercised here are the frames `dispatcher.cpp`
// actually emits and the real SDK parses them (test_session_feed.cpp's own header note restates why
// this matters more than it looks: a double standing in for `client::Client` would let a suite pass
// over request/reply shapes the daemon never produces).
//
// Hoisted out of test_session_feed.cpp (its original home) so test_viewport_feed.cpp's e4 picking
// fixtures do not carry a second, drifting copy of the SAME attach wiring -- a caller that needs
// MORE than a bare attach (test_viewport_feed.cpp additionally mocks `editor.select`) extends the
// returned `Wired` locally with `wired.channel->on(...)` rather than this header growing a
// per-caller parameter for every wire verb a future T1 might need.

#pragma once

#include "context/editor/client/client.h"
#include "context/editor/contract/json.h"

#include "mock_channel.h"
#include "panels_test.h" // CHECK()

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace panelstest
{

// A real Client over the scripted channel, attached (so it has a client id) and holding `channel`.
struct Wired
{
    clientmock::MockChannel* channel = nullptr;
    std::unique_ptr<context::editor::client::Client> client;
};

[[nodiscard]] inline Wired make_client(std::uint64_t client_id)
{
    auto channel = std::make_unique<clientmock::MockChannel>();
    clientmock::MockChannel* raw = channel.get();
    // The attach reply is FLAT in `result` -- NOT an envelope (mock_channel.h's standing warning).
    raw->on("attach",
            [client_id](const clientmock::Request&)
            {
                context::editor::contract::Json result = context::editor::contract::Json::object();
                result.set("protocolMajor",
                           context::editor::contract::Json(static_cast<std::uint64_t>(
                               context::editor::contract::kProtocolMajor)));
                result.set("clientId", context::editor::contract::Json(client_id));
                context::editor::contract::Json caps = context::editor::contract::Json::array();
                caps.push_back(context::editor::contract::Json(std::string("describe")));
                result.set("capabilities", std::move(caps));
                context::editor::contract::Json scopes = context::editor::contract::Json::array();
                scopes.push_back(context::editor::contract::Json(std::string("read")));
                scopes.push_back(context::editor::contract::Json(std::string("session_control")));
                result.set("scopes", std::move(scopes));
                return result;
            });

    Wired out;
    out.channel = raw;
    out.client = std::make_unique<context::editor::client::Client>(std::move(channel));
    context::editor::client::AttachOptions options;
    options.scope = "read,session";
    options.token = "t";
    std::string error;
    CHECK(out.client->attach(options, error));
    CHECK(out.client->client_id() == client_id);
    return out;
}

} // namespace panelstest
