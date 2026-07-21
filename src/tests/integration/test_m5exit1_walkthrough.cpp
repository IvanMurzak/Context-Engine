// M5 exit criterion 1 — the HEADLESS OBSERVER-EDITOR WALKTHROUGH (ROADMAP §1 M5 Exit / issue #168),
// registered as the blocking `m5-exit-1-walkthrough` ctest and run by the CI build job's "M5 exit gate"
// named step on all three build-matrix legs — with NO CEF, NO GPU, NO daemon (the R-EDIT-001
// testable-by-construction editor: every panel is a headless context_gui_uitree surface). It walks the
// exact M5-exit user journey end to end, chaining the seven landed observer panels over the ONE bridge
// contract + the ONE `context set` write path:
//
//   open a project (compose a real scene from files, L-19)
//     -> inspect: scene-tree (F2) lists the derived world; selecting an entity drives the inspector
//        (F3); the Problems panel (F4) groups the live diagnostics
//     -> play it (F5): the playbar starts a session over the edit-state VIEW (L-51) and yields a
//        rendered PlayFrame
//     -> make an override edit through the inspector (F3): a CAS-guarded L-35 override write lands
//        through the gateway, captured as an undo checkpoint (F7)
//     -> undo (F7): the edit is reverted through the SAME CAS-guarded write path (never a blind
//        clobber, R-HUX-001)
//     -> the viewport (F1) observes the play frame — the SAME render::RenderSnapshot the playbar
//        produced (NO second render path), composited via the L-41 surface handoff; the golden-scene
//        equivalence within the T1 feature set is the SSIM gate on the render / render-web CI jobs.
//
// It ALSO measures the R-HUX-011 human-interaction loops (selection, gesture->viewport, inspector
// commit) from instrumented timestamps in the real event path — a SHOULD MEASUREMENT (recorded in
// docs/human-latency-budget.md), not a hard threshold gate: the assertion is that each loop was
// measured and its instrumentation seam fired, not that it beat a number on a shared CI runner.

#include "context/editor/gui/panels/builders/scene_tree_builder.h"
#include "context/editor/gui/panels/inspector/inspector_model.h"
#include "context/editor/gui/panels/inspector/inspector_panel.h"
#include "context/editor/gui/panels/problems/problems_panel.h"
#include "context/editor/gui/panels/scenetree/scene_tree_model.h"
#include "context/editor/gui/panels/scenetree/scene_tree_panel.h"
#include "context/editor/gui/playbar/playbar_model.h"
#include "context/editor/gui/playbar/playbar_panel.h"
#include "context/editor/gui/playbar/session_control.h"
#include "context/editor/gui/session/undo/undo_journal.h"
#include "context/editor/gui/uitree/panel.h"
#include "context/editor/gui/viewport/viewport_panel.h"

#include "context/editor/compose/flatten.h"
#include "context/editor/compose/json_pointer.h"
#include "context/editor/compose/scene_model.h"
#include "context/editor/serializer/canonical.h"

#include "context/render/render_world.h"

#include "m5_exit_test.h"

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace compose = context::editor::compose;
namespace inspector = context::editor::gui::panels::inspector;
namespace problems = context::editor::gui::panels::problems;
namespace scenetree = context::editor::gui::panels::scenetree;
namespace panelbuilders = context::editor::gui::panels::builders;
namespace playbar = context::editor::gui::playbar;
namespace undo = context::editor::gui::session::undo;
namespace uitree = context::editor::gui::uitree;
namespace viewport = context::editor::gui::viewport;
namespace compositor = context::editor::gui::compositor;
namespace render = context::render;
namespace serializer = context::editor::serializer;

using m5exit::canonical;
using m5exit::jnum;
using m5exit::jstr;
using m5exit::value_equal;
using m5exit::WalkthroughGateway;

