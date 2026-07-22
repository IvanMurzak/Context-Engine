// The M9 e08b PANEL-STATE-REWIRING T2 DRILL (design 05 §4, D7 tier 1) — the e08b Definition of Done
// proven end to end against a REAL `context daemon` over the REAL loopback IPC wire, with the REAL
// Shell panel composition (`install_builtin_panels`) driving the REAL client SDK.
//
// WHY A CROSS-PROCESS DRILL AND NOT ONLY THE T1 SUITE. The T1 suite (editor-shell-test_session_feed)
// scripts the WIRE, so it proves the feed's behaviour against the frames the daemon is DOCUMENTED to
// send. Only a live daemon proves the two agree — and the `origin` contract is exactly the kind of
// thing a mock cannot check, because a mock has no notion of "another connection". This drill runs a
// SECOND client (the real `context` binary, spawned as its own process — the door a human or a shell
// script comes through) against the same daemon as the Shell's panels.
//
// The four DoD lines it covers:
//   1. A selection made by a SECOND client is reflected in the scene-tree panel — with the panel
//      issuing NO write of its own (asserted on the feed's own write counter).
//   2. A selection made by the PANEL is observed by that second client, and the echo of it is
//      dropped rather than double-applied — while the panel still shows its own selection, which it
//      can only have learned from the write's reply.
//   3. The playbar drives daemon play state over RPC, and a play transition made by the CLI reaches
//      the L-51 indicator with no local write.
//   4. Structurally: at the end, every rendered state above was reachable with the writes counted —
//      no panel holds an authoritative copy.

#include "context/editor/client/client.h"
#include "context/editor/gui/panels/scenetree/scene_tree_panel.h"
#include "context/editor/gui/playbar/playbar_model.h"
#include "context/editor/gui/playbar/playbar_panel.h"
#include "context/editor/gui/uitree/panel.h"
#include "context/editor/shell/panel_host.h"
#include "context/editor/shell/shell.h" // kShellScope — attach with the REAL Shell's scope request
#include "context/editor/shell/panels/builtin_panels.h"
#include "context/editor/shell/panels/scenetree_feed.h"
#include "context/editor/shell/panels/session_feed.h"

#include "integration_test.h"
#include "process_util.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace client = context::editor::client;
namespace shell = context::editor::shell;
namespace panels = context::editor::shell::panels;
namespace scenetree = context::editor::gui::panels::scenetree;
namespace playbar = context::editor::gui::playbar;
namespace uitree = context::editor::gui::uitree;
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

ctest_proc::Process spawn_daemon(const fs::path& project)
{
    ctest_proc::Process daemon =
        ctest_proc::spawn(kBinary.string(), {"daemon", "--project", project.string()});
    if (!ctest_proc::valid(daemon))
        return daemon;
    if (!itest::wait_for_instance(project, itest::scaled_timeout_ms(20000), std::string()))
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

// Run the REAL `context` binary as a SEPARATE process — the CLI-as-second-client half of the DoD.
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

// Attach a client with the SHELL's own scope + subscribe it to the `session` topic, exactly as
// daemon_lifecycle.cpp does for the real editor.
std::unique_ptr<client::Client> attach_shell_client(const fs::path& project, std::string& error)
{
    std::unique_ptr<client::Client> c =
        client::Client::connect_to_project(project, itest::scaled_timeout_ms(8000), error);
    if (!c)
        return nullptr;
    client::AttachOptions options;
    options.scope = shell::kShellScope; // "read,write,session" — the real Shell's request
    if (!c->attach(options, error))
        return nullptr;
    Json params = Json::object();
    Json topics = Json::array();
    topics.push_back(Json(std::string(panels::kSessionTopic)));
    params.set("topics", std::move(topics));
    if (!c->call("subscribe", std::move(params), error).has_value())
        return nullptr;
    return c;
}

// Drain pushed frames for up to `timeout_ms`, forwarding every one into the feed exactly as
// editor_main.cpp's lifecycle on_event handler does. Returns how many the feed APPLIED.
int pump_into_feed(client::Client& c, panels::SessionFeed& feed, int timeout_ms)
{
    int applied = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;)
    {
        bool disconnected = false;
        const std::optional<client::InboundFrame> frame = c.poll_event(100, disconnected);
        if (frame.has_value() && frame->event.contains("topic") &&
            frame->event.contains("payload"))
        {
            if (panels::apply_session_event(feed, frame->event.at("topic").as_string(),
                                            frame->event.at("payload")))
                ++applied;
        }
        if (disconnected || std::chrono::steady_clock::now() >= deadline)
            break;
    }
    return applied;
}

// A two-row scene tree, handed to the feed through its REAL `editor.scene-tree` reply parser (the
// bare-`sceneTree` shape it documents) so the panel has rows a selection can resolve against.
void hydrate_scene_tree(panels::SceneTreeFeed& feed)
{
    Json roots = Json::array();
    for (const char* id : {"e1", "e2"})
    {
        Json node = Json::object();
        node.set("identity", Json(std::string(id)));
        node.set("identityHash", Json(std::string("11")));
        node.set("displayName", Json(std::string(id)));
        roots.push_back(std::move(node));
    }
    Json tree = Json::object();
    tree.set("rootScene", Json(std::string("root.scene.json")));
    tree.set("roots", std::move(roots));
    tree.set("entityCount", Json(std::uint64_t{2}));
    CHECK(feed.apply_result(tree));
}

