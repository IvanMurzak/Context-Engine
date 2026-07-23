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
//   5. SHUTDOWN-REPLY SURVIVAL (CE #352 regression) — a client that asks an otherwise-idle daemon to
//      shut down gets its reply even when another connection arrives in the same instant.
//
// EVERY wait here is BOUNDED (CE #352): the per-step waits have their own deadlines, and the
// stage watchdog below caps the WHOLE drill, so a regression always fails loudly and fast with the
// stage it died in — it can never stall the gate. See `kWatchdogBudgetMs`.

#include "context/editor/shell/daemon_lifecycle.h"

#include "context/editor/client/instance.h"
#include "context/editor/contract/handshake.h"
#include "integration_test.h"
#include "process_util.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
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

// --- the stage watchdog: this drill can FAIL, never HANG (CE #352) --------------------------------
// Individual waits below are all deadline-bounded, but several of them delegate into the client SDK's
// `Client::call`, whose response read BLOCKS by design (wire.h: a sleep-poll there would lose the
// reply of a daemon that answers and exits in the same breath). That read is bounded only by the peer
// being alive, so a wedged daemon would stall the whole ctest gate — locally it blocks the dev gate
// outright and in CI it burns a leg's wall-clock before ctest's TIMEOUT reports a verdict with no
// diagnosis. A watchdog thread converts that into a loud, fast, SELF-DIAGNOSING failure naming the
// exact stage: it is the harness-wide guarantee, not a substitute for the per-step deadlines.
//
// Budget: ~15x a healthy run (≈4 s), scaled for the instrumented legs, and comfortably inside the
// ctest TIMEOUT (300 s) so OUR message is what a failure prints, not ctest's.
constexpr int kWatchdogBudgetMs = 60000;

std::atomic<const char*> g_stage{"startup"};

void stage(const char* name)
{
    g_stage.store(name, std::memory_order_relaxed);
}

