// M5 exit criterion 3 — the M5 OBSERVER-EDITOR SEAM CHECKLIST as an executable audit (ROADMAP §1 M5
// Exit / issue #168), registered as the blocking `m5-exit-3-seam-checklist` ctest and run by the CI
// build job's "M5 exit gate" named step on all three build-matrix legs. The milestone-closing mirror of
// the M2 data-model seam checklist (m2-exit-6): "the M5 editor-GUI seam set is IN the frozen M5 surface —
// M5 does not exit with any of them missing." One assertion per seam, exercising the real public
// surface, so a regression that quietly drops a seam turns this milestone gate red. The seams
// (decomposition 2026-07-10 § Seams; ROADMAP §1 M5; REQUIREMENTS R-EDIT-001 / R-UI-007 / R-A11Y-001 /
// R-HUX-011 / R-PLAY-* / R-HUX-005; DESIGN L-41 / L-51 / L-22 / L-35 / L-30 / L-20 / R-HUX-001):
//
//   1  R-EDIT-001 extension contract — every built-in panel is built ON it; deny-by-default trust boundary
//   2  L-41 per-platform CEF surface-handoff seam — never SendExternalBeginFrame
//   3  the headless UI-logic tree + a11y-harness hook (R-A11Y-001) — CI-assertable without CEF
//   4  F2 scene-tree observer over the composed derived world (L-35) + the selection loop
//   5  F3 inspector override-write through the ONE `context set` path, L-20 gesture-end / L-30 rebase-or-drop
//   6  F4 Problems observer — grouped diagnostics + provisional->stable promotion; MINTS NO codes
//   7  F5 play-in-editor SessionControl seam — L-51 edit/play split + L-22 hot-reload classification
//   8  F1 viewport observer over context_render — the SAME RenderSnapshot the play output produces (no 2nd path)
//   9  F7 GUI session undo/redo — replays through the SAME CAS-guarded path, R-HUX-001 no-blind-clobber
//   10 F6 a11y coverage — registered_panels() ⟷ coverage.manifest.jsonl; every registered panel a11y-clean
//   11 R-HUX-011 human-loop seams — the selection / gesture->viewport / inspector-commit listener seams
//   12 error-catalog minting discipline — viewport.* + play.* are the reserved minted domains (pinned strings)

#include "context/editor/gui/a11y/harness.h"
#include "context/editor/gui/a11y/registry.h"
#include "context/editor/gui/compositor/surface.h"
#include "context/editor/gui/contract/registry.h"
#include "context/editor/gui/panels/inspector/inspector_panel.h"
#include "context/editor/gui/panels/problems/problems_panel.h"
#include "context/editor/gui/panels/scenetree/scene_tree_model.h"
#include "context/editor/gui/panels/scenetree/scene_tree_panel.h"
#include "context/editor/gui/playbar/playbar_model.h"
#include "context/editor/gui/playbar/session_control.h"
#include "context/editor/gui/session/undo/undo_journal.h"
#include "context/editor/gui/uitree/builtin.h"
#include "context/editor/gui/uitree/panel.h"
#include "context/editor/gui/viewport/viewport_model.h"
#include "context/editor/gui/viewport/viewport_panel.h"

#include "context/editor/compose/compose_write.h"
#include "context/editor/compose/flatten.h"
#include "context/editor/compose/scene_model.h"
#include "context/editor/serializer/canonical.h"

#include "context/render/render_world.h"

#include "m5_exit_test.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace a11y = context::editor::gui::a11y;
namespace compose = context::editor::compose;
namespace compositor = context::editor::gui::compositor;
namespace contract = context::editor::gui::contract;
namespace inspector = context::editor::gui::panels::inspector;
namespace problems = context::editor::gui::panels::problems;
namespace scenetree = context::editor::gui::panels::scenetree;
namespace playbar = context::editor::gui::playbar;
namespace undo = context::editor::gui::session::undo;
namespace uitree = context::editor::gui::uitree;
namespace viewport = context::editor::gui::viewport;
namespace render = context::render;
namespace serializer = context::editor::serializer;

