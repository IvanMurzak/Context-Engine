// The M9 e08a DAEMON EDITOR SESSION STATE — T1 unit tier (R-QA-013: happy path, edge cases, AND
// failure paths, shipped with the behaviour).
//
// Covers the state machine + its persisted projection, with no wire and no daemon (the RPC surface
// is proven over the real transport in test_kernel_server.cpp; the cross-process multi-CLIENT
// propagation + echo-suppression drill is src/tests/integration/test_e08a_daemon_session_state.cpp):
//   * selection: the four apply modes, set semantics (de-dup, first-mention order), and the
//     "nothing actually changed" verdict that keeps a no-op from publishing an echo;
//   * cameras: opaque transform/projection round-trip + the unchanged-write verdict;
//   * play: the L-51 edit/playing/paused machine mirroring gui::playbar (including the
//     play.not_running refusals and the stop-discards-the-tick-counter rule);
//   * persistence: write -> read round-trip through a REAL `.editor/session.json`, a missing file
//     as a clean `fresh` boot, and the LOUD corrupt-file recovery (quarantined aside, defaults
//     loaded, a report to announce) across malformed JSON, a wrong shape, and a FUTURE version.

#include "context/editor/editorkernel/editor_session_state.h"

#include "editorkernel_test.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using context::editor::contract::Json;
using context::editor::editorkernel::CameraState;
using context::editor::editorkernel::EditorPlayState;
using context::editor::editorkernel::EditorSessionState;
using context::editor::editorkernel::kEditorSessionStateInvalidCode;
using context::editor::editorkernel::parse_selection_mode;
using context::editor::editorkernel::persist_session_state;
using context::editor::editorkernel::play_state_token;
using context::editor::editorkernel::PlayOutcome;
using context::editor::editorkernel::restore_session_state;
using context::editor::editorkernel::SelectionMode;
using context::editor::editorkernel::selection_mode_token;
using context::editor::editorkernel::session_state_path;
using context::editor::editorkernel::SessionRestoreOutcome;
using context::editor::editorkernel::SessionRestoreReport;

