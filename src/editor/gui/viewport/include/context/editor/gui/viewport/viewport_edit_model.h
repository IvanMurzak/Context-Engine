// The in-context viewport override-editing MODEL (M8.5 a19, R-HUX-006 MUST core; R-CLI-006 / L-35 /
// L-20 / L-30 / R-A11Y-001): the headless, CEF-free logic of manipulating a COMPOSED scene-instance
// entity directly in the viewport so the edit lands as the correct L-35 OVERRIDE write with visible
// provenance — the GUI face of scene composition (L-35).
//
// A transform gizmo (move/rotate/scale) or a property edit on a composed entity becomes an override
// entry through the SAME composed write path `context set` runs (compose::plan_write) — default in the
// OUTERMOST instancing scene, retargetable to the defining template (--edit-template) or a mid-level
// instancing scene (--at-instance) as GUI affordances — NEVER a parallel write path. Session state
// (selection, the write target, the in-flight gesture) lives HERE in memory (L-20: authored state
// exists only in files; the GUI commits at gesture end, no Save button). The gesture-end commit routes
// through inspector::commit_override_write — the ONE L-30 rebase-or-drop engine the inspector commit
// and the session undo/redo replay also use. Provenance (R-CLI-006 read side) is surfaced at the point
// of edit via compose::provenance_for / provenance_json — the SAME emitter `context query` serializes
// through. The whole model is CI-assertable WITHOUT booting CEF (R-EDIT-001).

#pragma once

#include "context/editor/gui/panels/inspector/inspector_panel.h" // OverrideWriteGateway, CommitResult

#include "context/editor/compose/compose_write.h" // WriteTarget / WriteRequest
#include "context/editor/compose/flatten.h"       // ComposedScene / ProvenanceEntry / SceneResolver
#include "context/editor/serializer/json_tree.h"

#include <cstdint>
#include <string>
#include <vector>

namespace context::editor::gui::viewport
{

namespace compose = context::editor::compose;
namespace inspector = context::editor::gui::panels::inspector;
namespace serializer = context::editor::serializer;

// Which authored file an in-viewport edit writes into (R-CLI-006 / L-35) — the GUI affordance form of
// the compose::WriteTarget the write path selects. `outermost` (default) writes an override in the
// root instancing scene (it wins); `edit_template` writes the entity's authored value in its defining
// scene; `at_instance` writes an override in a mid-level instancing scene named by an id-path prefix.
enum class EditTarget
{
    outermost,
    edit_template,
    at_instance,
};

// A viewport manipulation gizmo (R-HUX-006). `move` translates the entity's transform position;
// `rotate` / `scale` set the addressed rotation / scale field. All commit through the ONE override
// write path (a gizmo introduces the field as an override even if the template omits it, per L-35).
enum class Gizmo
{
    move,
    rotate,
    scale,
};

// The transform-component pointers the gizmos address (schema v1 transforms author `position`; rotate
// / scale address their own fields when the composed entity carries them).
inline constexpr const char* kPositionPointer = "/components/transform/position";
inline constexpr const char* kRotationPointer = "/components/transform/rotation";
inline constexpr const char* kScalePointer = "/components/transform/scale";

// One selectable composed entity, as the viewport lists it (mirrors scenetree's SceneSelection key).
struct ComposedEntityRef
{
    std::string identity;             // the id-path joined with '/', the selection key
    std::uint64_t identity_hash = 0;  // L-37 composed identity hash
    std::vector<std::string> id_path; // the L-35 id-path from the flatten root
    bool instanced = false;           // came from an instanced sub-scene (id_path.size() > 1)
    std::string name;                 // the entity's authored `name` (display; empty when absent)
};

class ViewportEditModel
{
public:
    ViewportEditModel() = default;

    // Load: flatten `root_scene` via `resolver`; retain the composed entities for selection +
    // provenance. Returns false when the root scene does not resolve to any composed entity. Clears
    // any selection + in-flight gesture. Re-open()ing after a commit is how fresh composed state is
    // observed (the disk resolver snapshots on-disk reads — pass a FRESH resolver).
    bool open(const compose::SceneResolver& resolver, std::string root_scene,
              const compose::ComposeLimits& limits = {});

    [[nodiscard]] bool loaded() const noexcept { return loaded_; }
    [[nodiscard]] const std::string& root_scene() const noexcept { return root_scene_; }
    [[nodiscard]] const std::vector<ComposedEntityRef>& entities() const noexcept { return refs_; }

