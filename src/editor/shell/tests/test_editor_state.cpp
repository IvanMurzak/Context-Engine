// `.editor/editor-state.json` (03 §1): the round-trip, the debounce, the crash-safe atomic replace,
// the no-op-on-identical rule, the degrade on a malformed document, the retry after a failed write,
// and — M9 e09d, design 07 §6 — the LOUD, non-blocking corrupt recovery (quarantine aside + defaults
// + a report), which is the half of "disposable by contract" that a silent reset would fake.

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

// M9 e09c — the session undo journal's blob. The Shell owns this file (03 §1 / C-F3), so the journal
// rides it like `layout`/`panels`: an opaque payload the store round-trips without interpreting.
void test_the_undo_blob_round_trips_and_follows_the_identical_value_rule()
{
    const std::string journal = R"({"redo":[],"undo":[{"edits":[]}],"version":1})";

    EditorState state;
    state.undo = Json(journal);
    const EditorState back = EditorState::from_json(state.to_json());
    CHECK(back.undo.is_string());
    CHECK(back.undo.as_string() == journal);

    // ABSENT is the honest "no journal yet" a fresh project restores. An EMPTY string is treated the
    // same on the way out, so the two cannot drift into meaning different things on the way back in.
    EditorState empty;
    CHECK(!empty.to_json().contains("undo"));
    CHECK(EditorState::from_json(empty.to_json()).undo.is_null());
    EditorState blank;
    blank.undo = Json(std::string(""));
    CHECK(!blank.to_json().contains("undo"));

    // A non-string `undo` (a hand-edited document) is NOT adopted — it degrades to "no journal"
    // rather than travelling one hop further as garbage.
    Json hostile = Json::object();
    hostile.set("version", Json(kEditorStateSchemaVersion));
    hostile.set("undo", Json::object());
    CHECK(EditorState::from_json(hostile).undo.is_null());

    // Adding the member did NOT bump the schema version: a bump would refuse every editor-state
    // document already on a user's disk (editor_state.h states the rule).
    CHECK(kEditorStateSchemaVersion == 1);

    const fs::path root = shelltest::make_temp_project("context-shell-state", "undo");
    EditorStateStore store(root, 0);
    store.load();
    store.set_undo(Json(journal), 0);
    CHECK(store.dirty());
    CHECK(store.flush_now());
    CHECK(store.write_count() == 1);

    // The owner loop re-offers the SAME journal every frame — that must cost nothing.
    store.set_undo(Json(journal), 0);
    CHECK(!store.dirty());
    CHECK(!store.flush_now());
    CHECK(store.write_count() == 1);

    // A journal that MOVED still lands, and reads back off real disk.
    const std::string moved = R"({"redo":[],"undo":[],"version":1})";
    store.set_undo(Json(moved), 0);
    CHECK(store.dirty());
    CHECK(store.flush_now());
    EditorStateStore reopened(root, 0);
    reopened.load();
    CHECK(reopened.state().undo.as_string() == moved);

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

// ------------------------------------------------------------- e10d: the schemaVersion guard (T1)

void test_schema_version_mismatch_degrades_to_null_state_with_a_diagnostic()
{
    // THE HONEST-DEGRADATION CLAUSE (M9 e10d, T1). A document written by a FUTURE build carries a
    // `version` this build does not understand. It must be NEITHER crashed on NOR silently
    // reinterpreted under this build's field meanings — it degrades to a NULL state and REPORTS why.
    const Json future = Json::parse(R"({
        "version": 999,
        "windows": [ {"x": 10, "y": 20, "width": 1280, "height": 800, "maximized": false} ],
        "layout": {"dock": "left"},
        "panels": {"inspector": {"open": true}}
    })");
    std::string diagnostic = "not-yet-set";
    const EditorState degraded = EditorState::from_json(future, &diagnostic);
    // NULL state: none of the future document's windows/layout/panels was reinterpreted.
    CHECK(degraded.windows.empty());
    CHECK(degraded.layout.is_null());
    CHECK(degraded.panels.is_null());
    // ...and the loss is REPORTED, not silent — the diagnostic names the found + supported versions.
    CHECK(!diagnostic.empty());
    CHECK(diagnostic.find("999") != std::string::npos);
    CHECK(diagnostic.find(std::to_string(kEditorStateSchemaVersion)) != std::string::npos);

    // A PAST/foreign version (not merely a higher one) is guarded the same way — the rule is
    // "present and != supported", not "greater than". Version 0 must not be read as version 1.
    std::string past_diag;
    const EditorState past = EditorState::from_json(
        Json::parse(R"({"version": 0, "windows": [ {"x": 1, "y": 2, "width": 3, "height": 4} ]})"),
        &past_diag);
    CHECK(past.windows.empty());
    CHECK(!past_diag.empty());

    // The MATCHING version reads normally, and passing no diagnostic sink still works (the guard's
    // out-param is optional, so every pre-e10d call site keeps compiling and degrades safely).
    std::string ok_diag = "cleared?";
    const EditorState ok = EditorState::from_json(
        Json::parse(R"({"version": 1, "windows": [ {"x": 7, "y": 8, "width": 640, "height": 480} ]})"),
        &ok_diag);
    CHECK(ok.windows.size() == 1u);
    CHECK(ok.windows[0].x == 7);
    CHECK(ok_diag.empty()); // a successful parse CLEARS the diagnostic

    // An ABSENT version is NOT a mismatch — a pre-versioning / partially-written document still
    // degrades tolerantly (this is what keeps `test_malformed_and_missing...` and the corrupted-
    // extent reads working). The guard fires only on a version that is present AND wrong.
    std::string absent_diag = "cleared?";
    const EditorState no_version = EditorState::from_json(
        Json::parse(R"({"windows": [ {"x": 5, "y": 6, "width": 800, "height": 600} ]})"), &absent_diag);
    CHECK(no_version.windows.size() == 1u);
    CHECK(absent_diag.empty());
}