namespace
{
fs::path make_temp_project(const char* tag)
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir = fs::temp_directory_path() /
                   ("ctx-e08a-" + std::string(tag) + "-" + std::to_string(stamp));
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

void write_session_file(const fs::path& project, const std::string& text)
{
    std::error_code ec;
    fs::create_directories(project / ".editor", ec);
    std::ofstream f(session_state_path(project), std::ios::binary | std::ios::trunc);
    f << text;
}

std::string read_text(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return std::string();
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::vector<std::string> ids(std::initializer_list<const char*> list)
{
    std::vector<std::string> out;
    for (const char* s : list)
        out.emplace_back(s);
    return out;
}

Json number(double v)
{
    return Json(v);
}

// --- selection ------------------------------------------------------------------------------------

void test_selection_modes()
{
    EditorSessionState state;
    CHECK(state.selection().empty());

    // replace: the ids BECOME the selection, de-duplicated, first-mention order preserved.
    CHECK(state.apply_selection(ids({"a", "b", "a"}), SelectionMode::replace));
    CHECK(state.selection() == ids({"a", "b"}));

    // An identical re-select changes NOTHING — the verdict the caller uses to skip publishing an
    // event (a no-op fact would be exactly the echo `origin` exists to prevent).
    CHECK(!state.apply_selection(ids({"a", "b"}), SelectionMode::replace));
    CHECK(state.selection() == ids({"a", "b"}));

    // add: union, and adding an already-present id is a no-op.
    CHECK(state.apply_selection(ids({"c"}), SelectionMode::add));
    CHECK(state.selection() == ids({"a", "b", "c"}));
    CHECK(!state.apply_selection(ids({"a", "c"}), SelectionMode::add));

    // toggle: present ids leave, absent ids join — in one pass.
    CHECK(state.apply_selection(ids({"b", "d"}), SelectionMode::toggle));
    CHECK(state.selection() == ids({"a", "c", "d"}));

    // remove: set difference; removing an absent id is a no-op.
    CHECK(state.apply_selection(ids({"c"}), SelectionMode::remove));
    CHECK(state.selection() == ids({"a", "d"}));
    CHECK(!state.apply_selection(ids({"zzz"}), SelectionMode::remove));

    // Clearing the selection is `replace` with an empty list (a real change, then a no-op).
    CHECK(state.apply_selection({}, SelectionMode::replace));
    CHECK(state.selection().empty());
    CHECK(!state.apply_selection({}, SelectionMode::replace));
}

void test_selection_mode_tokens_round_trip()
{
    for (const SelectionMode mode : {SelectionMode::replace, SelectionMode::add,
                                     SelectionMode::toggle, SelectionMode::remove})
    {
        const std::string token = selection_mode_token(mode);
        const auto parsed = parse_selection_mode(token);
        CHECK(parsed.has_value());
        CHECK(*parsed == mode);
    }
    // An unknown token is REFUSED rather than silently falling back to `replace` — a silent fallback
    // would mutate more of the human's selection than the caller asked for.
    CHECK(!parse_selection_mode("").has_value());
    CHECK(!parse_selection_mode("Replace").has_value());
    CHECK(!parse_selection_mode("clear").has_value());
}

// --- cameras --------------------------------------------------------------------------------------

void test_cameras_are_opaque_and_change_detected()
{
    EditorSessionState state;
    Json transform = Json::object();
    transform.set("x", number(1));
    Json projection = Json::object();
    projection.set("fov", number(60));

    CHECK(state.set_camera("main", transform, projection));
    CHECK(state.cameras().size() == 1);
    CHECK(state.cameras().at("main").transform.at("x").as_number() == 1.0);
    CHECK(state.cameras().at("main").projection.at("fov").as_number() == 60.0);

    // An identical write is not a change (no event).
    CHECK(!state.set_camera("main", transform, projection));

    // A different transform IS, and a second viewport is its own entry.
    Json moved = Json::object();
    moved.set("x", number(2));
    CHECK(state.set_camera("main", moved, projection));
    CHECK(state.set_camera("side", Json(), Json()));
    CHECK(state.cameras().size() == 2);
    // A camera with neither member is legitimate — the daemon is a custodian, not an interpreter.
    CHECK(state.cameras().at("side").transform.is_null());
}

// --- play control (L-51) --------------------------------------------------------------------------

void test_play_state_machine_mirrors_the_playbar()
{
    EditorSessionState state;
    CHECK(state.play_state() == EditorPlayState::edit);
    CHECK(state.sim_tick() == 0);

    // In `edit` there is no live session: pause and step are REFUSED with the reserved code, while
    // stop is idempotent (a stopped transport bar's stop button does nothing).
    const PlayOutcome pause_in_edit = state.pause();
    CHECK(!pause_in_edit.ok);
    CHECK(pause_in_edit.error_code == "play.not_running");
    CHECK(!pause_in_edit.changed);
    const PlayOutcome step_in_edit = state.step(1);
    CHECK(!step_in_edit.ok);
    CHECK(step_in_edit.error_code == "play.not_running");
    const PlayOutcome stop_in_edit = state.stop();
    CHECK(stop_in_edit.ok);
    CHECK(!stop_in_edit.changed); // benign no-op => no event is published
    CHECK(state.play_state() == EditorPlayState::edit);

    // edit -> playing.
    const PlayOutcome started = state.play();
    CHECK(started.ok);
    CHECK(started.changed);
    CHECK(started.state == EditorPlayState::playing);
    // Already playing is a benign no-op (ok, but nothing changed => nothing is published).
    const PlayOutcome again = state.play();
    CHECK(again.ok);
    CHECK(!again.changed);

    // Stepping advances the tick counter and leaves playing/paused alone.
    const PlayOutcome stepped = state.step(3);
    CHECK(stepped.ok);
    CHECK(stepped.changed);
    CHECK(stepped.sim_tick == 3);
    CHECK(state.play_state() == EditorPlayState::playing);
    // A zero-tick step advances nothing — no change, no event.
    CHECK(!state.step(0).changed);

    // playing -> paused; the tick counter survives (it is the SAME live session).
    const PlayOutcome paused = state.pause();
    CHECK(paused.ok);
    CHECK(paused.changed);
    CHECK(paused.state == EditorPlayState::paused);
    CHECK(state.sim_tick() == 3);
    CHECK(!state.pause().changed); // already paused

    // Stepping from `paused` is allowed (that is what a frame-step button does).
    CHECK(state.step(1).changed);
    CHECK(state.sim_tick() == 4);
    CHECK(state.play_state() == EditorPlayState::paused);

    // paused -> playing RESUMES the same session (tick counter retained).
    CHECK(state.play().changed);
    CHECK(state.sim_tick() == 4);

    // stop discards the runtime state (L-51) — back to `edit` with the counter cleared.
    const PlayOutcome stopped = state.stop();
    CHECK(stopped.ok);
    CHECK(stopped.changed);
    CHECK(stopped.state == EditorPlayState::edit);
    CHECK(state.sim_tick() == 0);
}

void test_play_state_tokens_are_the_l51_indicator_vocabulary()
{
    // The tokens the `play-state` event carries — byte-identical to gui::playbar::state_token(), so
    // the L-51 indicator the playbar renders is fed straight off the topic (asserted against the
    // real playbar in the e08a integration drill, which links both libraries).
    CHECK(std::string(play_state_token(EditorPlayState::edit)) == "edit");
    CHECK(std::string(play_state_token(EditorPlayState::playing)) == "playing");
    CHECK(std::string(play_state_token(EditorPlayState::paused)) == "paused");
}

// --- persistence ----------------------------------------------------------------------------------

void test_persist_and_restore_round_trip()
{
    const fs::path project = make_temp_project("persist");

    EditorSessionState saved;
    saved.apply_selection(ids({"scene/root", "scene/root/child"}), SelectionMode::replace);
    Json transform = Json::object();
    transform.set("pos", number(7));
    saved.set_camera("main", transform, Json());
    saved.play(); // a LIVE play state, deliberately not persisted

    std::string error;
    CHECK(persist_session_state(project, saved, error));
    CHECK(error.empty());
    CHECK(fs::exists(session_state_path(project)));
    // The temp file never survives a successful write.
    CHECK(!fs::exists(project / ".editor" / "session.json.tmp"));

    EditorSessionState restored;
    const SessionRestoreReport report = restore_session_state(project, restored);
    CHECK(report.outcome == SessionRestoreOutcome::restored);
    CHECK(report.quarantined_path.empty());
    CHECK(restored.selection() == ids({"scene/root", "scene/root/child"}));
    CHECK(restored.cameras().size() == 1);
    CHECK(restored.cameras().at("main").transform.at("pos").as_number() == 7.0);
    // Play state is NOT persisted: a restarted daemon holds no live session, so reviving `playing`
    // would be a lie about L-51 provenance.
    CHECK(restored.play_state() == EditorPlayState::edit);
    CHECK(restored.sim_tick() == 0);

    // The persisted document is diffable: cameras are an ARRAY of objects carrying their key, never
    // a map-keyed object (the L-33 encoding discipline).
    const std::string text = read_text(session_state_path(project));
    CHECK(text.find("\"cameras\": [") != std::string::npos);
    CHECK(text.find("\"viewportId\": \"main\"") != std::string::npos);

    std::error_code ec;
    fs::remove_all(project, ec);
}

void test_missing_file_is_a_clean_fresh_boot()
{
    const fs::path project = make_temp_project("fresh");
    EditorSessionState state;
    const SessionRestoreReport report = restore_session_state(project, state);
    CHECK(report.outcome == SessionRestoreOutcome::fresh);
    CHECK(report.quarantined_path.empty());
    CHECK(report.detail.empty());
    CHECK(state.selection().empty());
    // A first boot must NOT mint the file — nothing has been persisted yet.
    CHECK(!fs::exists(session_state_path(project)));
    std::error_code ec;
    fs::remove_all(project, ec);
}

// The 07 §6 contract: corrupt => renamed aside + defaults loaded + reported LOUDLY, never blocking.
void assert_corrupt_recovery(const char* tag, const std::string& body)
{
    const fs::path project = make_temp_project(tag);
    write_session_file(project, body);

    EditorSessionState state;
    state.apply_selection(ids({"pre-existing"}), SelectionMode::replace);
    const SessionRestoreReport report = restore_session_state(project, state);

    CHECK(report.outcome == SessionRestoreOutcome::recovered);
    CHECK(!report.detail.empty());                    // the caller has something to SAY
    CHECK(!report.quarantined_path.empty());          // it was moved aside
    CHECK(fs::exists(fs::path(report.quarantined_path)));
    CHECK(read_text(fs::path(report.quarantined_path)) == body); // the evidence is preserved intact
    CHECK(!fs::exists(session_state_path(project)));   // ...and out of the daemon's way
    // Defaults are loaded: the pre-existing in-memory selection is NOT left half-applied.
    CHECK(state.selection() == ids({"pre-existing"}) || state.selection().empty());

    // A SECOND corrupt file recovers again, to a distinct quarantine name (no clobbering evidence).
    write_session_file(project, body);
    const SessionRestoreReport again = restore_session_state(project, state);
    CHECK(again.outcome == SessionRestoreOutcome::recovered);
    CHECK(!again.quarantined_path.empty());
    CHECK(again.quarantined_path != report.quarantined_path);

    std::error_code ec;
    fs::remove_all(project, ec);
}

void test_corrupt_file_recovery()
{
    assert_corrupt_recovery("corrupt-json", "{ this is not json ");
    assert_corrupt_recovery("corrupt-empty", "");
    assert_corrupt_recovery("corrupt-shape", "[1, 2, 3]");
    assert_corrupt_recovery("corrupt-selection", "{\"selection\": {\"ids\": [7]}}");
    assert_corrupt_recovery("corrupt-cameras", "{\"cameras\": {\"main\": {}}}");
    // A document from a FUTURE version is corrupt too: half-applying members we cannot read would be
    // worse than forgetting the selection.
    assert_corrupt_recovery("corrupt-version", "{\"version\": 99, \"selection\": {\"ids\": []}}");
}

void test_apply_json_tolerates_an_additive_document()
{
    // Forward tolerance WITHIN a known version: a missing optional section and an unknown member are
    // not corruption (the file is additive by design).
    EditorSessionState state;
    Json doc = Json::object();
    doc.set("version", Json(static_cast<std::int64_t>(1)));
    doc.set("somethingNewLater", Json(true));
    CHECK(state.apply_json(doc));
    CHECK(state.selection().empty());
    CHECK(state.cameras().empty());
}

void test_the_recovery_code_is_the_editor_domain_code()
{
    // C-F4: the editor session file's diagnostic is its OWN code, never the R-QA-005
    // `session.state_invalid` of the deterministic `session *` file-harness family.
    CHECK(std::string(kEditorSessionStateInvalidCode) == "editor.session_state_invalid");
}
} // namespace

int main()
{
    test_selection_modes();
    test_selection_mode_tokens_round_trip();
    test_cameras_are_opaque_and_change_detected();
    test_play_state_machine_mirrors_the_playbar();
    test_play_state_tokens_are_the_l51_indicator_vocabulary();
    test_persist_and_restore_round_trip();
    test_missing_file_is_a_clean_fresh_boot();
    test_corrupt_file_recovery();
    test_apply_json_tolerates_an_additive_document();
    test_the_recovery_code_is_the_editor_domain_code();
    EDITORKERNEL_TEST_MAIN_END();
}
