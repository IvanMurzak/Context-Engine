// T1 for the keybindings read/watch bridge (M9 e07c): the file read on bind, the generation-bumping
// watch (change / no-change / identical-rewrite / remove / recreate), the size-cap degrade, verbatim
// byte round-trip (the Shell ships bytes and never parses the keymap), and the full JSON-RPC binding
// over a real BridgeRouter.
//
// WHAT THIS PROVES AND WHAT IT DELEGATES. This bridge being D10 boundary-clean is asserted
// STRUCTURALLY, not here: it links no kernel-internal target (the shell-boundary gate enforces it) and
// touches only std::filesystem + streams (grep keybindings_bridge.* for context_filesync finds
// nothing). What this file proves is the wire behaviour: the override file's bytes reach editor-core
// through `keybindings.get` with a generation that moves exactly when the content does. The keymap
// SCHEMA validation + merge is editor-core's (keymap.ts, webui-ts tier), never the Shell's — so there
// is deliberately no schema assertion here; a malformed file rides through as raw bytes.

#include "context/editor/shell/keybindings_bridge.h"

#include "context/editor/shell/ipc_bridge.h"

#include "shell_test.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>

using namespace context::editor::shell;
namespace fs = std::filesystem;
using Json = context::editor::contract::Json;

namespace
{

void write_file(const fs::path& path, const std::string& content)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << content;
}

// Nudge a file's mtime forward so poll()'s cheap stat-gate opens even when the byte size is unchanged —
// the only way to reach the "content re-read but identical" path deterministically.
void bump_mtime(const fs::path& path)
{
    std::error_code ec;
    const fs::file_time_type now = fs::last_write_time(path, ec);
    fs::last_write_time(path, now + std::chrono::seconds(5), ec);
}

Json dispatch_get(BridgeRouter& router, bool& refused)
{
    Json request = Json::object();
    request.set("jsonrpc", Json("2.0"));
    request.set("id", Json(7));
    request.set("method", Json(kKeybindingsGetMethod));
    request.set("params", Json::object());
    const BridgeDispatch dispatch = router.dispatch(request.dump());
    const Json response = Json::parse(dispatch.response);
    refused = response.contains("error");
    if (refused)
    {
        return Json();
    }
    return response.at("result");
}

// A small, valid-looking override (the Shell never parses it — it is here to prove bytes round-trip).
const char* const kSample =
    "{\n  \"$schema\": \"https://schemas.context-engine.dev/m9/keybindings.schema.json\",\n"
    "  \"version\": 1,\n  \"bindings\": [ { \"key\": \"Ctrl+B\", \"command\": \"view.theme.toggle\" } ]\n}\n";

void absent_file_reads_as_absent()
{
    const fs::path dir = shelltest::make_temp_project("ctx-keybind", "absent");
    KeybindingsBridge bridge;
    bridge.bind_path(dir / "keybindings.json"); // does not exist
    CHECK(!bridge.present());
    CHECK(bridge.generation() == 0); // nothing observed to change from the absent baseline
    CHECK(bridge.text().empty());
    const Json snap = bridge.snapshot_json();
    CHECK(!snap.at("present").as_bool());
    CHECK(snap.at("generation").as_int() == 0);
    CHECK(snap.at("text").as_string().empty());
    shelltest::cleanup(dir);
}

void present_file_read_on_bind()
{
    const fs::path dir = shelltest::make_temp_project("ctx-keybind", "present");
    const fs::path file = dir / "keybindings.json";
    write_file(file, kSample);
    KeybindingsBridge bridge;
    bridge.bind_path(file);
    CHECK(bridge.present());
    CHECK(bridge.generation() == 1); // absent -> present is one observed change
    CHECK(bridge.text() == kSample);
    const Json snap = bridge.snapshot_json();
    CHECK(snap.at("present").as_bool());
    CHECK(snap.at("generation").as_int() == 1);
    CHECK(snap.at("text").as_string() == kSample); // verbatim bytes, not parsed
    shelltest::cleanup(dir);
}

void poll_unchanged_is_a_noop()
{
    const fs::path dir = shelltest::make_temp_project("ctx-keybind", "noop");
    const fs::path file = dir / "keybindings.json";
    write_file(file, kSample);
    KeybindingsBridge bridge;
    bridge.bind_path(file);
    const std::uint64_t gen = bridge.generation();
    CHECK(!bridge.poll()); // no change since bind
    CHECK(!bridge.poll());
    CHECK(bridge.generation() == gen);
    shelltest::cleanup(dir);
}

void poll_detects_a_content_change()
{
    const fs::path dir = shelltest::make_temp_project("ctx-keybind", "change");
    const fs::path file = dir / "keybindings.json";
    write_file(file, kSample);
    KeybindingsBridge bridge;
    bridge.bind_path(file);
    const std::uint64_t gen = bridge.generation();
    write_file(file, std::string(kSample) + "\n// edited\n");
    bump_mtime(file);
    CHECK(bridge.poll()); // a real change
    CHECK(bridge.generation() == gen + 1);
    CHECK(bridge.text() == std::string(kSample) + "\n// edited\n");
    shelltest::cleanup(dir);
}

