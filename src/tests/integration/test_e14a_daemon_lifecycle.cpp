// The M9 e14a daemon-lifecycle-spine T2 DEV-MODE DRILL (design 07 §4 / 03 §7 / 05 §2): the process
// model proven end to end against a REAL `context daemon` over the REAL loopback IPC wire (the
// m1-exit-* cross-process precedent). The packaged-shape T2 is e15/e16; this proves the FUNCTIONAL
// flows in dev-mode, exactly as the split ruling assigns.
//
// Four scenarios, one per DoD line:
//   1. SPAWN + the token via STDIO — no live daemon -> the Shell spawns `context daemon` as a child and
//      reads the D20 attach token off the child's stdout (never argv/env); owned; last-client exit
//      sends a clean `shutdown` and the daemon dies.
//   2. ATTACH to a pre-existing EXTERNAL daemon — never owned, and shutdown_at_exit LEAVES IT RUNNING.
//   3. EXIT POLICY — an owned daemon another client (a persistent RPC client) still holds is left
//      running on exit, not shut down.
//   4. RECONNECT — the daemon is lost -> the lifecycle enters the read-only STATE and auto-re-establishes
//      (respawns the owned daemon) with a fresh subscription (the e02 re-snapshot).

#include "context/editor/shell/daemon_lifecycle.h"

#include "context/editor/client/instance.h"
#include "context/editor/contract/handshake.h"
#include "integration_test.h"
#include "process_util.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>

namespace shell = context::editor::shell;
namespace client = context::editor::client;
namespace contract = context::editor::contract;
namespace fs = std::filesystem;

#if !defined(CONTEXT_BINARY)
#error "CONTEXT_BINARY (the built `context` executable path) must be defined by CMake"
#endif

