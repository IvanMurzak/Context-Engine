// `.editor/editor-state.json` (03 §1): the round-trip, the debounce, the crash-safe atomic replace,
// the no-op-on-identical rule, the degrade on a malformed document, and the retry after a failed write.

#include "context/editor/shell/editor_state.h"

#include "shell_test.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

using namespace context::editor::shell;
namespace fs = std::filesystem;
using context::editor::contract::Json;

namespace
{

std::string read_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

WindowPlacement placement(std::int32_t x, std::int32_t y, std::uint32_t w, std::uint32_t h,
                          bool maximized = false, std::string monitor = "\\\\.\\DISPLAY1")
{
    WindowPlacement p;
    p.monitor = std::move(monitor);
    p.x = x;
    p.y = y;
    p.width = w;
    p.height = h;
    p.maximized = maximized;
    return p;
}

void test_path_is_the_editor_owned_file()
{
    const fs::path root = "/projects/demo";
    const fs::path expected = fs::path("/projects/demo") / ".editor" / "editor-state.json";
    CHECK(editor_state_path(root) == expected);
    // The DAEMON owns .editor/session.json; the Shell owns this one. Two writers on one file is the
    // torn write the split exists to prevent (03 §1).
    CHECK(editor_state_path(root).filename() != "session.json");
}

void test_document_round_trips()
{
    EditorState state;
    state.windows.push_back(placement(10, 20, 1280, 800));
    state.windows.push_back(placement(-1900, 0, 1920, 1080, true, "\\\\.\\DISPLAY2"));
    Json layout = Json::object();
    layout.set("dock", Json("left"));
    state.layout = layout;

    const EditorState back = EditorState::from_json(state.to_json());
    CHECK(back.windows.size() == 2u);
    CHECK(back.windows[0] == state.windows[0]);
    // A NEGATIVE x — a window on a monitor left of the primary — must survive; reading it unsigned
    // would place the window off the far right of the desktop on the next boot.
    CHECK(back.windows[1].x == -1900);
    CHECK(back.windows[1].maximized);
    CHECK(back.windows[1].monitor == "\\\\.\\DISPLAY2");
    CHECK(back.layout.at("dock").as_string() == "left");
}

void test_a_maximized_window_still_records_its_restore_rect()
{
    // Restoring a maximized window with no restore rect leaves it stuck full-screen the first time
    // the user un-maximizes it.
    EditorState state;
    state.windows.push_back(placement(120, 60, 1024, 768, true));
    const EditorState back = EditorState::from_json(state.to_json());
    CHECK(back.windows[0].maximized);
    CHECK(back.windows[0].width == 1024u);
    CHECK(back.windows[0].height == 768u);
    CHECK(back.windows[0].x == 120);
}

void test_malformed_and_missing_documents_degrade_rather_than_refuse()
{
    const fs::path root = shelltest::make_temp_project("context-shell-state", "malformed");

    {
        // MISSING: a fresh project.
        EditorStateStore store(root);
        bool loaded = true;
        store.load(&loaded);
        CHECK(!loaded);
        CHECK(store.state().windows.empty());
    }

    // MALFORMED: a half-written or hand-edited document. A session file that will not load is a
    // user losing their layout, so it degrades to defaults instead of failing the boot.
    const fs::path path = editor_state_path(root);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    {
        std::ofstream out(path, std::ios::binary);
        out << "{\"windows\": [ {\"x\": 10,";
    }
    {
        EditorStateStore store(root);
        bool loaded = true;
        store.load(&loaded);
        CHECK(!loaded); // false is how a caller distinguishes "fresh" from "salvaged"
        CHECK(store.state().windows.empty());
    }

    // A NEGATIVE extent in a corrupted document falls back rather than wrapping to an enormous
    // unsigned one the swapchain would then be asked to configure.
    {
        std::ofstream out(path, std::ios::binary);
        out << "{\"windows\":[{\"x\":5,\"y\":5,\"width\":-10,\"height\":600}]}";
    }
    {
        EditorStateStore store(root);
        store.load();
        CHECK(store.state().windows.size() == 1u);
        CHECK(store.state().windows[0].width == 1280u); // the default, not 4294967286
        CHECK(store.state().windows[0].height == 600u);
    }

    shelltest::cleanup(root);
}

void test_writes_are_debounced()
{
    const fs::path root = shelltest::make_temp_project("context-shell-state", "debounce");
    EditorStateStore store(root, 500'000);
    store.load();

    // A window drag emits a placement change per mouse-move. Each marks the store dirty; none of
    // them writes until `debounce_us` has elapsed since the FIRST of them (leading-edge, not a
    // quiet period — the next assertion pins which of the two this is).
    store.set_placement(0, placement(0, 0, 1280, 800), 1'000);
    CHECK(store.dirty());
    CHECK(!store.flush_if_due(1'000));
    store.set_placement(0, placement(5, 0, 1280, 800), 100'000);
    CHECK(!store.flush_if_due(400'000));
    CHECK(store.write_count() == 0);

    // The quiet period elapses (measured from the FIRST dirtying change).
    CHECK(store.flush_if_due(501'000));
    CHECK(store.write_count() == 1);
    CHECK(!store.dirty());
    CHECK(fs::exists(editor_state_path(root)));

    // Nothing pending: a flush is a no-op, not a write.
    CHECK(!store.flush_if_due(2'000'000));
    CHECK(store.write_count() == 1);

    shelltest::cleanup(root);
}

void test_an_identical_placement_does_not_dirty_the_store()
{
    const fs::path root = shelltest::make_temp_project("context-shell-state", "identical");
    EditorStateStore store(root, 1'000);
    store.load();

    const WindowPlacement p = placement(0, 0, 1280, 800);
    store.set_placement(0, p, 0);
    CHECK(store.flush_if_due(10'000));
    CHECK(store.write_count() == 1);

    // A window that merely repainted re-reports the SAME placement. Without this rule that is a
    // file write per poll, forever, on a completely idle editor.
    store.set_placement(0, p, 20'000);
    CHECK(!store.dirty());
    CHECK(!store.flush_if_due(100'000));
    CHECK(store.write_count() == 1);

    // A real change still lands.
    store.set_placement(0, placement(40, 0, 1280, 800), 200'000);
    CHECK(store.dirty());
    CHECK(store.flush_if_due(300'000));
    CHECK(store.write_count() == 2);

    shelltest::cleanup(root);
}

void test_flush_now_ignores_the_debounce()
{
    const fs::path root = shelltest::make_temp_project("context-shell-state", "shutdown");
    EditorStateStore store(root, 10'000'000); // a long quiet period
    store.load();
    store.set_placement(0, placement(1, 2, 640, 480), 0);
    // The clean-shutdown path: waiting out a quiet period on the way down would just lose the last
    // change the user made.
    CHECK(store.flush_now());
    CHECK(store.write_count() == 1);
    CHECK(!store.flush_now()); // nothing pending
    shelltest::cleanup(root);
}

void test_the_write_is_atomic_and_leaves_no_temp_behind()
{
    const fs::path root = shelltest::make_temp_project("context-shell-state", "atomic");
    EditorStateStore store(root, 0);
    store.load();
    store.set_placement(0, placement(7, 8, 900, 700), 0);
    CHECK(store.flush_if_due(1));

    const fs::path path = editor_state_path(root);
    CHECK(fs::exists(path));
    // The staging file is renamed OVER the target, so it must not survive the write — a leftover
    // temp is a half-written document waiting to be renamed by a later attempt. Scan for ANY
    // sibling staging file rather than one hardcoded name: the staging name carries a
    // process-unique token, so asserting `<target>.tmp` alone would assert a path that can never
    // exist and would pass no matter how many temps were left behind.
    const std::string stem = path.filename().string() + ".tmp";
    int leftover_temps = 0;
    for (const fs::directory_entry& entry : fs::directory_iterator(path.parent_path()))
    {
        if (entry.path().filename().string().rfind(stem, 0) == 0)
        {
            ++leftover_temps;
        }
    }
    CHECK(leftover_temps == 0);

    // The bytes on disk parse and carry what was set.
    const EditorState reloaded = EditorState::from_json(Json::parse(read_file(path)));
    CHECK(reloaded.windows.size() == 1u);
    CHECK(reloaded.windows[0].x == 7);
    CHECK(reloaded.windows[0].width == 900u);

    // And a fresh store loads it as an EXISTING document.
    EditorStateStore reopened(root);
    bool loaded = false;
    reopened.load(&loaded);
    CHECK(loaded);
    CHECK(reopened.state().windows.size() == 1u);
    CHECK(reopened.state().windows[0].height == 700u);

    shelltest::cleanup(root);
}

void test_a_failed_write_stays_dirty_so_the_next_flush_retries()
{
    // A path whose parent cannot be created: the write fails, is REPORTED, and the store stays
    // dirty. A transient full disk must not silently drop the layout for the rest of the session.
    const fs::path root = shelltest::make_temp_project("context-shell-state", "failing");
    const fs::path blocker = root / "blocked";
    {
        std::ofstream out(blocker, std::ios::binary);
        out << "not a directory";
    }
    EditorStateStore store(blocker / "sub", 0);
    store.load();
    store.set_placement(0, placement(1, 1, 100, 100), 0);
    CHECK(!store.flush_if_due(1));
    CHECK(store.dirty()); // still pending — the next flush retries
    CHECK(store.write_count() == 0);
    CHECK(!store.last_error().empty());

    shelltest::cleanup(root);
}

void test_placement_index_grows_the_vector()
{
    const fs::path root = shelltest::make_temp_project("context-shell-state", "index");
    EditorStateStore store(root, 0);
    store.load();
    // Window 2 recorded before 0 and 1 exist — index 0 is the D13 menu/welcome window, so the
    // vector's INDICES are meaningful and must not be compacted.
    store.set_placement(2, placement(3, 3, 300, 300), 0);
    CHECK(store.state().windows.size() == 3u);
    CHECK(store.state().windows[2].width == 300u);
    CHECK(store.flush_if_due(1));

    EditorStateStore reopened(root);
    reopened.load();
    CHECK(reopened.state().windows.size() == 3u);
    CHECK(reopened.state().windows[2].x == 3);
    shelltest::cleanup(root);
}

void test_out_of_range_numbers_degrade_to_defaults_not_ub()
{
    // M9 e05d3 inherited fix: `as_int()` on an out-of-int64-range double is UB (UBSan
    // float-cast-overflow), and this file is hand-editable/corruptible on-disk input — Json::parse
    // accepts `1e300` happily. The hardened readers range-check the DOUBLE first, so a hostile
    // placement reads as the DEFAULT rather than tripping the cast (or wrapping to a huge unsigned
    // extent the swapchain would then be handed).
    const Json doc = Json::parse(R"({
        "version": 1,
        "windows": [
            {"x": 1e300, "y": -1e300, "width": 1e300, "height": -5, "maximized": false},
            {"x": 2147483647, "y": -2147483648, "width": 4294967295, "height": 1, "maximized": false}
        ]
    })");
    const EditorState state = EditorState::from_json(doc);
    CHECK(state.windows.size() == 2u);
    // Out-of-range / negative-extent fields fall back to WindowPlacement's defaults.
    const WindowPlacement defaults;
    CHECK(state.windows[0].x == defaults.x);
    CHECK(state.windows[0].y == defaults.y);
    CHECK(state.windows[0].width == defaults.width);
    CHECK(state.windows[0].height == defaults.height);
    // The exact type bounds still READ — the guard rejects only what cannot be represented.
    CHECK(state.windows[1].x == 2147483647);
    CHECK(state.windows[1].y == -2147483647 - 1);
    CHECK(state.windows[1].width == 4294967295u);
    CHECK(state.windows[1].height == 1u);
}

// ------------------------------------------------------------------- e14b: the presence marker

void test_presence_marker_is_written_by_the_shell_and_read_back(void)
{
    // The Shell is the SINGLE writer of editor-state.json (C-F3): the D15/C-F23 presence marker rides
    // this store, never a second writer. Set on boot, cleared on clean exit — an opener reads its
    // presence/absence from the serialized document to decide focus-vs-spawn.
    const fs::path root = shelltest::make_temp_project("context-shell-state", "presence");
    context::editor::client::PresenceMarker marker;
    marker.pid = 5150;
    marker.boot_nonce = "boot-nonce-e14b";

    {
        EditorStateStore store(root, 0);
        store.load();
        store.set_presence(marker, 0);
        CHECK(store.dirty());
        // Re-asserting the SAME marker each frame must be free (no dirty), like set_placement.
        store.set_presence(marker, 1);
        CHECK(store.flush_now());
        CHECK(store.write_count() == 1);
    }

    // A separate reader (the opener's view) sees the marker in the serialized document.
    const std::optional<context::editor::client::PresenceMarker> read =
        context::editor::client::parse_presence_from_editor_state(read_file(editor_state_path(root)));
    CHECK(read.has_value());
    CHECK(read->boot_nonce == "boot-nonce-e14b");
    CHECK(read->pid == 5150);

    // Clearing it (clean exit) drops the key entirely — ABSENCE is the honest "no editor present".
    {
        EditorStateStore store(root, 0);
        store.load();
        CHECK(store.state().presence.has_value()); // it loaded the marker back
        store.clear_presence(0);
        CHECK(store.dirty());
        CHECK(store.flush_now());
    }
    CHECK(!context::editor::client::parse_presence_from_editor_state(
               read_file(editor_state_path(root)))
               .has_value());

    shelltest::cleanup(root);
}

} // namespace

int main()
{
    test_path_is_the_editor_owned_file();
    test_presence_marker_is_written_by_the_shell_and_read_back();
    test_document_round_trips();
    test_a_maximized_window_still_records_its_restore_rect();
    test_malformed_and_missing_documents_degrade_rather_than_refuse();
    test_writes_are_debounced();
    test_an_identical_placement_does_not_dirty_the_store();
    test_flush_now_ignores_the_debounce();
    test_the_write_is_atomic_and_leaves_no_temp_behind();
    test_a_failed_write_stays_dirty_so_the_next_flush_retries();
    test_placement_index_grows_the_vector();
    test_out_of_range_numbers_degrade_to_defaults_not_ub();
    SHELL_TEST_MAIN_END();
}
