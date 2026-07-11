// Session undo/redo journal tests (M5-F7): gesture-batch checkpointing (L-20), undo/redo replayed as
// CAS-guarded override writes through the inspector gateway seam, the L-30 rebase-or-drop policy under
// a concurrent writer, the R-HUX-001 no-blind-clobber guard (a field a co-writer touched is DROPPED,
// never overwritten), and `.editor/session.json` JSON round-tripping. Happy + edge + failure (R-QA-013).

#include "context/editor/gui/session/undo/undo_journal.h"

#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_parse.h"

#include "undo_test.h"

#include <string>
#include <vector>

namespace undo = context::editor::gui::session::undo;
namespace ser = context::editor::serializer;

using undo::Checkpoint;
using undo::FieldEdit;
using undo::ReplayResult;
using undo::UndoJournal;
using undotest::FakeGateway;
using undotest::jnum;
using undotest::jstr;
using undotest::JsonValue;
using Status = ReplayResult::Status;

namespace
{

[[nodiscard]] FieldEdit make_edit(const std::string& pointer, JsonValue before, JsonValue after)
{
    FieldEdit e;
    e.root_scene = "root.scene.json";
    e.id_path = {"aaaaaaaaaaaaaaa1", "ccccccccccccccc1"};
    e.pointer = pointer;
    e.before = std::move(before);
    e.after = std::move(after);
    return e;
}

[[nodiscard]] bool value_equal(const JsonValue& a, const JsonValue& b)
{
    std::string sa;
    std::string sb;
    return ser::serialize_canonical(a, sa) && ser::serialize_canonical(b, sb) && sa == sb;
}

[[nodiscard]] std::string canonical(const JsonValue& v)
{
    std::string s;
    if (!ser::serialize_canonical(v, s))
    {
        s.clear();
    }
    return s;
}

} // namespace