void test_store_load_reports_a_schema_mismatch_without_crashing()
{
    // The STORE path of the guard: a future-version document on disk loads to a NULL state,
    // `loaded_existing` stays false (so the empty layout is NOT restored as the user's), and
    // `schema_diagnostic()` distinguishes "future build wrote this" from an ordinary fresh boot.
    // No crash on ANY path is the whole point (T1).
    const fs::path root = shelltest::make_temp_project("context-shell-state", "schema");
    const fs::path path = editor_state_path(root);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    {
        std::ofstream out(path, std::ios::binary);
        out << R"({"version": 42, "windows": [ {"x": 3, "y": 3, "width": 300, "height": 300} ],)"
               R"( "layout": {"dock": "right"}})";
    }

    EditorStateStore store(root);
    bool loaded = true;
    store.load(&loaded);
    CHECK(!loaded); // a mismatch is NOT a successful load of the user's state
    CHECK(!store.schema_diagnostic().empty());
    CHECK(store.schema_diagnostic().find("42") != std::string::npos);
    CHECK(store.state().windows.empty());
    CHECK(store.state().layout.is_null());

    // A subsequent VALID (matching-version) document loads normally AND clears the diagnostic — the
    // signal is per-load, not sticky.
    {
        std::ofstream out(path, std::ios::binary);
        out << R"({"version": 1, "windows": [ {"x": 9, "y": 9, "width": 900, "height": 900} ]})";
    }
    EditorStateStore reopened(root);
    bool loaded2 = false;
    reopened.load(&loaded2);
    CHECK(loaded2);
    CHECK(reopened.schema_diagnostic().empty());
    CHECK(reopened.state().windows.size() == 1u);
    CHECK(reopened.state().windows[0].x == 9);

    shelltest::cleanup(root);
}

// --------------------------------------------------------- e10d: N-window layout persistence (T2)