using m5exit::jstr;
using m5exit::value_equal;
using m5exit::WalkthroughGateway;

namespace
{

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

// A SessionControl double yielding a real render::RenderSnapshot (seam 7/8).
class SeamSession final : public playbar::SessionControl
{
public:
    std::uint32_t drawables = 2;
    bool running = false;
    std::uint64_t tick = 0;
    playbar::ControlOutcome start() override
    {
        running = true;
        playbar::ControlOutcome o;
        o.ok = true;
        o.frame.snapshot.items.resize(drawables);
        return o;
    }
    playbar::ControlOutcome step(std::uint64_t t) override
    {
        tick += t;
        playbar::ControlOutcome o;
        o.ok = true;
        o.frame.sim_tick = tick;
        o.frame.snapshot.sim_tick = tick;
        o.frame.snapshot.items.resize(drawables);
        return o;
    }
    void discard() override
    {
        running = false;
        tick = 0;
    }
    playbar::HotReloadOutcome apply_hot_reload(const playbar::LiveEdit& e) override
    {
        playbar::HotReloadOutcome o;
        o.ok = true;
        o.reload_class = e.shape_or_layout_change ? playbar::HotReloadClass::restart_class
                                                  : playbar::HotReloadClass::live_preserving;
        o.state_preserved = !e.shape_or_layout_change;
        return o;
    }
};

} // namespace