namespace
{

// A lean in-memory SceneResolver built from authored scene JSON strings — the "files are the truth"
// (L-19) re-derivation seam, without touching disk (mirrors m2_exit_test.h's MapResolver).
class MapResolver final : public compose::SceneResolver
{
public:
    bool add(const std::string& path, const std::string& json)
    {
        serializer::CanonicalizeResult parsed = serializer::canonicalize(json);
        if (!parsed.is_json)
            return false;
        std::optional<compose::SceneDoc> doc = compose::build_scene_doc(path, parsed.root);
        if (!doc.has_value())
            return false;
        docs_[path] = std::move(*doc);
        return true;
    }

    [[nodiscard]] const compose::SceneDoc* resolve(std::string_view path) const override
    {
        auto it = docs_.find(std::string(path));
        return it == docs_.end() ? nullptr : &it->second;
    }

private:
    std::map<std::string, compose::SceneDoc, std::less<>> docs_;
};

// The play session the F5 playbar drives — a headless SessionControl double that yields a real
// render::RenderSnapshot (the SAME type the F1 viewport observes, so play output flows F5 -> F1 with no
// second render path). Mirrors the playbar's own test double (test_playbar_model.cpp / test_playbar_
// panel.cpp): the demo scenario always instantiates + steps cleanly.
class WalkthroughSession final : public playbar::SessionControl
{
public:
    std::uint32_t drawables = 4;
    std::uint32_t directional_lights = 1;
    std::uint64_t tick = 0;
    bool running = false;

    playbar::ControlOutcome start() override
    {
        running = true;
        tick = 0;
        playbar::ControlOutcome out;
        out.ok = true;
        out.frame = frame();
        return out;
    }

    playbar::ControlOutcome step(std::uint64_t ticks) override
    {
        tick += ticks;
        playbar::ControlOutcome out;
        out.ok = true;
        out.frame = frame();
        return out;
    }

    void discard() override
    {
        running = false;
        tick = 0;
    }

    playbar::HotReloadOutcome apply_hot_reload(const playbar::LiveEdit& edit) override
    {
        playbar::HotReloadOutcome out;
        out.ok = true;
        out.reload_class = edit.shape_or_layout_change ? playbar::HotReloadClass::restart_class
                                                       : playbar::HotReloadClass::live_preserving;
        out.state_preserved = !edit.shape_or_layout_change;
        out.frame = frame();
        return out;
    }

private:
    [[nodiscard]] playbar::PlayFrame frame() const
    {
        playbar::PlayFrame f;
        f.sim_tick = tick;
        f.snapshot.sim_tick = tick;
        f.snapshot.items.resize(drawables);
        f.snapshot.directional_lights.resize(directional_lights);
        return f;
    }
};

// steady_clock elapsed nanoseconds between two marks — the R-HUX-011 instrumented-timestamp primitive
// (cast to long long: a steady_clock rep is implementation-defined; conventions.md § Coding conventions).
[[nodiscard]] long long elapsed_ns(std::chrono::steady_clock::time_point a,
                                   std::chrono::steady_clock::time_point b)
{
    return static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}

} // namespace

