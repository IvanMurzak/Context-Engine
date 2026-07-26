// T1/T2 for the M9 e09c SESSION UNDO HOST (R-QA-013): the three halves undo_feed.h closes, each
// asserted rather than asserted-by-construction.
//
//   * RECORD  — a checkpoint reaches the journal and moves its depth + the dirty flag.
//   * REPLAY  — undo/redo actually re-issue writes through the BOUND gateway, and a field a
//               co-writer moved is DROPPED loudly with the value left untouched (R-HUX-001). Undo is
//               not a privileged path: it takes the same CAS + rebase-or-drop policy a live gesture
//               takes, over the same `commit_override_write` engine.
//   * PERSIST — the journal round-trips through a REAL `EditorStateStore` writing a REAL
//               `.editor/editor-state.json`, and — the DoD line that matters —
//               `test_the_journal_survives_an_editor_restart` tears EVERYTHING down (store, host,
//               feed, journal), rebuilds them from that file exactly as a fresh process would, and
//               proves Ctrl+Z STILL REVERTS THE PRE-RESTART EDIT. Nothing about that is structural:
//               it fails the moment the blob stops being written, stops being read, or stops being
//               replayable.
//
// THE GATEWAY DOUBLE MODELS THE PROJECT FILES, NOT THE WIRE. It holds the composed value + a CAS
// token and applies exactly the `attempt`/`read` contract `inspector_panel.h` publishes, so the L-30
// engine under test is the REAL one. The wire half is asserted where it belongs, in two halves:
// the FRAME in `test_wire_override_gateway.cpp` (`an_undo_replay_sends_the_same_edit_frame_a_gesture_does`
// — a real `UndoFeed` over the real gateway, and the outbound `edit` params it produces), and WHICH
// OBJECT it left through in `test_builtin_panels.cpp` (the composition root's own gateway INSTANCE,
// asserted through its per-instance counters — there is no second write path).
//
// Deliberately survives the restart: the double outlives the "process" in
// `test_the_journal_survives_an_editor_restart`, because the project's authored files outlive an
// editor restart too. A double that reset with the editor would make the restart assert vacuous.
//
// PLANTED-VIOLATION VERIFICATION (conventions.md — "a gate that cannot fail is worse than no gate").
// Every assertion family here, and in the sibling e09c cases in `test_inspector_feed.cpp` /
// `test_builtin_panels.cpp` / `test_wire_override_gateway.cpp` / `../../tests/test_editor_state.cpp`,
// was falsified by planting the corresponding defect and watching the NAMED test go RED — twenty
// plants across two rounds, each restored with a timestamp-INVALIDATING byte copy
// (`shutil.copyfile` + `os.utime(path, None)`, never `copy2`, or ninja skips the rebuild and the next
// plant silently runs against the previous plant's binary) and each round closed by a post-restore
// run that came back GREEN on a byte-exact tree (md5-verified against the out-of-tree backup):
//
//   * `EditorState::to_json` drops the `undo` blob / `from_json` never reads it back;
//   * `EditorStateStore::set_undo` never marks the store dirty (so nothing is ever flushed);
//   * `UndoFeed::bind_gateway` is a no-op (replay has no write path);
//   * `UndoFeed::load_blob` reports success without loading;
//   * `UndoFeed::record` does not dirty the journal (so nothing is ever persisted);
//   * the checkpoint is snapshotted AFTER `commit()` consumes the gesture (null/null pair);
//   * a loudly-DROPPED commit is journaled as an undo step;
//   * the composition root binds the journal to NO gateway;
//   * `publish_undo_state` never reaches the store / `restore_undo_state` never adopts the blob;
//   * `replay_edit` falls through to the `cas.mismatch` comparison for a field it could not READ;
//   * `undo()`/`redo()` CONSUME the checkpoint on an `error` aggregate that landed nothing;
//   * `UndoFeed::run_replay` dirties + touches unconditionally instead of on a depth delta;
//   * `take_replay_landed` is never raised (so the Inspector is never re-armed);
//   * the provider reports a DROPPED replay as `dispatched` (the pre-refine `status != none`);
//   * `pump_panel_feeds` never consumes the landed-replay flag;
//   * `restore_undo_state` claims success for a well-formed but EMPTY journal;
//   * `InspectorFeed::request_refresh` arms an EMPTY identity;
//   * an undo replay sends the `after` value (the wrong direction) on the wire.

