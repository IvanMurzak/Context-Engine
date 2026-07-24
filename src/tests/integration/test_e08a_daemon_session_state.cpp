// The M9 e08a DAEMON SESSION-STATE T2 DRILL (design 05 §4 / 03 §1 / 07 §6, D7 tier 1) — the
// multi-CLIENT proof, end to end against a REAL `context daemon` over the REAL loopback IPC wire
// (the m1-exit-* / e14a cross-process precedent).
//
// The e08a DoD's "propagates to a SECOND CLIENT (the CLI **and** a scripted agent client)" is proven
// with BOTH kinds of client against ONE daemon, deliberately not two of the same kind:
//   * the SCRIPTED AGENT CLIENT = context_client (the shipped SDK any out-of-tree agent uses),
//     subscribed to the `session` topic;
//   * the CLI = the REAL built `context` binary, spawned as a separate process
//     (`context attach --editor-select …`) — the same door a human or a shell script comes through.
//
// Four scenarios, one per DoD line:
//   1. PROPAGATION + ECHO SUPPRESSION — the CLI selects, the agent observes the fact stamped with
//      the CLI's `origin` (not its own) and reads the same selection back; then the agent selects
//      and receives its OWN fact with origin == its client id — the frame it must DROP. That one
//      rule, applied to a single shared stream, is the whole echo-suppression contract.
//   2. PLAY CONTROL over RPC — the CLI drives play/step/stop; the agent observes the play-state
//      sequence, and the tokens are asserted BYTE-IDENTICAL to gui::playbar::state_token() (this
//      drill links the real playbar), so the L-51 indicator is fed from the topic with no
//      translation layer to drift.
//   3. PERSISTENCE across a daemon restart — a clean shutdown writes `.editor/session.json`
//      (daemon = single writer, 03 §1) and the NEXT daemon restores it on attach.
//   4. CORRUPT-FILE RECOVERY — a mangled session file is renamed aside, defaults are loaded, the
//      daemon still boots and serves (LOUD + non-blocking, 07 §6), and the diagnostic is observable
//      by a client replaying the ring from seq 0.

#include "context/editor/client/client.h"
#include "context/editor/editorkernel/editor_session_state.h"
#include "context/editor/gui/playbar/playbar_model.h"

#include "integration_test.h"
#include "process_util.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace client = context::editor::client;
namespace ek = context::editor::editorkernel;
namespace playbar = context::editor::gui::playbar;
namespace fs = std::filesystem;
using context::editor::contract::Json;

#if !defined(CONTEXT_BINARY)
#error "CONTEXT_BINARY (the built `context` executable path) must be defined by CMake"
#endif

namespace
{
const fs::path kBinary = fs::path(CONTEXT_BINARY);

void remove_tree(const fs::path& path)
{
    std::error_code ec;
    fs::remove_all(path, ec); // best-effort
}

// Spawn a real `context daemon --project <dir>` and wait for its discovery hint.
ctest_proc::Process spawn_daemon(const fs::path& project, const std::string& previous_hint)
{
    ctest_proc::Process daemon =
        ctest_proc::spawn(kBinary.string(), {"daemon", "--project", project.string()});
    if (!ctest_proc::valid(daemon))
        return daemon;
    if (!itest::wait_for_instance(project, itest::scaled_timeout_ms(20000), previous_hint))
    {
        ctest_proc::kill(daemon);
        ctest_proc::release(daemon);
        return ctest_proc::Process{};
    }
    return daemon;
}

void reap(ctest_proc::Process& daemon)
{
    if (!ctest_proc::valid(daemon))
        return;
    int code = 0;
    if (!ctest_proc::wait_for(daemon, itest::scaled_timeout_ms(8000), code))
        ctest_proc::kill(daemon);
    ctest_proc::release(daemon);
}

// Run the REAL `context` binary as a SEPARATE process and return its exit code (-1 on spawn failure
// / timeout). This is the CLI-as-second-client half of the DoD.
int run_cli(const std::vector<std::string>& args)
{
    ctest_proc::Process p = ctest_proc::spawn(kBinary.string(), args);
    if (!ctest_proc::valid(p))
        return -1;
    int code = -1;
    if (!ctest_proc::wait_for(p, itest::scaled_timeout_ms(30000), code))
    {
        ctest_proc::kill(p);
        code = -1;
    }
    ctest_proc::release(p);
    return code;
}

// Attach a scripted AGENT client (the shipped SDK) with session scope, subscribed to `session`.
std::unique_ptr<client::Client> attach_agent(const fs::path& project, const char* topic,
                                             std::string& error)
{
    std::unique_ptr<client::Client> agent =
        client::Client::connect_to_project(project, itest::scaled_timeout_ms(8000), error);
    if (!agent)
        return nullptr;
    client::AttachOptions options;
    options.scope = "session";
    if (!agent->attach(options, error))
        return nullptr;
    Json params = Json::object();
    Json topics = Json::array();
    topics.push_back(Json(std::string(topic)));
    params.set("topics", std::move(topics));
    if (!agent->call("subscribe", std::move(params), error).has_value())
        return nullptr;
    return agent;
}

// Drain pushed frames for up to `timeout_ms`, collecting the payloads on `topic`.
std::vector<Json> collect_facts(client::Client& c, const char* topic, int timeout_ms)
{
    std::vector<Json> facts;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;)
    {
        bool disconnected = false;
        const std::optional<client::InboundFrame> frame = c.poll_event(100, disconnected);
        if (frame.has_value() && frame->event.contains("topic") &&
            frame->event.at("topic").as_string() == topic)
            facts.push_back(frame->event.at("payload"));
        if (disconnected || std::chrono::steady_clock::now() >= deadline)
            break;
    }
    return facts;
}