void start_watchdog()
{
    std::thread(
        []
        {
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(itest::scaled_timeout_ms(kWatchdogBudgetMs));
            while (std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::fprintf(stderr,
                         "WATCHDOG: editor-shell-daemon-lifecycle-t2 exceeded its %d ms budget while "
                         "in stage '%s' — a wait did not return. Failing loudly instead of hanging.\n",
                         itest::scaled_timeout_ms(kWatchdogBudgetMs), g_stage.load());
            std::fflush(stderr);
            std::_Exit(1); // immediate + non-zero: ctest reports Failed with the stage above
        })
        .detach();
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
    stage("1. spawn+token-over-stdio: spawn_or_attach");
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
    stage("1. spawn+token-over-stdio: shutdown_at_exit + wait for the daemon to die");
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
    stage("2. attach-external: booting the out-of-band daemon");
    const fs::path project = itest::make_temp_project("e14a-external");
    // Start a daemon OUT OF BAND (an external one the Shell must attach to, never own).
    ctest_proc::Process daemon =
        ctest_proc::spawn(kBinary.string(), {"daemon", "--project", project.string()});
    CHECK(ctest_proc::valid(daemon));
    CHECK(itest::wait_for_instance(project, itest::scaled_timeout_ms(15000)));

    shell::DaemonLifecycle lifecycle;
    std::string error;
    stage("2. attach-external: spawn_or_attach onto the external daemon");
    const bool attached = lifecycle.spawn_or_attach(project, kBinary, error);
    CHECK(attached);
    CHECK(!lifecycle.owns_daemon());
    CHECK(lifecycle.ownership() == shell::DaemonOwnership::attached_external);
    CHECK(!lifecycle.token_via_stdio()); // discovered from instance.json, not spawned
    const std::string endpoint = lifecycle.instance().endpoint;

    // Exit: an external daemon is attached-to, NEVER owned -> it stays alive.
    stage("2. attach-external: shutdown_at_exit must LEAVE the external daemon running");
    lifecycle.shutdown_at_exit();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(endpoint_reachable(endpoint)); // still live after our exit

    // Clean up the external daemon we started. SIGKILL/TerminateProcess, so the reap is prompt —
    // but bound it anyway (release() blocks in waitpid on POSIX) so a wedged child cannot stall here.
    stage("2. attach-external: reaping the out-of-band daemon");
    ctest_proc::kill(daemon);
    int reaped_code = -1;
    CHECK(ctest_proc::wait_for(daemon, itest::scaled_timeout_ms(10000), reaped_code));
    ctest_proc::release(daemon);
    remove_tree(project);
}

// ------------------------------------------------- 3. exit policy: an attached CLI keeps it alive

void test_an_attached_client_keeps_an_owned_daemon_alive_on_exit()
{
    stage("3. exit-policy: spawn the owned daemon");
    const fs::path project = itest::make_temp_project("e14a-keepalive");
    shell::DaemonLifecycle lifecycle;
    lifecycle.set_reconnect_policy(shell::ReconnectPolicy{500, 2000, 2});
    std::string error;
    CHECK(lifecycle.spawn_or_attach(project, kBinary, error));
    CHECK(lifecycle.owns_daemon());
    const std::string endpoint = lifecycle.instance().endpoint;

    // A SECOND client attaches and STAYS attached (a running CLI / agent).
    stage("3. exit-policy: second client attaches");
    itest::RpcClient other;
    CHECK(other.connect(project, itest::scaled_timeout_ms(10000)));
    CHECK(itest::is_ok(other.attach(contract::kProtocolMajor, {"describe"}, "session")));

    // The `clients` topic delta must reach the lifecycle's census; pump until it sees the other client.
    stage("3. exit-policy: pumping until the census sees the other client");
    bool saw_read_only = false;
    const bool sees_other = pump_until(
        lifecycle, [&] { return lifecycle.census().others() >= 1; },
        itest::scaled_timeout_ms(10000), saw_read_only);
    CHECK(sees_other);

    // Exit: owned BUT another client holds an attachment -> leave the daemon running for them.
    stage("3. exit-policy: shutdown_at_exit must LEAVE the daemon running");
    lifecycle.shutdown_at_exit();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(endpoint_reachable(endpoint)); // still alive because the other client is still attached

    // Clean up: the other client shuts the surviving daemon down (also proves it was genuinely live).
    // NB (CE #352): `endpoint_reachable()` just above is a connect-then-immediately-close, i.e. it
    // unparks the daemon's acceptor microseconds before this `shutdown` — the exact interleaving that
    // used to cost this reply. Scenario 5 pins that mechanism directly; keep the two adjacent here.
    stage("3. exit-policy: the other client shuts the surviving daemon down");
    const std::optional<contract::Json> stopped = other.call("shutdown", contract::Json::object());
    if (!itest::is_ok(stopped))
        std::fprintf(stderr,
                     "  diagnosis: shutdown from the surviving client failed. transport=%s "
                     "endpoint_reachable=%d response=%s\n",
                     other.error().c_str(), endpoint_reachable(endpoint) ? 1 : 0,
                     stopped.has_value() ? stopped->dump(0).c_str() : "(none — transport failure)");
    CHECK(itest::is_ok(stopped));
    other.close();
    remove_tree(project);
}

// ----------------------------------------------- 4. reconnect: daemon lost -> read-only -> reattach

void test_daemon_lost_enters_read_only_then_auto_reattaches()
{
    stage("4. reconnect: spawn the owned daemon");
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
        stage("4. reconnect: killing the daemon out from under the lifecycle");
        itest::RpcClient killer;
        CHECK(killer.connect(project, itest::scaled_timeout_ms(10000)));
        CHECK(itest::is_ok(killer.attach(contract::kProtocolMajor, {"describe"}, "session")));
        (void)killer.call("shutdown", contract::Json::object());
        killer.close();
    }

    // The lifecycle notices the loss (read-only STATE), then re-establishes on the backoff schedule by
    // respawning its owned daemon, with a fresh subscription (the e02 re-snapshot) — a NEW attach
    // generation. Read-only must have been observed along the way (03 §7).
    stage("4. reconnect: pumping until read-only is observed and the lifecycle re-attaches");
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
    stage("4. reconnect: shutting the respawned daemon down");
    lifecycle.shutdown_at_exit();
    remove_tree(project);
}

// ------------------------------------------- 5. the `shutdown` reply survives a concurrent connect