namespace
{

std::int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Drive the lifecycle's owner-loop pump until `pred` holds or `timeout_ms` elapses. Records whether the
// read-only STATE was observed at any point (the reconnect proof).
template <class Pred>
bool pump_until(shell::DaemonLifecycle& lifecycle, Pred pred, int timeout_ms, bool& saw_read_only)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;)
    {
        lifecycle.pump(now_ms());
        if (lifecycle.read_only())
            saw_read_only = true;
        if (pred())
            return true;
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// Can a fresh transport still reach `endpoint`? Used to assert a daemon is (still) alive / is gone.
bool endpoint_reachable(const std::string& endpoint)
{
    return client::make_transport_channel(endpoint, 300) != nullptr;
}

void remove_tree(const fs::path& path)
{
    std::error_code ec;
    fs::remove_all(path, ec); // best-effort
}

const fs::path kBinary = fs::path(CONTEXT_BINARY);

// ---------------------------------------------------------------- 1. spawn + token via stdio

void test_spawn_reads_the_token_over_stdio_and_owns_the_daemon()
{
    const fs::path project = itest::make_temp_project("e14a-spawn");
    shell::DaemonLifecycle lifecycle;
    lifecycle.set_reconnect_policy(shell::ReconnectPolicy{500, 2000, 2});

    std::string error;
    const bool attached = lifecycle.spawn_or_attach(project, kBinary, error);
    CHECK(attached);
    CHECK(error.empty());
    // No live daemon existed, so the Shell SPAWNED one and OWNS it.
    CHECK(lifecycle.owns_daemon());
    CHECK(lifecycle.ownership() == shell::DaemonOwnership::spawned_owned);
    // The D20 token arrived over the child's STDOUT, not argv/env or a race with instance.json.
    CHECK(lifecycle.token_via_stdio());
    CHECK(!lifecycle.instance().token.empty());
    CHECK(!lifecycle.instance().endpoint.empty());
    CHECK(!lifecycle.read_only());
    CHECK(lifecycle.client() != nullptr);

    const std::string endpoint = lifecycle.instance().endpoint;
    CHECK(endpoint_reachable(endpoint)); // the spawned daemon is live

    // Last-client exit: owned + nobody else attached -> a clean in-band shutdown, and the daemon dies.
    lifecycle.shutdown_at_exit();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(8000);
    bool gone = false;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (!endpoint_reachable(endpoint))
        {
            gone = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(gone); // the owned daemon shut down on our exit
}

// ------------------------------------------------------- 2. attach external + never shut it down

void test_attach_to_an_external_daemon_never_owns_or_shuts_it_down()
{
    const fs::path project = itest::make_temp_project("e14a-external");
    // Start a daemon OUT OF BAND (an external one the Shell must attach to, never own).
    ctest_proc::Process daemon =
        ctest_proc::spawn(kBinary.string(), {"daemon", "--project", project.string()});
    CHECK(ctest_proc::valid(daemon));
    CHECK(itest::wait_for_instance(project, itest::scaled_timeout_ms(15000)));

    shell::DaemonLifecycle lifecycle;
    std::string error;
    const bool attached = lifecycle.spawn_or_attach(project, kBinary, error);
    CHECK(attached);
    CHECK(!lifecycle.owns_daemon());
    CHECK(lifecycle.ownership() == shell::DaemonOwnership::attached_external);
    CHECK(!lifecycle.token_via_stdio()); // discovered from instance.json, not spawned
    const std::string endpoint = lifecycle.instance().endpoint;

    // Exit: an external daemon is attached-to, NEVER owned -> it stays alive.
    lifecycle.shutdown_at_exit();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(endpoint_reachable(endpoint)); // still live after our exit

    // Clean up the external daemon we started.
    ctest_proc::kill(daemon);
    ctest_proc::release(daemon);
    remove_tree(project);
}

// ------------------------------------------------- 3. exit policy: an attached CLI keeps it alive

void test_an_attached_client_keeps_an_owned_daemon_alive_on_exit()
{
    const fs::path project = itest::make_temp_project("e14a-keepalive");
    shell::DaemonLifecycle lifecycle;
    lifecycle.set_reconnect_policy(shell::ReconnectPolicy{500, 2000, 2});
    std::string error;
    CHECK(lifecycle.spawn_or_attach(project, kBinary, error));
    CHECK(lifecycle.owns_daemon());
    const std::string endpoint = lifecycle.instance().endpoint;

    // A SECOND client attaches and STAYS attached (a running CLI / agent).
    itest::RpcClient other;
    CHECK(other.connect(project, itest::scaled_timeout_ms(10000)));
    CHECK(itest::is_ok(other.attach(contract::kProtocolMajor, {"describe"}, "session")));

    // The `clients` topic delta must reach the lifecycle's census; pump until it sees the other client.
    bool saw_read_only = false;
    const bool sees_other = pump_until(
        lifecycle, [&] { return lifecycle.census().others() >= 1; },
        itest::scaled_timeout_ms(10000), saw_read_only);
    CHECK(sees_other);

    // Exit: owned BUT another client holds an attachment -> leave the daemon running for them.
    lifecycle.shutdown_at_exit();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(endpoint_reachable(endpoint)); // still alive because the other client is still attached

    // Clean up: the other client shuts the surviving daemon down (also proves it was genuinely live).
    CHECK(itest::is_ok(other.call("shutdown", contract::Json::object())));
    other.close();
    remove_tree(project);
}

// ----------------------------------------------- 4. reconnect: daemon lost -> read-only -> reattach

void test_daemon_lost_enters_read_only_then_auto_reattaches()
{
    const fs::path project = itest::make_temp_project("e14a-reconnect");
    shell::DaemonLifecycle lifecycle;
    lifecycle.set_reconnect_policy(shell::ReconnectPolicy{500, 2000, 2});
    std::string error;
    CHECK(lifecycle.spawn_or_attach(project, kBinary, error));
    CHECK(lifecycle.owns_daemon());
    CHECK(!lifecycle.read_only());
    const std::uint64_t gen0 = lifecycle.attach_generation();

    // Kill the daemon out from under the lifecycle: a separate client attaches with session scope and
    // sends `shutdown` (an abrupt-enough loss — the wire drops under the lifecycle's subscription).
    {
        itest::RpcClient killer;
        CHECK(killer.connect(project, itest::scaled_timeout_ms(10000)));
        CHECK(itest::is_ok(killer.attach(contract::kProtocolMajor, {"describe"}, "session")));
        (void)killer.call("shutdown", contract::Json::object());
        killer.close();
    }

    // The lifecycle notices the loss (read-only STATE), then re-establishes on the backoff schedule by
    // respawning its owned daemon, with a fresh subscription (the e02 re-snapshot) — a NEW attach
    // generation. Read-only must have been observed along the way (03 §7).
    bool saw_read_only = false;
    const bool reattached = pump_until(
        lifecycle,
        [&] { return !lifecycle.read_only() && lifecycle.attach_generation() > gen0; },
        itest::scaled_timeout_ms(30000), saw_read_only);
    CHECK(saw_read_only);            // the daemon-lost read-only STATE was entered
    CHECK(reattached);               // and it auto-reattached
    CHECK(lifecycle.owns_daemon());  // a respawned owned daemon
    CHECK(lifecycle.client() != nullptr);

    // Clean up the respawned daemon (owned + sole client -> clean shutdown).
    lifecycle.shutdown_at_exit();
    remove_tree(project);
}

} // namespace

int main()
{
    test_spawn_reads_the_token_over_stdio_and_owns_the_daemon();
    test_attach_to_an_external_daemon_never_owns_or_shuts_it_down();
    test_an_attached_client_keeps_an_owned_daemon_alive_on_exit();
    test_daemon_lost_enters_read_only_then_auto_reattaches();
    ITEST_MAIN_END();
}