int main()
{
    // === Seam 1 — R-EDIT-001 extension contract: deny-by-default trust boundary ======================
    {
        contract::ExtensionRegistry registry;
        contract::Contribution panel;
        panel.id = "builtin.scene-tree";
        panel.kind = contract::ContributionKind::panel;
        panel.title = "Scene Tree";
        CHECK(registry.register_contribution(panel).ok); // a built-in panel registers on the contract
        CHECK(registry.contains("builtin.scene-tree"));

        // Deny an out-of-window contract version (mirrors R-CLI-010 protocol negotiation).
        contract::Contribution bad_version;
        bad_version.id = "ext.future";
        bad_version.contract_version = contract::kContractMajor + 1;
        const contract::RegistrationResult r1 = registry.register_contribution(bad_version);
        CHECK(!r1.ok && r1.error_code == contract::kErrUnsupportedContractVersion);

        // Deny a non-conformant renderer sandbox (Node integration ON is never allowed).
        contract::Contribution bad_sandbox;
        bad_sandbox.id = "ext.unsafe";
        bad_sandbox.sandbox.node_integration = true;
        const contract::RegistrationResult r2 = registry.register_contribution(bad_sandbox);
        CHECK(!r2.ok && r2.error_code == contract::kErrSandboxNonconformant);

        // The default sandbox is conformant + least-privilege (read/query baseline, no ambient write).
        CHECK(contract::sandbox_conformant(contract::SandboxPolicy{}));
    }

    // === Seam 2 — L-41 per-platform CEF surface-handoff seam (never external_begin_frame) ============
    {
        compositor::SurfaceCapabilities gpu; // gpu_compositing on
        CHECK(compositor::select_mode(compositor::HostPlatform::windows, gpu) ==
              compositor::CompositingMode::accelerated_osr);
        compositor::SurfaceCapabilities linux_def; // no mesa_x11_ozone
        CHECK(compositor::select_mode(compositor::HostPlatform::linux_, linux_def) ==
              compositor::CompositingMode::software_osr);
        CHECK(compositor::select_mode(compositor::HostPlatform::macos, gpu) ==
              compositor::CompositingMode::iosurface);
        for (const compositor::HostPlatform p :
             {compositor::HostPlatform::windows, compositor::HostPlatform::macos,
              compositor::HostPlatform::linux_})
        {
            CHECK(!compositor::make_handoff(p, gpu).external_begin_frame); // L-41 forbids it everywhere
        }
    }

    // === Seam 3 — the headless UI-logic tree + a11y-harness hook (R-A11Y-001), no CEF ================
    {
        const uitree::Panel placeholder = uitree::make_placeholder_panel();
        CHECK(uitree::audit_a11y(placeholder).empty());   // a11y-clean by construction
        CHECK(!uitree::focus_order(placeholder).empty()); // the command has a keyboard path
        CHECK(!uitree::render_html(placeholder).empty());  // the DOM the CEF host paints
    }

    // === Seam 4 — F2 scene-tree observer over the composed derived world (L-35) + selection ==========
    {
        MapResolver r;
        CHECK(r.add("root.scene.json", R"({"$schema": "ctx:scene", "version": 1, "entities": [
          {"id": "ccccccccccccccc1", "name": "Hero", "components": {"transform": {"position": [0,0,0]}}}]})"));
        const compose::ComposedScene scene = compose::flatten("root.scene.json", r);
        CHECK(scene.ok);
        scenetree::SceneTreePanel tree;
        tree.set_model(scenetree::build_scene_tree(scene));
        CHECK(tree.model().entity_count == 1);
        int events = 0;
        tree.add_selection_listener([&](const scenetree::SceneSelection&) { ++events; });
        CHECK(tree.select("ccccccccccccccc1")); // the selection loop seam
        CHECK(events == 1);
        CHECK(tree.selection().identity == "ccccccccccccccc1");
    }

    // === Seam 5 — F3 inspector override-write through the ONE path, L-20/L-30 (applied + drop) ========
    {
        // Applied: base hash matches -> the override write lands.
        WalkthroughGateway gw;
        gw.file_hash = 100;
        gw.field_values["/name"] = jstr("Old");
        compose::WriteRequest req;
        req.root_scene = "root.scene.json";
        req.id_path = {"ccccccccccccccc1"};
        req.pointer = "/name";
        req.value = jstr("New");
        const inspector::CommitResult applied = inspector::commit_override_write(
            gw, req, req.root_scene, req.id_path, req.pointer, jstr("Old"), /*base_raw_hash=*/100);
        CHECK(applied.status == inspector::CommitResult::Status::applied);
        CHECK(value_equal(gw.field_values["/name"], jstr("New")));

        // L-30 DROP: a concurrent writer changed THIS field -> dropped loudly (never a blind clobber),
        // reusing the EXISTING cas.mismatch catalog code (F3 mints none).
        WalkthroughGateway gw2;
        gw2.file_hash = 105; // != base 100 -> CAS mismatch
        gw2.field_values["/name"] = jstr("Hijacked");
        const inspector::CommitResult dropped = inspector::commit_override_write(
            gw2, req, req.root_scene, req.id_path, req.pointer, jstr("Old"), /*base_raw_hash=*/100);
        CHECK(dropped.status == inspector::CommitResult::Status::dropped);
        CHECK(dropped.code == "cas.mismatch");
        // The CAS-guarded attempt was REFUSED by the hash mismatch, so the co-writer's value was never
        // overwritten (the L-30 no-silent-overwrite invariant) — the drop is loud, not a clobber.
        CHECK(value_equal(gw2.field_values["/name"], jstr("Hijacked")));
    }

    // === Seam 6 — F4 Problems observer: grouping + provisional->stable promotion; MINTS NO codes =====
    {
        problems::ProblemsPanel panel;
        std::vector<problems::ProblemDiagnostic> diags;
        problems::ProblemDiagnostic d;
        d.code = "schema.unknown_field"; // an EXISTING code — the panel mints none
        d.message = "unknown field";
        d.severity = problems::Severity::error;
        d.nav.file = "root.scene.json";
        d.stability = context::editor::bridge::Stability::settling;
        d.generation = 3;
        diags.push_back(d);
        panel.set_diagnostics(std::move(diags));
        CHECK(panel.model().total == 1);
        CHECK(panel.model().provisional == 1);
        panel.on_derivation_settled(3, context::editor::bridge::Stability::stable);
        CHECK(panel.model().provisional == 0); // promoted to stable on settle
    }

    // === Seam 7 — F5 play-in-editor SessionControl seam: L-51 edit/play + L-22 hot reload ============
    {
        SeamSession control;
        control.drawables = 2;
        playbar::PlaybarModel model(&control);
        CHECK(model.state() == playbar::PlayState::edit);
        CHECK(model.play().ok);
        CHECK(model.state() == playbar::PlayState::playing); // L-51: a live session over the edit view
        CHECK(control.running);
        // L-22: a data-value edit is live-preserving; a shape/layout change is restart-class.
        CHECK(model.hot_reload(playbar::LiveEdit{"/name", false}).reload_class ==
              playbar::HotReloadClass::live_preserving);
        CHECK(model.hot_reload(playbar::LiveEdit{"/components/x", true}).reload_class ==
              playbar::HotReloadClass::restart_class);
        CHECK(model.stop().ok);
        CHECK(model.state() == playbar::PlayState::edit); // L-51: runtime state discarded on stop
        CHECK(!control.running);
        // A control issued in edit state fail-closes with the reserved play.not_running code.
        CHECK(model.step(1).error_code == playbar::kPlayNotRunningCode);
    }

    // === Seam 8 — F1 viewport observer: the SAME RenderSnapshot the play output produces (no 2nd path)
    {
        SeamSession control;
        control.drawables = 5;
        playbar::PlaybarModel model(&control);
        CHECK(model.play().ok);
        CHECK(model.step(1).ok);
        viewport::ViewportPanel vp;
        vp.set_snapshot(model.last_frame().snapshot); // PlayFrame::snapshot IS render::RenderSnapshot
        CHECK(vp.scene().drawables == 5u);             // F5 -> F1 with no second render path
        vp.set_present_env(compositor::HostPlatform::linux_, compositor::SurfaceCapabilities{},
                           /*adapter_available=*/true, /*scene_render_ok=*/true, 640, 480);
        CHECK(vp.present().ok);
        // Adapter absent -> the observer REPORTS it (R-HEAD-002), never fabricates a frame.
        vp.set_present_env(compositor::HostPlatform::linux_, compositor::SurfaceCapabilities{},
                           /*adapter_available=*/false, /*scene_render_ok=*/true, 640, 480);
        CHECK(vp.present().error_code == viewport::kViewportAdapterAbsentCode);
    }

    // === Seam 9 — F7 undo/redo replays through the SAME CAS path (R-HUX-001 no-blind-clobber) ========
    {
        // Clean revert.
        WalkthroughGateway gw;
        gw.file_hash = 100;
        gw.field_values["/name"] = jstr("New");
        undo::UndoJournal journal(&gw);
        undo::FieldEdit e;
        e.root_scene = "root.scene.json";
        e.id_path = {"ccccccccccccccc1"};
        e.pointer = "/name";
        e.before = jstr("Old");
        e.after = jstr("New");
        journal.capture(e);
        CHECK(journal.undo().status == undo::ReplayResult::Status::applied);
        CHECK(value_equal(gw.field_values["/name"], jstr("Old")));

        // R-HUX-001: a co-writer changed THIS field -> undo DROPS, never overwrites (reuses cas.mismatch).
        WalkthroughGateway gw2;
        gw2.file_hash = 105;
        gw2.field_values["/name"] = jstr("Hijacked");
        undo::UndoJournal journal2(&gw2);
        journal2.capture(e);
        const undo::ReplayResult r = journal2.undo();
        CHECK(r.status == undo::ReplayResult::Status::dropped);
        CHECK(r.edits.size() == 1 && r.edits[0].code == "cas.mismatch");
        CHECK(gw2.attempts == 0); // never a write -> the co-writer is not clobbered
        CHECK(value_equal(gw2.field_values["/name"], jstr("Hijacked")));
    }

    // === Seam 10 — F6 a11y coverage: registry non-empty + every registered panel a11y-clean ==========
    {
        const std::vector<a11y::RegisteredPanel> panels = a11y::registered_panels();
        CHECK(!panels.empty());
        bool saw_problems = false;
        for (const a11y::RegisteredPanel& rp : panels)
        {
            const a11y::PanelReport report = a11y::scan_panel(rp.id, rp.factory());
            CHECK(report.passed); // a11y-clean + every command keyboard-reachable
            if (rp.id == "builtin.problems")
                saw_problems = true;
        }
        CHECK(saw_problems); // the M5-exit coverage completion registered F4 Problems
    }

    // === Seam 11 — R-HUX-011 human-loop seams: the three interaction-loop listeners fire =============
    {
        // selection loop (F2)
        MapResolver r;
        CHECK(r.add("root.scene.json", R"({"$schema": "ctx:scene", "version": 1, "entities": [
          {"id": "ccccccccccccccc1", "name": "Hero"}]})"));
        scenetree::SceneTreePanel tree;
        tree.set_model(scenetree::build_scene_tree(compose::flatten("root.scene.json", r)));
        int sel = 0;
        tree.add_selection_listener([&](const scenetree::SceneSelection&) { ++sel; });
        CHECK(tree.select("ccccccccccccccc1"));
        CHECK(sel == 1);

        // gesture->viewport loop (F1)
        viewport::ViewportPanel vp;
        int views = 0;
        vp.add_view_update_listener([&](const viewport::ViewportUpdate&) { ++views; });
        vp.frame_scene();
        CHECK(views == 1);

        // inspector-commit loop (F3)
        WalkthroughGateway gw;
        gw.field_values["/name"] = jstr("Old");
        inspector::InspectorModel m;
        m.root_scene = "root.scene.json";
        m.id_path = {"ccccccccccccccc1"};
        m.identity = "ccccccccccccccc1";
        m.has_entity = true;
        {
            inspector::InspectorField f;
            f.pointer = "/name";
            f.label = "name";
            f.kind = inspector::WidgetKind::text;
            f.value = jstr("Old");
            f.editable = true;
            m.fields.push_back(std::move(f));
        }
        inspector::InspectorPanel ipanel(&gw);
        ipanel.set_model(m, 100);
        int commits = 0;
        ipanel.add_commit_listener([&](const inspector::CommitResult&) { ++commits; });
        CHECK(ipanel.stage_edit("/name", jstr("New")));
        CHECK(ipanel.commit().status == inspector::CommitResult::Status::applied);
        CHECK(commits == 1);
    }

    // === Seam 12 — error-catalog minting discipline: viewport.* + play.* are the reserved domains =====
    {
        // The M5 waves minted exactly two reserved code domains (viewport.* by F1, play.* by F5); their
        // strings are pinned here so a rename desyncs from src/editor/contract/src/error_catalog.cpp.
        CHECK(std::string(viewport::kViewportAdapterAbsentCode) == "viewport.adapter_absent");
        CHECK(std::string(viewport::kViewportSurfaceUnavailableCode) == "viewport.surface_unavailable");
        CHECK(std::string(viewport::kViewportRenderFailedCode) == "viewport.render_failed");
        CHECK(std::string(playbar::kPlayNotRunningCode) == "play.not_running");
        CHECK(std::string(playbar::kPlaySessionFailedCode) == "play.session_failed");
        CHECK(std::string(playbar::kPlayStepFailedCode) == "play.step_failed");
        CHECK(std::string(playbar::kPlayHotReloadFailedCode) == "play.hot_reload_failed");
    }

    M5_EXIT_MAIN_END();
}
