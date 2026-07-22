// T1 for the watched-themes read/watch bridge (M9 e06b): the directory enumeration on bind, the
// generation-bumping watch (add / edit / identical-rewrite / remove / whole-directory removal), the
// suffix filter, the size cap, the count cap, deterministic ordering, verbatim byte round-trip (the
// Shell ships bytes and never parses a theme), and the full JSON-RPC binding over a real
// BridgeRouter.
//
// WHAT THIS PROVES AND WHAT IT DELEGATES. This bridge being D10 boundary-clean is asserted
// STRUCTURALLY, not here: it links no kernel-internal target (the shell-boundary gate enforces it)
// and touches only std::filesystem + streams. What this file proves is the WIRE behaviour — that a
// user's theme files reach editor-core through `themes.get` with a generation that moves exactly
// when their content does. The theme SCHEMA validation is editor-core's (theme.ts `ThemeRegistry`,
// webui-ts tier), never the Shell's, so there is deliberately no schema assertion here: a malformed
// theme rides through as raw bytes and is rejected on the other side, which is what keeps "a bad
// theme is never a broken UI" a property of ONE code path.

#include "context/editor/shell/themes_bridge.h"

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

// Nudge a file's mtime forward so poll()'s cheap listing-gate opens even when the byte size is
// unchanged — the only way to reach the "content re-read but identical" path deterministically.
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
    request.set("id", Json(11));
    request.set("method", Json(kThemesGetMethod));
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

// A small, valid-LOOKING theme. The Shell never parses it — it is here to prove bytes round-trip.
std::string sample_theme(const std::string& name)
{
    return "{\n  \"$schema\": \"https://context-engine.dev/schemas/theme/v1.json\",\n"
           "  \"version\": 1,\n  \"name\": \"" +
           name + "\"\n}\n";
}

void empty_directory_reads_as_no_themes()
{
    const fs::path dir = shelltest::make_temp_project("ctx-themes", "empty");
    ThemesBridge bridge;
    bridge.bind_directory(dir / "themes"); // does not exist
    CHECK(bridge.count() == 0);
    CHECK(bridge.generation() == 0); // nothing observed to change from the empty baseline
    const Json snap = bridge.snapshot_json();
    CHECK(snap.at("generation").as_int() == 0);
    CHECK(snap.at("themes").size() == 0);
    shelltest::cleanup(dir);
}

void present_files_read_on_bind()
{
    const fs::path dir = shelltest::make_temp_project("ctx-themes", "present");
    const fs::path themes = dir / "themes";
    write_file(themes / "solarized.theme.json", sample_theme("Solarized"));
    write_file(themes / "nord.theme.json", sample_theme("Nord"));
    ThemesBridge bridge;
    bridge.bind_directory(themes);
    CHECK(bridge.count() == 2);
    CHECK(bridge.generation() == 1); // empty -> two files is one observed change
    // SORTED by id, so the wire order is deterministic rather than filesystem-order-dependent.
    CHECK(bridge.themes()[0].id == "user.nord");
    CHECK(bridge.themes()[1].id == "user.solarized");
    CHECK(bridge.themes()[1].text == sample_theme("Solarized")); // verbatim bytes, not parsed

    const Json snap = bridge.snapshot_json();
    CHECK(snap.at("themes").size() == 2);
    CHECK(snap.at("themes").at(std::size_t{0}).at("id").as_string() == "user.nord");
    CHECK(snap.at("themes").at(std::size_t{0}).at("source").as_string() == kThemeSourceUser);
    CHECK(snap.at("themes").at(std::size_t{1}).at("text").as_string() == sample_theme("Solarized"));
    shelltest::cleanup(dir);
}

void non_theme_files_are_ignored()
{
    const fs::path dir = shelltest::make_temp_project("ctx-themes", "filter");
    const fs::path themes = dir / "themes";
    write_file(themes / "mine.theme.json", sample_theme("Mine"));
    write_file(themes / "README.md", "not a theme\n");
    write_file(themes / "mine.theme.json.bak", sample_theme("Backup"));
    write_file(themes / "theme.json", sample_theme("NoStem")); // suffix-only: no stem to name it
    ThemesBridge bridge;
    bridge.bind_directory(themes);
    CHECK(bridge.count() == 1);
    CHECK(bridge.themes()[0].id == "user.mine");
    shelltest::cleanup(dir);
}

