// Session undo/redo a11y surface tests (M5-F7, R-A11Y-001 / R-CLI-001): the headless uitree Panel the
// journal exposes is accessibility-conformant BY CONSTRUCTION (no audit_a11y violations), keyboard-
// reachable (every available action is a focusable, command-bound widget), and deterministic. Commands
// appear ONLY when the action is available, so there is never an unreachable-command violation. No CEF.

#include "context/editor/gui/session/undo/undo_journal.h"

#include "context/editor/gui/uitree/panel.h"

#include "undo_test.h"

#include <string>

namespace undo = context::editor::gui::session::undo;
namespace uitree = context::editor::gui::uitree;

using undo::FieldEdit;
using undo::UndoJournal;
using undotest::FakeGateway;
using undotest::jstr;
using undotest::JsonValue;

namespace
{

[[nodiscard]] FieldEdit edit(const std::string& pointer, JsonValue before, JsonValue after)
{
    FieldEdit e;
    e.root_scene = "root.scene.json";
    e.id_path = {"aaaaaaaaaaaaaaa1", "ccccccccccccccc1"};
    e.pointer = pointer;
    e.before = std::move(before);
    e.after = std::move(after);
    return e;
}

} // namespace

int main()
{
    // --- empty journal: an a11y-clean surface with NO exposed command (nothing to undo/redo) --------
    {
        UndoJournal journal;
        const uitree::Panel ui = journal.build_panel();
        CHECK(uitree::audit_a11y(ui).empty());
        CHECK(!ui.has_command(UndoJournal::kUndoCommand));
        CHECK(!ui.has_command(UndoJournal::kRedoCommand));
        CHECK(uitree::focus_order(ui).empty()); // no focusable action -> no unreachable command
        CHECK(uitree::render_html(ui).find("0 undoable, 0 redoable") != std::string::npos);
    }

    // --- an undoable journal: the Undo command is exposed + keyboard-reachable ----------------------
    {
        UndoJournal journal;
        journal.capture(edit("/name", jstr("Old"), jstr("New")));
        const uitree::Panel ui = journal.build_panel();
        CHECK(uitree::audit_a11y(ui).empty());
        CHECK(ui.has_command(UndoJournal::kUndoCommand));
        CHECK(!ui.has_command(UndoJournal::kRedoCommand));
        CHECK(uitree::focus_order(ui).size() == 1); // exactly the Undo button
        CHECK(uitree::render_html(ui).find("1 undoable, 0 redoable") != std::string::npos);

        // Deterministic render (identical state -> byte-identical HTML).
        CHECK(uitree::render_html(journal.build_panel()) == uitree::render_html(journal.build_panel()));
    }

    // --- both actions available (undo once with a field remaining): both commands exposed -----------
    {
        FakeGateway gw;
        gw.file_hash = 100;
        gw.field_values["/a"] = jstr("A2");
        gw.field_values["/b"] = jstr("B2");

        UndoJournal journal(&gw);
        journal.capture(edit("/a", jstr("A1"), jstr("A2")));
        journal.capture(edit("/b", jstr("B1"), jstr("B2")));
        CHECK(journal.undo().ok()); // reverts /b -> undo stack still has /a, redo stack has /b
        CHECK(journal.can_undo());
        CHECK(journal.can_redo());

        const uitree::Panel ui = journal.build_panel();
        CHECK(uitree::audit_a11y(ui).empty());
        CHECK(ui.has_command(UndoJournal::kUndoCommand));
        CHECK(ui.has_command(UndoJournal::kRedoCommand));
        CHECK(uitree::focus_order(ui).size() == 2); // Undo + Redo buttons, both keyboard-reachable
        CHECK(uitree::render_html(ui).find("1 undoable, 1 redoable") != std::string::npos);
    }

    UNDO_TEST_MAIN_END();
}