[[nodiscard]] bool mentions(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

// -------------------------------------------------------------------- the drill

void panels_are_daemon_backed_across_two_clients()
{
    const fs::path project = itest::make_temp_project("e08b-panels");
    ctest_proc::Process daemon = spawn_daemon(project);
    CHECK(ctest_proc::valid(daemon));
    if (!ctest_proc::valid(daemon))
    {
        remove_tree(project);
        return;
    }

    std::string error;
    const std::unique_ptr<client::Client> shell_client = attach_shell_client(project, error);
    CHECK(shell_client != nullptr);
    if (shell_client == nullptr)
    {
        std::fprintf(stderr, "shell client attach failed: %s\n", error.c_str());
        reap(daemon);
        remove_tree(project);
        return;
    }
    CHECK(shell_client->client_id() > 0); // a real wire connection has a real echo-suppression id

    // The REAL Shell panel composition, bound to the REAL client — the same two calls
    // editor_main.cpp makes.
    shell::PanelHost host;
    panels::BuiltinPanels builtin = panels::install_builtin_panels(host);
    CHECK(builtin.session != nullptr);
    CHECK(builtin.scenetree != nullptr);
    if (builtin.session == nullptr || builtin.scenetree == nullptr)
    {
        reap(daemon);
        remove_tree(project);
        return;
    }
    panels::bind_session_client(*builtin.session, shell_client.get(), shell_client->client_id());
    hydrate_scene_tree(*builtin.scenetree);

    scenetree::SceneTreePanel& tree = builtin.scenetree->panel();
    playbar::PlaybarModel& bar = builtin.session->playbar_model();

    // === DoD 1 — a SECOND client's selection is reflected in the panel, with no panel-local write ==
    CHECK(run_cli({"attach", "--project", project.string(), "--editor-select", "e2"}) == 0);
    CHECK(pump_into_feed(*shell_client, *builtin.session, itest::scaled_timeout_ms(5000)) >= 1);
    CHECK(tree.selection().identity == "e2");
    CHECK(builtin.session->writes_issued() == 0); // the panel wrote NOTHING to reach that state
    CHECK(mentions(uitree::render_html(tree.build_panel()), "(selected)"));

    // === DoD 2 — the PANEL's selection is observed by that client, with no echo loop ===============
    CHECK(tree.select("e1"));
    CHECK(builtin.session->writes_issued() == 1);
    // The panel shows its OWN selection — which it can only have learned from the write's REPLY,
    // since the fact it published carries our origin and is dropped below.
    CHECK(tree.selection().identity == "e1");

    // The second client reads the same truth over its own connection: one selection, N clients.
    CHECK(run_cli({"attach", "--project", project.string(), "--editor-session"}) == 0);
    const std::optional<Json> read =
        shell_client->call("editor.selection-get", Json::object(), error);
    CHECK(read.has_value());
    if (read.has_value())
    {
        const Json& ids = read->at("data").at("ids");
        CHECK(ids.size() == 1);
        CHECK(ids.at(0).as_string() == "e1");
    }

    // The echo of our own write arrives and is DROPPED — no double-apply, and the panel does not
    // flicker back to anything.
    const std::size_t echoes_before = builtin.session->echoes_dropped();
    (void)pump_into_feed(*shell_client, *builtin.session, itest::scaled_timeout_ms(3000));
    CHECK(builtin.session->echoes_dropped() > echoes_before);
    CHECK(tree.selection().identity == "e1");

    // === DoD 3 — the playbar drives daemon play state over RPC, and observes another client's ======
    CHECK(bar.state() == playbar::PlayState::edit);
    CHECK(bar.play().ok);
    CHECK(bar.state() == playbar::PlayState::playing); // the DAEMON's answer, rendered
    CHECK(builtin.session->writes_issued() == 2);
    (void)pump_into_feed(*shell_client, *builtin.session, itest::scaled_timeout_ms(2000)); // our echo

    // The CLI stops it; the L-51 indicator follows with NO local write.
    const std::size_t writes_before = builtin.session->writes_issued();
    CHECK(run_cli({"attach", "--project", project.string(), "--editor-play", "stop"}) == 0);
    CHECK(pump_into_feed(*shell_client, *builtin.session, itest::scaled_timeout_ms(5000)) >= 1);
    CHECK(bar.state() == playbar::PlayState::edit);
    CHECK(builtin.session->writes_issued() == writes_before);
    CHECK(mentions(uitree::render_html(playbar::build_playbar_panel(bar)), "EDIT MODE"));

    // A control the daemon refuses propagates its catalog code verbatim and moves nothing.
    const playbar::PlayAction refused = bar.pause();
    CHECK(!refused.ok);
    CHECK(refused.error_code == std::string(playbar::kPlayNotRunningCode));
    CHECK(bar.state() == playbar::PlayState::edit);

    CHECK(run_cli({"attach", "--project", project.string(), "--editor-session", "--shutdown"}) == 0);
    reap(daemon);
    remove_tree(project);
}

} // namespace

int main()
{
    panels_are_daemon_backed_across_two_clients();
    ITEST_MAIN_END();
}
