// T1 (the LIVE half) — context_client driven against a REAL `context daemon` process over the real
// IPC wire. The mock suite (test_subscription.cpp) proves the consumer's state machine; this proves
// the mock and the daemon actually agree, which no amount of mocking can.
//
//   1. attach + subscribe + snapshot-then-delta: a real edit over the wire produces real events that
//      the consumer applies, with real ack cursors.
//   2. D20 enforcement (default ON): an attach carrying NO token is REFUSED by the live daemon, and
//      the same attach WITH the discovered token succeeds.
//
// CONTEXT_BINARY is the built `context` executable path (a compile-time define). The daemon child is
// always reaped (shutdown, else killed) so the test never hangs or leaks a process.

#include "context/editor/client/client.h"
#include "context/editor/client/subscription.h"

#include "client_test.h"
#include "process_util.h"

#include <chrono>
#include <memory>
#include <optional>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

using context::editor::client::AttachOptions;
using context::editor::client::Client;
using context::editor::client::ClientEvent;
using context::editor::client::discover_instance;
using context::editor::client::InstanceInfo;
using context::editor::client::SubscriptionConsumer;
using context::editor::client::SubscriptionSpec;
using context::editor::contract::Json;

#ifndef CONTEXT_BINARY
#error "CONTEXT_BINARY (path to the built context executable) must be defined by the build."
#endif

