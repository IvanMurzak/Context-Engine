// The M9 e14b PER-PROJECT SINGLE-INSTANCE ARBITRATION T2 DEV-MODE DRILL (design 07 §4 / 05, D15/C-F23):
// the `context edit .` opener proven end to end as a REAL separate process against a live "editor" (the
// drill's own presence marker + a FocusRequestWatcher on a background thread — the Shell owner-loop
// behaviour without a GUI, so it is headless-safe on every OS leg incl. Session 0). The packaged-shape
// drill is e15/e16; this proves the FUNCTIONAL flows in dev-mode, exactly as the split ruling assigns.
//
// Three scenarios, one per DoD line:
//   1. FOCUS — a live editor is present (marker + a watcher that consumes the focus request); a second
//      `context edit <dir>` FOCUSES it (reports "focused", the watcher raised its window) instead of
//      duplicating — the C-F23 single-instance behaviour.
//   2. SPAWN (different project) — no marker; `context edit <fresh-dir>` reports "spawn" (a new process
//      would be launched). Run with --no-launch so CI never hosts a lingering GUI.
//   3. STALE MARKER — a marker with NO live editor behind it; the focus handshake times out and the
//      opener reports "spawn" (crash-robust liveness: a dead editor never acknowledges).
//
// C-F3 is respected throughout: the opener only READS editor-state.json; the focus handshake rides the
// SEPARATE `.editor/focus-request` file. The drill writes the presence marker the way the Shell does
// (its single-writer store), never a second writer racing editor-state.json.

#include "context/common/child_process.h"
#include "context/editor/client/arbitration.h"
#include "context/editor/contract/json.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>

namespace client = context::editor::client;
namespace contract = context::editor::contract;
namespace process = context::common::process;
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

fs::path make_temp_project(const char* tag)
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir = fs::temp_directory_path() / ("ctx-e14b-t2-" + std::string(tag) + "-" +
                                                std::to_string(stamp));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / ".editor", ec);
    return dir;
}

void remove_tree(const fs::path& path)
{
    std::error_code ec;
    fs::remove_all(path, ec);
}

// Write a presence marker the way the Shell's single-writer store does — a minimal editor-state
// document carrying the `presence` object.
void publish_presence(const fs::path& project, const client::PresenceMarker& marker)
{
    contract::Json doc = contract::Json::object();
    doc.set("version", contract::Json(static_cast<std::int64_t>(1)));
    doc.set("presence", marker.to_json());
    std::error_code ec;
    fs::create_directories(project / ".editor", ec);
    std::ofstream out(project / ".editor" / "editor-state.json", std::ios::binary | std::ios::trunc);
    out << doc.dump(2);
}

// Run `context edit <args...>` as a REAL child process; return its stdout (the R-CLI-008 envelope) and
// exit code. Drains stdout line by line to EOF (the pretty-printed envelope is multi-line).
struct RunResult
{
    std::string stdout_text;
    int exit_code = -1;
    bool exited = false;
};

RunResult run_context_edit(const std::vector<std::string>& edit_args)
{
    process::SpawnOptions options;
    options.executable = kBinary;
    options.args.push_back("edit");
    for (const std::string& a : edit_args)
        options.args.push_back(a);
    options.capture_stdout = true;

    RunResult result;
    std::string error;
    process::ChildProcess child = process::ChildProcess::spawn(options, error);
    if (!child.valid())
    {
        std::fprintf(stderr, "spawn failed: %s\n", error.c_str());
        return result;
    }
    std::string line;
    while (child.read_line(line, 10000))
    {
        result.stdout_text += line;
        result.stdout_text += '\n';
    }
    int code = 0;
    result.exited = child.wait(5000, code);
    result.exit_code = code;
    return result;
}

// Extract data.action from the printed envelope.
std::string action_of(const std::string& envelope_text)
{
    try
    {
        const contract::Json doc = contract::Json::parse(envelope_text);
        return doc.at("data").at("action").as_string();
    }
    catch (const std::exception&)
    {
        return std::string();
    }
}

bool marker_present_of(const std::string& envelope_text)
{
    try
    {
        return contract::Json::parse(envelope_text).at("data").at("markerPresent").as_bool();
    }
    catch (const std::exception&)
    {
        return false;
    }
}

// ---------------------------------------------------------------- 1. focus a live editor

void test_second_open_focuses_the_live_editor()
{
    const fs::path project = make_temp_project("focus");
    client::PresenceMarker marker;
    marker.pid = client::current_process_id();
    marker.boot_nonce = client::make_boot_nonce();
    publish_presence(project, marker);

    // The "editor": a watcher polling for focus requests on a background thread (the Shell owner loop).
    client::FocusRequestWatcher watcher(project);
    std::atomic<bool> stop{false};
    std::atomic<int> raised{0};
    std::thread editor(
        [&]
        {
            while (!stop.load())
            {
                if (watcher.poll())
                    raised.fetch_add(1);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });

    // A REAL second opener process. --no-launch so a "spawn" verdict never hosts a GUI; a "focus"
    // verdict still focuses (the handshake is not a launch).
    const RunResult run =
        run_context_edit({project.string(), "--no-launch", "--focus-timeout-ms", "5000"});
    stop.store(true);
    editor.join();

    CHECK(run.exited);
    CHECK(run.exit_code == 0);
    CHECK(marker_present_of(run.stdout_text));
    CHECK(action_of(run.stdout_text) == "focused"); // single-instance: focused, not duplicated
    CHECK(raised.load() == 1);                       // the live editor raised its window exactly once
    CHECK(watcher.served() == 1);
    CHECK(!fs::exists(client::focus_request_path(project))); // the request was consumed, not left behind

    remove_tree(project);
}

// ---------------------------------------------------------------- 2. different project spawns anew

void test_open_of_a_project_with_no_editor_spawns()
{
    const fs::path project = make_temp_project("spawn");
    const RunResult run = run_context_edit({project.string(), "--no-launch"});
    CHECK(run.exited);
    CHECK(run.exit_code == 0);
    CHECK(!marker_present_of(run.stdout_text));
    CHECK(action_of(run.stdout_text) == "spawn"); // a different project -> a new process
    remove_tree(project);
}

// ---------------------------------------------------------------- 3. stale marker -> spawn

void test_a_stale_marker_with_no_live_editor_spawns()
{
    const fs::path project = make_temp_project("stale");
    client::PresenceMarker marker;
    marker.pid = 999999; // a pid that is not us and is not running an editor
    marker.boot_nonce = client::make_boot_nonce();
    publish_presence(project, marker); // a marker, but NO watcher behind it (a crashed editor)

    const RunResult run =
        run_context_edit({project.string(), "--no-launch", "--focus-timeout-ms", "400"});
    CHECK(run.exited);
    CHECK(run.exit_code == 0);
    CHECK(marker_present_of(run.stdout_text));   // the marker WAS seen
    CHECK(action_of(run.stdout_text) == "spawn"); // ...but it was stale, so spawn (crash-robust)
    remove_tree(project);
}

} // namespace

int main()
{
    test_second_open_focuses_the_live_editor();
    test_open_of_a_project_with_no_editor_spawns();
    test_a_stale_marker_with_no_live_editor_spawns();
    return g_failures == 0 ? 0 : 1;
}