std::optional<Json> last_fact(const std::vector<Json>& facts, const std::string& kind)
{
    std::optional<Json> found;
    for (const Json& f : facts)
        if (f.contains("event") && f.at("event").as_string() == kind)
            found = f;
    return found;
}

// -------------------------------------------------- 1 + 2. propagation, echo suppression, play

void test_propagation_and_echo_suppression_and_play()
{
    const fs::path project = itest::make_temp_project("e08a-multiclient");
    ctest_proc::Process daemon = spawn_daemon(project, std::string());
    CHECK(ctest_proc::valid(daemon));
    if (!ctest_proc::valid(daemon))
    {
        remove_tree(project);
        return;
    }

    std::string error;
    const std::unique_ptr<client::Client> agent = attach_agent(project, "session", error);
    CHECK(agent != nullptr);
    if (agent == nullptr)
    {
        std::fprintf(stderr, "agent attach failed: %s\n", error.c_str());
        reap(daemon);
        remove_tree(project);
        return;
    }
    // The agent knows its own echo-suppression identity, straight off the attach reply.
    CHECK(agent->client_id() > 0);

    // --- the CLI (a SEPARATE process) selects -----------------------------------------------------
    CHECK(run_cli({"attach", "--project", project.string(), "--editor-select",
                   "root/child,root/other"}) == 0);

    const std::vector<Json> facts = collect_facts(*agent, "session", itest::scaled_timeout_ms(5000));
    const std::optional<Json> selection_fact = last_fact(facts, "selection-changed");
    CHECK(selection_fact.has_value());
    if (selection_fact.has_value())
    {
        // The fact is stamped with the CLI's id — a DIFFERENT client — so the agent APPLIES it.
        CHECK(selection_fact->at("origin").as_int() > 0);
        CHECK(static_cast<std::uint64_t>(selection_fact->at("origin").as_int()) !=
              agent->client_id());
        CHECK(selection_fact->at("ids").size() == 2);
        CHECK(selection_fact->at("ids").at(0).as_string() == "root/child");
    }

    // ...and the agent reads the SAME selection back over its own connection: one truth, N clients.
    const std::optional<Json> read = agent->call("editor.selection-get", Json::object(), error);
    CHECK(read.has_value());
    if (read.has_value())
    {
        const Json& ids = read->at("data").at("ids");
        CHECK(ids.size() == 2);
        CHECK(ids.at(1).as_string() == "root/other");
    }

    // --- the agent selects: it receives its OWN fact, which is the echo it must DROP --------------
    Json own = Json::object();
    Json own_ids = Json::array();
    own_ids.push_back(Json(std::string("root/agent-pick")));
    own.set("ids", std::move(own_ids));
    CHECK(agent->call("editor.select", std::move(own), error).has_value());

    const std::vector<Json> own_facts =
        collect_facts(*agent, "session", itest::scaled_timeout_ms(5000));
    const std::optional<Json> own_fact = last_fact(own_facts, "selection-changed");
    CHECK(own_fact.has_value());
    if (own_fact.has_value())
    {
        CHECK(static_cast<std::uint64_t>(own_fact->at("origin").as_int()) == agent->client_id());
    }
    // The CLI (a third attach) sees the agent's selection — propagation is symmetric.
    CHECK(run_cli({"attach", "--project", project.string(), "--editor-session"}) == 0);

    // --- play control over RPC, driven by the CLI, observed by the agent (L-51) -------------------
    CHECK(run_cli({"attach", "--project", project.string(), "--editor-play", "play"}) == 0);
    CHECK(run_cli({"attach", "--project", project.string(), "--editor-play", "step",
                   "--editor-ticks", "2"}) == 0);
    CHECK(run_cli({"attach", "--project", project.string(), "--editor-play", "stop"}) == 0);

    const std::vector<Json> play_facts =
        collect_facts(*agent, "session", itest::scaled_timeout_ms(5000));
    std::vector<std::string> states;
    for (const Json& f : play_facts)
        if (f.contains("event") && f.at("event").as_string() == "play-state")
            states.push_back(f.at("state").as_string());
    CHECK(states.size() == 3);
    if (states.size() == 3)
    {
        // The L-51 indicator vocabulary, asserted against the REAL playbar the indicator renders
        // from — this drill links context_gui_playbar precisely so the two cannot drift.
        CHECK(states[0] == playbar::state_token(playbar::PlayState::playing));
        CHECK(states[1] == playbar::state_token(playbar::PlayState::playing)); // step keeps state
        CHECK(states[2] == playbar::state_token(playbar::PlayState::edit));    // stop discards
    }

    // A play control issued in `edit` state is REFUSED (play.not_running), not silently absorbed —
    // the CLI surfaces the daemon's exit class rather than a bland success.
    CHECK(run_cli({"attach", "--project", project.string(), "--editor-play", "pause"}) != 0);

    CHECK(run_cli({"attach", "--project", project.string(), "--editor-session", "--shutdown"}) == 0);
    reap(daemon);
    remove_tree(project);
}

