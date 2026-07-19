// The D10 out-of-tree consumer smoke: an ordinary client, built against the INSTALLED context_client
// package and nothing else (see CMakeLists.txt).
//
// It deliberately exercises the SDK's public surface WITHOUT a daemon, so the boundary gate stays a
// pure build/link/API assertion that needs no fixture: discovery on an empty project, the framing
// helpers, the reconnect backoff policy, and the generated client schema. What is being proven is
// not the behavior (the engine's own ctests cover that) but that a stranger can reach this surface
// through the published package at all.

#include "context/editor/client/client.h"
#include "context/editor/client/instance.h"
#include "context/editor/client/schema.h"
#include "context/editor/client/subscription.h"
#include "context/editor/client/wire.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace client = context::editor::client;

namespace
{
int failures = 0;

void check(bool condition, const char* what)
{
    if (!condition)
    {
        std::cerr << "consumer smoke FAILED: " << what << "\n";
        ++failures;
    }
}
} // namespace

int main()
{
    // --- discovery ------------------------------------------------------------------------------
    const std::optional<client::InstanceInfo> parsed = client::parse_instance_document(
        R"({"endpoint":"/tmp/ctx.sock","pid":1,"protocolMajor":1,"token":"abc"})");
    check(parsed.has_value(), "parse_instance_document accepts a well-formed document");
    check(parsed && parsed->endpoint == "/tmp/ctx.sock", "endpoint round-trips");
    check(parsed && parsed->token == "abc", "attach token round-trips");
    check(!client::parse_instance_document("{}").has_value(), "a document with no endpoint is rejected");

    // No daemon on a fresh temp dir -> discovery reports "none", it does not throw or hang.
    const std::filesystem::path empty = std::filesystem::temp_directory_path();
    check(!client::discover_instance(empty / "context-consumer-smoke-absent", 1).has_value(),
          "discovery on a project with no daemon yields nothing");

    // --- framing --------------------------------------------------------------------------------
    const std::string request =
        client::build_request(1, "describe", context::editor::contract::Json::object());
    check(request.find("\"method\":\"describe\"") != std::string::npos, "build_request emits the method");

    const std::optional<client::InboundFrame> event = client::parse_frame(
        R"({"jsonrpc":"2.0","method":"event","params":{"subId":"sub-1","event":{"seq":3}}})");
    check(event && event->kind == client::FrameKind::event, "a pushed event frame is classified");
    check(event && event->sub_id == "sub-1", "the event's subscription id is exposed");

    // --- subscription consumer types --------------------------------------------------------------
    client::BackoffPolicy backoff;
    backoff.initial_ms = 10;
    backoff.multiplier = 2;
    backoff.max_ms = 40;
    check(backoff.delay_for_attempt(0) == 10, "backoff starts at initial_ms");
    check(backoff.delay_for_attempt(9) == 40, "backoff is bounded by max_ms");

    // --- the generated client schema ----------------------------------------------------------------
    const context::editor::contract::Json schema = client::client_schema();
    check(schema.contains("rpcMethods"), "the client schema publishes the RPC surface");
    check(schema.contains("eventTopics"), "the client schema publishes the event topics");

    if (failures == 0)
        std::cout << "context_client consumer smoke: OK (out-of-tree build against the installed package)\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
