// T1 for the per-user config store (M9 e06d): the merge-preserving read-modify-write, the CLOSED
// settable vocabulary `config.set` validates against, the generation-bumping watch, the total reader,
// the UNIQUE-temp atomic write (the e14c `.tmp` collision), and the full `config.*` binding over a real
// BridgeRouter.
//
// WHAT THIS PROVES, AND WHY IT IS NOT THE WHOLE SINGLE-WRITER STORY. Behaviour tests can prove that
// THIS writer is correct; they cannot prove that no OTHER writer exists — a second one would simply
// make them pass more slowly. So the C-F14 single-writer claim is carried by a SOURCE gate
// (`tools/check_config_writers.py`, ctest `editor-shell-config-writers`), which fails on a tree where
// any other translation unit writes the document, and which FAILED on the pre-e06d tree where
// welcome.cpp had its own writer. The two are complements: this file says the write is right, the gate
// says it is the only one.
//
// The RECENTS/THEME co-existence case below is the regression test for the defect that motivated all
// of it: `record_recent_project` used to replace the whole document, so opening a project silently
// discarded the theme the user had just chosen.

#include "context/editor/shell/user_config.h"

#include "context/editor/shell/ipc_bridge.h"
#include "context/editor/shell/keybindings_bridge.h" // default_keybindings_path() - the reported keymap
#include "context/editor/shell/welcome.h"

#include "shell_test.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <vector>

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

// Nudge a file's mtime forward so poll()'s cheap stat-gate opens even when the byte size is unchanged.
void bump_mtime(const fs::path& path)
{
    std::error_code ec;
    const fs::file_time_type now = fs::last_write_time(path, ec);
    fs::last_write_time(path, now + std::chrono::seconds(5), ec);
}

[[nodiscard]] Json dispatch(BridgeRouter& router, const char* method, Json params, bool& refused,
                            std::string& error_code)
{
    Json request = Json::object();
    request.set("jsonrpc", Json("2.0"));
    request.set("id", Json(11));
    request.set("method", Json(std::string(method)));
    request.set("params", std::move(params));
    const BridgeDispatch result = router.dispatch(request.dump());
    const Json response = Json::parse(result.response);
    refused = response.contains("error");
    if (refused)
    {
        const Json& error = response.at("error");
        if (error.is_object() && error.contains("data") && error.at("data").is_object() &&
            error.at("data").contains("reason"))
        {
            error_code = error.at("data").at("reason").as_string();
        }
        return Json();
    }
    return response.at("result");
}

// ------------------------------------------------------------------------------- the reader

void reader_is_total_over_a_hostile_file()
{
    const fs::path dir = shelltest::make_temp_project("ctx-config", "reader");
    const fs::path config = dir / "config.json";

    // Absent, malformed, and non-object files all mean the same thing to every caller: nothing was
    // recorded. A THROW here would take down the welcome screen and the boot theme read alike.
    CHECK(read_user_config(config).is_object());
    CHECK(read_user_config(config).object_members().empty());

    write_file(config, "{ this is not json");
    CHECK(read_user_config(config).object_members().empty());

    write_file(config, "[1, 2, 3]");
    CHECK(read_user_config(config).object_members().empty());

    write_file(config, "{\"theme\": \"builtin.light\"}");
    CHECK(read_user_config(config).at(kConfigThemeKey).as_string() == "builtin.light");

    // An empty path (no HOME/USERPROFILE) is a supported host state, not an error.
    CHECK(read_user_config(fs::path()).object_members().empty());

    shelltest::cleanup(dir);
}

void oversized_file_reads_as_empty()
{
    const fs::path dir = shelltest::make_temp_project("ctx-config", "oversized");
    const fs::path config = dir / "config.json";
    // Just past the cap, and valid JSON, so the ONLY reason to refuse it is the size.
    std::string huge = "{\"theme\": \"" + std::string(kMaxUserConfigBytes + 16u, 'a') + "\"}";
    write_file(config, huge);
    CHECK(read_user_config(config).object_members().empty());
    shelltest::cleanup(dir);
}

// ------------------------------------------------------------------------------- the writer