// ---------------------- 2b. N windows are N origins + the e10d SECOND-WINDOW SELECTION DRILL 1
//
// e10a makes N native editor windows real, and each one opens its OWN wire connection to the daemon
// precisely so it has its OWN `origin` (the Shell's window factory in `editor_main.cpp` calls
// `attach_to_project` per window). That is the premise every later cross-window drill rests on —
// e10c/e10d's echo-suppression checks are meaningless if two windows share one identity — and it is
// only true because ids are minted PER WIRE CONNECTION, which nothing but a real daemon can show.
//
// The registry's half (each window REPORTS its own connection's id, and two windows sharing one
// connection collapse to one origin) is asserted CEF-free in `editor-shell-test_window_registry`.
// This is the wire half: two connections to ONE daemon on ONE project get two different, non-zero
// ids — deliberately checked here, in the drill that owns the `origin` contract, rather than
// duplicated into a second daemon-spawning test.
//
// M9 e10d — INHERITED DRILL 1 (the "second WINDOW" selection-sync clause e08b deferred here because
// no second window existed then; now one does). Distinct origins are necessary but NOT sufficient: the
// DoD is that a SELECTION made in window A CONVERGES in window B THROUGH THE DAEMON, and that A's own
// echo is SUPPRESSED at A. So this drill now also asserts, over the SAME two real wires:
//   * CONVERGENCE — window B reads the SAME selection back (`editor.selection-get`), so B's view really
//     became A's, not merely that B saw a frame go by;
//   * ECHO SUPPRESSION AT THE SOURCE — window A receives its OWN `selection-changed` stamped with A's
//     origin, the frame A must DROP (a single-window build has no second origin, so this A-applies /
//     A-drops distinction cannot even be EXPRESSED there — which is the whole reason the clause waited
//     for a real second window);
//   * SYMMETRY — the same holds with the roles reversed, so neither direction is a fluke of ordering.
void test_two_windows_get_distinct_wire_origins()
{
    const fs::path project = itest::make_temp_project("e10a-window-origins");
    ctest_proc::Process daemon = spawn_daemon(project, std::string());
    CHECK(ctest_proc::valid(daemon));
    if (!ctest_proc::valid(daemon))
    {
        remove_tree(project);
        return;
    }

    std::string error;
    // Two attaches, exactly as two windows of one editor make them.
    const std::unique_ptr<client::Client> window0 = attach_agent(project, "session", error);
    const std::unique_ptr<client::Client> window1 = attach_agent(project, "session", error);
    CHECK(window0 != nullptr);
    CHECK(window1 != nullptr);
    if (window0 != nullptr && window1 != nullptr)
    {
        // Non-zero: 0 is the daemon itself AND "not attached", so it is never an identity.
        CHECK(window0->client_id() > 0);
        CHECK(window1->client_id() > 0);
        // DISTINCT: the whole point. Same daemon, same project, same process — different wires.
        CHECK(window0->client_id() != window1->client_id());

        // ...and the identity is LIVE, not just a number handed out at attach: a change made over
        // window 1's wire publishes a fact stamped with window 1's origin, which is what makes
        // window 0 apply it and window 1 drop it as its own echo.
        Json params = Json::object();
        Json ids = Json::array();
        ids.push_back(Json(std::string("root/from-window-1")));
        params.set("ids", std::move(ids));
        CHECK(window1->call("editor.select", std::move(params), error).has_value());

        const std::vector<Json> facts =
            collect_facts(*window0, "session", itest::scaled_timeout_ms(5000));
        const std::optional<Json> fact = last_fact(facts, "selection-changed");
        CHECK(fact.has_value());
        if (fact.has_value())
        {
            CHECK(static_cast<std::uint64_t>(fact->at("origin").as_int()) == window1->client_id());
            CHECK(static_cast<std::uint64_t>(fact->at("origin").as_int()) != window0->client_id());
        }

        // e10d Drill 1 — CONVERGENCE: window B (window0) does not merely SEE window A's frame; its own
        // view of the daemon's selection became window A's. One truth, N windows (05 §4).
        const std::optional<Json> b_view = window0->call("editor.selection-get", Json::object(), error);
        CHECK(b_view.has_value());
        if (b_view.has_value())
        {
            const Json& ids_b = b_view->at("data").at("ids");
            CHECK(ids_b.size() == 1);
            CHECK(ids_b.at(0).as_string() == "root/from-window-1");
        }

        // e10d Drill 1 — ECHO SUPPRESSION AT THE SOURCE: window A (window1) ALSO receives the fact it
        // caused, stamped with ITS OWN origin — the frame the client drops rather than re-applying.
        // The daemon broadcasts to every subscriber including the author, so this frame is on window1's
        // stream too; the `origin == self` is the ONLY thing that tells A "this is my own echo".
        const std::vector<Json> source_facts =
            collect_facts(*window1, "session", itest::scaled_timeout_ms(3000));
        const std::optional<Json> own_echo = last_fact(source_facts, "selection-changed");
        CHECK(own_echo.has_value());
        if (own_echo.has_value())
        {
            CHECK(static_cast<std::uint64_t>(own_echo->at("origin").as_int()) == window1->client_id());
        }

        // e10d Drill 1 — SYMMETRY: reverse the roles. window0 (now the source) selects; window1
        // converges through the daemon and window0 sees its own echo. Neither direction is privileged.
        Json rev = Json::object();
        Json rev_ids = Json::array();
        rev_ids.push_back(Json(std::string("root/from-window-0")));
        rev.set("ids", std::move(rev_ids));
        CHECK(window0->call("editor.select", std::move(rev), error).has_value());

        const std::vector<Json> rev_facts =
            collect_facts(*window1, "session", itest::scaled_timeout_ms(5000));
        const std::optional<Json> rev_fact = last_fact(rev_facts, "selection-changed");
        CHECK(rev_fact.has_value());
        if (rev_fact.has_value())
        {
            // window1 APPLIES it (a foreign origin — window0's), so its own view converges.
            CHECK(static_cast<std::uint64_t>(rev_fact->at("origin").as_int()) == window0->client_id());
            CHECK(static_cast<std::uint64_t>(rev_fact->at("origin").as_int()) != window1->client_id());
        }
        const std::optional<Json> a_view = window1->call("editor.selection-get", Json::object(), error);
        CHECK(a_view.has_value());
        if (a_view.has_value())
        {
            const Json& ids_a = a_view->at("data").at("ids");
            CHECK(ids_a.size() == 1);
            CHECK(ids_a.at(0).as_string() == "root/from-window-0");
        }
    }

    CHECK(run_cli({"attach", "--project", project.string(), "--editor-session", "--shutdown"}) == 0);
    reap(daemon);
    remove_tree(project);
}

