// The inspector panel (M5-F3, R-EDIT-001 / R-DATA-002 / L-35 / L-20 / L-30 / R-CLI-005 / R-A11Y-001 /
// R-HUX-011): renders the selected entity's component schemas as a headless context_gui_uitree Panel
// of editable widgets, and commits edits as L-35 override writes through the EXISTING `context set`
// write path (the single source of truth — no parallel writer), CAS-guarded (L-20 gesture-end commit),
// applying the L-30 rebase-or-drop policy under a concurrent writer. The whole panel is CI-assertable
// WITHOUT booting CEF (R-EDIT-001 testable-by-construction editor).
//
// The panel commits through the OverrideWriteGateway seam: the disk-backed gateway (gui/viewport)
// implements it over compose::plan_write + filesync's atomic CAS write (the `context set` path);
// headless tests inject an in-memory implementation over compose::plan_write. The panel owns NO
// disk / no filesync / no compose dependency — only the pure BOUNDARY-CLEAN write envelope
// (OverrideWriteRequest, M9 e05d3) and the L-20/L-30 commit policy. Wiring the gateway to the live
// daemon write path (a WIRE gateway over RPC `edit` + `--if-match`) is e09's task, out of this
// headless panel's scope.

#pragma once

#include "context/editor/gui/panels/inspector/inspector_model.h"

#include "context/editor/gui/uitree/panel.h"