namespace
{
fs::path make_temp_project(const char* tag)
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir =
        fs::temp_directory_path() / ("ctx-client-e2e-" + std::string(tag) + "-" + std::to_string(stamp));
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

// Ask a live daemon to stop, then reap the child (kill it if it will not go quietly, so CI never
// inherits a stray process).
void shutdown_daemon(ctest_proc::Process& child, const InstanceInfo& instance)
{
    AttachOptions options;
    options.scope = "write,session";
    options.token = instance.token;
    std::unique_ptr<context::editor::client::WireChannel> channel =
        context::editor::client::make_transport_channel(instance.endpoint, 3000);
    if (channel)
    {
        Client client(std::move(channel));
        std::string error;
        if (client.attach(options, error))
            (void)client.call("shutdown", Json::object(), error);
    }
    int exit_code = 0;
    if (!ctest_proc::wait_for(child, 5000, exit_code))
        ctest_proc::kill(child);
    ctest_proc::release(child);
}

// Kill + reap a child we are abandoning (a failed boot).
void abandon_daemon(ctest_proc::Process& child)
{
    ctest_proc::kill(child);
    ctest_proc::release(child);
}

// --- 1. live subscription: snapshot-then-delta + real events + acks -------------------------------
void test_live_subscription_receives_real_events()
{
    const fs::path project = make_temp_project("sub");
    ctest_proc::Process daemon =
        ctest_proc::spawn(CONTEXT_BINARY, {"daemon", "--project", project.string()});
    CHECK(ctest_proc::valid(daemon));

    const std::optional<InstanceInfo> instance = discover_instance(project, 15000);
    CHECK(instance.has_value());
    if (!instance.has_value())
    {
        abandon_daemon(daemon);
        return;
    }
    // The daemon publishes a D20 token — enforcement is ON by default since e02.
    CHECK(!instance->token.empty());

    std::unique_ptr<context::editor::client::WireChannel> channel =
        context::editor::client::make_transport_channel(instance->endpoint, 5000);
    CHECK(channel != nullptr);
    Client client(std::move(channel));

    AttachOptions options;
    options.scope = "write,session";
    options.token = instance->token;
    std::string error;
    CHECK(client.attach(options, error));
    CHECK(error.empty());

    std::vector<ClientEvent> seen;
    SubscriptionConsumer::Options consumer_options;
    consumer_options.ack_interval = 1; // ack eagerly so the retention floor is exercised
    consumer_options.poll_timeout_ms = 100;
    SubscriptionConsumer consumer(client, options, consumer_options);
    consumer.on_event([&seen](const std::string&, const ClientEvent& e) { seen.push_back(e); });
    consumer.add(SubscriptionSpec{{}, ""}); // every topic

    CHECK(consumer.start(error));
    CHECK(error.empty());
    // The snapshot-then-delta contract: a snapshot arrived, carrying the live incarnation id.
    CHECK(consumer.stats().snapshots_taken == 1);
    CHECK(!consumer.incarnation_id().empty());
    CHECK(consumer.states()[0].live);

    // Drive REAL work through the daemon: the edit lands on disk, then `reconcile` folds it in and
    // SETTLES — advancing the derived-world generation and publishing the `derivation.settled`
    // quiescence event. (A bare `edit` deliberately does not settle, so it publishes nothing; the
    // events this subscription is here to observe come from the settle.)
    Json edit_params = Json::object();
    edit_params.set("path", Json(std::string("proj/e2e.scene")));
    edit_params.set("content", Json(std::string("entity: 1")));
    CHECK(client.call("edit", std::move(edit_params), error).has_value());
    CHECK(client.call("reconcile", Json::object(), error).has_value());

    // Pump until the events the edit produced have been applied (bounded, so CI never hangs).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (seen.empty() && std::chrono::steady_clock::now() < deadline)
        CHECK(consumer.pump(error));

    CHECK(!seen.empty());
    if (!seen.empty())
    {
        // Every delivered event carries the wire envelope, in the live incarnation, after the
        // snapshot cursor.
        CHECK(seen.front().seq > 0);
        CHECK(seen.front().incarnation_id == consumer.incarnation_id());
        CHECK(!seen.front().topic.empty());
        CHECK(consumer.states()[0].last_seq >= seen.front().seq);
    }

    // Acks reached the daemon (the retention floor advances off the client's cursor).
    CHECK(consumer.flush_acks(error));
    CHECK(consumer.states()[0].acked_seq == consumer.states()[0].last_seq);
    CHECK(consumer.stats().acks_sent > 0);

    consumer.stop();
    shutdown_daemon(daemon, *instance);

    std::error_code ec;
    fs::remove_all(project, ec);
}

// --- 2. D20: tokenless attach is DENIED by the live daemon (enforcement default ON) ---------------
void test_tokenless_attach_is_denied()
{
    const fs::path project = make_temp_project("auth");
    ctest_proc::Process daemon =
        ctest_proc::spawn(CONTEXT_BINARY, {"daemon", "--project", project.string()});
    CHECK(ctest_proc::valid(daemon));

    const std::optional<InstanceInfo> instance = discover_instance(project, 15000);
    CHECK(instance.has_value());
    if (!instance.has_value())
    {
        abandon_daemon(daemon);
        return;
    }
    CHECK(!instance->token.empty());

    // No token -> the daemon refuses the handshake. This is the C-F1 step-3 assertion: with the
    // enforcement default flipped ON, an unauthenticated attach cannot reach any verb.
    {
        std::unique_ptr<context::editor::client::WireChannel> channel =
            context::editor::client::make_transport_channel(instance->endpoint, 5000);
        CHECK(channel != nullptr);
        Client anonymous(std::move(channel));
        AttachOptions options;
        options.token.clear();
        std::string error;
        bool rejected = false;
        CHECK(!anonymous.attach(options, error, &rejected));
        CHECK(rejected); // a daemon-side refusal, not a transport hiccup
        CHECK(!anonymous.attached());
    }

    // A WRONG token is refused the same way.
    {
        std::unique_ptr<context::editor::client::WireChannel> channel =
            context::editor::client::make_transport_channel(instance->endpoint, 5000);
        CHECK(channel != nullptr);
        Client impostor(std::move(channel));
        AttachOptions options;
        options.token = "0000000000000000";
        std::string error;
        bool rejected = false;
        CHECK(!impostor.attach(options, error, &rejected));
        CHECK(rejected);
    }

    // The DISCOVERED token attaches cleanly — enforcement gates impostors, not legitimate clients.
    {
        std::unique_ptr<context::editor::client::WireChannel> channel =
            context::editor::client::make_transport_channel(instance->endpoint, 5000);
        CHECK(channel != nullptr);
        Client legitimate(std::move(channel));
        AttachOptions options;
        options.scope = "write,session";
        options.token = instance->token;
        std::string error;
        CHECK(legitimate.attach(options, error));
        CHECK(error.empty());
        CHECK(legitimate.attached());
    }

    shutdown_daemon(daemon, *instance);

    std::error_code ec;
    fs::remove_all(project, ec);
}

// --- 3. connect_to_project: discovery + connect + token seeding in one step -----------------------
void test_connect_to_project_seeds_the_token()
{
    const fs::path project = make_temp_project("discover");
    ctest_proc::Process daemon =
        ctest_proc::spawn(CONTEXT_BINARY, {"daemon", "--project", project.string()});
    CHECK(ctest_proc::valid(daemon));

    const std::optional<InstanceInfo> instance = discover_instance(project, 15000);
    CHECK(instance.has_value());
    if (!instance.has_value())
    {
        abandon_daemon(daemon);
        return;
    }

    AttachOptions options;
    options.scope = "write,session";
    std::string error;
    std::unique_ptr<Client> client = Client::connect_to_project(project, 5000, error);
    CHECK(client != nullptr);
    if (client)
    {
        // The discovered token rides on the Client — a consumer never reads the instance file, and
        // never plumbs the token through its own attach options.
        CHECK(client->instance().token == instance->token);
        CHECK(client->connected());
        CHECK(options.token.empty()); // still unset: attach() falls back to the discovered one
        CHECK(client->attach(options, error));
        CHECK(client->attached());
    }

    shutdown_daemon(daemon, *instance);

    std::error_code ec;
    fs::remove_all(project, ec);
}
} // namespace

int main()
{
    test_live_subscription_receives_real_events();
    test_tokenless_attach_is_denied();
    test_connect_to_project_seeds_the_token();
    CLIENT_TEST_MAIN_END();
}