// -------------------------------------------------- 3. persistence across a daemon restart

void test_session_state_survives_a_daemon_restart()
{
    const fs::path project = itest::make_temp_project("e08a-persist");

    ctest_proc::Process first = spawn_daemon(project, std::string());
    CHECK(ctest_proc::valid(first));
    if (!ctest_proc::valid(first))
    {
        remove_tree(project);
        return;
    }
    const std::string first_hint = itest::read_file(project / ".editor" / "instance.json");

    CHECK(run_cli({"attach", "--project", project.string(), "--editor-select",
                   "persisted/one,persisted/two", "--shutdown"}) == 0);
    reap(first);

    // The DAEMON wrote it (03 §1 single writer) on the clean shutdown.
    CHECK(fs::exists(project / ".editor" / "session.json"));

    // A FRESH daemon on the same project restores it, and a client sees it on attach.
    ctest_proc::Process second = spawn_daemon(project, first_hint);
    CHECK(ctest_proc::valid(second));
    if (ctest_proc::valid(second))
    {
        std::string error;
        const std::unique_ptr<client::Client> agent = attach_agent(project, "session", error);
        CHECK(agent != nullptr);
        if (agent != nullptr)
        {
            const std::optional<Json> read =
                agent->call("editor.selection-get", Json::object(), error);
            CHECK(read.has_value());
            if (read.has_value())
            {
                const Json& ids = read->at("data").at("ids");
                CHECK(ids.size() == 2);
                CHECK(ids.at(0).as_string() == "persisted/one");
            }
        }
        CHECK(run_cli({"attach", "--project", project.string(), "--editor-session", "--shutdown"}) ==
              0);
        reap(second);
    }
    remove_tree(project);
}

