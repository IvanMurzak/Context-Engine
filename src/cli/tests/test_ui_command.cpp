// `context ui <verb>` CLI backend tests (M7 T5 / a5, ui_command.cpp): the headless runtime-UI
// drive/assert verbs over a UI-scene file. Writes a small ctx:ui-hud scene to a temp file and drives
// dump / query / send / assert through cli::run, asserting the R-CLI-008 envelope + the ui.* failure
// classes (happy path, edge, and failure — R-QA-013). Pure C++, no GPU/daemon.

#include "context/cli/app.h"
#include "context/editor/contract/envelope.h"
#include "context/editor/contract/json.h"

#include "cli_test.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <vector>

using context::cli::run;
using context::editor::contract::Envelope;
using context::editor::contract::Json;
namespace fs = std::filesystem;

namespace
{
// A representative ctx:ui-hud scene: a column HUD panel with a data-bound score label + health bar and
// a play button whose pointerdown scores points through the UI->state action path.
const char* kScene = R"({
  "$schema": "ctx:ui-hud",
  "version": 1,
  "state": [
    {"id": "5c07e100a1b2c3d4", "name": "score", "key": 100, "initial": 0},
    {"id": "5c07e200b2c3d4e5", "name": "health", "key": 200, "initial": 100}
  ],
  "root": {
    "id": "a1b2c3d4e5f60789",
    "role": "panel",
    "name": "hud-root",
    "layout": {"position": "flow", "flow": "column", "size": [220, 96], "gap": 6},
    "style": {"background": [0, 0, 0, 160], "padding": 8},
    "children": [
      {"role": "label", "name": "score-label", "style": {"foreground": [255, 214, 0, 255]}, "bind": {"value": "score"}},
      {"role": "progressbar", "name": "health-bar", "bind": {"value": "health"}},
      {"role": "button", "name": "play-button", "on": {"pointerdown": [
        {"action": "add-state", "state": "score", "delta": 10},
        {"action": "add-state", "state": "health", "delta": -5}
      ]}}
    ]
  }
})";

fs::path make_temp(const char* tag)
{
    const auto stamp = static_cast<long long>(
        std::chrono::steady_clock::now().time_since_epoch().count() & 0xffffffffLL);
    return fs::temp_directory_path() / ("ctx-ui-" + std::string(tag) + "-" + std::to_string(stamp));
}

void write_file(const fs::path& path, const std::string& content)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

// Find a node object by its `name` in a dump's `nodes` array; returns a null Json when absent.
const Json& find_node(const Json& nodes, const std::string& name)
{
    static const Json kNull;
    for (std::size_t i = 0; i < nodes.size(); ++i)
        if (nodes.at(i).at("name").as_string() == name)
            return nodes.at(i);
    return kNull;
}
} // namespace

