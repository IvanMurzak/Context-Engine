// Session undo/redo journal tests (M5-F7): gesture-batch checkpointing (L-20), undo/redo replayed as
// CAS-guarded override writes through the inspector gateway seam, the L-30 rebase-or-drop policy under
// a concurrent writer, the R-HUX-001 no-blind-clobber guard (a field a co-writer touched is DROPPED,
// never overwritten), and the host-persisted JSON round-trip. Since M9 e2 it also covers the FILE
// edit atom: undo of a delete RESTORES, redo re-deletes and adopts the fresh handle, a refused
// replay KEEPS the step, and the JSON round-trip carries file steps (including a pre-e2 journal
// that has none). Happy + edge + failure (R-QA-013).

#include "context/editor/gui/session/undo/undo_journal.h"

#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_parse.h"

#include "undo_test.h"

#include <string>
#include <vector>

namespace undo = context::editor::gui::session::undo;
namespace ser = context::editor::serializer;
namespace uitree = context::editor::gui::uitree;

using undo::Checkpoint;
using undo::FieldEdit;
using undo::FileEdit;
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

// The FILE write double. Deliberately no more capable than the real write path: it records what was
// asked, answers what the daemon would, and never reaches into the journal.
class FakeFileGateway final : public context::editor::gui::panels::files::FileWriteGateway
{
public:
    using Result = context::editor::gui::panels::files::FileWriteResult;

    struct Call
    {
        std::string verb;
        std::string a;
        std::string b;
    };

    std::vector<Call> calls;
    bool refuse = false;
    std::string token = "00000000000000000000000000000aaa";

    Result move_file(const std::string& from, const std::string& to) override
    {
        calls.push_back({"move", from, to});
        return answer(from, to, "");
    }
    Result delete_file(const std::string& path) override
    {
        calls.push_back({"delete", path, ""});
        return answer(path, "", token);
    }
    Result restore_file(const std::string& restore_token) override
    {
        calls.push_back({"restore", restore_token, ""});
        return answer("", "", restore_token);
    }

private:
    [[nodiscard]] Result answer(const std::string& path, const std::string& other,
                                const std::string& tok) const
    {
        Result out;
        if (refuse)
        {
            out.status = Result::Status::refused;
            out.code = "asset.restore_destination_exists";
            out.message = "a different file now occupies that path";
            out.path = path;
            return out;
        }
        out.status = Result::Status::applied;
        out.path = path;
        out.other_path = other;
        out.restore_token = tok;
        return out;
    }
};

[[nodiscard]] FileEdit make_delete(const std::string& path, const std::string& token)
{
    FileEdit e;
    e.op = FileEdit::Op::remove;
    e.from = path;
    e.restore_token = token;
    return e;
}

