// The daemon lifecycle spine (M9 e14a) — the PURE decision pieces, asserted with no real daemon:
// the exit-policy decision, the `clients`-topic census that drives it, the read-only/reconnect state
// machine, and the reported-not-fatal failure of spawn_or_attach when nothing is reachable. The
// end-to-end spawn-vs-attach + token-via-stdio + exit policy + reattach against a REAL daemon is the
// T2 integration drill (src/tests/integration/test_e14a_daemon_lifecycle.cpp).

#include "context/editor/shell/daemon_lifecycle.h"

#include "shell_test.h"

#include <string>

using namespace context::editor::shell;
namespace contract = context::editor::contract;
namespace fs = std::filesystem;

namespace
{

contract::Json clients_event(const char* kind)
{
    contract::Json payload = contract::Json::object();
    payload.set("event", contract::Json(std::string(kind)));
    return payload;
}

// -------------------------------------------------------------------------- exit-policy decision

void test_exit_policy_only_shuts_down_an_owned_daemon_of_which_we_are_the_last_client()
{
    // Owned + nobody else attached => the Shell is the daemon's last client, so a clean shutdown.
    CHECK(decide_daemon_exit_action(/*owns*/ true, /*others*/ 0) == DaemonExitAction::shutdown_owned);
    // Owned but another client (a CLI) is attached => leave it running for them.
    CHECK(decide_daemon_exit_action(true, 1) == DaemonExitAction::leave_running);
    CHECK(decide_daemon_exit_action(true, 5) == DaemonExitAction::leave_running);
    // An external daemon is NEVER owned, so it is never shut down regardless of the client count.
    CHECK(decide_daemon_exit_action(false, 0) == DaemonExitAction::leave_running);
    CHECK(decide_daemon_exit_action(false, 3) == DaemonExitAction::leave_running);
    // A negative count (never expected) is treated as "no other clients".
    CHECK(decide_daemon_exit_action(true, -1) == DaemonExitAction::shutdown_owned);
}

// ------------------------------------------------------------------------------- client census

void test_census_counts_other_clients_over_the_clients_topic()
{
    ClientCensus census;
    census.reset_to_self();
    CHECK(census.attached() == 1); // this process's own attachment is the baseline
    CHECK(census.others() == 0);

    census.on_clients_event(clients_event("attached")); // a CLI attaches
    CHECK(census.attached() == 2);
    CHECK(census.others() == 1); // "someone else is using this daemon" — keep it alive

    census.on_clients_event(clients_event("attached")); // and another
    CHECK(census.others() == 2);

    census.on_clients_event(clients_event("detached"));
    census.on_clients_event(clients_event("detached"));
    CHECK(census.attached() == 1);
    CHECK(census.others() == 0); // back to just us -> now shutting down would be correct
}

void test_census_ignores_garbage_and_never_underflows()
{
    ClientCensus census;
    census.reset_to_self();
    // A detach with no prior attach must not drive the count negative.
    census.on_clients_event(clients_event("detached"));
    census.on_clients_event(clients_event("detached"));
    CHECK(census.attached() >= 0);
    CHECK(census.others() == 0);

    // Non-object / missing-field / wrong-type payloads are ignored, not counted.
    census.on_clients_event(contract::Json());                     // null
    census.on_clients_event(contract::Json::object());             // no "event"
    census.on_clients_event(clients_event("something-else"));      // unknown event
    CHECK(census.attached() == 1 || census.attached() == 0);       // unchanged from the (floored) baseline
}

// ---------------------------------------------------------------- reconnect / read-only state

void test_reconnect_controller_enters_read_only_and_backs_off()
{
    ReconnectPolicy policy;
    policy.initial_ms = 100;
    policy.max_ms = 800;
    policy.multiplier = 2;
    ReconnectController link(policy);

    // A fresh controller is attached (read-write).
    CHECK(link.state() == DaemonLinkState::attached);

    // The daemon drops: read-only, first reattach armed at now + initial.
    link.note_lost(1'000);
    CHECK(link.state() == DaemonLinkState::read_only);
    CHECK(!link.attempt_due(1'050)); // not yet
    CHECK(link.attempt_due(1'100));  // initial delay elapsed

    // A failed attempt grows the delay (100 -> 200 -> 400 ...), clamped to max_ms.
    link.note_attempt_result(false, 1'100);
    CHECK(link.state() == DaemonLinkState::read_only);
    CHECK(!link.attempt_due(1'250));
    CHECK(link.attempt_due(1'300)); // +200
    link.note_attempt_result(false, 1'300);
    CHECK(link.attempt_due(1'700)); // +400
    link.note_attempt_result(false, 1'700);
    CHECK(link.attempt_due(2'500)); // +800 (clamped at max)

    // A successful attempt returns to attached and resets the ladder.
    link.note_attempt_result(true, 2'500);
    CHECK(link.state() == DaemonLinkState::attached);
    CHECK(link.attempts() == 0);

    // delay_for_attempt is a bounded pure function.
    CHECK(link.delay_for_attempt(0) == 100);
    CHECK(link.delay_for_attempt(1) == 200);
    CHECK(link.delay_for_attempt(10) == 800); // clamped
}

void test_reconnect_controller_fixed_delay_policy()
{
    ReconnectPolicy fixed;
    fixed.initial_ms = 250;
    fixed.max_ms = 5000;
    fixed.multiplier = 1; // multiplier <= 1 is a legitimate FIXED-delay policy, not a misuse
    ReconnectController link(fixed);
    CHECK(link.delay_for_attempt(0) == 250);
    CHECK(link.delay_for_attempt(4) == 250);
}

// --------------------------------------------------------- coordinator: reported-not-fatal failure

void test_spawn_or_attach_with_nothing_reachable_is_read_only_not_fatal()
{
    const fs::path project = shelltest::make_temp_project("context-e14a", "nodaemon");
    DaemonLifecycle lifecycle;
    // No daemon to discover AND an empty daemon binary to spawn => it cannot establish, but it does not
    // throw or crash: it reports the failure and drops into the read-only STATE so pump() keeps trying.
    std::string error;
    const bool attached = lifecycle.spawn_or_attach(project, fs::path{}, error);
    CHECK(!attached);
    CHECK(!error.empty());
    CHECK(lifecycle.read_only());
    CHECK(!lifecycle.attached());
    CHECK(lifecycle.client() == nullptr);
    CHECK(lifecycle.ownership() == DaemonOwnership::none);
    CHECK(!lifecycle.owns_daemon());

    // Exit with nothing owned is a no-op (never shuts down a daemon it does not own); idempotent.
    lifecycle.shutdown_at_exit();
    lifecycle.shutdown_at_exit();

    shelltest::cleanup(project);
}

} // namespace

int main()
{
    test_exit_policy_only_shuts_down_an_owned_daemon_of_which_we_are_the_last_client();
    test_census_counts_other_clients_over_the_clients_topic();
    test_census_ignores_garbage_and_never_underflows();
    test_reconnect_controller_enters_read_only_and_backs_off();
    test_reconnect_controller_fixed_delay_policy();
    test_spawn_or_attach_with_nothing_reachable_is_read_only_not_fatal();
    SHELL_TEST_MAIN_END();
}