// CE #352 regression. A client that asks the daemon to shut down MUST receive the reply, even when
// another connection arrives in the same instant.
//
// The daemon serves `shutdown` by setting its stop flag while BUILDING the reply; the reply is then
// queued and written on the connection thread's next pass. serve()'s teardown used to force-unblock
// (half-close) every connection the moment the acceptor observed the stop flag, discarding anything
// still queued — the reply included. It hid because the acceptor is normally PARKED in accept() and
// is woken only by the replying thread, AFTER its flush. Any connection arriving in that window
// unparks it early and the reply is destroyed; the caller sees "peer disconnected before responding".
//
// This drives exactly that interleaving. Scenario 3 already reproduces it by accident — its
// `endpoint_reachable()` sits one statement before its own shutdown — but only sometimes: a
// single-variable A/B against a live daemon measured 19/40 replies lost WITH that one throwaway
// connect and 0/40 WITHOUT it. A coin flip is a flake report, not a gate (which is exactly how this
// defect spent so long miscatalogued), so rather than pad repetitions this scenario holds the
// acceptor unparked for the WHOLE duration of the call: a churn thread connects and closes
// continuously while `shutdown` is in flight. Measured that way, the defect is caught in 5/5 runs
// (28/30 counting every pre-fix run), and the fixed daemon passed 85/85.
void test_shutdown_reply_survives_a_concurrent_connect()
{
    // With the churn the detection is near-deterministic, so a handful of attempts is a gate rather
    // than a lottery. Each attempt owns a fresh daemon (a served `shutdown` ends that daemon).
    constexpr int kAttempts = 3;
    for (int attempt = 0; attempt < kAttempts; ++attempt)
    {
        stage("5. shutdown-reply survival: booting a daemon for one attempt");
        const fs::path project = itest::make_temp_project("e14a-shutdown-reply");
        shell::DaemonLifecycle lifecycle;
        std::string error;
        CHECK(lifecycle.spawn_or_attach(project, kBinary, error));
        const std::string endpoint = lifecycle.instance().endpoint;

        itest::RpcClient client;
        CHECK(client.connect(project, itest::scaled_timeout_ms(10000)));
        CHECK(itest::is_ok(client.attach(contract::kProtocolMajor, {"describe"}, "session")));

        // The census must SEE `client` before the exit policy is evaluated, or shutdown_at_exit would
        // take the last-client path and stop the daemon itself — leaving nothing for the interleaving
        // below to prove. Bounded, like every other wait here.
        bool saw_read_only = false;
        CHECK(pump_until(
            lifecycle, [&] { return lifecycle.census().others() >= 1; },
            itest::scaled_timeout_ms(10000), saw_read_only));

        // Drop the Shell's own wire so `client` is the sole attached client, exactly as in scenario 3
        // after the Shell exits; the spawned daemon is left running (detached, never killed here).
        lifecycle.shutdown_at_exit();

        // THE INTERLEAVING: keep the acceptor unparked for the whole duration of the call, so the
        // window between "stop flag set" and "reply written" is always covered.
        stage("5. shutdown-reply survival: concurrent connect churn, then shutdown");
        CHECK(endpoint_reachable(endpoint));
        std::atomic<bool> churning{true};
        std::thread churn(
            [&]
            {
                while (churning.load(std::memory_order_relaxed))
                {
                    (void)endpoint_reachable(endpoint); // connect + immediate close; false once stopped
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            });
        const std::optional<contract::Json> reply =
            client.call("shutdown", contract::Json::object());
        churning.store(false, std::memory_order_relaxed);
        churn.join();
        if (!itest::is_ok(reply))
            std::fprintf(stderr,
                         "  CE#352 regression (attempt %d/%d): the daemon lost the `shutdown` reply. "
                         "transport=%s response=%s\n",
                         attempt + 1, kAttempts, client.error().c_str(),
                         reply.has_value() ? reply->dump(0).c_str() : "(none — transport failure)");
        CHECK(itest::is_ok(reply));
        client.close();

        // The daemon really does stop on that reply (bounded wait — never an unbounded poll).
        stage("5. shutdown-reply survival: the daemon must actually stop");
        const auto deadline =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(itest::scaled_timeout_ms(10000));
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
        CHECK(gone);
        remove_tree(project);
    }
}

} // namespace

int main()
{
    start_watchdog(); // FIRST: from here on the drill can fail, but it can never hang (CE #352)
    test_spawn_reads_the_token_over_stdio_and_owns_the_daemon();
    test_attach_to_an_external_daemon_never_owns_or_shuts_it_down();
    test_an_attached_client_keeps_an_owned_daemon_alive_on_exit();
    test_daemon_lost_enters_read_only_then_auto_reattaches();
    test_shutdown_reply_survives_a_concurrent_connect();
    stage("done");
    ITEST_MAIN_END();
}