void write_is_atomic_and_leaves_no_temp_behind()
{
    const fs::path dir = shelltest::make_temp_project("ctx-config", "atomic");
    const fs::path config = dir / "nested" / "config.json";

    Json doc = Json::object();
    doc.set(kConfigThemeKey, Json(std::string("builtin.dark")));
    std::string error;
    CHECK(write_user_config(config, doc, &error));
    CHECK(error.empty());
    CHECK(fs::exists(config)); // the parent dir was created

    // No staging file survives a successful publish. A leftover `.tmp` is how the e14c collision
    // announced itself, so its ABSENCE is worth asserting rather than assuming.
    std::size_t strays = 0;
    for (const fs::directory_entry& entry : fs::directory_iterator(config.parent_path()))
    {
        if (entry.path().filename().string() != "config.json")
        {
            ++strays;
        }
    }
    CHECK(strays == 0);

    CHECK(read_user_config(config).at(kConfigThemeKey).as_string() == "builtin.dark");
    shelltest::cleanup(dir);
}

void temp_names_are_unique_per_write()
{
    // THE e14c DEFECT, asserted at its mechanism. The old writer staged through ONE fixed
    // `<config>.tmp`, so two launches writing at once (the welcome window and a project window — the
    // ordinary case) could interleave and publish each other's partial bytes.
    //
    // Asserted on the NAME GENERATOR rather than through a contrived IO failure, because that is the
    // property that actually fixes it: a test that could only observe the published file would pass
    // unchanged with the collision still in place, which is the definition of a vacuous regression
    // test. Two calls must differ from each other AND from the old fixed name.
    const fs::path config = fs::path("C:/nowhere/.context/config.json");
    const fs::path first = staging_path_for(config);
    const fs::path second = staging_path_for(config);

    CHECK(first != second);
    CHECK(first != fs::path(config.string() + ".tmp")); // the exact name that collided
    CHECK(second != fs::path(config.string() + ".tmp"));
    // Both still stage BESIDE the target (same directory), which is what keeps the publish a rename
    // rather than a cross-device copy.
    CHECK(first.parent_path() == config.parent_path());
    CHECK(second.parent_path() == config.parent_path());
    CHECK(shelltest::mentions(first.filename().string(), "config.json.tmp."));
}

void write_refuses_without_a_path_or_an_object()
{
    Json doc = Json::object();
    std::string error;
    CHECK(!write_user_config(fs::path(), doc, &error));
    CHECK(shelltest::mentions(error, "no user config path"));

    const fs::path dir = shelltest::make_temp_project("ctx-config", "non-object");
    error.clear();
    CHECK(!write_user_config(dir / "config.json", Json(std::string("nope")), &error));
    CHECK(shelltest::mentions(error, "object"));
    shelltest::cleanup(dir);
}

// --------------------------------------------------------------- the merge-preserving contract

void set_preserves_every_other_member()
{
    const fs::path dir = shelltest::make_temp_project("ctx-config", "merge");
    const fs::path config = dir / "config.json";
    // A document written by some OTHER feature — including a member this build has never heard of.
    write_file(config,
               "{\n  \"version\": 1,\n  \"recents\": [ {\"path\": \"/p/a\", \"name\": \"a\"} ],\n"
               "  \"windowDefaults\": {\"width\": 1280},\n  \"fromAFutureBuild\": true\n}\n");

    UserConfigStore store;
    store.bind_path(config);
    std::string code;
    std::string message;
    CHECK(store.set(kConfigThemeKey, Json(std::string("builtin.high-contrast-dark")), code, message));
    CHECK(store.writes() == 1);

    const Json after = read_user_config(config);
    CHECK(after.at(kConfigThemeKey).as_string() == "builtin.high-contrast-dark");
    CHECK(after.contains("recents") && after.at("recents").size() == 1);
    CHECK(after.at("windowDefaults").at("width").as_int() == 1280);
    // The member this build does not understand SURVIVES. An older editor must not truncate a config
    // a newer one wrote — that is the whole reason this is a read-modify-write and not a serialize.
    CHECK(after.at("fromAFutureBuild").as_bool());
    shelltest::cleanup(dir);
}