void test_n_window_layout_and_placements_persist_and_restore()
{
    // THE N-WINDOW PERSISTENCE DoD (M9 e10d, T2), reusing THIS serializer — there is no second
    // persistence path. Three peer windows, each with its OWN placement, plus the editor-owned
    // layout + panels blobs, are written once and read back by a FRESH store across a "restart":
    // window-0-primary and the peer ORDER (the indices are meaningful, D13) both survive.
    const fs::path root = shelltest::make_temp_project("context-shell-state", "nwindow");
    {
        EditorStateStore store(root, 0);
        store.load();
        // Window 0 is the D13 menu/welcome primary; 1 and 2 are docking peers on other monitors.
        store.set_placement(0, placement(0, 0, 1280, 800, false, "\\\\.\\DISPLAY1"), 0);
        store.set_placement(1, placement(-1920, 40, 1920, 1080, true, "\\\\.\\DISPLAY2"), 0);
        store.set_placement(2, placement(1280, 0, 1024, 768, false, "\\\\.\\DISPLAY3"), 0);
        // The opaque editor-core layout tree + per-panel state — the Shell round-trips them verbatim.
        Json layout = Json::object();
        layout.set("orientation", Json("horizontal"));
        Json groups = Json::array();
        groups.push_back(Json("scene"));
        groups.push_back(Json("inspector"));
        layout.set("groups", groups);
        store.set_layout(layout, 0);
        Json panels = Json::object();
        panels.set("inspector", Json("expanded"));
        store.set_panels(panels, 0);
        CHECK(store.flush_now());
    }

    // The RESTART: a brand-new store over the same project reads the whole N-window arrangement back.
    EditorStateStore restored(root);
    bool loaded = false;
    restored.load(&loaded);
    CHECK(loaded);
    CHECK(restored.schema_diagnostic().empty());
    const EditorState& state = restored.state();
    CHECK(state.windows.size() == 3u);
    // window-0-primary preserved (index 0 is the menu/welcome host).
    CHECK(state.windows[0] == placement(0, 0, 1280, 800, false, "\\\\.\\DISPLAY1"));
    // Each peer's placement — monitor, restored rect, maximized — is intact and in order.
    CHECK(state.windows[1].monitor == "\\\\.\\DISPLAY2");
    CHECK(state.windows[1].x == -1920);
    CHECK(state.windows[1].maximized);
    CHECK(state.windows[2].monitor == "\\\\.\\DISPLAY3");
    CHECK(state.windows[2].x == 1280);
    CHECK(!state.windows[2].maximized);
    // The layout tree + panel blobs the peers reference restore verbatim.
    CHECK(state.layout.at("orientation").as_string() == "horizontal");
    CHECK(state.layout.at("groups").size() == 2u);
    CHECK(state.layout.at("groups").at(0).as_string() == "scene");
    CHECK(state.panels.at("inspector").as_string() == "expanded");
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

// ------------------------------------------- e09d: LOUD, non-blocking corrupt recovery (07 §6, T1)

// One helper for the shape every recovery case asserts, so a new case cannot forget half of it: the
// original file is GONE from its own path, its EXACT bytes are at the quarantine, the state is back
// to defaults, and the report says `recovered` with a reason.
void check_recovered(EditorStateStore& store, const fs::path& root, const std::string& original,
                     const fs::path& expected_quarantine)
{
    bool loaded = true;
    store.load(&loaded);
    CHECK(!loaded); // a recovery is NOT a successful load of the user's state
    CHECK(store.state().windows.empty());
    CHECK(store.state().layout.is_null());

    const EditorStateRestoreReport& report = store.restore_report();
    CHECK(report.outcome == EditorStateRestoreOutcome::recovered);
    CHECK(!report.detail.empty()); // LOUD means it can say WHY, not just that it happened
    CHECK(report.path == editor_state_path(root).string());
    CHECK(report.quarantined_path == expected_quarantine.string());

    // The file is off its own path, so the next write starts clean...
    CHECK(!fs::exists(editor_state_path(root)));
    // ...and the bytes are still THERE. "Moved aside" is a salvage claim, and a test that only
    // checked for a file at the quarantine path would pass against an empty one.
    CHECK(fs::exists(expected_quarantine));
    CHECK(read_file(expected_quarantine) == original);
}

void test_a_corrupt_document_is_quarantined_loudly_and_never_blocks()
{
    // THE e09d DoD LINE (07 §6): `.editor/editor-state.json` is disposable BY CONTRACT, so a
    // document that will not load must never block the boot — and must never be reset SILENTLY
    // either. Before e09d the malformed path did exactly that: caught the parse error, took the
    // defaults, and said nothing, so a user lost their window layout AND (since e09c) their undo
    // history with no diagnostic and no bytes to recover from.
    const fs::path root = shelltest::make_temp_project("context-shell-state", "recover");
    const fs::path path = editor_state_path(root);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    const std::string original = "{\"windows\": [ {\"x\": 10,";
    {
        std::ofstream out(path, std::ios::binary);
        out << original;
    }

    EditorStateStore store(root, 0);
    check_recovered(store, root, original, editor_state_quarantine_path(root, 0));
    CHECK(store.restore_report().detail.find("not well-formed JSON") != std::string::npos);

    // NON-BLOCKING, asserted end to end rather than by the absence of a throw: the very same store
    // goes on to record a placement and write a good document, which a fresh store reads back. A
    // recovery that left the store unusable would satisfy every assertion above and still be a bug.
    store.set_placement(0, placement(7, 7, 700, 700), 0);
    CHECK(store.flush_now());
    CHECK(store.last_error().empty());
    EditorStateStore reopened(root);
    bool reloaded = false;
    reopened.load(&reloaded);
    CHECK(reloaded);
    CHECK(reopened.restore_report().outcome == EditorStateRestoreOutcome::restored);
    CHECK(reopened.state().windows.size() == 1u);
    CHECK(reopened.state().windows[0].x == 7);

    shelltest::cleanup(root);
}

void test_recovery_covers_every_unusable_document_shape()
{
    // Four shapes, all of them things a real disk hands back, and NONE of them a "malformed JSON"
    // the parse would reject: an EMPTY file (an interrupted write, or a filesystem that created the
    // entry and lost the contents), a top-level ARRAY and a top-level SCALAR (well-formed JSON that
    // is not a document — `from_json` is deliberately tolerant of odd MEMBERS but has no reading of
    // a non-object, so before this it reported a clean "restored" for a file it understood nothing
    // of), and TRAILING GARBAGE after a good document.
    const char* const shapes[] = {"", "[]", "\"just a string\"", "{\"version\": 1} trailing"};
    int n = 0;
    for (const char* shape : shapes)
    {
        const std::string tag = "shape" + std::to_string(n++);
        const fs::path root = shelltest::make_temp_project("context-shell-state", tag.c_str());
        const fs::path path = editor_state_path(root);
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        {
            std::ofstream out(path, std::ios::binary);
            out << shape;
        }
        EditorStateStore store(root, 0);
        check_recovered(store, root, shape, editor_state_quarantine_path(root, 0));
        shelltest::cleanup(root);
    }
}

void test_a_foreign_schema_version_is_moved_aside_rather_than_overwritten_in_place()
{
    // A FUTURE build's document is not corrupt — it is unreadable BY THIS BUILD (the e10d guard) —
    // and the tempting reading is "leave it alone, the newer build will want it". That reading is
    // wrong, and this test pins why: leaving it is not preservation. The store runs on defaults and
    // the FIRST dirty flush replaces the file, so the newer build's state is destroyed either way;
    // quarantining is the only version where a copy survives. Asserted by reading the ORIGINAL bytes
    // back out of the quarantine AFTER a subsequent write has landed on the real path.
    const fs::path root = shelltest::make_temp_project("context-shell-state", "foreign");
    const fs::path path = editor_state_path(root);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const std::string future =
        R"({"version": 42, "windows": [ {"x": 3, "y": 3, "width": 300, "height": 300} ]})";
    {
        std::ofstream out(path, std::ios::binary);
        out << future;
    }

    EditorStateStore store(root, 0);
    check_recovered(store, root, future, editor_state_quarantine_path(root, 0));
    // The e10d signal is PRESERVED, not replaced by the recovery: a caller can still tell "a foreign
    // build wrote this" from "the bytes were garbage", and the report's detail carries that reason.
    CHECK(!store.schema_diagnostic().empty());
    CHECK(store.schema_diagnostic().find("42") != std::string::npos);
    CHECK(store.restore_report().detail == store.schema_diagnostic());

    store.set_placement(0, placement(1, 1, 100, 100), 0);
    CHECK(store.flush_now());
    CHECK(read_file(path) != future);                                        // overwritten, as it would have been anyway
    CHECK(read_file(editor_state_quarantine_path(root, 0)) == future);        // ...but not LOST

    shelltest::cleanup(root);
}

void test_quarantine_names_do_not_collide()
{
    // A user whose disk is producing torn writes produces SEVERAL of them, and a fixed quarantine
    // name would mean each recovery destroys the evidence of the last one.
    const fs::path root = shelltest::make_temp_project("context-shell-state", "collide");
    const fs::path path = editor_state_path(root);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    for (int n = 0; n < 3; ++n)
    {
        const std::string bad = "{ broken " + std::to_string(n);
        {
            std::ofstream out(path, std::ios::binary);
            out << bad;
        }
        EditorStateStore store(root, 0);
        check_recovered(store, root, bad, editor_state_quarantine_path(root, n));
    }
    // All three are still on disk, each holding its OWN bytes.
    CHECK(read_file(editor_state_quarantine_path(root, 0)) == "{ broken 0");
    CHECK(read_file(editor_state_quarantine_path(root, 1)) == "{ broken 1");
    CHECK(read_file(editor_state_quarantine_path(root, 2)) == "{ broken 2");

    shelltest::cleanup(root);
}

void test_fresh_and_restored_are_not_recoveries()
{
    // The counter-cases, and the reason they matter: a recovery diagnostic that also fires on an
    // ordinary first boot is a diagnostic every user learns to ignore, which costs exactly the
    // loudness 07 §6 asked for. A MISSING file is `fresh` and quarantines nothing; a GOOD file is
    // `restored`.
    const fs::path root = shelltest::make_temp_project("context-shell-state", "quiet");
    {
        EditorStateStore store(root, 0);
        store.load();
        CHECK(store.restore_report().outcome == EditorStateRestoreOutcome::fresh);
        CHECK(store.restore_report().detail.empty());
        CHECK(store.restore_report().quarantined_path.empty());
        CHECK(store.restore_report().path == editor_state_path(root).string());
        CHECK(!fs::exists(editor_state_quarantine_path(root, 0)));

        store.set_placement(0, placement(4, 4, 400, 400), 0);
        CHECK(store.flush_now());
    }
    EditorStateStore reopened(root);
    reopened.load();
    CHECK(reopened.restore_report().outcome == EditorStateRestoreOutcome::restored);
    CHECK(reopened.restore_report().detail.empty());
    CHECK(reopened.restore_report().quarantined_path.empty());
    CHECK(!fs::exists(editor_state_quarantine_path(root, 0)));

    shelltest::cleanup(root);
}

void test_the_quarantine_path_is_a_sibling_and_not_the_owned_file()
{
    // The quarantine must not itself be a second session file: same directory, DIFFERENT name, and
    // never the document the store owns (a convention slip there would have the recovery rename the
    // file onto itself, which on POSIX silently succeeds and destroys nothing but is not a
    // quarantine either).
    const fs::path root = "/projects/demo";
    CHECK(editor_state_quarantine_path(root, 0).parent_path() ==
          editor_state_path(root).parent_path());
    CHECK(editor_state_quarantine_path(root, 0) != editor_state_path(root));
    CHECK(editor_state_quarantine_path(root, 0).filename() == "editor-state.corrupt.json");
    CHECK(editor_state_quarantine_path(root, 7).filename() == "editor-state.corrupt-7.json");
    // The daemon's file and quarantine are a DIFFERENT pair (03 §1) — neither of these may collide
    // with `session.json` / `session.corrupt.json`, which the daemon is the single writer of.
    CHECK(editor_state_quarantine_path(root, 0).filename() != "session.corrupt.json");

    // The catalog string is pinned on BOTH sides of the seam: here, where the constant lives, and in
    // src/editor/contract/tests/test_error_catalog.cpp against the registered row. The contract
    // library cannot include a shell header (the dependency runs the other way), so the two literals
    // agreeing is what makes the promote-a-local-string pattern safe.
    CHECK(std::string(kEditorStateInvalidCode) == "editor.editor_state_invalid");
}

// ------------------------------------------------- e09d refine: preservation is a PRECONDITION

void test_a_document_that_cannot_be_preserved_is_never_written_over()
{
    // THE DATA-LOSS REGRESSION. Before this, a quarantine whose rename FAILED reported "…could NOT
    // be renamed aside and remains at <path>" — and then the boot's very next act destroyed exactly
    // those bytes: editor_main publishes the presence marker and calls flush_now(), which
    // atomic-writes defaults over the file the message had just promised was still there. The user
    // lost their window layout AND their undo history with no copy anywhere, which is the silent
    // reset e09d exists to eliminate, with a reassuring diagnostic on top.
    //
    // Forced portably by making preservation IMPOSSIBLE: every quarantine slot is occupied, and the
    // slot the exhausted-bound fallback lands on is a NON-EMPTY DIRECTORY — a destination both
    // `rename` and `copy_file` refuse on every platform.
    const fs::path root = shelltest::make_temp_project("context-shell-state", "nopreserve");
    const fs::path path = editor_state_path(root);
    std::error_code ec;

    // shelltest::write_file, not a raw ofstream, and that matters MORE here than anywhere else in
    // this file: it ASSERTS the fixture landed. If these 64 occupying entries silently failed to
    // appear, `preserve_unusable_document` would find a free slot, preservation would SUCCEED, and
    // every assertion below would hold vacuously against a test that proved nothing.
    const std::string original = R"({"windows": [ {"x": 42,)"; // malformed on purpose
    shelltest::write_file(path, original);
    fs::create_directories(editor_state_quarantine_path(root, 0), ec);
    shelltest::write_file(editor_state_quarantine_path(root, 0) / "occupied.txt", "x");
    for (int n = 1; n < 64; ++n)
    {
        shelltest::write_file(editor_state_quarantine_path(root, n), "taken");
    }

    EditorStateStore store(root, 0);
    bool loaded = true;
    store.load(&loaded);
    CHECK(!loaded);

    const EditorStateRestoreReport& report = store.restore_report();
    CHECK(report.outcome == EditorStateRestoreOutcome::recovered);
    CHECK(report.preservation_failed);       // the fact a caller must be able to act on...
    CHECK(report.quarantined_path.empty());  // ...and no quarantine is CLAIMED that does not exist
    CHECK(report.detail.find("could NOT be preserved") != std::string::npos);

    // THE ASSERTION THIS TEST EXISTS FOR: the boot's write is REFUSED, and the user's bytes survive
    // it byte-for-byte. Without the guard this flush succeeds and `original` is gone forever.
    store.set_presence([] {
        context::editor::client::PresenceMarker m;
        m.pid = 4321;
        m.boot_nonce = "nonce";
        return m;
    }(), 0);
    CHECK(store.dirty());
    CHECK(!store.flush_now());
    CHECK(!store.last_error().empty());
    CHECK(store.last_error().find("refusing to write") != std::string::npos);
    CHECK(store.dirty()); // still dirty, so a later flush retries rather than dropping the change
    CHECK(fs::exists(path));
    CHECK(read_file(path) == original);

    // ...and it is a REFUSAL, not a wedge: once the obstruction clears, the retry inside write()
    // preserves the document and the store resumes saving.
    fs::remove_all(editor_state_quarantine_path(root, 0), ec);
    CHECK(store.flush_now());
    CHECK(!store.restore_report().preservation_failed);
    CHECK(read_file(editor_state_quarantine_path(root, 0)) == original); // salvaged after all
    CHECK(read_file(path) != original);                                  // ...and now safe to write

    shelltest::cleanup(root);
}

void test_exhausting_the_quarantine_slots_says_which_salvage_it_replaced()
{
    // The bound silently REPLACED the oldest quarantine, and the comment describing it claimed the
    // opposite ("the last candidate is overwritten"; the initialiser makes it the FIRST). Destroying
    // a prior salvage may be the least-bad option at the bound, but it is not something to do
    // quietly — a user chasing a lost layout needs to know a copy was consumed.
    const fs::path root = shelltest::make_temp_project("context-shell-state", "slots");
    const fs::path path = editor_state_path(root);
    for (int n = 0; n < 64; ++n)
    {
        // write_file asserts each slot landed — the same vacuity guard as the sibling test above.
        shelltest::write_file(editor_state_quarantine_path(root, n),
                             "older salvage " + std::to_string(n));
    }
    const std::string original = "{ not json";
    shelltest::write_file(path, original);

    EditorStateStore store(root, 0);
    store.load();
    const EditorStateRestoreReport& report = store.restore_report();
    CHECK(report.outcome == EditorStateRestoreOutcome::recovered);
    CHECK(!report.preservation_failed);
    CHECK(report.quarantined_path == editor_state_quarantine_path(root, 0).string());
    CHECK(report.detail.find("slots are exhausted") != std::string::npos);
    CHECK(read_file(editor_state_quarantine_path(root, 0)) == original); // slot 0 replaced...
    CHECK(read_file(editor_state_quarantine_path(root, 1)) == "older salvage 1"); // ...and only it

    shelltest::cleanup(root);
}

void test_an_unusable_version_is_a_mismatch_and_never_a_cast_over_the_range()
{
    // TWO holes in one guard, both reached from a hand-edited file.
    //
    // (a) `1e300` went through `as_int()`, a `static_cast<int64_t>` of the stored double — UB the
    //     blocking `sanitize (ASan+UBSan, ubuntu)` leg reports as `float-cast-overflow`.
    //     `Json::parse` accepts it happily (test_out_of_range_numbers_degrade_to_defaults_not_ub
    //     pins that for the placement members); the version read was the one that had never been
    //     routed through the shared range guard, and since e09d it decides whether the file is
    //     MOVED AND REPLACED.
    // (b) a version that is present but NOT A NUMBER (`"1"`, null, an object) skipped the guard
    //     entirely and was reinterpreted under this build's field meanings — reported as a clean
    //     "restored". The header's rule is "present AND wrong"; each of these is present and wrong.
    const char* const shapes[] = {
        R"({"version": 1e300, "windows": []})",
        R"({"version": -1e300, "windows": []})",
        R"({"version": "1", "windows": []})",
        R"({"version": null, "windows": []})",
        R"({"version": {}, "windows": []})",
    };
    int n = 0;
    for (const char* shape : shapes)
    {
        const std::string tag = "ver" + std::to_string(n++);
        const fs::path root = shelltest::make_temp_project("context-shell-state", tag.c_str());
        shelltest::write_file(editor_state_path(root), shape);
        EditorStateStore store(root, 0);
        check_recovered(store, root, shape, editor_state_quarantine_path(root, 0));
        CHECK(!store.schema_diagnostic().empty());
        shelltest::cleanup(root);
    }
}

void test_a_minimal_but_valid_document_is_restored_rather_than_quarantined()
{
    // THE OVER-QUARANTINE DIRECTION, which nothing pinned. Quarantining is destructive-ish (it moves
    // the user's file), so a regression that classified "parsed, but carries nothing we recognise"
    // as unusable would move perfectly good documents aside — and would have passed the whole suite,
    // because every recovery case feeds it something genuinely broken. These three are the honest
    // minimum a real project produces, and all of them must load.
    const char* const shapes[] = {"{}", R"({"version": 1})", R"({"windows": []})"};
    int n = 0;
    for (const char* shape : shapes)
    {
        const std::string tag = "minimal" + std::to_string(n++);
        const fs::path root = shelltest::make_temp_project("context-shell-state", tag.c_str());
        const fs::path path = editor_state_path(root);
        shelltest::write_file(path, shape);
        EditorStateStore store(root, 0);
        bool loaded = false;
        store.load(&loaded);
        CHECK(loaded);
        CHECK(store.restore_report().outcome == EditorStateRestoreOutcome::restored);
        CHECK(!store.restore_report().preservation_failed);
        CHECK(store.restore_report().quarantined_path.empty());
        CHECK(store.schema_diagnostic().empty());
        CHECK(fs::exists(path));                                        // NOT moved aside
        CHECK(!fs::exists(editor_state_quarantine_path(root, 0)));
        shelltest::cleanup(root);
    }
}

void test_a_document_that_is_simply_absent_is_never_announced_as_a_recovery()
{
    // THE OVER-CORRECTION GUARD, and a regression the preservation work itself introduced before
    // this test existed. `exists(path_, ec)` can report an ERROR (a locked parent, `.editor` present
    // as a FILE, a transient Windows sharing error) rather than a clean yes/no, and treating that as
    // "absent" is a silent reset — so it now falls through to the unusable path. But the unusable
    // path ends in "nothing could be preserved, so never write over those bytes", and for a document
    // that IS NOT THERE that would announce a recovery which never happened AND wedge the store into
    // never saving layout again, for the whole session, over bytes that do not exist.
    //
    // The rename and the copy answer what the probe could not: both refusing with "no such file" IS
    // the absence proof. Asserted here through the ordinary absent case, which must stay a silent,
    // writable `fresh` — the property the tri-state exists to keep.
    const fs::path root = shelltest::make_temp_project("context-shell-state", "absent");
    EditorStateStore store(root, 0);
    bool loaded = true;
    store.load(&loaded);
    CHECK(!loaded);
    CHECK(store.restore_report().outcome == EditorStateRestoreOutcome::fresh);
    CHECK(!store.restore_report().preservation_failed);
    CHECK(store.restore_report().detail.empty());
    CHECK(store.restore_report().quarantined_path.empty());
    CHECK(!fs::exists(editor_state_quarantine_path(root, 0)));

    // ...and the store still SAVES. A fresh project that could not write its layout would be the
    // wedge this test exists to rule out.
    store.set_placement(0, placement(5, 5, 500, 500), 0);
    CHECK(store.flush_now());
    CHECK(store.last_error().empty());
    CHECK(fs::exists(editor_state_path(root)));

    shelltest::cleanup(root);
}

void test_a_failed_write_re_arms_the_debounce_rather_than_retrying_every_pump()
{
    // The owner loop pumps `flush_if_due` at ~250 Hz and the store stays dirty on failure by design,
    // so a persistent failure used to re-run the whole write path once per FRAME — and since e09d a
    // refused write re-runs the quarantine-slot probe with it. Re-arming the debounce paces the
    // retry instead. Asserted on the pre-existing failure mode (an unwritable parent), because it
    // was already true there and the fix covers both.
    const fs::path root = shelltest::make_temp_project("context-shell-state", "rearm");
    const fs::path blocker = root / "blocked";
    shelltest::write_file(blocker, "not a directory");

    EditorStateStore store(blocker / "sub", 1000);
    store.load();
    store.set_placement(0, placement(1, 1, 100, 100), 0);
    CHECK(!store.flush_if_due(5000)); // due, attempted, failed
    CHECK(store.dirty());
    CHECK(!store.last_error().empty());
    // The very next pump is NOT due again: the failure re-armed the clock at 5000.
    CHECK(!store.flush_if_due(5004));
    CHECK(store.dirty());
    // ...and it becomes due again one debounce later, so the retry still happens.
    CHECK(!store.flush_if_due(6001)); // still fails (the blocker is still a file), but it TRIED
    CHECK(store.dirty());

    shelltest::cleanup(root);
}

void test_the_report_carries_a_project_relative_path_for_the_problems_panel()
{
    // The panel's payload `file` member is the one ProblemsFeed RENDERS and GROUPS BY, and every
    // other row in it is project-relative (merge_command.cpp, test_problems_feed.cpp). An absolute
    // native path there would be the one `C:\…` group header in the list. stderr keeps the absolute
    // form, where the reader may have no project context at all — so the report carries both.
    const fs::path root = shelltest::make_temp_project("context-shell-state", "relpath");
    EditorStateStore store(root, 0);
    store.load();
    const EditorStateRestoreReport& report = store.restore_report();
    CHECK(report.path == editor_state_path(root).string());
    CHECK(report.project_relative_path == ".editor/editor-state.json");
    CHECK(report.project_relative_path.find('\\') == std::string::npos); // slash-separated always
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
    test_the_undo_blob_round_trips_and_follows_the_identical_value_rule();
    test_flush_now_ignores_the_debounce();
    test_the_write_is_atomic_and_leaves_no_temp_behind();
    test_a_failed_write_stays_dirty_so_the_next_flush_retries();
    test_placement_index_grows_the_vector();
    test_out_of_range_numbers_degrade_to_defaults_not_ub();
    test_schema_version_mismatch_degrades_to_null_state_with_a_diagnostic();
    test_store_load_reports_a_schema_mismatch_without_crashing();
    test_n_window_layout_and_placements_persist_and_restore();
    test_a_corrupt_document_is_quarantined_loudly_and_never_blocks();
    test_recovery_covers_every_unusable_document_shape();
    test_a_foreign_schema_version_is_moved_aside_rather_than_overwritten_in_place();
    test_quarantine_names_do_not_collide();
    test_fresh_and_restored_are_not_recoveries();
    test_the_quarantine_path_is_a_sibling_and_not_the_owned_file();
    test_a_document_that_cannot_be_preserved_is_never_written_over();
    test_exhausting_the_quarantine_slots_says_which_salvage_it_replaced();
    test_an_unusable_version_is_a_mismatch_and_never_a_cast_over_the_range();
    test_a_minimal_but_valid_document_is_restored_rather_than_quarantined();
    test_a_document_that_is_simply_absent_is_never_announced_as_a_recovery();
    test_a_failed_write_re_arms_the_debounce_rather_than_retrying_every_pump();
    test_the_report_carries_a_project_relative_path_for_the_problems_panel();
    SHELL_TEST_MAIN_END();
}
