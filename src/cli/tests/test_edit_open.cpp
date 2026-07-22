// M9 e14b — `context edit .` project-open entry path (edit_open.h): the open-vs-write shape
// disambiguation, the arbitration-driven envelope (focus / spawn), and the editor-binary locator.
// The CONCURRENT focus handshake against a real running editor is the T2 two-process drill
// (src/tests/integration/test_e14b_arbitration.cpp); here everything is single-process + `--no-launch`
// so no GUI is ever spawned.

#include "context/cli/edit_open.h"

#include "cli_test.h"

#include "context/editor/client/arbitration.h"
#include "context/editor/contract/json.h"

#include <chrono>
#include <fstream>
#include <string>
#include <system_error>

using context::cli::is_edit_open_shape;
using context::cli::locate_editor_binary;
using context::cli::run_edit_open;
using context::editor::contract::Envelope;
using context::editor::contract::Json;
namespace client = context::editor::client;
namespace fs = std::filesystem;

namespace
{

fs::path make_temp_project(const char* tag)
{
    static int counter = 0;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir = fs::temp_directory_path() /
                   ("ctx-e14b-cli-" + std::string(tag) + "-" + std::to_string(stamp) + "-" +
                    std::to_string(++counter));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / ".editor", ec);
    return dir;
}

void write_file(const fs::path& path, const std::string& text)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string editor_state_with_presence(const client::PresenceMarker& marker)
{
    Json doc = Json::object();
    doc.set("version", Json(static_cast<std::int64_t>(1)));
    doc.set("presence", marker.to_json());
    return doc.dump(2);
}

// The open shape is one directory positional; the write shape (two positionals) is NOT.
void test_open_shape_disambiguation()
{
    CHECK(is_edit_open_shape({"."}));
    CHECK(is_edit_open_shape({".."}));
    // A two-positional file write is the reserved operational verb, never the open path.
    CHECK(!is_edit_open_shape({"proj/a.scene", "entity: 1"}));
    // A lone non-directory positional is not the open path either.
    CHECK(!is_edit_open_shape({"definitely-not-a-real-directory-zzzz"}));
    // No positional at all.
    CHECK(!is_edit_open_shape({}));
    CHECK(!is_edit_open_shape({"--no-launch"}));

    // An existing directory IS the open shape, even with flags interleaved.
    const fs::path project = make_temp_project("shape");
    CHECK(is_edit_open_shape({project.string()}));
    CHECK(is_edit_open_shape({"--no-launch", project.string()}));
    CHECK(is_edit_open_shape({project.string(), "--focus-timeout-ms", "200"}));
    // With the timeout's value present, that value token is NOT a second positional.
    CHECK(is_edit_open_shape({"--focus-timeout-ms", "200", project.string()}));
    std::error_code ec;
    fs::remove_all(project, ec);
}

// No marker => the opener reports "spawn"; with --no-launch it launches nothing.
void test_no_marker_reports_spawn_without_launching()
{
    const fs::path project = make_temp_project("spawn");
    const Envelope env =
        run_edit_open({project.string(), "--no-launch"}, fs::path("context"));
    CHECK(env.ok());
    CHECK(env.exit_code() == 0);
    const Json& data = env.data();
    CHECK(data.at("action").as_string() == "spawn");
    CHECK(!data.at("markerPresent").as_bool());
    CHECK(!data.at("launched").as_bool());
    // C-F3: the opener never wrote editor-state.json.
    CHECK(!fs::exists(project / ".editor" / "editor-state.json"));
    std::error_code ec;
    fs::remove_all(project, ec);
}

// A STALE marker (no live editor consumes the focus request) also resolves to spawn.
void test_stale_marker_times_out_and_reports_spawn()
{
    const fs::path project = make_temp_project("stale");
    client::PresenceMarker marker;
    marker.pid = 424242;
    marker.boot_nonce = client::make_boot_nonce();
    write_file(project / ".editor" / "editor-state.json", editor_state_with_presence(marker));

    const Envelope env = run_edit_open(
        {project.string(), "--no-launch", "--focus-timeout-ms", "250"}, fs::path("context"));
    CHECK(env.ok());
    const Json& data = env.data();
    CHECK(data.at("markerPresent").as_bool());
    CHECK(data.at("action").as_string() == "spawn"); // stale -> spawn, not a phantom focus
    std::error_code ec;
    fs::remove_all(project, ec);
}

// A missing project argument is a usage error, not a crash.
void test_missing_argument_is_a_usage_error()
{
    const Envelope env = run_edit_open({"--no-launch"}, fs::path("context"));
    CHECK(!env.ok());
    CHECK(env.exit_code() == 2); // usage class
}

// A non-directory positional is rejected (the shape guard would normally stop it upstream, but the
// command is defensive on its own).
void test_non_directory_is_rejected()
{
    const Envelope env =
        run_edit_open({"no-such-directory-eviljfkdls", "--no-launch"}, fs::path("context"));
    CHECK(!env.ok());
    CHECK(env.exit_code() == 2);
}

// The locator resolves an install-layout sibling and a dev build-tree layout.
void test_editor_locator()
{
    const fs::path root = make_temp_project("locate");
#if defined(_WIN32)
    const std::string editor_name = "context_editor.exe";
    const std::string cli_name = "context.exe";
#else
    const std::string editor_name = "context_editor";
    const std::string cli_name = "context";
#endif
    // Install layout: the editor sits beside the CLI.
    const fs::path bin = root / "bin";
    std::error_code ec;
    fs::create_directories(bin, ec);
    write_file(bin / editor_name, "stub");
    const fs::path located = locate_editor_binary(bin / cli_name);
    CHECK(located.filename().string() == editor_name);
    CHECK(fs::exists(located));

    // Dev build tree: <build>/cli/context -> <build>/editor/shell/context_editor.
    const fs::path build = root / "build";
    fs::create_directories(build / "cli", ec);
    fs::create_directories(build / "editor" / "shell", ec);
    write_file(build / "editor" / "shell" / editor_name, "stub");
    const fs::path dev_located = locate_editor_binary(build / "cli" / cli_name);
    CHECK(fs::exists(dev_located));
    CHECK(dev_located.filename().string() == editor_name);

    fs::remove_all(root, ec);
}

} // namespace

int main()
{
    test_open_shape_disambiguation();
    test_no_marker_reports_spawn_without_launching();
    test_stale_marker_times_out_and_reports_spawn();
    test_missing_argument_is_a_usage_error();
    test_non_directory_is_rejected();
    test_editor_locator();
    CLI_TEST_MAIN_END();
}
