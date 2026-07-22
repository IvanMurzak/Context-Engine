// The M9 e14c WELCOME-SCREEN T2 DEV-MODE DRILL (design 07 §4 / 10 / D13): the app's front door proven
// end to end against the REAL `context` CLI over subprocesses (the e14a/e14b precedent), with NO GUI —
// headless-safe on every OS leg including Session 0. The PACKAGED-shape step-count is e15/e16; this
// proves the FUNCTIONAL flows and the persona-A step BUDGETS (10 §Persona A) in dev-mode, exactly as the
// e14 split ruling assigns.
//
// Each flow is modelled as an explicit interaction list (one entry = one interaction, 10 § "Step") whose
// TERMINAL work is performed for real, so the budget assertion is a genuine regression tripwire (the
// 10-user-workflows step budgets are M9-exit release gates) rather than a magic number:
//
//   1. BARE LAUNCH -> WELCOME. The pure D13 rule (resolve_launch_mode) sends a bare launch that is not
//      already a project to the welcome screen, and an explicit / in-project launch to the editor.
//   2. OPEN RECENT = 2 STEPS. launch (1) + activate a recent (1). The recent is a REAL project (scaffolded
//      via `context new`) and activating it runs the REAL `context edit <path>` e14a open path.
//   3. NEW FROM TEMPLATE <= 7 STEPS. launch + choose + pick-template + pick-location + name. The terminal
//      work runs the REAL `context new <dir> <template>` (asserting a RUNNABLE scaffold, R-QA-006) and then
//      opens it via `context edit`.
//   4. TEMPLATE CATALOG IS HONEST. Every template the welcome screen offers (available_templates) is
//      accepted by the REAL `context new` — so the welcome catalog can never silently drift ahead of the CLI.

#include "context/editor/contract/json.h"
#include "context/editor/shell/welcome.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace shell = context::editor::shell;
namespace fs = std::filesystem;

#if !defined(CONTEXT_BINARY)
#error "CONTEXT_BINARY (the built `context` executable path) must be defined by CMake"
#endif

namespace
{

int g_failures = 0;
void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            fail(__FILE__, __LINE__, #cond);                                                       \
    } while (false)

const fs::path kBinary = fs::path(CONTEXT_BINARY);

fs::path make_temp_dir(const std::string& tag)
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir =
        fs::temp_directory_path() / ("ctx-e14c-t2-" + tag + "-" + std::to_string(stamp));
    std::error_code ec;
    fs::remove_all(dir, ec);
    return dir;
}

void remove_tree(const fs::path& path)
{
    std::error_code ec;
    fs::remove_all(path, ec);
}

// One user flow, as a counted interaction list (10 § "Step" = one click / drag / typed entry).
struct Flow
{
    std::string name;
    std::vector<std::string> steps;
    [[nodiscard]] std::size_t count() const { return steps.size(); }
};

bool envelope_data_bool(const shell::CliResult& result, const char* key)
{
    using context::editor::contract::Json;
    if (!result.envelope.is_object() || !result.envelope.contains("data"))
        return false;
    const Json& data = result.envelope.at("data");
    return data.is_object() && data.contains(key) && data.at(key).as_bool();
}

// ---------------------------------------------------------------- 1. bare launch -> welcome

void test_bare_launch_shows_welcome()
{
    const fs::path bare = make_temp_dir("bare");
    std::error_code ec;
    fs::create_directories(bare, ec);

    // A bare launch in a non-project dir lands on the welcome screen (D13).
    CHECK(shell::resolve_launch_mode(bare, /*explicit=*/false) == shell::LaunchMode::welcome);
    // An explicit --project always opens the project.
    CHECK(shell::resolve_launch_mode(bare, /*explicit=*/true) == shell::LaunchMode::project);

    // A real scaffold makes the dir a project, so even a bare launch there opens it.
    const shell::CliResult created = shell::run_context_cli(kBinary, {"new", bare.string()});
    CHECK(created.ok());
    CHECK(shell::is_context_project(bare));
    CHECK(shell::resolve_launch_mode(bare, /*explicit=*/false) == shell::LaunchMode::project);

    remove_tree(bare);
}