void poll_unchanged_is_a_noop()
{
    const fs::path dir = shelltest::make_temp_project("ctx-themes", "noop");
    const fs::path themes = dir / "themes";
    write_file(themes / "mine.theme.json", sample_theme("Mine"));
    ThemesBridge bridge;
    bridge.bind_directory(themes);
    const std::uint64_t gen = bridge.generation();
    CHECK(!bridge.poll());
    CHECK(!bridge.poll());
    CHECK(bridge.generation() == gen);
    shelltest::cleanup(dir);
}

void poll_detects_an_edit()
{
    const fs::path dir = shelltest::make_temp_project("ctx-themes", "edit");
    const fs::path themes = dir / "themes";
    const fs::path file = themes / "mine.theme.json";
    write_file(file, sample_theme("Mine"));
    ThemesBridge bridge;
    bridge.bind_directory(themes);
    const std::uint64_t gen = bridge.generation();

    write_file(file, sample_theme("Mine Edited"));
    bump_mtime(file);
    CHECK(bridge.poll()); // a real content change — the hot-reload trigger
    CHECK(bridge.generation() == gen + 1);
    CHECK(bridge.themes()[0].text == sample_theme("Mine Edited"));
    shelltest::cleanup(dir);
}

void identical_rewrite_does_not_bump_generation()
{
    const fs::path dir = shelltest::make_temp_project("ctx-themes", "rewrite");
    const fs::path themes = dir / "themes";
    const fs::path file = themes / "mine.theme.json";
    write_file(file, sample_theme("Mine"));
    ThemesBridge bridge;
    bridge.bind_directory(themes);
    const std::uint64_t gen = bridge.generation();

    write_file(file, sample_theme("Mine")); // identical bytes
    bump_mtime(file);                       // force the listing-gate to open so the re-read runs
    CHECK(!bridge.poll());                  // re-read happened, content identical -> no bump
    CHECK(bridge.generation() == gen);
    shelltest::cleanup(dir);
}

void added_and_removed_files_move_the_generation()
{
    const fs::path dir = shelltest::make_temp_project("ctx-themes", "lifecycle");
    const fs::path themes = dir / "themes";
    write_file(themes / "a.theme.json", sample_theme("A"));
    ThemesBridge bridge;
    bridge.bind_directory(themes);
    const std::uint64_t gen = bridge.generation();

    write_file(themes / "b.theme.json", sample_theme("B"));
    CHECK(bridge.poll()); // a NEW file is an observed change even though `a` did not move
    CHECK(bridge.generation() == gen + 1);
    CHECK(bridge.count() == 2);

    std::error_code ec;
    fs::remove(themes / "b.theme.json", ec);
    CHECK(bridge.poll()); // and so is its removal
    CHECK(bridge.generation() == gen + 2);
    CHECK(bridge.count() == 1);
    CHECK(bridge.themes()[0].id == "user.a");

    fs::remove_all(themes, ec);
    CHECK(bridge.poll()); // the whole directory going away reads as "no watched themes"
    CHECK(bridge.count() == 0);
    CHECK(!bridge.poll()); // and stays idle afterwards
    shelltest::cleanup(dir);
}

void oversized_file_is_skipped_but_siblings_load()
{
    const fs::path dir = shelltest::make_temp_project("ctx-themes", "oversize");
    const fs::path themes = dir / "themes";
    write_file(themes / "huge.theme.json", std::string(kMaxThemeBytes + 1, 'x'));
    write_file(themes / "small.theme.json", sample_theme("Small"));
    ThemesBridge bridge;
    bridge.bind_directory(themes);
    // FAIL-CLOSED PER FILE, not per directory: one pathological file costs you that theme, not
    // every theme you have.
    CHECK(bridge.count() == 1);
    CHECK(bridge.themes()[0].id == "user.small");
    shelltest::cleanup(dir);
}