int main()
{
    // The R-HUX-011 human-loop measurements (nanoseconds, from instrumented timestamps in the real
    // event path). Recorded to stdout below and normatively documented in docs/human-latency-budget.md.
    // A SHOULD MEASUREMENT (not a hard gate): the assertion is that each loop was measured + its seam
    // fired, never that it beat a number on a shared CI runner.
    long long selection_loop_ns = -1;
    long long gesture_viewport_loop_ns = -1;
    long long inspector_commit_loop_ns = -1;

    // === OPEN A PROJECT — compose a real derived world from authored files (L-19) =====================
    MapResolver resolver;
    CHECK(resolver.add("root.scene.json", R"({
      "$schema": "ctx:scene", "version": 1,
      "entities": [
        {"id": "ccccccccccccccc1", "name": "Hero",
         "components": {"transform": {"position": [0, 0, 0]}}},
        {"id": "ccccccccccccccc2", "name": "Light",
         "components": {"transform": {"position": [1, 2, 3]}}}
      ]})"));
    const compose::ComposedScene scene = compose::flatten("root.scene.json", resolver);
    CHECK(scene.ok);
    CHECK(scene.entities.size() == 2);

    // === INSPECT — F2 scene-tree lists the derived world; selection drives F3 =========================
    scenetree::SceneTreePanel tree;
    tree.set_model(panelbuilders::build_scene_tree(scene));
    CHECK(tree.model().ok);
    CHECK(tree.model().entity_count == 2);

    // The R-HUX-011 SELECTION loop: selecting a tree row fires the selection event other panels consume.
    scenetree::SceneSelection observed_selection;
    int selection_events = 0;
    tree.add_selection_listener([&](const scenetree::SceneSelection& s) {
        observed_selection = s;
        ++selection_events;
    });
    const std::string kSelectedIdentity = "ccccccccccccccc1"; // the top-level "Hero" entity (id-path len 1)
    CHECK(scenetree::find_node(tree.model(), kSelectedIdentity) != nullptr);

    const auto sel_t0 = std::chrono::steady_clock::now();
    CHECK(tree.select(kSelectedIdentity)); // the real F2 selection event path
    const auto sel_t1 = std::chrono::steady_clock::now();
    selection_loop_ns = elapsed_ns(sel_t0, sel_t1);
    CHECK(selection_events == 1); // the loop seam fired (the host times input->paint around it)
    CHECK(observed_selection.identity == kSelectedIdentity);

    // F3 inspector: build the editable projection of the SELECTED composed entity. The model addresses
    // the real entity (root scene + id-path); the current /name value is read from the composed world.
    const serializer::JsonValue* selected_value = nullptr;
    for (const compose::ComposedEntity& e : scene.entities)
    {
        if (e.id_path.size() == 1 && e.id_path[0] == kSelectedIdentity)
        {
            selected_value = &e.value;
        }
    }
    CHECK(selected_value != nullptr);
    const serializer::JsonValue* name_value =
        selected_value != nullptr ? compose::resolve_json_pointer(*selected_value, "/name") : nullptr;
    CHECK(name_value != nullptr && name_value->string_value == "Hero");

    inspector::InspectorModel imodel;
    imodel.root_scene = scene.root_path;
    imodel.id_path = {kSelectedIdentity};
    imodel.identity = kSelectedIdentity;
    imodel.kind_id = "ctx:scene";
    imodel.has_entity = true;
    {
        inspector::InspectorField name_field;
        name_field.pointer = "/name";
        name_field.label = "name";
        name_field.kind = inspector::WidgetKind::text;
        name_field.value = jstr("Hero");
        name_field.editable = true;
        imodel.fields.push_back(std::move(name_field));
    }

    // The gateway = the ONE `context set` write path (the F3 commit AND the F7 undo replay route through
    // it). Seed it with the field's current on-disk value so the CAS token is coherent.
    WalkthroughGateway gateway;
    gateway.file_hash = 100;
    gateway.field_values["/name"] = jstr("Hero");

    inspector::InspectorPanel ipanel(&gateway);
    ipanel.set_model(imodel, /*base_raw_hash=*/100);
    CHECK(ipanel.has_selection());

    // The R-HUX-011 COMMIT loop: a commit listener is the seam the host times input->commit latency
    // around (the selection/commit loop other panels consume).
    std::optional<inspector::CommitResult> observed_commit;
    ipanel.add_commit_listener(
        [&](const inspector::CommitResult& r) { observed_commit = r; });

    // === INSPECT — F4 Problems panel groups the live diagnostics ======================================
    problems::ProblemsPanel problems_panel;
    {
        std::vector<problems::ProblemDiagnostic> diags;
        problems::ProblemDiagnostic warn;
        warn.code = "compose.override_shadowed";
        warn.message = "override shadows a later template value";
        warn.severity = problems::Severity::warning;
        warn.nav.file = "root.scene.json";
        warn.nav.line = 4;
        warn.stability = context::editor::bridge::Stability::settling; // provisional while the world churns
        warn.generation = 7;
        diags.push_back(warn);
        problems_panel.set_diagnostics(std::move(diags));
    }
    CHECK(problems_panel.model().total == 1);
    CHECK(problems_panel.model().warnings == 1);
    CHECK(problems_panel.model().provisional == 1); // provisional while settling
    // On derivation.settled the provisional diagnostic is PROMOTED to stable (R-BRIDGE-008 / R-FILE-003).
    problems_panel.on_derivation_settled(7, context::editor::bridge::Stability::stable);
    CHECK(problems_panel.model().provisional == 0);

    // === PLAY IT — F5 playbar starts a session over the edit-state VIEW (L-51) ========================
    WalkthroughSession session;
    session.drawables = 4;
    session.directional_lights = 1;
    playbar::PlaybarModel playbar_model(&session);

    int control_events = 0;
    playbar_model.add_control_listener(
        [&](const playbar::PlayControlEvent&) { ++control_events; });

    const playbar::PlayAction played = playbar_model.play();
    CHECK(played.ok);
    CHECK(playbar_model.state() == playbar::PlayState::playing);
    CHECK(playbar_model.is_running());
    CHECK(control_events == 1);
    // Advance a few fixed ticks — the produced frame is the observed play output (F1 viewport source).
    CHECK(playbar_model.step(3).ok);
    CHECK(playbar_model.sim_tick() == 3);
    CHECK(playbar_model.last_frame().snapshot.items.size() == 4u);          // 4 drawables in the play frame
    CHECK(playbar_model.last_frame().snapshot.directional_lights.size() == 1u);

    // === MAKE AN OVERRIDE EDIT — F3 inspector, captured as an undo checkpoint (F7) ====================
    undo::UndoJournal journal(&gateway);

    CHECK(ipanel.stage_edit("/name", jstr("Villain")));
    CHECK(ipanel.has_staged_edit());

    const auto commit_t0 = std::chrono::steady_clock::now();
    const inspector::CommitResult commit = ipanel.commit(); // the real L-20/L-30 CAS-guarded write
    const auto commit_t1 = std::chrono::steady_clock::now();
    inspector_commit_loop_ns = elapsed_ns(commit_t0, commit_t1);

    CHECK(commit.status == inspector::CommitResult::Status::applied);
    CHECK(observed_commit.has_value()); // the R-HUX-011 commit loop seam fired
    CHECK(value_equal(gateway.field_values["/name"], jstr("Villain"))); // the edit landed on the write path
    CHECK(ipanel.base_raw_hash() == 101);                               // the CAS token advanced

    // F7: capture the gesture as an undo checkpoint (one field per gesture, the inspector's common case).
    undo::FieldEdit edit;
    edit.root_scene = scene.root_path;
    edit.id_path = {kSelectedIdentity};
    edit.pointer = "/name";
    edit.before = jstr("Hero");
    edit.after = jstr("Villain");
    journal.capture(edit);
    CHECK(journal.can_undo());

    // === UNDO — F7 reverts through the SAME CAS-guarded write path (never a blind clobber) ============
    const undo::ReplayResult undone = journal.undo();
    CHECK(undone.status == undo::ReplayResult::Status::applied);
    CHECK(undone.ok());
    CHECK(value_equal(gateway.field_values["/name"], jstr("Hero"))); // reverted on the write path
    CHECK(!journal.can_undo());
    CHECK(journal.can_redo()); // the reverted checkpoint is redoable

    // Redo re-applies through the same path (round-trips cleanly).
    const undo::ReplayResult redone = journal.redo();
    CHECK(redone.status == undo::ReplayResult::Status::applied);
    CHECK(value_equal(gateway.field_values["/name"], jstr("Villain")));

    // === VIEWPORT — F1 observes the play frame (the SAME snapshot; no second render path) =============
    viewport::ViewportPanel vp;

    // The R-HUX-011 GESTURE->VIEWPORT loop: a view re-frame fires the update seam the host times
    // input->paint latency around.
    int view_updates = 0;
    vp.add_view_update_listener([&](const viewport::ViewportUpdate&) { ++view_updates; });

    // The play output flows straight into the viewport — PlayFrame::snapshot IS render::RenderSnapshot.
    vp.set_snapshot(playbar_model.last_frame().snapshot);
    CHECK(vp.scene().drawables == 4u);          // the SAME 4 drawables the playbar produced (F5 -> F1)
    CHECK(vp.scene().directional_lights == 1u);

    // Composite through the L-41 surface handoff — a presentable frame within the T1 feature set. The
    // native-vs-web golden-scene SSIM equivalence is the render / render-web CI-job gate (goldens/);
    // here we assert the observer's present seam is a valid L-41 handoff on this platform.
    compositor::SurfaceCapabilities caps; // gpu_compositing on by default
    vp.set_present_env(compositor::current_platform(), caps, /*adapter_available=*/true,
                       /*scene_render_ok=*/true, /*width=*/1280, /*height=*/720);
    CHECK(vp.present().ok);
    CHECK(vp.present().width == 1280u && vp.present().height == 720u);
    CHECK(!vp.present().handoff.external_begin_frame); // L-41 forbids SendExternalBeginFrame

    const auto view_t0 = std::chrono::steady_clock::now();
    vp.frame_scene(); // the real F1 gesture->viewport re-frame
    const auto view_t1 = std::chrono::steady_clock::now();
    gesture_viewport_loop_ns = elapsed_ns(view_t0, view_t1);
    CHECK(view_updates == 1);            // the loop seam fired
    CHECK(vp.view_generation() == 1u);

    // === STOP — F5 discards the runtime session state (L-51: never written to files) ==================
    CHECK(playbar_model.stop().ok);
    CHECK(playbar_model.state() == playbar::PlayState::edit);
    CHECK(!session.running);

    // === Every panel in the journey renders an a11y-clean, keyboard-navigable surface =================
    // (The dedicated all-panels + coverage-manifest gate is m5-exit-2; this asserts the panels the
    // walkthrough actually drove are conformant in the states it drove them to.)
    const uitree::Panel tree_ui = tree.build_panel();
    const uitree::Panel inspector_ui = ipanel.build_panel();
    const uitree::Panel problems_ui = problems_panel.build_panel();
    const uitree::Panel viewport_ui = vp.build_panel();
    const uitree::Panel playbar_ui = playbar::build_playbar_panel(playbar_model);
    const uitree::Panel undo_ui = journal.build_panel();
    for (const uitree::Panel* ui :
         {&tree_ui, &inspector_ui, &problems_ui, &viewport_ui, &playbar_ui, &undo_ui})
    {
        CHECK(uitree::audit_a11y(*ui).empty());     // R-A11Y-001 semantic/ARIA + reachable commands
        CHECK(!uitree::render_html(*ui).empty());   // the DOM the CEF host paints
    }

    // === R-HUX-011 — the three human-interaction loops were MEASURED (recorded, SHOULD) ===============
    // Measured from instrumented steady_clock timestamps in the real event path above (NOT synthetic
    // benchmarks). A measurement, not a threshold gate: assert each loop fired + produced a real
    // (non-negative, finite) reading. The normative budget table lives in docs/human-latency-budget.md.
    CHECK(selection_loop_ns >= 0);
    CHECK(gesture_viewport_loop_ns >= 0);
    CHECK(inspector_commit_loop_ns >= 0);
    std::printf("[m5-exit] R-HUX-011 human-loop latency (measured, SHOULD; ns): "
                "selection=%lld gesture->viewport=%lld inspector-commit=%lld\n",
                selection_loop_ns, gesture_viewport_loop_ns, inspector_commit_loop_ns);

    M5_EXIT_MAIN_END();
}
