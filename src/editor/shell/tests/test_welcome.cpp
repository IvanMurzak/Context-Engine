// Unit tests for the M9 e14c welcome surface (welcome.h / welcome.cpp): the pure launch-mode rule, the
// `~/.context/config.json` recents read/write round-trip, and the `welcome.*` bridge — its state
// projection, the injectable folder-picker seam, and the `context new` / `context edit` wrapper driven
// through an injected CLI runner (so the welcome logic is exercised with NO live CLI). The one part not
// covered here is the literal OS folder dialog, which cannot run headless — the same carve-out the real
// window backend and the live CEF path take; it is reached through the injectable seam these tests fake.

#include "context/editor/contract/json.h"
#include "context/editor/shell/ipc_bridge.h"
#include "context/editor/shell/welcome.h"
#include "shell_test.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace shell = context::editor::shell;
namespace contract = context::editor::contract;
namespace fs = std::filesystem;

namespace
{

// Build a fake `context` envelope: { ok, data:{ action, runnable } }.
contract::Json fake_envelope(bool ok, const std::string& action, bool runnable)
{
    contract::Json data = contract::Json::object();
    if (!action.empty())
        data.set("action", contract::Json(action));
    data.set("runnable", contract::Json(runnable));
    contract::Json env = contract::Json::object();
    env.set("ok", contract::Json(ok));
    env.set("data", std::move(data));
    return env;
}

shell::CliResult fake_result(bool ok, const std::string& action, bool runnable)
{
    shell::CliResult result;
    result.ran = true;
    result.exit_code = ok ? 0 : 1;
    result.envelope = fake_envelope(ok, action, runnable);
    return result;
}

// ---------------------------------------------------------------- 1. the pure launch-mode rule

void test_launch_mode_rule()
{
    CHECK(std::string(shell::launch_mode_token(shell::LaunchMode::welcome)) == "welcome");
    CHECK(std::string(shell::launch_mode_token(shell::LaunchMode::project)) == "project");

    const fs::path bare = shelltest::make_temp_project("welcome", "bare");
    // A bare launch in a non-project dir -> welcome; explicit --project -> project regardless.
    CHECK(shell::resolve_launch_mode(bare, /*explicit=*/false) == shell::LaunchMode::welcome);
    CHECK(shell::resolve_launch_mode(bare, /*explicit=*/true) == shell::LaunchMode::project);
    CHECK(!shell::is_context_project(bare));

    // Drop a project.json -> the dir is a project, so even a bare launch opens it.
    {
        std::ofstream out(bare / "project.json", std::ios::binary);
        out << "{}";
    }
    CHECK(shell::is_context_project(bare));
    CHECK(shell::resolve_launch_mode(bare, /*explicit=*/false) == shell::LaunchMode::project);

    shelltest::cleanup(bare);
}

// ---------------------------------------------------------------- 2. recents read/write round-trip

void test_recents_round_trip()
{
    const fs::path root = shelltest::make_temp_project("welcome", "recents");
    const fs::path config = root / ".context" / "config.json";

    // A missing config yields an empty list (a first-ever launch), never an error.
    CHECK(shell::read_recent_projects(config).empty());

    const fs::path project_a = root / "alpha";
    const fs::path project_b = root / "beta";
    fs::create_directories(project_a);
    fs::create_directories(project_b);

    std::string error;
    CHECK(shell::record_recent_project(config, project_a, 1000, 10, &error));
    CHECK(error.empty());
    CHECK(shell::record_recent_project(config, project_b, 2000, 10, &error));

    std::vector<shell::RecentProject> recents = shell::read_recent_projects(config);
    CHECK(recents.size() == 2);
    // Most-recent-first: beta (recorded last) leads.
    CHECK(recents[0].name == "beta");
    CHECK(recents[0].last_opened_ms == 2000);
    CHECK(recents[1].name == "alpha");

    // Re-recording alpha moves it to the front WITHOUT duplicating.
    CHECK(shell::record_recent_project(config, project_a, 3000, 10, &error));
    recents = shell::read_recent_projects(config);
    CHECK(recents.size() == 2);
    CHECK(recents[0].name == "alpha");
    CHECK(recents[0].last_opened_ms == 3000);

    // The cap is honored.
    for (int i = 0; i < 8; ++i)
    {
        const fs::path extra = root / ("proj-" + std::to_string(i));
        fs::create_directories(extra);
        CHECK(shell::record_recent_project(config, extra, 4000 + i, 3, &error));
    }
    recents = shell::read_recent_projects(config);
    CHECK(recents.size() == 3);

    // An empty config path (no HOME/USERPROFILE resolved) is an honest IO refusal, never a crash:
    // returns false with a diagnostic so recording a recent can never block opening a project.
    error.clear();
    CHECK(!shell::record_recent_project(fs::path(), project_a, 5000, 10, &error));
    CHECK(!error.empty());

    shelltest::cleanup(root);
}

// A corrupt config is treated as "no recents" (loudly recoverable on the next write), not a crash.
void test_recents_tolerates_corruption()
{
    const fs::path root = shelltest::make_temp_project("welcome", "corrupt");
    const fs::path config = root / "config.json";
    {
        std::ofstream out(config, std::ios::binary);
        out << "{ this is not json";
    }
    CHECK(shell::read_recent_projects(config).empty());
    // A subsequent record repairs it.
    std::string error;
    CHECK(shell::record_recent_project(config, root / "p", 1, 10, &error));
    CHECK(shell::read_recent_projects(config).size() == 1);
    shelltest::cleanup(root);
}

// ---------------------------------------------------------------- 3. state() projection

void test_state_projection()
{
    const fs::path root = shelltest::make_temp_project("welcome", "state");
    const fs::path config = root / "config.json";
    std::string error;
    CHECK(shell::record_recent_project(config, root / "myproj", 42, 10, &error));

    shell::WelcomeBridge welcome;
    welcome.set_launch_mode(shell::LaunchMode::welcome);
    welcome.set_project_name("");
    welcome.set_config_path(config);

    const contract::Json state = welcome.state();
    CHECK(state.at("mode").as_string() == "welcome");
    CHECK(state.at("recents").is_array());
    CHECK(state.at("recents").size() == 1);
    CHECK(state.at("recents").at(0).at("name").as_string() == "myproj");
    CHECK(state.at("templates").is_array());
    CHECK(state.at("templates").size() >= 1);
    CHECK(state.at("templates").at(0).at("name").as_string() == "default");
    CHECK(welcome.states_served() == 1);

    // Project mode reports the project name and mode.
    shell::WelcomeBridge project_welcome;
    project_welcome.set_launch_mode(shell::LaunchMode::project);
    project_welcome.set_project_name("my-game");
    project_welcome.set_config_path(config);
    const contract::Json pstate = project_welcome.state();
    CHECK(pstate.at("mode").as_string() == "project");
    CHECK(pstate.at("projectName").as_string() == "my-game");

    shelltest::cleanup(root);
}

// ---------------------------------------------------------------- 4. the injectable folder picker

void test_pick_folder_seam()
{
    shell::WelcomeBridge welcome;

    // A picker that returns a path -> picked:true + the path.
    welcome.bind_folder_picker([]() -> std::optional<fs::path>
                               { return fs::path("C:/games/demo"); });
    contract::Json picked = welcome.pick_folder();
    CHECK(picked.at("picked").as_bool());
    CHECK(picked.at("path").as_string() == "C:/games/demo");

    // A picker that cancels -> picked:false, an ORDINARY result (not an error).
    welcome.bind_folder_picker([]() -> std::optional<fs::path> { return std::nullopt; });
    contract::Json cancelled = welcome.pick_folder();
    CHECK(!cancelled.at("picked").as_bool());
    CHECK(cancelled.at("path").as_string().empty());
    CHECK(welcome.folders_picked() == 2);
}

// ---------------------------------------------------------------- 5. open / new via injected CLI

void test_open_and_new_via_cli()
{
    // open_project shells out to `context edit <path>` and reports the envelope's action.
    {
        shell::WelcomeBridge welcome;
        std::vector<std::string> seen;
        welcome.bind_cli_runner(
            [&seen](const std::vector<std::string>& args) -> shell::CliResult
            {
                seen = args;
                return fake_result(true, "spawn", false);
            });
        contract::Json out;
        std::string error_code;
        CHECK(welcome.open_project("C:/games/demo", out, error_code));
        CHECK(out.at("opened").as_bool());
        CHECK(out.at("action").as_string() == "spawn");
        CHECK(seen.size() == 2);
        CHECK(seen[0] == "edit");
        CHECK(seen[1] == "C:/games/demo");
    }

    // An empty path is refused with a local bad-params code (not routed to the CLI).
    {
        shell::WelcomeBridge welcome;
        bool ran = false;
        welcome.bind_cli_runner(
            [&ran](const std::vector<std::string>&) -> shell::CliResult
            {
                ran = true;
                return shell::CliResult{};
            });
        contract::Json out;
        std::string error_code;
        CHECK(!welcome.open_project("", out, error_code));
        CHECK(error_code == shell::kErrWelcomeBadParams);
        CHECK(!ran);
    }

    // new_project scaffolds via `context new <dir> <template>`, then opens.
    {
        shell::WelcomeBridge welcome;
        std::vector<std::vector<std::string>> calls;
        welcome.bind_cli_runner(
            [&calls](const std::vector<std::string>& args) -> shell::CliResult
            {
                calls.push_back(args);
                if (!args.empty() && args[0] == "new")
                    return fake_result(true, "", true);
                return fake_result(true, "spawn", false);
            });
        contract::Json out;
        std::string error_code;
        CHECK(welcome.new_project("C:/games/fresh", "default", out, error_code));
        CHECK(out.at("created").as_bool());
        CHECK(out.at("runnable").as_bool());
        CHECK(out.at("opened").as_bool());
        CHECK(calls.size() == 2);
        CHECK(calls[0][0] == "new");
        CHECK(calls[0][1] == "C:/games/fresh");
        CHECK(calls[0][2] == "default");
        CHECK(calls[1][0] == "edit");
    }

    // An unknown template is refused locally, before any CLI call.
    {
        shell::WelcomeBridge welcome;
        bool ran = false;
        welcome.bind_cli_runner(
            [&ran](const std::vector<std::string>&) -> shell::CliResult
            {
                ran = true;
                return shell::CliResult{};
            });
        contract::Json out;
        std::string error_code;
        CHECK(!welcome.new_project("C:/games/fresh", "3d-fps-not-real", out, error_code));
        CHECK(error_code == shell::kErrWelcomeUnknownTemplate);
        CHECK(!ran);
    }

    // A CLI FAILURE (`context edit` exits non-zero) is an ORDINARY result, not a hard refusal:
    // open_project still returns true (it produced a valid response) with opened:false in the envelope,
    // and error_code stays empty (the local bad-params refusal is a DIFFERENT path).
    {
        shell::WelcomeBridge welcome;
        std::vector<std::string> seen;
        welcome.bind_cli_runner(
            [&seen](const std::vector<std::string>& args) -> shell::CliResult
            {
                seen = args;
                return fake_result(false, "", false);
            });
        contract::Json out;
        std::string error_code;
        CHECK(welcome.open_project("C:/games/demo", out, error_code));
        CHECK(!out.at("opened").as_bool());
        CHECK(error_code.empty());
        CHECK(seen.size() == 2);
        CHECK(seen[0] == "edit");
    }

    // A scaffold FAILURE (`context new` exits non-zero) reports created:false and does NOT attempt to
    // open the never-created directory — the `context edit` follow-up is gated on success.
    {
        shell::WelcomeBridge welcome;
        std::vector<std::vector<std::string>> calls;
        welcome.bind_cli_runner(
            [&calls](const std::vector<std::string>& args) -> shell::CliResult
            {
                calls.push_back(args);
                return fake_result(false, "", false);
            });
        contract::Json out;
        std::string error_code;
        CHECK(welcome.new_project("C:/games/fresh", "default", out, error_code));
        CHECK(!out.at("created").as_bool());
        CHECK(!out.at("opened").as_bool());
        CHECK(calls.size() == 1);
        CHECK(calls[0][0] == "new");
    }
}

// ---------------------------------------------------------------- 6. install + live dispatch

// Dispatch one JSON-RPC request string through a router and return the parsed response.
contract::Json dispatch(shell::BridgeRouter& router, const std::string& method,
                        const contract::Json& params)
{
    contract::Json request = contract::Json::object();
    request.set("jsonrpc", contract::Json(std::string("2.0")));
    request.set("id", contract::Json(static_cast<std::int64_t>(1)));
    request.set("method", contract::Json(method));
    request.set("params", params);
    const shell::BridgeDispatch result = router.dispatch(request.dump());
    return contract::Json::parse(result.response);
}

void test_install_and_dispatch()
{
    const fs::path root = shelltest::make_temp_project("welcome", "dispatch");
    const fs::path config = root / "config.json";

    shell::WelcomeBridge welcome;
    welcome.set_launch_mode(shell::LaunchMode::welcome);
    welcome.set_config_path(config);
    welcome.bind_folder_picker([]() -> std::optional<fs::path>
                               { return fs::path("D:/picked"); });

    shell::BridgeRouter router;
    CHECK(welcome.install(router));
    CHECK(router.has_method(shell::kWelcomeStateMethod));
    CHECK(router.has_method(shell::kWelcomePickFolderMethod));
    CHECK(router.has_method(shell::kWelcomeOpenMethod));
    CHECK(router.has_method(shell::kWelcomeNewProjectMethod));

    // welcome.state round-trips a valid result.
    {
        const contract::Json response = dispatch(router, shell::kWelcomeStateMethod,
                                                 contract::Json::object());
        CHECK(response.contains("result"));
        CHECK(response.at("result").at("mode").as_string() == "welcome");
    }
    // welcome.pickFolder reaches the injected picker.
    {
        const contract::Json response = dispatch(router, shell::kWelcomePickFolderMethod,
                                                 contract::Json::object());
        CHECK(response.at("result").at("picked").as_bool());
        CHECK(response.at("result").at("path").as_string() == "D:/picked");
    }
    // welcome.open with no path is refused with the local bad-params code.
    {
        const contract::Json response = dispatch(router, shell::kWelcomeOpenMethod,
                                                 contract::Json::object());
        CHECK(response.contains("error"));
        CHECK(response.at("error").at("data").at("reason").as_string() == shell::kErrWelcomeBadParams);
    }

    shelltest::cleanup(root);
}

} // namespace

int main()
{
    test_launch_mode_rule();
    test_recents_round_trip();
    test_recents_tolerates_corruption();
    test_state_projection();
    test_pick_folder_seam();
    test_open_and_new_via_cli();
    test_install_and_dispatch();
    SHELL_TEST_MAIN_END();
}