// -------------------------------------------------- 4. corrupt-file recovery (loud, non-blocking)

void test_corrupt_session_file_recovers_loudly_without_blocking_the_boot()
{
    const fs::path project = itest::make_temp_project("e08a-corrupt");
    std::error_code ec;
    fs::create_directories(project / ".editor", ec);
    CHECK(itest::write_file_raw(project / ".editor" / "session.json", "{ not json at all"));

    ctest_proc::Process daemon = spawn_daemon(project, std::string());
    CHECK(ctest_proc::valid(daemon)); // NON-BLOCKING: the daemon still booted and published its hint
    if (!ctest_proc::valid(daemon))
    {
        remove_tree(project);
        return;
    }

    // The corrupt file was renamed ASIDE (its evidence preserved), leaving the daemon's path clear.
    CHECK(!fs::exists(project / ".editor" / "session.json"));
    CHECK(fs::exists(project / ".editor" / "session.corrupt.json"));

    std::string error;
    const std::unique_ptr<client::Client> agent = attach_agent(project, "diagnostics", error);
    CHECK(agent != nullptr);
    if (agent != nullptr)
    {
        // LOUD: the recovery happened before any client attached, so it is replayed out of the
        // R-CLI-015 ring — a client that asks from seq 0 learns about it. (`subscribe` above already
        // registered the topic; this second one replays the retained history.)
        Json params = Json::object();
        Json topics = Json::array();
        topics.push_back(Json(std::string("diagnostics")));
        params.set("topics", std::move(topics));
        params.set("sinceSeq", Json(static_cast<std::uint64_t>(0)));
        const std::optional<Json> replay = agent->call("subscribe", std::move(params), error);
        CHECK(replay.has_value());
        bool saw_recovery = false;
        if (replay.has_value() && replay->at("data").contains("catchup"))
        {
            const Json& events = replay->at("data").at("catchup");
            for (std::size_t i = 0; i < events.size(); ++i)
            {
                const Json& payload = events.at(i).at("payload");
                if (payload.contains("code") &&
                    payload.at("code").as_string() == ek::kEditorSessionStateInvalidCode)
                    saw_recovery = true;
            }
        }
        CHECK(saw_recovery);

        // Defaults were loaded: the session state is empty, not half-applied.
        const std::optional<Json> read =
            agent->call("editor.selection-get", Json::object(), error);
        CHECK(read.has_value());
        if (read.has_value())
            CHECK(read->at("data").at("ids").size() == 0);
    }

    CHECK(run_cli({"attach", "--project", project.string(), "--editor-session", "--shutdown"}) == 0);
    reap(daemon);
    remove_tree(project);
}
} // namespace

int main()
{
    test_propagation_and_echo_suppression_and_play();
    test_two_windows_get_distinct_wire_origins();
    test_session_state_survives_a_daemon_restart();
    test_corrupt_session_file_recovers_loudly_without_blocking_the_boot();
    ITEST_MAIN_END();
}