#include "context/editor/serializer/json_tree.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace context::editor::gui::panels::inspector
{

// Which authored file an override write lands in (the boundary-clean mirror of
// compose::WriteTarget — M9 e05d3, D10). `outermost` (the L-35 default) writes an override entry in
// the root instancing scene; `defining_template` edits the entity's authored value in its defining
// scene; `at_instance` writes an override in a mid-level instancing scene named by an id-path prefix.
enum class OverrideWriteTarget
{
    outermost,
    defining_template,
    at_instance,
};

// The boundary-clean override-write envelope (M9 e05d3, D10/D18 — owner ruling 2026-07-20): what
// the panel side of the OverrideWriteGateway seam hands a gateway, member-for-member the same shape
// as compose::WriteRequest but with NO kernel type, so the panel library carries no compose:: on its
// public link interface. Kernel-side gateway implementations convert through the ONE
// builders::to_write_request (gui/panels/builders/inspector_builder.h); a wire gateway (e09) maps it
// onto the daemon's RPC params directly.
struct OverrideWriteRequest
{
    std::string root_scene;           // the addressing (root) scene path
    std::vector<std::string> id_path; // L-35 id-path to the composed entity, from the root inward
    std::string pointer;              // RFC 6901 JSON pointer of the field inside the entity
    serializer::JsonValue value;      // the value to set
    OverrideWriteTarget target = OverrideWriteTarget::outermost;
    // target == at_instance: the id-path PREFIX naming the mid-level addressing scene (a strict,
    // non-empty prefix of id_path; the override addresses the entity by the remaining suffix).
    std::vector<std::string> at_instance;
};

// The result of ONE CAS-guarded override-write attempt through the `context set` write path.
struct WriteAttempt
{
    bool applied = false;       // the override write landed
    bool cas_mismatch = false;  // a concurrent writer advanced the target file (the L-30 trigger)
    std::string code;           // a catalog code on a non-applied outcome (cas.mismatch, compose.*)
    std::string message;        // human/AI-readable detail
    std::string file;           // envelope: the file the override landed in (on apply)
    std::string pointer;        // envelope: the JSON pointer written (on apply)
    std::uint64_t raw_hash = 0; // applied: the new file raw hash; cas_mismatch: the CURRENT raw hash
};

// The current state of one composed field, used to snapshot the gesture base + to judge an L-30
// field-path collision after a CAS mismatch (rebase iff the field is unchanged, drop iff it moved).
struct FieldState
{
    bool present = false;        // the field resolves in the current composed entity
    std::uint64_t raw_hash = 0;  // the outermost target file's CURRENT raw hash (the CAS token)
    serializer::JsonValue value; // the current composed value at the field (null when absent)
};

// The seam the inspector commits override writes through — the SAME `context set` write path (L-35
// single source of truth). Implemented by the disk-backed gateway (gui/viewport, over
// compose::plan_write + filesync's atomic CAS write — converting the boundary-clean request via
// builders::to_write_request) and by headless tests over an in-memory compose::plan_write; a WIRE
// implementation over the daemon's RPC arrives with e09. Both total (never throw). The request type
// is the boundary-clean OverrideWriteRequest (above), NEVER compose::WriteRequest — that is what
// keeps this header, and with it the whole panel library, off the kernel's link closure (D10).
class OverrideWriteGateway
{
public:
    virtual ~OverrideWriteGateway() = default;

    // Attempt the override write CAS-guarded on `expected_raw_hash` (the `--if-match` raw-byte hash).
    // On success: applied + the new file raw hash. On a concurrent-writer CAS mismatch: cas_mismatch +
    // the file's CURRENT raw hash. Any other failure carries a catalog `code` (compose.* / file.*).
    [[nodiscard]] virtual WriteAttempt attempt(const OverrideWriteRequest& request,
                                               std::uint64_t expected_raw_hash) const = 0;

    // The current composed value at (root_scene, id_path, pointer) + the outermost target file's
    // current raw hash — the L-30 re-read after a CAS mismatch (was this field path touched?).
    [[nodiscard]] virtual FieldState read(const std::string& root_scene,
                                          const std::vector<std::string>& id_path,
                                          const std::string& pointer) const = 0;
};

// The outcome of a gesture-end commit (L-20), applying the L-30 rebase-or-drop policy.
struct CommitResult
{
    enum class Status
    {
        none,    // no staged edit / no gateway — nothing was committed
        applied, // the write landed on the first CAS-guarded attempt
        rebased, // a concurrent writer touched an UNRELATED field -> rebased onto the new state (L-30)
        dropped, // a concurrent writer touched THIS field path -> dropped loudly, never overwrite (L-30)
        error,   // the write path refused the request (compose.write_target_not_found / immutable / …)
    };

    Status status = Status::none;
    std::string pointer;         // the field the gesture targeted
    std::string file;            // envelope: the override file (applied / rebased)
    std::string written_pointer; // envelope: the JSON pointer written (applied / rebased)
    std::uint64_t raw_hash = 0;  // the resulting file CAS token (applied / rebased)
    std::string code;            // dropped / error: the catalog code (cas.mismatch / compose.*)
    std::string message;         // human/AI-readable detail (the loud drop diagnostic, L-30)

    [[nodiscard]] bool ok() const noexcept { return status == Status::applied || status == Status::rebased; }
};

class InspectorPanel
{
public:
    // The R-EDIT-001 contribution id this built-in panel registers under.
    static constexpr const char* kContributionId = "builtin.inspector";
    // The command every editable widget binds so every edit has a keyboard/CLI path (R-A11Y-001 /
    // R-CLI-001: CLI-completeness as a structural accessibility property).
    static constexpr const char* kEditCommand = "inspector.edit";

    InspectorPanel() = default;
    explicit InspectorPanel(const OverrideWriteGateway* gateway) : gateway_(gateway) {}

    void set_gateway(const OverrideWriteGateway* gateway) noexcept { gateway_ = gateway; }

    // Set / refresh the inspected entity (the host resolves this from the F2 selection + the composed
    // world + the kind schema). `base_raw_hash` is the outermost target file's raw hash at snapshot
    // time — the CAS token a subsequent commit guards on. Clears any in-flight staged edit.
    void set_model(InspectorModel model, std::uint64_t base_raw_hash = 0);
    // Drop the selection (renders the no-selection placeholder).
    void clear();
    [[nodiscard]] const InspectorModel& model() const noexcept { return model_; }
    [[nodiscard]] bool has_selection() const noexcept { return model_.has_entity; }
    [[nodiscard]] std::uint64_t base_raw_hash() const noexcept { return base_raw_hash_; }

    // Stage an in-flight edit for one field (a gesture in progress; no write yet — L-20 commits at
    // gesture end). Records the field's base value so commit() can apply the L-30 decision. Returns
    // false for an unknown or non-editable field pointer — a readonly-KIND field is never editable
    // here, whatever a producer's `editable` claim said (the model layer owns that invariant).
    bool stage_edit(const std::string& pointer, serializer::JsonValue new_value);
    [[nodiscard]] bool has_staged_edit() const noexcept { return staged_.has_value(); }
    [[nodiscard]] const std::string& staged_pointer() const noexcept { return staged_pointer_; }
    void discard_edit();

    // Commit the staged edit at gesture end (L-20): an L-35 outermost override write through the
    // gateway (the `context set` path), CAS-guarded on the base hash. Under a concurrent writer
    // (cas.mismatch) applies L-30: REBASE onto the new state when this field path was untouched, or
    // DROP loudly (never a silent overwrite) when the same field path collided. Notifies every commit
    // listener with the outcome (the R-HUX-011 loop other panels consume). Returns Status::none when
    // there is no staged edit or no gateway.
    CommitResult commit();
    [[nodiscard]] const CommitResult& last_result() const noexcept { return last_result_; }

    // Build the headless uitree Panel for the current model: one focusable, command-bound widget per
    // present editable component field (textbox / checkbox), a11y-conformant by construction
    // (uitree::audit_a11y returns no violations) and deterministic (identical state -> byte-identical
    // render_html). A no-selection model renders an a11y-clean placeholder with no exposed command.
    [[nodiscard]] uitree::Panel build_panel() const;

    // Register a listener other surfaces use to react to a commit outcome — the applied/rebased write
    // AND the loud L-30 drop (R-HUX-011 selection/commit loop).
    using CommitListener = std::function<void(const CommitResult&)>;
    void add_commit_listener(CommitListener listener);

private:
    void notify(const CommitResult& result) const;

    const OverrideWriteGateway* gateway_ = nullptr;
    InspectorModel model_;
    std::uint64_t base_raw_hash_ = 0;

    // The in-flight gesture edit. `staged_pointer_` mirrors staged_->pointer for a cheap accessor.
    struct StagedEdit
    {
        std::string pointer;
        serializer::JsonValue new_value;
        serializer::JsonValue base_value; // the field value at stage time (the L-30 collision base)
    };
    std::optional<StagedEdit> staged_;
    std::string staged_pointer_;

    std::vector<CommitListener> listeners_;
    CommitResult last_result_;
};

// The shared L-20/L-30 override-write commit engine: attempt `request` through `gateway` CAS-guarded
// on `base_raw_hash`, then — under a concurrent-writer `cas.mismatch` — apply the L-30 rebase-or-drop
// policy at FIELD-PATH granularity. `collision_base` is the value the field is expected to currently
// hold (the caller's snapshot base): after a mismatch the engine re-reads (root_scene, id_path,
// pointer) and, if the current value STILL equals `collision_base`, the external change touched an
// UNRELATED field, so it rebases onto the new state and retries (bounded); if the current value
// DIFFERS, the same field path collided, so it drops LOUDLY (a `cas.mismatch` diagnostic, never a
// silent overwrite). Pure over the gateway seam, total (never throws), and owns NO panel/journal
// state — the ONE implementation of the L-30 policy, shared by the inspector's gesture commit and the
// session undo/redo replay (R-HUX-001: undo replays through the SAME CAS + rebase-or-drop path).
//
// The returned CommitResult carries `pointer` = `pointer`; on applied/rebased the landed file +
// written pointer + new raw hash; on a drop the current raw hash + the loud `cas.mismatch` code and
// message; on a write-path refusal (neither applied nor a CAS mismatch) Status::error + the code. The
// caller updates its own CAS token from `result.raw_hash` (applied/rebased/dropped) and decides
// whether to keep its staged/queued edit (an error keeps it; a drop consumes it).
[[nodiscard]] CommitResult commit_override_write(const OverrideWriteGateway& gateway,
                                                 const OverrideWriteRequest& request,
                                                 const std::string& root_scene,
                                                 const std::vector<std::string>& id_path,
                                                 const std::string& pointer,
                                                 const serializer::JsonValue& collision_base,
                                                 std::uint64_t base_raw_hash);

} // namespace context::editor::gui::panels::inspector
