// GUI session undo/redo journal (M5-F7): gesture-batch checkpointing + undo/redo replayed as
// CAS-guarded override writes through the inspector gateway seam (the ONE write path), with an
// up-front no-clobber guard + the shared L-30 rebase-or-drop engine so an undo never overwrites a
// concurrent writer (R-HUX-001). Plus canonical JSON (de)serialization for the host to persist
// (since e09c: `.editor/editor-state.json` — see the header on why NOT `.editor/session.json`).

#include "context/editor/gui/session/undo/undo_journal.h"

#include "context/editor/gui/uitree/node.h"

#include "context/editor/serializer/canonical.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace context::editor::gui::session::undo
{

namespace
{

using serializer::JsonValue;
using Status = inspector::CommitResult::Status;

// Two JSON values are equal iff their canonical serializations match — the engine's ONE notion of
// value identity (R-FILE-001), mirroring compose_write.cpp / inspector_panel.cpp's canonical_equal.
[[nodiscard]] bool canonical_equal(const JsonValue& a, const JsonValue& b)
{
    std::string sa;
    std::string sb;
    const bool oka = serializer::serialize_canonical(a, sa);
    const bool okb = serializer::serialize_canonical(b, sb);
    return oka == okb && sa == sb;
}

[[nodiscard]] JsonValue jstring(const std::string& s)
{
    JsonValue v;
    v.type = JsonValue::Type::string;
    v.string_value = s;
    return v;
}

// The value of member `key` on an object (any type), or nullptr when absent / not an object.
[[nodiscard]] const JsonValue* member(const JsonValue& obj, const char* key)
{
    if (obj.type != JsonValue::Type::object)
    {
        return nullptr;
    }
    for (const serializer::JsonMember& m : obj.members)
    {
        if (m.key == key)
        {
            return &m.value;
        }
    }
    return nullptr;
}

[[nodiscard]] const std::string* string_member(const JsonValue& obj, const char* key)
{
    const JsonValue* v = member(obj, key);
    return (v != nullptr && v->type == JsonValue::Type::string) ? &v->string_value : nullptr;
}

// The aggregate status of a checkpoint replay: dropped if ANY field collided, else error if any
// refused, else rebased if any rebased, else applied (none only when there were no edits).
[[nodiscard]] Status aggregate(const std::vector<inspector::CommitResult>& edits)
{
    if (edits.empty())
    {
        return Status::none;
    }
    bool any_dropped = false;
    bool any_error = false;
    bool any_rebased = false;
    for (const inspector::CommitResult& e : edits)
    {
        switch (e.status)
        {
        case Status::dropped:
            any_dropped = true;
            break;
        case Status::error:
            any_error = true;
            break;
        case Status::rebased:
            any_rebased = true;
            break;
        case Status::applied:
        case Status::none:
            break;
        }
    }
    if (any_dropped)
    {
        return Status::dropped;
    }
    if (any_error)
    {
        return Status::error;
    }
    if (any_rebased)
    {
        return Status::rebased;
    }
    return Status::applied;
}

// Whether a replay left the project completely UNTOUCHED — no field applied, none rebased. Paired
// with an `error` aggregate it is the precondition for returning the checkpoint to the stack it was
// popped from: a refusal wrote nothing, so the step is still exactly as undoable (or redoable) as it
// was a moment ago, and consuming it would cost the human a step over a transient outage.
//
// Both halves of that pair are load-bearing. A DROPPED aggregate also touches nothing, but a drop
// means the field MOVED — that step can never be replayed, so it is consumed (R-HUX-001; re-offering
// it would hand the human an undo guaranteed to refuse forever). And a PARTIALLY-landed batch is
// excluded by this predicate because re-offering it would replay the fields that already moved,
// whose up-front guard would then drop. (The Inspector produces one-edit checkpoints today, so the
// partial case is not reachable in this build; the guard is what keeps it correct if a batched
// gesture ever is.)
[[nodiscard]] bool nothing_landed(const std::vector<inspector::CommitResult>& edits)
{
    // `CommitResult::ok()` IS the applied-or-rebased predicate — spelling it out again here would be
    // a third copy of a rule that must not drift.
    return std::none_of(edits.begin(), edits.end(),
                        [](const inspector::CommitResult& e) { return e.ok(); });
}

// --- JSON serialization of a checkpoint (members authored sorted; the canonical writer re-sorts) ----

[[nodiscard]] JsonValue edit_to_json(const FieldEdit& edit)
{
    JsonValue obj;
    obj.type = JsonValue::Type::object;
    obj.members.push_back({"after", edit.after});
    obj.members.push_back({"before", edit.before});
    JsonValue id_path;
    id_path.type = JsonValue::Type::array;
    for (const std::string& seg : edit.id_path)
    {
        id_path.elements.push_back(jstring(seg));
    }
    obj.members.push_back({"id_path", std::move(id_path)});
    obj.members.push_back({"pointer", jstring(edit.pointer)});
    obj.members.push_back({"root_scene", jstring(edit.root_scene)});
    return obj;
}

// --- M9 e2: the FILE edit atom's JSON form ------------------------------------------------------
// Op crosses as a TOKEN, not as the enum's integer: a persisted journal outlives the build that
// wrote it, and renumbering the enum must never silently turn a recorded delete into a move.
inline constexpr const char* kFileOpMove = "move";
inline constexpr const char* kFileOpRemove = "remove";

[[nodiscard]] JsonValue file_edit_to_json(const FileEdit& edit)
{
    JsonValue obj;
    obj.type = JsonValue::Type::object;
    obj.members.push_back({"from", jstring(edit.from)});
    obj.members.push_back(
        {"op", jstring(edit.op == FileEdit::Op::remove ? kFileOpRemove : kFileOpMove)});
    obj.members.push_back({"restore_token", jstring(edit.restore_token)});
    obj.members.push_back({"to", jstring(edit.to)});
    return obj;
}

[[nodiscard]] bool file_edit_from_json(const JsonValue& obj, FileEdit& out)
{
    const std::string* op = string_member(obj, "op");
    const std::string* from = string_member(obj, "from");
    if (op == nullptr || from == nullptr || from->empty())
    {
        return false; // the operation and its subject are required
    }
    if (*op == kFileOpRemove)
    {
        out.op = FileEdit::Op::remove;
    }
    else if (*op == kFileOpMove)
    {
        out.op = FileEdit::Op::move;
    }
    else
    {
        return false; // an unknown op is malformed, never coerced to a default — the default would
                      // be `move`, and reading a recorded DELETE as a move is the worst guess here
    }
    out.from = *from;
    if (const std::string* to = string_member(obj, "to"))
    {
        out.to = *to;
    }
    if (const std::string* token = string_member(obj, "restore_token"))
    {
        out.restore_token = *token;
    }
    return out.op == FileEdit::Op::move ? !out.to.empty() : !out.restore_token.empty();
}

[[nodiscard]] JsonValue checkpoint_to_json(const Checkpoint& cp)
{
    JsonValue obj;
    obj.type = JsonValue::Type::object;
    JsonValue edits;
    edits.type = JsonValue::Type::array;
    for (const FieldEdit& e : cp.edits)
    {
        edits.elements.push_back(edit_to_json(e));
    }
    obj.members.push_back({"edits", std::move(edits)});
    JsonValue file_edits;
    file_edits.type = JsonValue::Type::array;
    for (const FileEdit& e : cp.file_edits)
    {
        file_edits.elements.push_back(file_edit_to_json(e));
    }
    obj.members.push_back({"file_edits", std::move(file_edits)});
    obj.members.push_back({"label", jstring(cp.label)});
    return obj;
}

// --- JSON parsing (total + robust: a malformed piece is skipped, never thrown) ----------------------

[[nodiscard]] bool edit_from_json(const JsonValue& obj, FieldEdit& out)
{
    const std::string* root_scene = string_member(obj, "root_scene");
    const std::string* pointer = string_member(obj, "pointer");
    if (root_scene == nullptr || pointer == nullptr)
    {
        return false; // the addressing is required
    }
    out.root_scene = *root_scene;
    out.pointer = *pointer;
    out.id_path.clear();
    if (const JsonValue* id_path = member(obj, "id_path"); id_path != nullptr &&
                                                           id_path->type == JsonValue::Type::array)
    {
        for (const JsonValue& seg : id_path->elements)
        {
            if (seg.type != JsonValue::Type::string)
            {
                return false; // a non-string id-path segment is malformed
            }
            out.id_path.push_back(seg.string_value);
        }
    }
    if (const JsonValue* before = member(obj, "before"))
    {
        out.before = *before;
    }
    if (const JsonValue* after = member(obj, "after"))
    {
        out.after = *after;
    }
    return true;
}

[[nodiscard]] bool checkpoint_from_json(const JsonValue& obj, Checkpoint& out)
{
    if (obj.type != JsonValue::Type::object)
    {
        return false;
    }
    if (const std::string* label = string_member(obj, "label"))
    {
        out.label = *label;
    }
    const JsonValue* edits = member(obj, "edits");
    if (edits == nullptr || edits->type != JsonValue::Type::array)
    {
        return false;
    }
    for (const JsonValue& entry : edits->elements)
    {
        FieldEdit edit;
        if (!edit_from_json(entry, edit))
        {
            return false;
        }
        out.edits.push_back(std::move(edit));
    }
    // M9 e2: `file_edits` is OPTIONAL on read (a journal written before e2 has none) but a PRESENT
    // wrong-shaped one is malformed, not ignored — silently dropping a recorded delete would hand
    // the human a Session History that has forgotten the one operation they most need to undo.
    if (const JsonValue* file_edits = member(obj, "file_edits"); file_edits != nullptr)
    {
        if (file_edits->type != JsonValue::Type::array)
        {
            return false;
        }
        for (const JsonValue& entry : file_edits->elements)
        {
            FileEdit edit;
            if (!file_edit_from_json(entry, edit))
            {
                return false;
            }
            out.file_edits.push_back(std::move(edit));
        }
    }
    return !out.empty();
}

// Parse a stack array into `out`; a malformed element aborts the whole load (return false).
[[nodiscard]] bool stack_from_json(const JsonValue* array, std::vector<Checkpoint>& out)
{
    if (array == nullptr)
    {
        return true; // an absent stack is simply empty
    }
    if (array->type != JsonValue::Type::array)
    {
        return false;
    }
    for (const JsonValue& entry : array->elements)
    {
        Checkpoint cp;
        if (!checkpoint_from_json(entry, cp))
        {
            return false;
        }
        out.push_back(std::move(cp));
    }
    return true;
}

// Is there anything on `cp` that a BOUND gateway could actually replay? (M9 e2.)
//
// This replaces the old `gateway_ == nullptr` guard at the top of undo()/redo(), and the
// replacement is exact rather than approximate: with only field edits and no override gateway it
// answers the same `false` the old test did, so the step is reported `none` and KEPT — which is what
// the whole keep-vs-consume contract rests on (`aggregate` maps an all-`none` replay to `applied`,
// so falling through with nothing bound would report a clean replay and CONSUME a step that never
// ran). What it adds is the other half: a checkpoint made entirely of FILE edits is replayable
// through the file gateway even when no override gateway exists, and vice versa.
[[nodiscard]] bool replayable(const Checkpoint& cp, const inspector::OverrideWriteGateway* gateway,
                              const files::FileWriteGateway* file_gateway)
{
    return (gateway != nullptr && !cp.edits.empty()) ||
           (file_gateway != nullptr && !cp.file_edits.empty());
}

} // namespace

void UndoJournal::record(Checkpoint checkpoint)
{
    if (checkpoint.empty())
    {
        return; // a gesture that captured nothing is not an undo step
    }
    undo_.push_back(std::move(checkpoint));
    redo_.clear(); // a new gesture invalidates the redo future
}

void UndoJournal::begin_gesture(std::string label)
{
    end_gesture(); // flush any already-open batch first
    open_gesture_ = Checkpoint{std::move(label), {}, {}};
}

void UndoJournal::capture(FieldEdit edit)
{
    if (open_gesture_.has_value())
    {
        open_gesture_->edits.push_back(std::move(edit));
        return;
    }
    // No open batch -> auto-checkpoint this lone edit as its own gesture (L-20).
    Checkpoint cp;
    cp.edits.push_back(std::move(edit));
    record(std::move(cp));
}

void UndoJournal::capture_file(FileEdit edit, std::string label)
{
    if (open_gesture_.has_value())
    {
        open_gesture_->file_edits.push_back(std::move(edit));
        return;
    }
    Checkpoint cp;
    cp.label = std::move(label);
    cp.file_edits.push_back(std::move(edit));
    record(std::move(cp));
}

void UndoJournal::end_gesture()
{
    if (!open_gesture_.has_value())
    {
        return;
    }
    Checkpoint cp = std::move(*open_gesture_);
    open_gesture_.reset();
    record(std::move(cp)); // no-op when the batch captured nothing
}

inspector::CommitResult UndoJournal::replay_edit(const FieldEdit& edit, bool redo) const
{
    const JsonValue& target = redo ? edit.after : edit.before;
    const JsonValue& expected = redo ? edit.before : edit.after;

    inspector::CommitResult res;
    res.pointer = edit.pointer;
    if (gateway_ == nullptr)
    {
        res.status = Status::none;
        return res;
    }

    // Up-front R-HUX-001 no-clobber guard: read the field's CURRENT value; if it no longer holds the
    // value we last wrote, a concurrent writer touched it -> drop loudly, never restore stale bytes.
    const inspector::FieldState current = gateway_->read(edit.root_scene, edit.id_path, edit.pointer);
    if (!current.present)
    {
        // COULD NOT READ != WAS CHANGED. Every gateway reports an unreadable field the same way — a
        // default FieldState (`present:false`, a null value, a 0 token): no daemon connection, a
        // refused `editor.inspect`, an entity that no longer resolves. Falling through to the
        // comparison below would read that null as "the value moved" and report `cas.mismatch`,
        // inventing a concurrent writer that does not exist — and, because a `dropped` checkpoint is
        // CONSUMED, silently costing the human the undo step over a transient outage.
        //
        // This path is reachable only because the journal reads BEFORE it attempts: on the gesture
        // path `attempt` refuses first, which is why `wire_override_gateway.h` § LIFETIME says the
        // ATTEMPT refusal, not the read, is what fails THAT path closed. It is an ERROR — nothing was
        // written, so the caller KEEPS the checkpoint (inspector_panel.h's caller contract: an error
        // keeps it, a drop consumes it).
        res.status = Status::error;
        res.code = kReadUnavailableCode;
        res.message = "the field `" + edit.pointer + "` could not be read, so the " +
                      std::string(redo ? "redo" : "undo") +
                      " was not attempted; nothing was written — try again once the project is "
                      "reachable";
        res.raw_hash = current.raw_hash;
        return res;
    }
    if (!canonical_equal(current.value, expected))
    {
        res.status = Status::dropped;
        res.code = "cas.mismatch";
        res.message = "the field `" + edit.pointer +
                      "` was changed by another writer since this edit; the " +
                      std::string(redo ? "redo" : "undo") +
                      " was dropped, not overwritten (R-HUX-001 / L-30) — re-apply it manually";
        res.raw_hash = current.raw_hash;
        return res;
    }

    // The boundary-clean envelope (M9 e05d3): the gateway converts it kernel-side, so the journal —
    // like the inspector panel it mirrors — carries no compose type.
    inspector::OverrideWriteRequest request;
    request.root_scene = edit.root_scene;
    request.id_path = edit.id_path;
    request.pointer = edit.pointer;
    request.value = target;
    request.target = inspector::OverrideWriteTarget::outermost; // L-35: the outermost scene wins

    // Route through the ONE L-20/L-30 engine, CAS-guarded on the just-read hash. If a writer races
    // between the read and the write, the engine re-reads and re-applies the field-path drop policy.
    return inspector::commit_override_write(*gateway_, request, edit.root_scene, edit.id_path,
                                            edit.pointer, expected, current.raw_hash);
}

inspector::CommitResult UndoJournal::replay_file_edit(FileEdit& edit, bool redo)
{
    inspector::CommitResult res;
    res.file = edit.from;

    if (file_gateway_ == nullptr)
    {
        // Honest refusal, not `none`: `none` means "there was nothing to replay", and reporting it
        // here would let `aggregate` call the checkpoint cleanly replayed and move it to the other
        // stack — losing a step that never ran. An `error` KEEPS it.
        res.status = Status::error;
        res.code = kFileWriteUnavailableCode;
        res.message = "no file write path is bound, so the " + std::string(redo ? "redo" : "undo") +
                      " of `" + edit.from + "` was not attempted; nothing was written";
        return res;
    }

    files::FileWriteResult out;
    switch (edit.op)
    {
    case FileEdit::Op::move:
        // Undo moves the file BACK; redo re-applies the original direction. Either way it is the
        // same engine operation the human's own rename issued — never a privileged restore of
        // remembered bytes, which is precisely what R-HUX-001 forbids.
        out = redo ? file_gateway_->move_file(edit.from, edit.to)
                   : file_gateway_->move_file(edit.to, edit.from);
        res.file = redo ? edit.to : edit.from;
        break;
    case FileEdit::Op::remove:
        out = redo ? file_gateway_->delete_file(edit.from)
                   : file_gateway_->restore_file(edit.restore_token);
        break;
    }

    if (out.ok())
    {
        res.status = Status::applied;
        // A REDONE delete re-quarantines the bytes under a possibly-new handle; adopting it here is
        // what keeps the NEXT undo of this step restorable.
        if (edit.op == FileEdit::Op::remove && redo && !out.restore_token.empty())
        {
            edit.restore_token = out.restore_token;
        }
        return res;
    }
    res.status = Status::error;
    res.code = out.code;
    res.message = out.message;
    return res;
}

ReplayResult UndoJournal::undo()
{
    ReplayResult r;
    // The gateway test is per-CHECKPOINT since e2 (a checkpoint may be all file edits, replayed
    // through the other gateway) — see `replayable` for why this cannot become an unconditional pop.
    if (undo_.empty() || !replayable(undo_.back(), gateway_, file_gateway_))
    {
        r.status = Status::none;
        last_ = r;
        return r;
    }
    Checkpoint cp = std::move(undo_.back());
    undo_.pop_back();
    r.label = cp.label;

    // Revert in REVERSE order (the last-applied field reverts first). File edits revert BEFORE
    // field edits for the same reason: they were applied last within a step that carries both, and
    // reverting a field write into a file that has not come back yet would refuse.
    bool all_ok = true;
    for (auto it = cp.file_edits.rbegin(); it != cp.file_edits.rend(); ++it)
    {
        inspector::CommitResult res = replay_file_edit(*it, /*redo=*/false);
        all_ok = all_ok && res.ok();
        r.edits.push_back(std::move(res));
    }
    for (auto it = cp.edits.rbegin(); it != cp.edits.rend(); ++it)
    {
        inspector::CommitResult res = replay_edit(*it, /*redo=*/false);
        all_ok = all_ok && (res.status == Status::applied || res.status == Status::rebased);
        r.edits.push_back(std::move(res));
    }
    r.status = aggregate(r.edits);
    settle(std::move(cp), r, all_ok, /*from=*/undo_, /*to=*/redo_);
    last_ = r;
    return r;
}

ReplayResult UndoJournal::redo()
{
    ReplayResult r;
    if (redo_.empty() || !replayable(redo_.back(), gateway_, file_gateway_))
    {
        r.status = Status::none;
        last_ = r;
        return r;
    }
    Checkpoint cp = std::move(redo_.back());
    redo_.pop_back();
    r.label = cp.label;

    // Re-apply in FORWARD order (the original application order) — the exact mirror of undo's
    // reverse pass, so field edits go first and file edits last.
    bool all_ok = true;
    for (const FieldEdit& edit : cp.edits)
    {
        inspector::CommitResult res = replay_edit(edit, /*redo=*/true);
        all_ok = all_ok && (res.status == Status::applied || res.status == Status::rebased);
        r.edits.push_back(std::move(res));
    }
    for (FileEdit& edit : cp.file_edits)
    {
        inspector::CommitResult res = replay_file_edit(edit, /*redo=*/true);
        all_ok = all_ok && res.ok();
        r.edits.push_back(std::move(res));
    }
    r.status = aggregate(r.edits);
    settle(std::move(cp), r, all_ok, /*from=*/redo_, /*to=*/undo_);
    last_ = r;
    return r;
}

void UndoJournal::settle(Checkpoint cp, const ReplayResult& r, bool all_ok,
                         std::vector<Checkpoint>& from, std::vector<Checkpoint>& to)
{
    if (all_ok)
    {
        to.push_back(std::move(cp)); // only a cleanly-replayed checkpoint crosses to the other stack
        return;
    }
    if (r.status == Status::error && nothing_landed(r.edits))
    {
        from.push_back(std::move(cp)); // a refusal wrote nothing -> the step is unchanged, so keep it
    }
    // Anything else is CONSUMED: a drop means the field moved and this step can never be replayed
    // (R-HUX-001), and a partially-landed batch cannot be re-offered (see `nothing_landed`).
}

serializer::JsonValue UndoJournal::to_json() const
{
    JsonValue doc;
    doc.type = JsonValue::Type::object;

    JsonValue undo_array;
    undo_array.type = JsonValue::Type::array;
    for (const Checkpoint& cp : undo_)
    {
        undo_array.elements.push_back(checkpoint_to_json(cp));
    }
    JsonValue redo_array;
    redo_array.type = JsonValue::Type::array;
    for (const Checkpoint& cp : redo_)
    {
        redo_array.elements.push_back(checkpoint_to_json(cp));
    }
    JsonValue version;
    version.type = JsonValue::Type::integer;
    version.int_value = kJournalVersion;

    doc.members.push_back({"redo", std::move(redo_array)});
    doc.members.push_back({"undo", std::move(undo_array)});
    doc.members.push_back({"version", std::move(version)});
    return doc;
}

bool UndoJournal::load_json(const serializer::JsonValue& doc)
{
    undo_.clear();
    redo_.clear();
    open_gesture_.reset();
    last_ = ReplayResult{};

    if (doc.type != JsonValue::Type::object)
    {
        return false;
    }
    std::vector<Checkpoint> loaded_undo;
    std::vector<Checkpoint> loaded_redo;
    if (!stack_from_json(member(doc, "undo"), loaded_undo) ||
        !stack_from_json(member(doc, "redo"), loaded_redo))
    {
        return false; // a malformed journal leaves the stacks empty
    }
    undo_ = std::move(loaded_undo);
    redo_ = std::move(loaded_redo);
    return true;
}

uitree::Panel UndoJournal::build_panel() const
{
    using uitree::Role;
    using uitree::UiNode;

    uitree::Panel panel("session.undo", "Session History");

    UiNode root(Role::region, "session.undo.panel");
    root.set_label("Session History");
    root.add_child(UiNode(Role::heading, "session.undo.heading")
                       .set_label("Session History")
                       .set_text("Session History"));

    std::ostringstream status;
    status << undo_.size() << " undoable, " << redo_.size() << " redoable";
    // M9 e2: NAME the next step, not just count it. A file operation's entry is otherwise
    // indistinguishable from any other in Session History — and "undo" on a panel that just deleted
    // a file is exactly the moment a human needs to read what they are about to undo.
    if (!undo_.empty() && !undo_.back().label.empty())
    {
        status << " - next undo: " << undo_.back().label;
    }
    root.add_child(UiNode(Role::status, "session.undo.status")
                       .set_label("Session History status")
                       .set_text(status.str()));

    // Expose a command ONLY when it is reachable (the action is available), so a widget always backs
    // every exposed command and audit_a11y never reports an unreachable command.
    UiNode actions(Role::group, "session.undo.actions");
    if (can_undo())
    {
        panel.add_command(kUndoCommand, "Undo");
        actions.add_child(UiNode(Role::button, "session.undo.button.undo")
                              .set_label("Undo")
                              .set_text("Undo")
                              .set_focusable(true)
                              .set_command(kUndoCommand));
    }
    if (can_redo())
    {
        panel.add_command(kRedoCommand, "Redo");
        actions.add_child(UiNode(Role::button, "session.undo.button.redo")
                              .set_label("Redo")
                              .set_text("Redo")
                              .set_focusable(true)
                              .set_command(kRedoCommand));
    }
    root.add_child(std::move(actions));

    panel.set_root(std::move(root));
    return panel;
}

} // namespace context::editor::gui::session::undo