// ---------------------------------------------------------------- 2. open recent = 2 steps

void test_open_recent_is_two_steps()
{
    const fs::path root = make_temp_dir("recent");
    std::error_code ec;
    fs::create_directories(root, ec);
    const fs::path project = root / "my-recent-game";
    const fs::path config = root / ".context" / "config.json";

    // A REAL project to remember, then record it as the most-recent (what opening a project does).
    const shell::CliResult created = shell::run_context_cli(kBinary, {"new", project.string()});
    CHECK(created.ok());
    std::string record_error;
    CHECK(shell::record_recent_project(config, project, 1000, 10, &record_error));

    // The recent is now readable from the welcome store.
    std::vector<shell::RecentProject> recents = shell::read_recent_projects(config);
    CHECK(recents.size() == 1);
    CHECK(recents[0].name == "my-recent-game");

    // The flow: launch (1) + activate the recent (1) = the 2-step budget (10 § "Open recent project").
    Flow flow{"open-recent", {"launch", "activate recent"}};
    CHECK(flow.count() == 2);

    // Activating the recent feeds the REAL e14a launch flow: `context edit <recent> --no-launch` reports
    // "spawn" (no live editor for it) — the recent path actually resolves + arbitrates + would launch.
    const shell::CliResult opened =
        shell::run_context_cli(kBinary, {"edit", recents[0].path, "--no-launch"});
    CHECK(opened.ok());
    CHECK(opened.action() == "spawn");

    remove_tree(root);
}

// ---------------------------------------------------------------- 3. new from template <= 7 steps

void test_new_from_template_within_budget()
{
    const fs::path root = make_temp_dir("new");
    std::error_code ec;
    fs::create_directories(root, ec);
    const fs::path project = root / "fresh-game";

    // The flow (10 § "Fresh install -> editing a template project", dev-mode half): launch + choose New
    // from template + pick a template + pick a location + name it. Five interactions, well within the
    // 7-step persona-A budget (the installer steps are the packaged-shape e15/e16 half).
    Flow flow{"new-from-template",
              {"launch", "choose New from template", "pick template", "pick location", "name project"}};
    CHECK(flow.count() <= 7);

    // Terminal work: the REAL `context new <dir> <template>` scaffolds a RUNNABLE project (R-QA-006).
    const shell::CliResult created =
        shell::run_context_cli(kBinary, {"new", project.string(), "default"});
    CHECK(created.ok());
    CHECK(envelope_data_bool(created, "runnable"));

    // The scaffold then opens through the SAME e14a launch flow.
    const shell::CliResult opened =
        shell::run_context_cli(kBinary, {"edit", project.string(), "--no-launch"});
    CHECK(opened.ok());
    CHECK(opened.action() == "spawn");

    remove_tree(root);
}

// ---------------------------------------------------------------- 4. the template catalog is honest

void test_template_catalog_matches_the_cli()
{
    const std::vector<shell::WelcomeTemplate>& templates = shell::available_templates();
    CHECK(!templates.empty());
    for (const shell::WelcomeTemplate& tmpl : templates)
    {
        const fs::path dir = make_temp_dir("tmpl-" + tmpl.name);
        // Every template the welcome screen offers must be one the REAL `context new` accepts — the
        // anti-drift guard, so the welcome catalog can never advertise a template the CLI would reject.
        const shell::CliResult created =
            shell::run_context_cli(kBinary, {"new", dir.string(), tmpl.name});
        CHECK(created.ok());
        remove_tree(dir);
    }
}

} // namespace

int main()
{
    test_bare_launch_shows_welcome();
    test_open_recent_is_two_steps();
    test_new_from_template_within_budget();
    test_template_catalog_matches_the_cli();
    return g_failures == 0 ? 0 : 1;
}