#include "context/editor/shell/panels/undo_feed.h"

#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_parse.h"
#include "context/editor/shell/editor_state.h"

#include "panels_test.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using namespace context::editor;
namespace panels = context::editor::shell::panels;
namespace inspector = context::editor::gui::panels::inspector;
namespace undo = context::editor::gui::session::undo;
namespace serializer = context::editor::serializer;
namespace fs = std::filesystem;
using context::editor::contract::Json;
using context::editor::shell::EditorStateStore;
using context::editor::shell::PanelHost;

namespace
{

constexpr const char* kPanelId = "builtin.session.undo";
constexpr const char* kScene = "scenes/main.scene.json";
constexpr const char* kPointer = "/components/camera/fov";

// --- tiny fixtures ------------------------------------------------------------------------------
//
// `make_temp_project` / `cleanup` / `read_file` come from panels_test.h — the harness this whole
// directory shares — rather than being retyped here.
using panelstest::cleanup;
using panelstest::read_file;

[[nodiscard]] fs::path make_temp_project(const char* tag)
{
    return panelstest::make_temp_project("context-undo-feed", tag);
}

[[nodiscard]] serializer::JsonValue jnum(double value)
{
    serializer::JsonValue v;
    v.type = serializer::JsonValue::Type::number;
    v.number_value = value;
    return v;
}

[[nodiscard]] std::string canonical(const serializer::JsonValue& value)
{
    std::string out;
    return serializer::serialize_canonical(value, out) ? out : std::string("<uncanonical>");
}

// The double: one composed field per (root_scene, id_path, pointer), plus the file's CAS token.
class FieldStore final : public inspector::OverrideWriteGateway
{
public:
    inspector::WriteAttempt attempt(const inspector::OverrideWriteRequest& request,
                                    std::uint64_t expected_raw_hash) const override
    {
        ++attempts;
        inspector::WriteAttempt out;
        // 0 == "no guard", the seam's documented convention (project_override_gateway.h).
        if (expected_raw_hash != 0 && expected_raw_hash != raw_hash)
        {
            out.cas_mismatch = true;
            out.code = "cas.mismatch";
            out.message = "the file moved";
            out.raw_hash = raw_hash;
            return out;
        }
        values[key(request.root_scene, request.id_path, request.pointer)] = request.value;
        ++raw_hash;
        out.applied = true;
        out.file = request.root_scene;
        out.pointer = request.pointer;
        out.raw_hash = raw_hash;
        return out;
    }

    inspector::FieldState read(const std::string& root_scene,
                               const std::vector<std::string>& id_path,
                               const std::string& pointer) const override
    {
        ++reads;
        inspector::FieldState state;
        if (!readable)
        {
            // The default every real gateway returns when it cannot resolve the field — no daemon
            // connection, a refused `editor.inspect`, an entity that is gone. present:false, a null
            // value, a 0 token.
            return state;
        }
        state.raw_hash = raw_hash;
        const auto it = values.find(key(root_scene, id_path, pointer));
        state.present = it != values.end();
        if (state.present)
        {
            state.value = it->second;
        }
        return state;
    }

    // The composed field's starting value, THROUGH `key()` — so a test never hand-builds the
    // private encoding. A hand-built key that drifted from `key()` would seed an entry nothing
    // reads, and every assertion would then pass against an absent field rather than a real one.
    void seed(const serializer::JsonValue& value)
    {
        values[key(kScene, {"cam"}, kPointer)] = value;
    }

    // A CO-WRITER moving the field out from under us: the value changes AND the file's token
    // advances, exactly as another client's write would leave it.
    void external_write(const serializer::JsonValue& value)
    {
        values[key(kScene, {"cam"}, kPointer)] = value;
        ++raw_hash;
    }

    [[nodiscard]] std::string field() const
    {
        const auto it = values.find(key(kScene, {"cam"}, kPointer));
        return it == values.end() ? std::string("<absent>") : canonical(it->second);
    }