void the_count_cap_is_enforced_deterministically()
{
    const fs::path dir = shelltest::make_temp_project("ctx-themes", "cap");
    const fs::path themes = dir / "themes";
    for (std::size_t index = 0; index < kMaxWatchedThemes + 5; ++index)
    {
        // Zero-padded so lexicographic order is numeric order and the survivors are predictable.
        std::string name = std::to_string(index);
        name.insert(name.begin(), static_cast<std::size_t>(3 - name.size()), '0');
        write_file(themes / (name + ".theme.json"), sample_theme(name));
    }
    ThemesBridge bridge;
    bridge.bind_directory(themes);
    CHECK(bridge.count() == kMaxWatchedThemes);
    CHECK(bridge.themes()[0].id == "user.000"); // the sorted prefix survives, not an arbitrary set
    shelltest::cleanup(dir);
}

void bytes_round_trip_verbatim()
{
    const fs::path dir = shelltest::make_temp_project("ctx-themes", "bytes");
    const fs::path themes = dir / "themes";
    // Quotes, backslashes, a newline, non-ASCII — the Shell ships these bytes unchanged; the JSON
    // envelope's own escaping must not corrupt them on the way back out.
    const std::string content = "{\"name\":\"Quo\\\"ted\", \"note\":\"\xC3\xA9 line\n\"}";
    write_file(themes / "odd.theme.json", content);
    ThemesBridge bridge;
    bridge.bind_directory(themes);
    CHECK(bridge.themes()[0].text == content);
    CHECK(bridge.snapshot_json().at("themes").at(std::size_t{0}).at("text").as_string() == content);
    shelltest::cleanup(dir);
}

void empty_path_is_permanently_empty()
{
    ThemesBridge bridge;
    bridge.bind_directory(fs::path{}); // the "no home directory" case
    CHECK(bridge.count() == 0);
    CHECK(!bridge.poll());
    CHECK(bridge.snapshot_json().at("themes").size() == 0);
}

void served_over_a_real_router()
{
    const fs::path dir = shelltest::make_temp_project("ctx-themes", "router");
    const fs::path themes = dir / "themes";
    const fs::path file = themes / "mine.theme.json";
    write_file(file, sample_theme("Mine"));
    ThemesBridge bridge;
    bridge.bind_directory(themes);

    BridgeRouter router;
    CHECK(bridge.install(router));
    // A second install collides on the method name and is refused (a wiring-bug guard).
    CHECK(!bridge.install(router));

    bool refused = false;
    const Json first = dispatch_get(router, refused);
    CHECK(!refused);
    CHECK(first.at("generation").as_int() == 1);
    CHECK(first.at("themes").size() == 1);
    CHECK(bridge.reads() == 1);

    // A live edit -> poll bumps the generation -> the NEXT get serves the new snapshot. That pair is
    // the whole hot-reload contract as editor-core sees it.
    write_file(file, sample_theme("Mine Edited"));
    bump_mtime(file);
    CHECK(bridge.poll());
    const Json second = dispatch_get(router, refused);
    CHECK(!refused);
    CHECK(second.at("generation").as_int() == 2);
    CHECK(second.at("themes").at(std::size_t{0}).at("text").as_string() == sample_theme("Mine Edited"));
    CHECK(bridge.reads() == 2);
    shelltest::cleanup(dir);
}

} // namespace

int main()
{
    empty_directory_reads_as_no_themes();
    present_files_read_on_bind();
    non_theme_files_are_ignored();
    poll_unchanged_is_a_noop();
    poll_detects_an_edit();
    identical_rewrite_does_not_bump_generation();
    added_and_removed_files_move_the_generation();
    oversized_file_is_skipped_but_siblings_load();
    the_count_cap_is_enforced_deterministically();
    bytes_round_trip_verbatim();
    empty_path_is_permanently_empty();
    served_over_a_real_router();
    SHELL_TEST_MAIN_END();
}