void recording_a_recent_project_keeps_the_theme()
{
    // THE REGRESSION TEST for the defect that motivated the single-writer refactor: e14c's
    // `record_recent_project` replaced the whole document, so opening a project discarded the theme.
    const fs::path dir = shelltest::make_temp_project("ctx-config", "recents-theme");
    const fs::path config = dir / "config.json";

    UserConfigStore store;
    store.bind_path(config);
    std::string code;
    std::string message;
    CHECK(store.set(kConfigThemeKey, Json(std::string("builtin.light")), code, message));

    std::string error;
    CHECK(record_recent_project(config, dir / "some-project", 1234, 10, &error));

    const Json after = read_user_config(config);
    CHECK(after.at(kConfigThemeKey).as_string() == "builtin.light"); // <- used to be gone
    CHECK(after.at(kConfigRecentsKey).size() == 1);
    CHECK(read_recent_projects(config).size() == 1);

    // ...and the reverse order: a theme written over a document that already holds recents keeps them.
    CHECK(store.poll()); // adopt the recents write made behind the store's back
    CHECK(store.set(kConfigThemeKey, Json(std::string("builtin.dark")), code, message));
    const Json again = read_user_config(config);
    CHECK(again.at(kConfigThemeKey).as_string() == "builtin.dark");
    CHECK(again.at(kConfigRecentsKey).size() == 1);
    shelltest::cleanup(dir);
}

// ------------------------------------------------------------------ the closed settable vocabulary

void set_refuses_an_unknown_key()
{
    const fs::path dir = shelltest::make_temp_project("ctx-config", "unknown-key");
    const fs::path config = dir / "config.json";
    UserConfigStore store;
    store.bind_path(config);

    std::string code;
    std::string message;
    CHECK(!store.set("recents", Json::array(), code, message));
    CHECK(code == kErrConfigUnknownKey);
    CHECK(shelltest::mentions(message, "not a settable key"));
    CHECK(store.refusals() == 1);
    CHECK(store.writes() == 0);
    CHECK(!fs::exists(config)); // a refused request writes NOTHING

    shelltest::cleanup(dir);
}

void set_refuses_a_malformed_theme_id()
{
    const fs::path dir = shelltest::make_temp_project("ctx-config", "bad-value");
    const fs::path config = dir / "config.json";
    UserConfigStore store;
    store.bind_path(config);

    std::string code;
    std::string message;
    CHECK(!store.set(kConfigThemeKey, Json(std::string()), code, message)); // empty
    CHECK(code == kErrConfigBadValue);
    CHECK(!store.set(kConfigThemeKey, Json(42), code, message)); // not a string
    CHECK(code == kErrConfigBadValue);
    CHECK(!store.set(kConfigThemeKey, Json(std::string("has space")), code, message));
    CHECK(code == kErrConfigBadValue);
    CHECK(!store.set(kConfigThemeKey, Json(std::string("line\nbreak")), code, message));
    CHECK(code == kErrConfigBadValue);
    CHECK(!store.set(kConfigThemeKey, Json(std::string(200u, 'x')), code, message)); // too long
    CHECK(code == kErrConfigBadValue);
    CHECK(store.writes() == 0);

    // FORM, not existence: an id the registry does not currently hold is legitimate (a user theme file
    // that is not present on this machine yet), and editor-core already falls back for one it cannot
    // resolve. The Shell cannot know the registry, so it must not pretend to.
    CHECK(store.set(kConfigThemeKey, Json(std::string("user.not-installed-here")), code, message));
    CHECK(store.writes() == 1);
    shelltest::cleanup(dir);
}

void set_refuses_when_there_is_no_home()
{
    UserConfigStore store;
    store.bind_path(fs::path()); // no HOME / USERPROFILE
    CHECK(!store.writable());
    std::string code;
    std::string message;
    CHECK(!store.set(kConfigThemeKey, Json(std::string("builtin.dark")), code, message));
    CHECK(code == kErrConfigWriteFailed);
    CHECK(store.theme().empty());
}

// ------------------------------------------------------------------------------- the watch

void generation_moves_only_on_a_real_change()
{
    const fs::path dir = shelltest::make_temp_project("ctx-config", "watch");
    const fs::path config = dir / "config.json";

    UserConfigStore store;
    store.bind_path(config); // absent
    CHECK(store.generation() == 0);
    CHECK(store.theme().empty());
    CHECK(!store.poll()); // still absent

    write_file(config, "{\"theme\": \"builtin.dark\"}");
    CHECK(store.poll());
    CHECK(store.generation() == 1);
    CHECK(store.theme() == "builtin.dark");
    CHECK(!store.poll()); // unchanged

    // A byte-identical rewrite (fresh mtime) is NOT a change — an editor must not re-notify itself
    // because a user saved the file without editing it.
    write_file(config, "{\"theme\": \"builtin.dark\"}");
    bump_mtime(config);
    CHECK(!store.poll());
    CHECK(store.generation() == 1);

    // A cosmetic reformat is likewise not a change: the document is the same document.
    write_file(config, "{\n    \"theme\":   \"builtin.dark\"\n}\n");
    bump_mtime(config);
    CHECK(!store.poll());

    write_file(config, "{\"theme\": \"builtin.light\"}");
    bump_mtime(config);
    CHECK(store.poll());
    CHECK(store.generation() == 2);
    CHECK(store.theme() == "builtin.light");

    std::error_code ec;
    fs::remove(config, ec);
    CHECK(store.poll());
    CHECK(store.generation() == 3);
    CHECK(store.theme().empty());
    shelltest::cleanup(dir);
}