int main()
{
    const fs::path scene = make_temp("scene") ;
    write_file(scene, kScene);
    const std::string p = scene.string();

    // --- dump: the whole tree + computed rects + resolved bound values ----------------------------
    {
        const Envelope env = run({"ui", "dump", p});
        CHECK(env.ok());
        // root + panel + 3 children = 5 nodes.
        CHECK(env.data().at("nodeCount").as_number() == 5.0);
        const Json& nodes = env.data().at("nodes");
        const Json& score = find_node(nodes, "score-label");
        CHECK(score.at("role").as_string() == "label");
        CHECK(score.at("boundState").as_string() == "score");
        CHECK(score.at("boundValue").as_number() == 0.0);
        const Json& health = find_node(nodes, "health-bar");
        CHECK(health.at("boundValue").as_number() == 100.0);
        // The layout pass produced a non-empty rect for the panel (size 220x96).
        const Json& panel = find_node(nodes, "hud-root");
        CHECK(panel.at("rect").at("w").as_number() == 220.0);
        CHECK(panel.at("rect").at("h").as_number() == 96.0);
        // The state store is echoed by name.
        CHECK(env.data().at("state").at("score").as_number() == 0.0);
        CHECK(env.data().at("state").at("health").as_number() == 100.0);
    }

    // --- query: one node by author name; unknown name -> ui.node_not_found ------------------------
    {
        const Envelope ok = run({"ui", "query", p, "health-bar"});
        CHECK(ok.ok());
        CHECK(ok.data().at("role").as_string() == "progressbar");
        CHECK(ok.data().at("boundValue").as_number() == 100.0);

        const Envelope missing = run({"ui", "query", p, "no-such-node"});
        CHECK(!missing.ok());
        CHECK(missing.error()->code == "ui.node_not_found");
    }

    // --- send click: the UI->state action path runs (score +10, health -5) ------------------------
    {
        const Envelope env = run({"ui", "send", p, "click", "--target", "play-button"});
        CHECK(env.ok());
        CHECK(env.data().at("state").at("score").as_number() == 10.0);
        CHECK(env.data().at("state").at("health").as_number() == 95.0);
    }

    // --- send focus: moves focus to the target ----------------------------------------------------
    {
        const Envelope env = run({"ui", "send", p, "focus", "--target", "play-button"});
        CHECK(env.ok());
        CHECK(env.data().at("focused").as_string() == "play-button");
    }

    // --- send text: sets the target node's text ---------------------------------------------------
    {
        const Envelope env = run({"ui", "send", p, "text", "--target", "score-label", "--text", "hi"});
        CHECK(env.ok());
        CHECK(env.data().at("text").as_string() == "hi");
    }

    // --- send failure paths: missing --target / missing --code -> ui.invalid_event ----------------
    {
        const Envelope no_target = run({"ui", "send", p, "click"});
        CHECK(!no_target.ok());
        CHECK(no_target.error()->code == "ui.invalid_event");

        const Envelope no_code = run({"ui", "send", p, "key", "--target", "play-button"});
        CHECK(!no_code.ok());
        CHECK(no_code.error()->code == "ui.invalid_event");

        const Envelope bad_event = run({"ui", "send", p, "wiggle", "--target", "play-button"});
        CHECK(!bad_event.ok());
        CHECK(bad_event.error()->code == "ui.invalid_event");

        const Envelope bad_target = run({"ui", "send", p, "click", "--target", "ghost"});
        CHECK(!bad_target.ok());
        CHECK(bad_target.error()->code == "ui.node_not_found");
    }

    // --- assert: pass paths ------------------------------------------------------------------------
    {
        CHECK(run({"ui", "assert", p, "score-label", "--role", "label"}).ok());
        CHECK(run({"ui", "assert", p, "health-bar", "--value", "100"}).ok());
        CHECK(run({"ui", "assert", p, "play-button", "--role", "button"}).ok());
        CHECK(run({"ui", "assert", p, "hud-root", "--child-count", "3"}).ok());
        CHECK(run({"ui", "assert", p, "score-label", "--visible"}).ok());
    }

    // --- assert: fail paths -> ui.assertion_failed / ui.node_not_found -----------------------------
    {
        const Envelope wrong_role = run({"ui", "assert", p, "score-label", "--role", "button"});
        CHECK(!wrong_role.ok());
        CHECK(wrong_role.error()->code == "ui.assertion_failed");

        const Envelope wrong_value = run({"ui", "assert", p, "health-bar", "--value", "50"});
        CHECK(!wrong_value.ok());
        CHECK(wrong_value.error()->code == "ui.assertion_failed");

        const Envelope no_binding = run({"ui", "assert", p, "play-button", "--value", "5"});
        CHECK(!no_binding.ok());
        CHECK(no_binding.error()->code == "ui.assertion_failed");

        const Envelope wrong_children = run({"ui", "assert", p, "hud-root", "--child-count", "2"});
        CHECK(!wrong_children.ok());
        CHECK(wrong_children.error()->code == "ui.assertion_failed");

        const Envelope missing = run({"ui", "assert", p, "ghost", "--exists"});
        CHECK(!missing.ok());
        CHECK(missing.error()->code == "ui.node_not_found");
    }

    // --- scene load failures: not-found + malformed -----------------------------------------------
    {
        const Envelope not_found = run({"ui", "dump", (make_temp("nope")).string()});
        CHECK(!not_found.ok());
        CHECK(not_found.error()->code == "ui.scene_not_found");

        const fs::path bad = make_temp("bad");
        write_file(bad, "{ this is not json");
        const Envelope invalid = run({"ui", "dump", bad.string()});
        CHECK(!invalid.ok());
        CHECK(invalid.error()->code == "ui.scene_invalid");
        std::error_code ec;
        fs::remove(bad, ec);
    }

    std::error_code ec;
    fs::remove(scene, ec);
    CLI_TEST_MAIN_END();
}
