// GUI session undo/redo journal (M5-F7): gesture-batch checkpointing + undo/redo replayed as
// CAS-guarded override writes through the inspector gateway seam (the ONE write path), with an
// up-front no-clobber guard + the shared L-30 rebase-or-drop engine so an undo never overwrites a
// concurrent writer (R-HUX-001). Plus canonical JSON (de)serialization for `.editor/session.json`.

#include "context/editor/gui/session/undo/undo_journal.h"

#include "context/editor/gui/uitree/node.h"

#include "context/editor/compose/compose_write.h"
#include "context/editor/serializer/canonical.h"

#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace context::editor::gui::session::undo
{

namespace
{

namespace compose = context::editor::compose;

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
    return !out.edits.empty();
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

} // namespace

void UndoJournal::record(Checkpoint checkpoint)
{
    if (checkpoint.edits.empty())
    {
        return; // a gesture that captured nothing is not an undo step
    }
    undo_.push_back(std::move(checkpoint));
    redo_.clear(); // a new gesture invalidates the redo future
}

void UndoJournal::begin_gesture(std::string label)
{
    end_gesture(); // flush any already-open batch first
    open_gesture_ = Checkpoint{std::move(label), {}};
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

    compose::WriteRequest request;
    request.root_scene = edit.root_scene;
    request.id_path = edit.id_path;
    request.pointer = edit.pointer;
    request.value = target;
    request.target = compose::WriteTarget::outermost; // L-35: the outermost instancing scene wins

    // Route through the ONE L-20/L-30 engine, CAS-guarded on the just-read hash. If a writer races
    // between the read and the write, the engine re-reads and re-applies the field-path drop policy.
    return inspector::commit_override_write(*gateway_, request, edit.root_scene, edit.id_path,
                                            edit.pointer, expected, current.raw_hash);
}

ReplayResult UndoJournal::undo()
{
    ReplayResult r;
    if (undo_.empty() || gateway_ == nullptr)
    {
        r.status = Status::none;
        last_ = r;
        return r;
    }
    Checkpoint cp = std::move(undo_.back());
    undo_.pop_back();
    r.label = cp.label;

    // Revert in REVERSE order (the last-applied field reverts first).
    bool all_ok = true;
    for (auto it = cp.edits.rbegin(); it != cp.edits.rend(); ++it)
    {
        inspector::CommitResult res = replay_edit(*it, /*redo=*/false);
        all_ok = all_ok && (res.status == Status::applied || res.status == Status::rebased);
        r.edits.push_back(std::move(res));
    }
    r.status = aggregate(r.edits);
    if (all_ok)
    {
        redo_.push_back(std::move(cp)); // only a cleanly-reverted checkpoint can be redone
    }
    last_ = r;
    return r;
}

ReplayResult UndoJournal::redo()
{
    ReplayResult r;
    if (redo_.empty() || gateway_ == nullptr)
    {
        r.status = Status::none;
        last_ = r;
        return r;
    }
    Checkpoint cp = std::move(redo_.back());
    redo_.pop_back();
    r.label = cp.label;

    // Re-apply in FORWARD order (the original application order).
    bool all_ok = true;
    for (const FieldEdit& edit : cp.edits)
    {
        inspector::CommitResult res = replay_edit(edit, /*redo=*/true);
        all_ok = all_ok && (res.status == Status::applied || res.status == Status::rebased);
        r.edits.push_back(std::move(res));
    }
    r.status = aggregate(r.edits);
    if (all_ok)
    {
        undo_.push_back(std::move(cp)); // a cleanly re-applied checkpoint returns to the undo stack
    }
    last_ = r;
    return r;
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