void a_self_write_does_not_read_back_as_an_external_change()
{
    const fs::path dir = shelltest::make_temp_project("ctx-config", "self-write");
    const fs::path config = dir / "config.json";
    UserConfigStore store;
    store.bind_path(config);

    std::string code;
    std::string message;
    CHECK(store.set(kConfigThemeKey, Json(std::string("builtin.dark")), code, message));
    const std::uint64_t after_write = store.generation();
    CHECK(after_write == 1);
    // The store already holds what it just wrote; polling must not bump the generation again, or every
    // save would look like a foreign edit to whatever watches it.
    CHECK(!store.poll());
    CHECK(store.generation() == after_write);
    shelltest::cleanup(dir);
}

// ------------------------------------------------------------------------------- the bridge

void bridge_serves_get_and_set()
{
    const fs::path dir = shelltest::make_temp_project("ctx-config", "bridge");
    const fs::path config = dir / "config.json";
    write_file(config, "{\"theme\": \"builtin.dark\", \"recents\": []}");

    UserConfigStore store;
    store.bind_path(config);
    BridgeRouter router;
    CHECK(store.install(router));

    bool refused = false;
    std::string code;
    Json snapshot = dispatch(router, kConfigGetMethod, Json::object(), refused, code);
    CHECK(!refused);
    CHECK(store.reads() == 1);
    CHECK(snapshot.at("writable").as_bool());
    CHECK(snapshot.at("path").as_string() == config.generic_string());
    CHECK(snapshot.at("config").at(kConfigThemeKey).as_string() == "builtin.dark");
    // The keymap file the Settings panel points the user at: reported even though it does not exist —
    // "here is where to create it" is the useful answer (e07c).
    CHECK(!snapshot.at("keybindingsPath").as_string().empty() ||
          default_keybindings_path().empty());

    Json params = Json::object();
    params.set("key", Json(std::string(kConfigThemeKey)));
    params.set("value", Json(std::string("builtin.light")));
    const Json stored = dispatch(router, kConfigSetMethod, std::move(params), refused, code);
    CHECK(!refused);
    CHECK(stored.at("stored").as_bool());
    CHECK(store.writes() == 1);
    CHECK(read_user_config(config).at(kConfigThemeKey).as_string() == "builtin.light");
    // The write went THROUGH the document: the pre-existing member is still there.
    CHECK(read_user_config(config).contains("recents"));

    // A refusal comes back as a JSON-RPC error carrying the stable code, not as a silent no-op.
    Json bad = Json::object();
    bad.set("key", Json(std::string("somethingElse")));
    bad.set("value", Json(true));
    (void)dispatch(router, kConfigSetMethod, std::move(bad), refused, code);
    CHECK(refused);
    CHECK(code == kErrConfigUnknownKey);

    // A `config.set` with no params at all is a refusal, never an accidental write.
    (void)dispatch(router, kConfigSetMethod, Json::object(), refused, code);
    CHECK(refused);
    CHECK(store.writes() == 1);
    shelltest::cleanup(dir);
}

} // namespace

int main()
{
    reader_is_total_over_a_hostile_file();
    oversized_file_reads_as_empty();
    write_is_atomic_and_leaves_no_temp_behind();
    temp_names_are_unique_per_write();
    write_refuses_without_a_path_or_an_object();
    set_preserves_every_other_member();
    recording_a_recent_project_keeps_the_theme();
    set_refuses_an_unknown_key();
    set_refuses_a_malformed_theme_id();
    set_refuses_when_there_is_no_home();
    generation_moves_only_on_a_real_change();
    a_self_write_does_not_read_back_as_an_external_change();
    bridge_serves_get_and_set();
    SHELL_TEST_MAIN_END();
}