    mutable std::map<std::string, serializer::JsonValue> values;
    mutable bool readable = true; // false => every read reports the "could not read" default
    mutable std::uint64_t raw_hash = 100;
    mutable std::size_t attempts = 0;
    mutable std::size_t reads = 0;

private:
    [[nodiscard]] static std::string key(const std::string& root_scene,
                                         const std::vector<std::string>& id_path,
                                         const std::string& pointer)
    {
        std::string out = root_scene;
        for (const std::string& segment : id_path)
        {
            out += '/';
            out += segment;
        }
        out += '#';
        out += pointer;
        return out;
    }
};

// One reversible edit of the camera fov: `before` -> `after`, exactly the shape InspectorFeed's
// `snapshot_checkpoint` produces from a staged gesture.
[[nodiscard]] undo::FieldEdit fov_edit(double before, double after)
{
    undo::FieldEdit edit;
    edit.root_scene = kScene;
    edit.id_path = {"cam"};
    edit.pointer = kPointer;
    edit.before = jnum(before);
    edit.after = jnum(after);
    return edit;
}

// --- the persistence transport --------------------------------------------------------------

void test_the_blob_round_trips_the_journal()
{
    undo::UndoJournal source;
    source.capture(fov_edit(60.0, 75.0));
    source.capture(fov_edit(75.0, 90.0));
    CHECK(source.undo_depth() == 2u);

    const Json blob = panels::undo_journal_to_blob(source);
    CHECK(blob.is_string());
    CHECK(!blob.as_string().empty());
    // The blob is the journal's OWN canonical serialization, not a re-encoding: the pointer and both
    // values are in there verbatim, which is what makes it replayable in another process.
    CHECK(panelstest::mentions(blob.as_string(), kPointer));
    CHECK(panelstest::mentions(blob.as_string(), kScene));

    undo::UndoJournal restored;
    CHECK(panels::undo_journal_from_blob(blob, restored));
    CHECK(restored.undo_depth() == 2u);
    CHECK(restored.redo_depth() == 0u);
    // Byte-identical re-serialization: the round-trip lost nothing at all.
    CHECK(panels::undo_journal_to_blob(restored).as_string() == blob.as_string());
}

void test_a_corrupt_or_absent_blob_degrades_to_an_empty_journal()
{
    undo::UndoJournal journal;
    journal.capture(fov_edit(60.0, 75.0));
    CHECK(journal.can_undo());

    // Not a string at all (a hand-edited object) — refused, and the stale history is NOT left
    // behind: a journal that half-survived a corrupt load would replay edits against a file it can
    // no longer reason about.
    CHECK(!panels::undo_journal_from_blob(Json::object(), journal));
    CHECK(!journal.can_undo());

    journal.capture(fov_edit(60.0, 75.0));
    CHECK(!panels::undo_journal_from_blob(Json(std::string("{ not json")), journal));
    CHECK(!journal.can_undo());

    journal.capture(fov_edit(60.0, 75.0));
    CHECK(!panels::undo_journal_from_blob(Json(std::string("")), journal)); // fresh project
    CHECK(!journal.can_undo());

    // Well-formed JSON that is not a journal document: parsed, refused by load_json, stacks empty.
    journal.capture(fov_edit(60.0, 75.0));
    CHECK(!panels::undo_journal_from_blob(Json(std::string("[1,2,3]")), journal));
    CHECK(!journal.can_undo());
}

// --- record + replay --------------------------------------------------------------------------

void test_a_recorded_checkpoint_replays_through_the_bound_gateway()
{
    FieldStore store;
    store.seed(jnum(75.0)); // the committed state

    PanelHost host;
    panels::UndoFeed feed(host, kPanelId);
    feed.bind_gateway(&store);
    CHECK(feed.has_gateway());

    feed.record(fov_edit(60.0, 75.0));
    CHECK(feed.checkpoints_recorded() == 1u);
    CHECK(feed.journal().can_undo());
    CHECK(feed.dirty()); // the persisted blob is now stale

    const std::size_t attempts_before = store.attempts;
    const undo::ReplayResult undone = feed.replay_undo();
    CHECK(undone.ok());
    CHECK(feed.replays_run() == 1u);
    CHECK(feed.replay_drops() == 0u);
    // The REPLAY ACTUALLY WROTE — through the gateway, not by mutating a model in memory.
    CHECK(store.attempts > attempts_before);
    CHECK(store.field() == canonical(jnum(60.0)));
    CHECK(feed.journal().can_redo());
    CHECK(!feed.journal().can_undo());

    const undo::ReplayResult redone = feed.replay_redo();
    CHECK(redone.ok());
    CHECK(store.field() == canonical(jnum(75.0)));
    CHECK(feed.journal().can_undo());
    CHECK(!feed.journal().can_redo());
}

void test_a_replay_whose_field_a_co_writer_moved_drops_loudly()
{
    // THE R-HUX-001 GUARANTEE. "Restore the previous bytes" is exactly what undo must NOT do.
    FieldStore store;
    store.seed(jnum(75.0));

    PanelHost host;
    panels::UndoFeed feed(host, kPanelId);
    feed.bind_gateway(&store);
    feed.record(fov_edit(60.0, 75.0));

    store.external_write(jnum(120.0)); // another human or agent set the same field

    const undo::ReplayResult result = feed.replay_undo();
    CHECK(!result.ok());
    CHECK(result.status == inspector::CommitResult::Status::dropped);
    CHECK(feed.replay_drops() == 1u);
    // The co-writer's value SURVIVES — the undo refused rather than overwriting it.
    CHECK(store.field() == canonical(jnum(120.0)));
    // A dropped checkpoint is consumed rather than re-offered: its field can never be reverted now,
    // so leaving it would hand the human an undo guaranteed to refuse again.
    CHECK(!feed.journal().can_undo());
    CHECK(!feed.journal().can_redo());
    CHECK(feed.dirty()); // the stacks moved, so the persisted blob is stale even on a drop
    CHECK(!result.edits.empty() && result.edits.front().code == "cas.mismatch");
}

// ------------------------------------------------- M9 e09b-3: the LOUD notice sink (design 05 §8)
//
// A REPLAY IS A WRITE, so its refusals are as loud as a gesture's — and arguably louder: the human
// pressed Ctrl+Z, and an undo that silently did nothing reads as an undo that worked while the file
// still holds the value they were trying to revert. These four cases pin what reaches the sink, what
// does NOT, and that the two non-landing outcomes stay distinguishable on the way out.

// One capture of what the sink was handed.
struct CapturedNotice
{
    std::string verb;
    inspector::CommitResult::Status status = inspector::CommitResult::Status::none;
    std::string code;
};

// ONE capture shape, shared by the cases below. Written once because the two hand-rolled copies it
// replaces had ALREADY drifted apart: only one of them filled `code`, so the same field meant "the
// failing edit's code" in one case and "always empty" in the other — and an assertion later added to
// the wrong one would have read as a product bug rather than as a test that never captured the value.
// (The anti-vacuity control below deliberately uses a bare counter instead; it wants no capture.)
[[nodiscard]] auto capture_into(std::vector<CapturedNotice>& out)
{
    return [&out](const char* verb, const undo::ReplayResult& result)
    {
        CapturedNotice captured;
        captured.verb = verb;
        captured.status = result.status;
        captured.code = result.edits.empty() ? std::string{} : result.edits.front().code;
        out.push_back(std::move(captured));
    };
}

void test_a_dropped_replay_is_handed_to_the_notice_sink()
{
    FieldStore store;
    store.seed(jnum(75.0));

    PanelHost host;
    panels::UndoFeed feed(host, kPanelId);
    feed.bind_gateway(&store);

    std::vector<CapturedNotice> notices;
    feed.bind_notice_sink(capture_into(notices));
    CHECK(feed.has_notice_sink());

    feed.record(fov_edit(60.0, 75.0));
    store.external_write(jnum(120.0)); // a co-writer takes the field

    CHECK(feed.replay_undo().status == inspector::CommitResult::Status::dropped);
    CHECK(feed.notices_sent() == 1u);
    // GUARDED: CHECK records a failure but does NOT abort (panels_test.h), so indexing straight after
    // the size assertion is out of bounds on exactly the run that assertion exists to catch — UB, and
    // on the ASan leg a heap-buffer-overflow report instead of a legible failure.
    CHECK(notices.size() == 1u);
    if (notices.size() == 1u)
    {
        // The VERB is what the human is told they were doing. "undo" and "redo" fail differently only
        // in the message, so a sink that reported the wrong one would produce a correct-looking toast
        // about an action the human did not take.
        CHECK(notices[0].verb == "undo");
        CHECK(notices[0].status == inspector::CommitResult::Status::dropped);
        CHECK(notices[0].code == "cas.mismatch");
    }
}

void test_a_refused_replay_reaches_the_sink_as_a_refusal_not_a_drop()
{
    FieldStore store;
    store.seed(jnum(75.0));
    store.readable = false; // the write path cannot even read the field

    PanelHost host;
    panels::UndoFeed feed(host, kPanelId);
    feed.bind_gateway(&store);

    std::vector<CapturedNotice> notices;
    feed.bind_notice_sink(capture_into(notices));

    feed.record(fov_edit(60.0, 75.0));
    CHECK(feed.replay_undo().status == inspector::CommitResult::Status::error);
    CHECK(feed.notices_sent() == 1u);
    CHECK(notices.size() == 1u); // guarded below — CHECK records, it does not abort
    if (notices.size() == 1u)
    {
        // DISTINGUISHABLE from a drop, which is the whole reason the sink carries the status: a
        // refusal KEPT the step (retry when the daemon is back) while a drop consumed it. Telling the
        // human the wrong one sends them looking for a co-writer who does not exist.
        CHECK(notices[0].status == inspector::CommitResult::Status::error);
        CHECK(notices[0].status != inspector::CommitResult::Status::dropped);
    }
}

// THE ANTI-VACUITY CONTROL. A sink that fired on every replay would satisfy both cases above while
// toasting the human on every successful Ctrl+Z — noise, and noise is exactly how the drop that
// matters goes unread.
void test_a_landed_replay_produces_no_notice()
{
    FieldStore store;
    store.seed(jnum(75.0));

    PanelHost host;
    panels::UndoFeed feed(host, kPanelId);
    feed.bind_gateway(&store);

    std::size_t calls = 0;
    feed.bind_notice_sink([&calls](const char*, const undo::ReplayResult&) { ++calls; });

    feed.record(fov_edit(60.0, 75.0));
    CHECK(feed.replay_undo().ok()); // it LANDED
    CHECK(feed.replay_redo().ok()); // and so did the redo
    CHECK(calls == 0u);
    CHECK(feed.notices_sent() == 0u);

    // A replay that never RAN (nothing to undo) is not a notice either — there was no refusal to
    // report, and telling the human "your undo was refused" when they had nothing to undo is a lie.
    CHECK(feed.replay_undo().ok()); // undo the redo, so the stack is walkable
    while (feed.journal().can_undo())
    {
        (void)feed.replay_undo();
    }
    const undo::ReplayResult none = feed.replay_undo();
    CHECK(none.status == inspector::CommitResult::Status::none);
    CHECK(calls == 0u);
}

void test_an_unbound_notice_sink_leaves_the_replay_accounting_intact()
{
    FieldStore store;
    store.seed(jnum(75.0));

    PanelHost host;
    panels::UndoFeed feed(host, kPanelId);
    feed.bind_gateway(&store);
    CHECK(!feed.has_notice_sink());

    feed.record(fov_edit(60.0, 75.0));
    store.external_write(jnum(120.0));

    CHECK(feed.replay_undo().status == inspector::CommitResult::Status::dropped);
    CHECK(feed.replay_drops() == 1u);
    // Counted APART from the drop, so "the human was told" and "a drop happened" are two facts a
    // regression cannot collapse into one.
    CHECK(feed.notices_sent() == 0u);
}

void test_an_unbound_feed_replays_nothing_rather_than_pretending()
{
    PanelHost host;
    panels::UndoFeed feed(host, kPanelId);
    CHECK(!feed.has_gateway());
    feed.record(fov_edit(60.0, 75.0));

    const undo::ReplayResult result = feed.replay_undo();
    CHECK(result.status == inspector::CommitResult::Status::none);
    CHECK(feed.replays_run() == 0u);
    // The checkpoint is still there — nothing was consumed by a replay that never happened.
    CHECK(feed.journal().can_undo());
}

void test_a_redo_whose_field_a_co_writer_moved_drops_loudly_too()
{
    // `replay_redo` is `replay_undo`'s twin, and an untested twin is where a copy-paste divergence
    // hides. The scenario is ordinary: an undo lands, a co-writer then touches the field, and the
    // redo must refuse exactly as the undo would have.
    FieldStore store;
    store.seed(jnum(75.0));

    PanelHost host;
    panels::UndoFeed feed(host, kPanelId);
    feed.bind_gateway(&store);
    feed.record(fov_edit(60.0, 75.0));
    CHECK(feed.replay_undo().ok()); // disk now holds 60; the step is on the redo stack
    CHECK(feed.journal().can_redo());
    feed.mark_clean();

    store.external_write(jnum(120.0)); // another writer takes the field

    const undo::ReplayResult result = feed.replay_redo();
    CHECK(result.status == inspector::CommitResult::Status::dropped);
    CHECK(feed.replay_drops() == 1u);
    CHECK(feed.replays_run() == 2u);
    CHECK(store.field() == canonical(jnum(120.0))); // the co-writer's value SURVIVES
    CHECK(!feed.journal().can_redo());              // consumed, like a dropped undo
    CHECK(feed.dirty());                            // the stacks moved, so the blob is stale
}

void test_a_replay_the_write_path_refuses_keeps_the_step_and_dirties_nothing()
{
    // THE DURABILITY HALF. A refusal is not a drop: nothing was written, so the human's step must
    // survive — and because the journal did not move, the persisted blob must NOT be marked stale
    // (a byte-identical rewrite of the session file every frame the daemon is down) and the panel
    // must NOT be re-rendered at an unchanged depth.
    FieldStore store;
    store.seed(jnum(75.0));
    store.readable = false; // the field cannot be read at all

    PanelHost host;
    panels::UndoFeed feed(host, kPanelId);
    CHECK(host.provide(kPanelId, feed.make_provider()));
    feed.bind_gateway(&store);
    feed.record(fov_edit(60.0, 75.0));
    feed.mark_clean();
    const std::uint64_t revision_before = host.revision(kPanelId);

    const undo::ReplayResult result = feed.replay_undo();
    CHECK(result.status == inspector::CommitResult::Status::error);
    CHECK(feed.replay_refusals() == 1u);
    CHECK(feed.replay_drops() == 0u);
    CHECK(store.attempts == 0u);   // never written
    CHECK(feed.journal().can_undo()); // KEPT
    CHECK(!feed.dirty());             // nothing moved -> nothing to re-persist
    CHECK(host.revision(kPanelId) == revision_before); // and nothing to re-render
    CHECK(!feed.take_replay_landed());                 // and certainly nothing to re-read

    // Reachable again: the SAME step still works.
    store.readable = true;
    CHECK(feed.replay_undo().ok());
    CHECK(store.field() == canonical(jnum(60.0)));
}

void test_a_landed_replay_raises_the_read_your_replays_flag_once()
{
    // A replay writes the field through the gateway DIRECTLY, so `InspectorPanel::commit` never runs
    // and the Inspector's own commit listener — which is what re-arms its re-read — never fires.
    // `pump_panel_feeds` consumes THIS flag instead. Without it the panel keeps rendering the
    // pre-undo value AND the pre-undo CAS token, so the human's next edit to that field is dropped
    // as a "concurrent writer" that is really their own Ctrl+Z.
    FieldStore store;
    store.seed(jnum(75.0));

    PanelHost host;
    panels::UndoFeed feed(host, kPanelId);
    feed.bind_gateway(&store);
    feed.record(fov_edit(60.0, 75.0));
    CHECK(!feed.take_replay_landed()); // recording is not replaying

    CHECK(feed.replay_undo().ok());
    CHECK(feed.take_replay_landed());  // raised…
    CHECK(!feed.take_replay_landed()); // …and CONSUMED, so one landed replay arms exactly one fetch

    // A DROPPED replay wrote nothing, so there is nothing to read back.
    feed.record(fov_edit(10.0, 60.0));
    store.external_write(jnum(120.0));
    CHECK(feed.replay_undo().status == inspector::CommitResult::Status::dropped);
    CHECK(!feed.take_replay_landed());
}

void test_the_touch_seam_repaints_the_surface_on_every_move()
{
    // Every mutator claims to touch the panel so the rendered depth keeps up. Asserted the way the
    // sibling feeds assert it (test_problems_feed.cpp), including the NEGATIVE case above.
    FieldStore store;
    store.seed(jnum(75.0));

    PanelHost host;
    panels::UndoFeed feed(host, kPanelId);
    CHECK(host.provide(kPanelId, feed.make_provider()));
    feed.bind_gateway(&store);

    const std::uint64_t before_record = host.revision(kPanelId);
    feed.record(fov_edit(60.0, 75.0));
    CHECK(host.revision(kPanelId) > before_record);

    const std::uint64_t before_undo = host.revision(kPanelId);
    CHECK(feed.replay_undo().ok());
    CHECK(host.revision(kPanelId) > before_undo);

    const std::uint64_t before_redo = host.revision(kPanelId);
    CHECK(feed.replay_redo().ok());
    CHECK(host.revision(kPanelId) > before_redo);

    const std::uint64_t before_load = host.revision(kPanelId);
    CHECK(!feed.load_blob(Json(std::string{}))); // an absent blob still re-renders (now empty)
    CHECK(host.revision(kPanelId) > before_load);
}

// --- the panel surface --------------------------------------------------------------------------

void test_the_provider_dispatches_the_two_commands()
{
    FieldStore store;
    store.seed(jnum(75.0));

    PanelHost host;
    panels::UndoFeed feed(host, kPanelId);
    feed.bind_gateway(&store);
    const shell::PanelProvider provider = feed.make_provider();
    CHECK(static_cast<bool>(provider.build));
    CHECK(static_cast<bool>(provider.invoke));
    // No gesture and no D6 state pair: the journal's state is the Shell's, persisted through the
    // store, never through the panel-state channel editor-core owns.
    CHECK(!static_cast<bool>(provider.gesture));
    CHECK(!static_cast<bool>(provider.get_state));

    // Nothing to undo yet -> the command is DECLINED, not silently swallowed.
    CHECK(!provider.invoke(undo::UndoJournal::kUndoCommand, Json::object()));
    CHECK(!provider.invoke("not.a.command", Json::object()));

    feed.record(fov_edit(60.0, 75.0));
    CHECK(provider.invoke(undo::UndoJournal::kUndoCommand, Json::object()));
    CHECK(store.field() == canonical(jnum(60.0)));
    CHECK(provider.invoke(undo::UndoJournal::kRedoCommand, Json::object()));
    CHECK(store.field() == canonical(jnum(75.0)));

    // The rendered surface reflects the live depth — the panel is a real projection of the journal,
    // not a placeholder.
    const gui::uitree::Panel panel = provider.build();
    CHECK(panelstest::mentions(gui::uitree::render_html(panel), "1 undoable"));

    // AND THE HONEST VERDICT. `invoke` answers `dispatched` iff the replay LANDED a write, because
    // that bit is what the palette shows the human as success (boot.ts § sessionActions). Every
    // assertion above passes identically under the older `status != none` spelling — `none` is false
    // and `applied` is true either way — so a DROPPED replay is the only case that tells them apart,
    // and without it the whole honest-verdict property is untested.
    store.external_write(jnum(999.0)); // a co-writer takes the field
    CHECK(!provider.invoke(undo::UndoJournal::kUndoCommand, Json::object()));
    CHECK(feed.replay_drops() == 1u);                // it DID run and DID refuse…
    CHECK(store.field() == canonical(jnum(999.0)));  // …leaving the co-writer's value untouched
}

// --- persistence through the ONE Shell-side seam ------------------------------------------------

void test_the_journal_reaches_the_editor_state_file_through_the_store()
{
    const fs::path root = make_temp_project("store");
    FieldStore store;
    store.seed(jnum(75.0));

    PanelHost host;
    panels::UndoFeed feed(host, kPanelId);
    feed.bind_gateway(&store);
    feed.record(fov_edit(60.0, 75.0));

    EditorStateStore state_store(root, 0);
    state_store.load();
    state_store.set_undo(feed.to_blob(), 0);
    CHECK(state_store.dirty());
    CHECK(state_store.flush_now());

    // It is IN THE EDITOR-OWNED FILE — not a sibling file the journal opened itself.
    const fs::path path = context::editor::shell::editor_state_path(root);
    CHECK(fs::exists(path));
    const std::string text = read_file(path);
    CHECK(panelstest::mentions(text, kPointer));
    CHECK(panelstest::mentions(text, "\"undo\""));
    // And NOT in the daemon's file — the C-F3 ownership split, asserted rather than assumed.
    CHECK(!fs::exists(root / ".editor" / "session.json"));

    // Re-offering an UNMOVED journal does not dirty the store: the owner loop calls this every frame.
    state_store.set_undo(feed.to_blob(), 0);
    CHECK(!state_store.dirty());

    cleanup(root);
}

void test_the_journal_survives_an_editor_restart()
{
    // ⭐ THE e09c DoD LINE: the journal persists to editor-state.json AND restores across a restart,
    // and undo still works afterwards.
    const fs::path root = make_temp_project("restart");
    // The project's authored files outlive the editor, so the double does too (see the file header).
    FieldStore project;
    project.values[std::string(kScene) + "/cam#" + kPointer] = jnum(75.0);

    // ---- session 1: a gesture committed (fov 60 -> 75) and the journal recorded it ----
    {
        PanelHost host;
        panels::UndoFeed feed(host, kPanelId);
        feed.bind_gateway(&project);
        feed.record(fov_edit(60.0, 75.0));
        CHECK(feed.dirty());

        EditorStateStore state_store(root, 0);
        state_store.load();
        state_store.set_undo(feed.to_blob(), 0);
        CHECK(state_store.flush_now());
    }
    // Everything the editor process held is now gone: the host, the feed, the journal, the store.

    // ---- session 2: a FRESH process boots on the same project ----
    {
        EditorStateStore state_store(root, 0);
        bool loaded_existing = false;
        state_store.load(&loaded_existing);
        CHECK(loaded_existing);
        CHECK(state_store.schema_diagnostic().empty());

        PanelHost host;
        panels::UndoFeed feed(host, kPanelId);
        feed.bind_gateway(&project);
        // Nothing yet — this is a brand-new journal, exactly as a fresh process starts.
        CHECK(!feed.journal().can_undo());

        CHECK(feed.load_blob(state_store.state().undo));
        CHECK(feed.journal().can_undo());
        CHECK(feed.journal().undo_depth() == 1u);
        // A just-loaded feed is CLEAN: re-publishing what we read back would dirty the store on
        // every boot for no change.
        CHECK(!feed.dirty());

        // AND UNDO STILL WORKS: it reverts the edit made BEFORE the restart, over the same wire
        // write path a live gesture takes.
        const undo::ReplayResult result = feed.replay_undo();
        CHECK(result.ok());
        CHECK(project.field() == canonical(jnum(60.0)));
        CHECK(feed.journal().can_redo());
    }

    cleanup(root);
}

void test_a_restart_from_a_corrupt_blob_boots_with_an_empty_history()
{
    // Honest degradation (07 §6): a corrupt session file costs the undo history, never the boot.
    const fs::path root = make_temp_project("corrupt");
    {
        EditorStateStore state_store(root, 0);
        state_store.load();
        state_store.set_undo(Json(std::string("{\"version\": 1, \"undo\": trunc")), 0);
        CHECK(state_store.flush_now());
    }

    EditorStateStore state_store(root, 0);
    state_store.load();
    PanelHost host;
    panels::UndoFeed feed(host, kPanelId);
    // DIRTY FIRST, so the postcondition below is a real transition rather than a restatement of the
    // constructor: `load_blob` promises the feed is CLEAN afterwards (it now matches disk), and a
    // virgin feed is already clean, which would make the assertion pass with the line deleted.
    feed.record(fov_edit(60.0, 75.0));
    CHECK(feed.dirty());
    CHECK(!feed.load_blob(state_store.state().undo)); // refused...
    CHECK(!feed.journal().can_undo());                // ...and empty, not half-loaded
    CHECK(!feed.dirty());

    cleanup(root);
}

} // namespace