[[nodiscard]] FileEdit make_move(const std::string& from, const std::string& to)
{
    FileEdit e;
    e.op = FileEdit::Op::move;
    e.from = from;
    e.to = to;
    return e;
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

    // --- AN UNREADABLE FIELD IS A REFUSAL, NOT A FABRICATED COLLISION -------------------------------
    // The up-front guard READS before it attempts, so it is the one place a failed read can be
    // mistaken for a moved value. Reachable only since M9 e09c gave the journal a host: on the
    // GESTURE path `attempt` refuses first, which is why `wire_override_gateway.h` § LIFETIME says
    // the ATTEMPT refusal is what fails THAT path closed. Getting this wrong invents a concurrent
    // writer out of a dropped daemon connection — and, because a DROP is consumed, silently costs
    // the human their undo step over an outage.
    {
        FakeGateway gw;
        gw.field_values["/name"] = jstr("New");
        gw.readable = false; // no connection / refused read / the entity no longer resolves

        UndoJournal journal(&gw);
        journal.capture(make_edit("/name", jstr("Old"), jstr("New")));

        const ReplayResult r = journal.undo();
        CHECK(r.status == Status::error); // NOT dropped — nothing was observed to have changed
        CHECK(!r.ok());
        // NOT `cas.mismatch`: naming the read-unavailable code IS the fabrication guard.
        CHECK(r.edits.size() == 1);
        CHECK(r.edits.size() == 1 &&
              r.edits[0].code == std::string(UndoJournal::kReadUnavailableCode));
        CHECK(r.edits.size() == 1 && !r.edits[0].message.empty());
        CHECK(gw.attempts == 0); // a field we cannot read is a field we must not overwrite
        CHECK(value_equal(gw.field_values["/name"], jstr("New"))); // untouched
        // AND THE STEP SURVIVES. This is the half that makes the journal durable rather than
        // merely persistent: a transient outage must not eat the user's history.
        CHECK(journal.can_undo());
        CHECK(journal.undo_depth() == 1);
        CHECK(!journal.can_redo()); // it did not cross to the redo stack either

        // Once the project is reachable again the SAME step still works.
        gw.readable = true;
        const ReplayResult retried = journal.undo();
        CHECK(retried.ok());
        CHECK(value_equal(gw.field_values["/name"], jstr("Old")));
        CHECK(!journal.can_undo());
        CHECK(journal.can_redo());
    }

    // --- a WRITE-PATH refusal (not a CAS event) also keeps the step, from the other direction -------
    {
        FakeGateway gw;
        gw.field_values["/name"] = jstr("New");
        gw.refuse_write_code = "shell.no_daemon"; // the read succeeds; the WRITE is refused

        UndoJournal journal(&gw);
        journal.capture(make_edit("/name", jstr("Old"), jstr("New")));

        const ReplayResult r = journal.undo();
        CHECK(r.status == Status::error);
        CHECK(r.edits.size() == 1 && r.edits[0].code == "shell.no_daemon");
        CHECK(gw.attempts == 1);                                   // it DID try
        CHECK(value_equal(gw.field_values["/name"], jstr("New"))); // and wrote nothing
        CHECK(journal.can_undo());                                 // so the step is kept
        CHECK(!journal.can_redo());
    }

    // --- a REDO refused the same way is kept on the REDO stack, not silently migrated ---------------
    {
        FakeGateway gw;
        gw.field_values["/name"] = jstr("New");

        UndoJournal journal(&gw);
        journal.capture(make_edit("/name", jstr("Old"), jstr("New")));
        CHECK(journal.undo().ok()); // the checkpoint is now on the redo stack
        CHECK(journal.can_redo());

        gw.readable = false;
        const ReplayResult r = journal.redo();
        CHECK(r.status == Status::error);
        CHECK(journal.can_redo()); // KEPT where it was…
        CHECK(journal.redo_depth() == 1);
        CHECK(!journal.can_undo()); // …and NOT moved to the other stack
        CHECK(value_equal(gw.field_values["/name"], jstr("Old")));
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

    // --- host-persisted round-trip: to_json -> canonical -> parse -> load_json ----------------------
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


    // ============================ M9 e2: the FILE edit atom ======================================

    // --- undo of a DELETE restores; redo re-deletes -------------------------------------------------
    {
        FakeFileGateway files;
        UndoJournal journal;
        journal.set_file_gateway(&files);
        journal.capture_file(make_delete("art/hero.png", "tok-1"), "delete art/hero.png");
        CHECK(journal.can_undo());
        CHECK(journal.undo_depth() == 1);

        const ReplayResult undone = journal.undo();
        CHECK(undone.status == Status::applied);
        CHECK(files.calls.size() == 1);
        CHECK(files.calls[0].verb == "restore");
        // Through the TOKEN — the undo restores the quarantined bytes, it does not re-write bytes
        // the journal remembered (a "restore previous bytes" undo is what R-HUX-001 forbids).
        CHECK(files.calls[0].a == "tok-1");
        CHECK(journal.can_redo());
        CHECK(!journal.can_undo());

        // REDO re-deletes, and ADOPTS the fresh handle the write path returned. Without that, the
        // next undo of this step would try to restore through a token nothing is filed under.
        const ReplayResult redone = journal.redo();
        CHECK(redone.status == Status::applied);
        CHECK(files.calls.size() == 2);
        CHECK(files.calls[1].verb == "delete");
        CHECK(files.calls[1].a == "art/hero.png");
        CHECK(journal.can_undo());

        files.calls.clear();
        CHECK(journal.undo().status == Status::applied);
        CHECK(files.calls.size() == 1);
        CHECK(files.calls[0].a == files.token); // the REFRESHED token, not the stale "tok-1"
    }

    // --- undo of a MOVE moves it back; redo re-applies the original direction -----------------------
    {
        FakeFileGateway files;
        UndoJournal journal;
        journal.set_file_gateway(&files);
        journal.capture_file(make_move("art/hero.png", "art/villain.png"), "rename art/hero.png");

        CHECK(journal.undo().status == Status::applied);
        CHECK(files.calls[0].verb == "move");
        CHECK(files.calls[0].a == "art/villain.png"); // BACK
        CHECK(files.calls[0].b == "art/hero.png");

        CHECK(journal.redo().status == Status::applied);
        CHECK(files.calls[1].a == "art/hero.png"); // FORWARD again
        CHECK(files.calls[1].b == "art/villain.png");
    }

    // --- a REFUSED replay wrote nothing, so the step is KEPT ----------------------------------------
    {
        FakeFileGateway files;
        files.refuse = true;
        UndoJournal journal;
        journal.set_file_gateway(&files);
        journal.capture_file(make_delete("art/hero.png", "tok-1"), "delete art/hero.png");

        const ReplayResult refused = journal.undo();
        CHECK(refused.status == Status::error);
        CHECK(!refused.ok());
        CHECK(refused.edits.size() == 1);
        CHECK(refused.edits[0].code == "asset.restore_destination_exists");
        CHECK(!refused.edits[0].message.empty());
        // The path travels in `file`; `pointer` stays empty because a file operation has no field.
        CHECK(refused.edits[0].file == "art/hero.png");
        CHECK(refused.edits[0].pointer.empty());
        // KEPT — a refusal costs the human nothing, exactly as for a refused field write.
        CHECK(journal.can_undo());
        CHECK(!journal.can_redo());

        // The producible sibling: stop refusing and the SAME step replays.
        files.refuse = false;
        CHECK(journal.undo().status == Status::applied);
        CHECK(!journal.can_undo());
    }

    // --- NO file gateway: `none`, and the step is kept (never silently consumed) ---------------------
    {
        UndoJournal journal; // neither gateway bound
        journal.capture_file(make_delete("art/hero.png", "tok-1"), "delete art/hero.png");
        const ReplayResult r = journal.undo();
        CHECK(r.status == Status::none);
        CHECK(journal.can_undo());

        // Sibling: bind the gateway and the same step replays. This pair is what proves the `none`
        // above is the guard doing its job rather than a journal that lost the step.
        FakeFileGateway files;
        journal.set_file_gateway(&files);
        CHECK(journal.undo().status == Status::applied);
    }

    // --- a file step is VISIBLE in Session History (the DoD's own words) ------------------------------
    {
        FakeFileGateway files;
        UndoJournal journal;
        journal.set_file_gateway(&files);
        journal.capture_file(make_delete("art/hero.png", "tok-1"), "delete art/hero.png");

        const std::string html = uitree::render_html(journal.build_panel());
        CHECK(html.find("1 undoable") != std::string::npos);
        // NAMED, not just counted: "1 undoable" is indistinguishable between a field edit and the
        // deletion of the human's file, which is precisely the step they most need to recognise.
        CHECK(html.find("delete art/hero.png") != std::string::npos);
        CHECK(journal.build_panel().has_command(UndoJournal::kUndoCommand));
    }

    // --- the JSON round-trip carries file steps, and tolerates a PRE-e2 journal ---------------------
    {
        FakeFileGateway files;
        UndoJournal journal;
        journal.set_file_gateway(&files);
        journal.capture_file(make_delete("art/hero.png", "tok-1"), "delete art/hero.png");
        journal.capture_file(make_move("a.json", "b.json"), "rename a.json");

        const std::string serialized = canonical(journal.to_json());
        UndoJournal restored;
        restored.set_file_gateway(&files);
        CHECK(restored.load_json(ser::parse_json(serialized).root));
        CHECK(restored.undo_depth() == 2);
        // Replaying out of the RESTORED journal proves the round-trip kept the op and its subject —
        // a checkpoint that came back as the wrong op would restore where it should move.
        CHECK(restored.undo().status == Status::applied);
        CHECK(files.calls.back().verb == "move");
        CHECK(files.calls.back().a == "b.json");

        // A journal written BEFORE e2 has no `file_edits` member at all: it must still load.
        UndoJournal legacy;
        JsonValue doc;
        doc.type = JsonValue::Type::object;
        JsonValue undo_stack;
        undo_stack.type = JsonValue::Type::array;
        JsonValue cp;
        cp.type = JsonValue::Type::object;
        JsonValue edits;
        edits.type = JsonValue::Type::array;
        JsonValue edit;
        edit.type = JsonValue::Type::object;
        edit.members.push_back({"root_scene", jstr("root.scene.json")});
        edit.members.push_back({"pointer", jstr("/name")});
        edit.members.push_back({"before", jstr("Old")});
        edit.members.push_back({"after", jstr("New")});
        edits.elements.push_back(std::move(edit));
        cp.members.push_back({"edits", std::move(edits)});
        cp.members.push_back({"label", jstr("legacy")});
        undo_stack.elements.push_back(std::move(cp));
        doc.members.push_back({"undo", std::move(undo_stack)});
        CHECK(legacy.load_json(doc));
        CHECK(legacy.undo_depth() == 1);

        // A PRESENT but wrong-shaped `file_edits` is malformed, NOT ignored: silently dropping a
        // recorded delete would hand the human a history that forgot the destructive step.
        UndoJournal broken;
        JsonValue bad_doc = doc;
        bad_doc.members.back().value.elements[0].members.push_back({"file_edits", jstr("nope")});
        CHECK(!broken.load_json(bad_doc));
        CHECK(!broken.can_undo());

        // ...and so is an unknown `op` token — defaulting it would read a recorded DELETE as a move.
        UndoJournal unknown_op;
        JsonValue op_doc = doc;
        JsonValue file_edits;
        file_edits.type = JsonValue::Type::array;
        JsonValue fe;
        fe.type = JsonValue::Type::object;
        fe.members.push_back({"from", jstr("art/hero.png")});
        fe.members.push_back({"op", jstr("obliterate")});
        file_edits.elements.push_back(std::move(fe));
        op_doc.members.back().value.elements[0].members.push_back({"file_edits", std::move(file_edits)});
        CHECK(!unknown_op.load_json(op_doc));
    }

    UNDO_TEST_MAIN_END();
}