void identical_rewrite_does_not_bump_generation()
{
    const fs::path dir = shelltest::make_temp_project("ctx-keybind", "rewrite");
    const fs::path file = dir / "keybindings.json";
    write_file(file, kSample);
    KeybindingsBridge bridge;
    bridge.bind_path(file);
    const std::uint64_t gen = bridge.generation();
    write_file(file, kSample); // identical bytes
    bump_mtime(file);          // force the cheap stat-gate to open, so the re-read actually runs
    CHECK(!bridge.poll());     // re-read happened, but the content is identical -> no bump
    CHECK(bridge.generation() == gen);
    shelltest::cleanup(dir);
}

void file_removed_then_recreated()
{
    const fs::path dir = shelltest::make_temp_project("ctx-keybind", "lifecycle");
    const fs::path file = dir / "keybindings.json";
    write_file(file, kSample);
    KeybindingsBridge bridge;
    bridge.bind_path(file);
    const std::uint64_t present_gen = bridge.generation();

    std::error_code ec;
    fs::remove(file, ec);
    CHECK(bridge.poll()); // present -> absent
    CHECK(!bridge.present());
    CHECK(bridge.generation() == present_gen + 1);
    CHECK(!bridge.poll()); // still absent -> idle no-op
    CHECK(bridge.generation() == present_gen + 1);

    write_file(file, kSample);
    CHECK(bridge.poll()); // absent -> present again
    CHECK(bridge.present());
    CHECK(bridge.generation() == present_gen + 2);
    CHECK(bridge.text() == kSample);
    shelltest::cleanup(dir);
}

void oversized_file_is_served_absent()
{
    const fs::path dir = shelltest::make_temp_project("ctx-keybind", "oversize");
    const fs::path file = dir / "keybindings.json";
    write_file(file, std::string(kMaxKeybindingsBytes + 1, 'x'));
    KeybindingsBridge bridge;
    bridge.bind_path(file);
    CHECK(!bridge.present()); // over the cap -> treated as unreadable -> absent (defaults stand)
    CHECK(bridge.text().empty());
    shelltest::cleanup(dir);
}

void bytes_round_trip_verbatim()
{
    const fs::path dir = shelltest::make_temp_project("ctx-keybind", "bytes");
    const fs::path file = dir / "keybindings.json";
    // Quotes, backslashes, a newline, non-ASCII — the Shell ships these bytes unchanged; the JSON
    // envelope's own escaping must not corrupt them on the way back out.
    const std::string content = "{\"k\":\"Ctrl+\\\"\", \"note\":\"\xC3\xA9 line\n\"}";
    write_file(file, content);
    KeybindingsBridge bridge;
    bridge.bind_path(file);
    CHECK(bridge.text() == content);
    CHECK(bridge.snapshot_json().at("text").as_string() == content);
    shelltest::cleanup(dir);
}

void empty_path_is_permanently_absent()
{
    KeybindingsBridge bridge;
    bridge.bind_path(fs::path{}); // "no home directory" case
    CHECK(!bridge.present());
    CHECK(!bridge.poll());
    CHECK(!bridge.present());
    CHECK(bridge.snapshot_json().at("present").as_bool() == false);
}

void served_over_a_real_router()
{
    const fs::path dir = shelltest::make_temp_project("ctx-keybind", "router");
    const fs::path file = dir / "keybindings.json";
    write_file(file, kSample);
    KeybindingsBridge bridge;
    bridge.bind_path(file);

    BridgeRouter router;
    CHECK(bridge.install(router));
    // A second install collides on the method name and is refused (a wiring-bug guard).
    CHECK(!bridge.install(router));

    bool refused = false;
    const Json first = dispatch_get(router, refused);
    CHECK(!refused);
    CHECK(first.at("present").as_bool());
    CHECK(first.at("generation").as_int() == 1);
    CHECK(first.at("text").as_string() == kSample);
    CHECK(bridge.reads() == 1);

    // A live edit -> poll bumps the generation -> the NEXT get serves the new snapshot.
    write_file(file, std::string(kSample) + "x");
    bump_mtime(file);
    CHECK(bridge.poll());
    const Json second = dispatch_get(router, refused);
    CHECK(!refused);
    CHECK(second.at("generation").as_int() == 2);
    CHECK(second.at("text").as_string() == std::string(kSample) + "x");
    CHECK(bridge.reads() == 2);
    shelltest::cleanup(dir);
}

} // namespace

int main()
{
    absent_file_reads_as_absent();
    present_file_read_on_bind();
    poll_unchanged_is_a_noop();
    poll_detects_a_content_change();
    identical_rewrite_does_not_bump_generation();
    file_removed_then_recreated();
    oversized_file_is_served_absent();
    bytes_round_trip_verbatim();
    empty_path_is_permanently_absent();
    served_over_a_real_router();
    SHELL_TEST_MAIN_END();
}