    // --- selection (session state, L-20) ---------------------------------------------------------
    // Select the composed entity whose id-path joins to `identity`. Cancels any in-flight gesture.
    bool select(const std::string& identity);
    void clear_selection();
    [[nodiscard]] bool has_selection() const noexcept { return has_selection_; }
    [[nodiscard]] const std::string& selection_identity() const noexcept { return selection_identity_; }
    [[nodiscard]] const std::vector<std::string>& selected_id_path() const;
    // The selected entity's current COMPOSED value at `pointer` (overrides applied), or nullptr.
    [[nodiscard]] const serializer::JsonValue* value_at(const std::string& pointer) const;
    // True iff an override contributor touched `pointer` inside the selected entity.
    [[nodiscard]] bool overridden_at(const std::string& pointer) const;

    // --- provenance (R-CLI-006 read side, rendered at the point of edit) --------------------------
    // The winning-value-first provenance chain for the selected entity's `pointer` — which template
    // supplied the value and which instancing level overrode it. Empty without a selection.
    [[nodiscard]] std::vector<compose::ProvenanceEntry> provenance(const std::string& pointer) const;
    // The chain as canonical JSON — the SAME emitter `context query` serializes provenance through
    // (compose::provenance_json), so the display matches the query surface by construction.
    [[nodiscard]] std::string provenance_json(const std::string& pointer) const;

    // --- write target (R-CLI-006 retarget affordances) -------------------------------------------
    void set_edit_target(EditTarget target) noexcept { edit_target_ = target; }
    [[nodiscard]] EditTarget edit_target() const noexcept { return edit_target_; }
    // The mid-level addressing prefix (used only when edit_target == at_instance): a strict, non-empty
    // prefix of the selected id-path naming the mid-level instancing scene.
    void set_at_instance(std::vector<std::string> prefix) { at_instance_ = std::move(prefix); }
    [[nodiscard]] const std::vector<std::string>& at_instance() const noexcept { return at_instance_; }

    // --- the L-20 gizmo gesture lifecycle --------------------------------------------------------
    void set_gizmo(Gizmo gizmo) noexcept { gizmo_ = gizmo; }
    [[nodiscard]] Gizmo gizmo() const noexcept { return gizmo_; }
    // The transform pointer the current gizmo addresses.
    [[nodiscard]] const char* gizmo_pointer() const noexcept;
    [[nodiscard]] bool gesture_active() const noexcept { return gesture_active_; }
    [[nodiscard]] const std::string& gesture_pointer() const noexcept { return gesture_pointer_; }
    [[nodiscard]] const serializer::JsonValue& pending_value() const noexcept { return pending_; }

    // Begin a gesture for the current gizmo on the selected entity. Snapshots the field's current
    // composed value (the L-30 collision base) and — on the outermost path — the root scene file's raw
    // hash (the CAS token) via the gateway. NOTHING touches disk (L-20). Returns false without a
    // selection or when the gizmo's field does not resolve in the composed entity.
    bool begin_gesture(const inspector::OverrideWriteGateway& gateway);
    // move gizmo: translate the pending position by (dx, dy, dz). Integer results stay integer-typed
    // (authored positions are integers; keeps `context set '[..]'` byte-parity). No-op without an
    // active gesture on a 3-element numeric array.
    void translate(double dx, double dy, double dz);
    // Any gizmo / property edit: set the pending value directly (rotate/scale/a property literal).
    void set_pending_value(serializer::JsonValue value);
    void cancel_gesture();

    // Commit the in-flight gesture at gesture end (L-20): build the WriteRequest for the selected
    // target + gizmo pointer + pending value and route it through inspector::commit_override_write —
    // the ONE L-30 rebase-or-drop engine (the SAME `context set` path). Consumes the gesture on
    // apply/rebase/drop; keeps it on a write-path refusal (Status::error) so the caller can retry.
    // Does NOT re-flatten — re-open() with a fresh resolver to observe the committed state.
    inspector::CommitResult commit_gesture(const inspector::OverrideWriteGateway& gateway);

    // One-line status summary for the panel's live status region (R-A11Y-001: state surfaces as text).
    [[nodiscard]] std::string status_text() const;

private:
    [[nodiscard]] const compose::ComposedEntity* selected_entity() const;

    bool loaded_ = false;
    std::string root_scene_;
    compose::ComposedScene scene_;
    std::vector<ComposedEntityRef> refs_;

    bool has_selection_ = false;
    std::size_t selected_index_ = 0; // index into scene_.entities (valid iff has_selection_)
    std::string selection_identity_;

    EditTarget edit_target_ = EditTarget::outermost;
    std::vector<std::string> at_instance_;
    Gizmo gizmo_ = Gizmo::move;

    bool gesture_active_ = false;
    std::string gesture_pointer_;
    serializer::JsonValue collision_base_;
    serializer::JsonValue pending_;
    std::uint64_t base_raw_hash_ = 0;
};

} // namespace context::editor::gui::viewport