int main()
{
    // --- pinned command / contribution identities ---------------------------------------------------
    CHECK(std::string(UndoJournal::kUndoCommand) == "session.undo");
    CHECK(std::string(UndoJournal::kRedoCommand) == "session.redo");
    CHECK(std::string(UndoJournal::kContributionId) == "builtin.session.undo");

    // --- recording: an empty checkpoint is not an undo step; a captured edit is ---------------------
    {
        UndoJournal journal;
        journal.record(Checkpoint{}); // empty -> ignored
        CHECK(!journal.can_undo());
        CHECK(journal.undo_depth() == 0);
        journal.capture(make_edit("/name", jstr("Old"), jstr("New")));
        CHECK(journal.can_undo());
        CHECK(journal.undo_depth() == 1);
        CHECK(!journal.can_redo());
    }

    // --- undo with no gateway is a no-op (none) -----------------------------------------------------
    {
        UndoJournal journal; // no gateway
        journal.capture(make_edit("/name", jstr("Old"), jstr("New")));
        const ReplayResult r = journal.undo();
        CHECK(r.status == Status::none);
        CHECK(!r.ok());
        CHECK(journal.can_undo()); // nothing was consumed
    }

    // --- undo/redo happy path: revert to `before`, then re-apply `after` (CAS-guarded) --------------
    {
        FakeGateway gw;
        gw.file_hash = 100;
        gw.field_values["/name"] = jstr("New"); // disk currently holds the edited (after) value

        UndoJournal journal(&gw);
        journal.capture(make_edit("/name", jstr("Old"), jstr("New")));

        const ReplayResult undone = journal.undo();
        CHECK(undone.status == Status::applied);
        CHECK(undone.ok());
        CHECK(undone.edits.size() == 1);
        CHECK(value_equal(gw.field_values["/name"], jstr("Old"))); // reverted on disk
        CHECK(!journal.can_undo());
        CHECK(journal.can_redo()); // the reverted checkpoint is redoable

        const ReplayResult redone = journal.redo();
        CHECK(redone.status == Status::applied);
        CHECK(redone.ok());
        CHECK(value_equal(gw.field_values["/name"], jstr("New"))); // re-applied
        CHECK(journal.can_undo());
        CHECK(!journal.can_redo());
    }

    // --- L-30 REBASE under undo: a concurrent writer touched an UNRELATED field ----------------------
    {
        FakeGateway gw;
        gw.file_hash = 100;
        gw.field_values["/components/camera/fov"] = jnum(2.0); // the after value on disk
        // Between the undo's read and its write, a co-writer advances the file + edits an UNRELATED field.
        gw.on_first_attempt = [&]() {
            gw.file_hash = 101;
            gw.field_values["/name"] = jstr("other");
        };

        UndoJournal journal(&gw);
        journal.capture(make_edit("/components/camera/fov", jnum(1.0), jnum(2.0)));

        const ReplayResult r = journal.undo();
        CHECK(r.status == Status::rebased); // unrelated field moved -> rebased onto the new state
        CHECK(r.ok());
        CHECK(gw.attempts == 2); // stale first attempt + the rebased attempt
        CHECK(value_equal(gw.field_values["/components/camera/fov"], jnum(1.0))); // reverted
        CHECK(journal.can_redo());
    }

    // --- R-HUX-001 NO BLIND CLOBBER: a co-writer changed THIS field -> undo DROPS, never overwrites --
    {
        FakeGateway gw;
        gw.file_hash = 105;
        gw.field_values["/name"] = jstr("Hijacked"); // a concurrent AI writer changed our field

        UndoJournal journal(&gw);
        journal.capture(make_edit("/name", jstr("Old"), jstr("New")));

        const ReplayResult r = journal.undo();
        CHECK(r.status == Status::dropped);
        CHECK(!r.ok());
        CHECK(r.edits.size() == 1);
        CHECK(r.edits[0].code == "cas.mismatch"); // reuses the existing catalog code (no new mint)
        CHECK(!r.edits[0].message.empty());        // the loud L-30 diagnostic
        CHECK(gw.attempts == 0);                   // NEVER a write attempt -> the co-writer is not clobbered
        CHECK(value_equal(gw.field_values["/name"], jstr("Hijacked"))); // untouched
        CHECK(!journal.can_undo());                // the dropped checkpoint was consumed
        CHECK(!journal.can_redo());                // a dropped undo is not redoable
    }

    // --- redo also refuses to clobber a co-writer (drops loudly) ------------------------------------
    {
        FakeGateway gw;
        gw.file_hash = 100;
        gw.field_values["/name"] = jstr("New");

        UndoJournal journal(&gw);
        journal.capture(make_edit("/name", jstr("Old"), jstr("New")));
        CHECK(journal.undo().ok()); // now disk holds "Old", checkpoint is on the redo stack

        // A co-writer changes the field before the redo runs.
        gw.field_values["/name"] = jstr("Hijacked");
        const ReplayResult r = journal.redo();
        CHECK(r.status == Status::dropped);
        CHECK(gw.attempts == 1); // only the undo's write ever landed; the redo never wrote
        CHECK(value_equal(gw.field_values["/name"], jstr("Hijacked")));
        CHECK(!journal.can_redo()); // consumed, not clobbered
    }

    // --- L-20 gesture batch: a multi-field gesture is ONE undo step; reverts every field ------------
    {
        FakeGateway gw;
        gw.file_hash = 100;
        gw.field_values["/a"] = jstr("A2");
        gw.field_values["/b"] = jstr("B2");

        UndoJournal journal(&gw);
        journal.begin_gesture("move+rename");
        journal.capture(make_edit("/a", jstr("A1"), jstr("A2")));
        journal.capture(make_edit("/b", jstr("B1"), jstr("B2")));
        journal.end_gesture();
        CHECK(journal.undo_depth() == 1); // ONE checkpoint for the whole gesture (not two)

        const ReplayResult r = journal.undo();
        CHECK(r.ok());
        CHECK(r.edits.size() == 2); // both fields reverted in one undo step
        CHECK(value_equal(gw.field_values["/a"], jstr("A1")));
        CHECK(value_equal(gw.field_values["/b"], jstr("B1")));
        CHECK(journal.undo_depth() == 0);
        CHECK(journal.redo_depth() == 1);
    }

    // --- an empty gesture batch records nothing -----------------------------------------------------
    {
        UndoJournal journal;
        journal.begin_gesture("noop");
        journal.end_gesture();
        CHECK(!journal.can_undo());
    }

    // --- recording a new gesture invalidates the redo future ----------------------------------------
    {
        FakeGateway gw;
        gw.file_hash = 100;
        gw.field_values["/name"] = jstr("New");

        UndoJournal journal(&gw);
        journal.capture(make_edit("/name", jstr("Old"), jstr("New")));
        CHECK(journal.undo().ok());
        CHECK(journal.can_redo());
        journal.capture(make_edit("/other", jstr("x"), jstr("y"))); // a new edit
        CHECK(!journal.can_redo()); // redo future dropped
    }

    // --- empty-stack undo / redo are no-ops (none) --------------------------------------------------
    {
        FakeGateway gw;
        UndoJournal journal(&gw);
        CHECK(journal.undo().status == Status::none);
        CHECK(journal.redo().status == Status::none);
    }

    // --- `.editor/session.json` round-trip: to_json -> canonical -> parse -> load_json --------------
    {
        UndoJournal journal;
        journal.begin_gesture("g1");
        journal.capture(make_edit("/a", jstr("A1"), jstr("A2")));
        journal.capture(make_edit("/b", jnum(1.0), jnum(2.0)));
        journal.end_gesture();
        journal.capture(make_edit("/c", jstr("C1"), jstr("C2")));

        const std::string serialized = canonical(journal.to_json());
        ser::ParseResult parsed = ser::parse_json(serialized);
        CHECK(parsed.ok);

        UndoJournal restored;
        CHECK(restored.load_json(parsed.root));
        CHECK(restored.undo_depth() == journal.undo_depth());
        CHECK(restored.redo_depth() == journal.redo_depth());
        // The re-serialized journal is byte-identical (values + addressing survived the round trip).
        CHECK(canonical(restored.to_json()) == serialized);
    }

    // --- a redo-populated journal round-trips both stacks -------------------------------------------
    {
        FakeGateway gw;
        gw.file_hash = 100;
        gw.field_values["/name"] = jstr("New");
        UndoJournal journal(&gw);
        journal.capture(make_edit("/name", jstr("Old"), jstr("New")));
        CHECK(journal.undo().ok()); // populates the redo stack

        const std::string serialized = canonical(journal.to_json());
        UndoJournal restored;
        CHECK(restored.load_json(ser::parse_json(serialized).root));
        CHECK(restored.undo_depth() == 0);
        CHECK(restored.redo_depth() == 1);
    }

    // --- load_json robustness: a malformed / wrong-shape tree leaves an EMPTY journal + returns false --
    {
        UndoJournal journal;
        journal.capture(make_edit("/name", jstr("Old"), jstr("New")));
        CHECK(journal.can_undo());

        CHECK(!journal.load_json(jstr("not an object"))); // wrong top type
        CHECK(!journal.can_undo());                       // left empty

        // An "undo" member that is present but not an array is malformed.
        JsonValue bad;
        bad.type = JsonValue::Type::object;
        bad.members.push_back({"undo", jstr("nope")});
        CHECK(!journal.load_json(bad));
        CHECK(!journal.can_undo());

        // An empty object is a valid empty journal.
        JsonValue empty;
        empty.type = JsonValue::Type::object;
        CHECK(journal.load_json(empty));
        CHECK(!journal.can_undo());
    }

    UNDO_TEST_MAIN_END();
}