int main()
{
    test_the_blob_round_trips_the_journal();
    test_a_corrupt_or_absent_blob_degrades_to_an_empty_journal();
    test_a_recorded_checkpoint_replays_through_the_bound_gateway();
    test_a_replay_whose_field_a_co_writer_moved_drops_loudly();
    test_a_redo_whose_field_a_co_writer_moved_drops_loudly_too();
    test_a_replay_the_write_path_refuses_keeps_the_step_and_dirties_nothing();
    test_a_dropped_replay_is_handed_to_the_notice_sink();
    test_a_refused_replay_reaches_the_sink_as_a_refusal_not_a_drop();
    test_a_landed_replay_produces_no_notice();
    test_an_unbound_notice_sink_leaves_the_replay_accounting_intact();
    test_a_landed_replay_raises_the_read_your_replays_flag_once();
    test_the_touch_seam_repaints_the_surface_on_every_move();
    test_an_unbound_feed_replays_nothing_rather_than_pretending();
    test_the_provider_dispatches_the_two_commands();
    test_the_journal_reaches_the_editor_state_file_through_the_store();
    test_the_journal_survives_an_editor_restart();
    test_a_restart_from_a_corrupt_blob_boots_with_an_empty_history();
    PANELS_TEST_MAIN_END();
}
