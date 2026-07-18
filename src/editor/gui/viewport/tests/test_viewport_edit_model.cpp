// The a19 viewport override-editing MODEL tests (R-HUX-006 / R-CLI-006 / L-35 / L-20 / L-30): drive
// the headless ViewportEditModel over an in-memory composed world — selection, provenance (winning-
// value-first, matching the compose::provenance_json query emitter), the gizmo gesture, the outermost
// override + edit-template + at-instance retarget requests, and the L-30 rebase-or-drop paths.

#include "context/editor/gui/viewport/viewport_edit_model.h"

#include "context/editor/compose/flatten.h"

#include "viewport_edit_test.h"

#include <string>
#include <vector>

using namespace context::editor::gui::viewport;

namespace
{

// The composed Torch entity from a fresh flatten (for independent provenance parity assertions).
[[nodiscard]] const compose::ComposedEntity* find_torch(const compose::ComposedScene& scene)
{
    for (const compose::ComposedEntity& e : scene.entities)
    {
        if (e.id_path == std::vector<std::string>{vpedit::kInst, vpedit::kTorch})
        {
            return &e;
        }
    }
    return nullptr;
}

} // namespace

int main()
{
    const std::string position = kPositionPointer;

    // --- open + selection ------------------------------------------------------------------------
    {
        vpedit::MapResolver world = vpedit::make_world();
        ViewportEditModel model;
        CHECK(model.open(world, vpedit::kRootScene));
        CHECK(model.loaded());
        // Camera (native) + Floor + Torch (instanced) = 3 composed entities.
        CHECK(model.entities().size() == 3);

        CHECK(!model.has_selection());
        CHECK(model.select(vpedit::torch_identity()));
        CHECK(model.has_selection());
        CHECK(model.selection_identity() == vpedit::torch_identity());
        CHECK((model.selected_id_path() == std::vector<std::string>{vpedit::kInst, vpedit::kTorch}));

        // The composed value is the OUTERMOST override [5,0,0] (the template authored [1,0,0]).
        const serializer::JsonValue* value = model.value_at(position);
        CHECK(value != nullptr);
        CHECK(vpedit::value_equal(*value, vpedit::jparse("[5, 0, 0]")));
        CHECK(model.overridden_at(position));

        // Selecting an unknown identity fails and leaves the prior selection untouched-by-return.
        CHECK(!model.select("nope/nope"));
    }

    // --- provenance is a winning-value-first CHAIN matching the query emitter (R-CLI-006, DoD-2) ---
    {
        vpedit::MapResolver world = vpedit::make_world();
        ViewportEditModel model;
        CHECK(model.open(world, vpedit::kRootScene));
        CHECK(model.select(vpedit::torch_identity()));

        const std::vector<compose::ProvenanceEntry> chain = model.provenance(position);
        CHECK(chain.size() == 2);
        // Winning value first: the outermost override (level 0), then the defining template (level 1).
        CHECK(chain.front().source == compose::ProvenanceEntry::Source::override_value);
        CHECK(chain.front().level == 0);
        CHECK(chain.back().source == compose::ProvenanceEntry::Source::template_value);
        CHECK(chain.back().level == 1);

        // The model's provenance display is BYTE-IDENTICAL to an independent flatten's canonical
        // provenance JSON — the SAME emitter `context query` serializes provenance through.
        const compose::ComposedScene fresh = compose::flatten(vpedit::kRootScene, world);
        const compose::ComposedEntity* torch = find_torch(fresh);
        CHECK(torch != nullptr);
        const std::string expected =
            compose::provenance_json(compose::provenance_for(*torch, position));
        CHECK(model.provenance_json(position) == expected);
    }

    // --- move gizmo -> OUTERMOST override write (the DoD-1 core) ----------------------------------
    {
        vpedit::MapResolver world = vpedit::make_world();
        ViewportEditModel model;
        CHECK(model.open(world, vpedit::kRootScene));
        CHECK(model.select(vpedit::torch_identity()));
        CHECK(model.gizmo() == Gizmo::move);
        CHECK(model.edit_target() == EditTarget::outermost);

        vpedit::FakeGateway gw;
        gw.field_values[position] = vpedit::jparse("[5, 0, 0]"); // the current outermost override
        CHECK(model.begin_gesture(gw));
        CHECK(model.gesture_active());
        model.translate(2, 0, 0); // [5,0,0] -> [7,0,0]
        CHECK(vpedit::value_equal(model.pending_value(), vpedit::jparse("[7, 0, 0]")));

        const inspector::CommitResult r = model.commit_gesture(gw);
        CHECK(r.status == inspector::CommitResult::Status::applied);
        CHECK(!model.gesture_active()); // consumed at gesture end (L-20)
        CHECK(gw.last_request.target == compose::WriteTarget::outermost);
        CHECK(gw.last_request.pointer == position);
        CHECK((gw.last_request.id_path == std::vector<std::string>{vpedit::kInst, vpedit::kTorch}));
        CHECK(vpedit::value_equal(gw.field_values[position], vpedit::jparse("[7, 0, 0]")));
    }

    // --- retarget: --edit-template writes the DEFINING template (DoD-1 retarget) ------------------
    {
        vpedit::MapResolver world = vpedit::make_world();
        ViewportEditModel model;
        CHECK(model.open(world, vpedit::kRootScene));
        CHECK(model.select(vpedit::torch_identity()));
        model.set_edit_target(EditTarget::edit_template);

        vpedit::FakeGateway gw;
        CHECK(model.begin_gesture(gw));
        model.set_pending_value(vpedit::jparse("[3, 3, 3]"));
        const inspector::CommitResult r = model.commit_gesture(gw);
        CHECK(r.status == inspector::CommitResult::Status::applied);
        CHECK(gw.last_request.target == compose::WriteTarget::defining_template);
    }

    // --- retarget: --at-instance writes a MID-LEVEL instancing scene (DoD-1 retarget) ------------
    {
        vpedit::MapResolver world = vpedit::make_world();
        ViewportEditModel model;
        CHECK(model.open(world, vpedit::kRootScene));
        CHECK(model.select(vpedit::torch_identity()));
        model.set_edit_target(EditTarget::at_instance);
        model.set_at_instance({vpedit::kInst});

        vpedit::FakeGateway gw;
        CHECK(model.begin_gesture(gw));
        model.set_pending_value(vpedit::jparse("[4, 4, 4]"));
        const inspector::CommitResult r = model.commit_gesture(gw);
        CHECK(r.status == inspector::CommitResult::Status::applied);
        CHECK(gw.last_request.target == compose::WriteTarget::at_instance);
        CHECK((gw.last_request.at_instance == std::vector<std::string>{vpedit::kInst}));
    }

    // --- L-30: a concurrent writer touching THIS field path -> DROP loudly (DoD-3) ----------------
    {
        vpedit::MapResolver world = vpedit::make_world();
        ViewportEditModel model;
        CHECK(model.open(world, vpedit::kRootScene));
        CHECK(model.select(vpedit::torch_identity()));

        vpedit::FakeGateway gw;
        gw.field_values[position] = vpedit::jparse("[5, 0, 0]");
        CHECK(model.begin_gesture(gw)); // captures base hash 100 + collision base [5,0,0]
        model.translate(1, 0, 0);
        // A concurrent writer advances the file AND moves the SAME field before our CAS check.
        gw.on_first_attempt = [&gw, &position]() {
            gw.file_hash += 5;
            gw.field_values[position] = vpedit::jparse("[9, 9, 9]");
        };
        const inspector::CommitResult r = model.commit_gesture(gw);
        CHECK(r.status == inspector::CommitResult::Status::dropped);
        CHECK(r.code == "cas.mismatch");
        CHECK(gw.attempts == 1); // never a second, overwriting attempt (L-30: never a silent overwrite)
    }

    // --- L-30: a concurrent writer touching an UNRELATED field -> REBASE + retry (DoD-3) ----------
    {
        vpedit::MapResolver world = vpedit::make_world();
        ViewportEditModel model;
        CHECK(model.open(world, vpedit::kRootScene));
        CHECK(model.select(vpedit::torch_identity()));

        vpedit::FakeGateway gw;
        gw.field_values[position] = vpedit::jparse("[5, 0, 0]"); // OUR field stays put on re-read
        CHECK(model.begin_gesture(gw));
        model.translate(1, 0, 0);
        // The concurrent writer advances the file but leaves our field untouched (an unrelated edit).
        gw.on_first_attempt = [&gw]() { gw.file_hash += 5; };
        const inspector::CommitResult r = model.commit_gesture(gw);
        CHECK(r.status == inspector::CommitResult::Status::rebased);
        CHECK(gw.attempts == 2); // rebased onto the new state, then applied
    }

    // --- no-selection guards ----------------------------------------------------------------------
    {
        vpedit::MapResolver world = vpedit::make_world();
        ViewportEditModel model;
        CHECK(model.open(world, vpedit::kRootScene));
        vpedit::FakeGateway gw;
        CHECK(!model.begin_gesture(gw)); // no selection
        const inspector::CommitResult r = model.commit_gesture(gw);
        CHECK(r.status == inspector::CommitResult::Status::none);
    }

    VPEDIT_TEST_MAIN_END();
}
