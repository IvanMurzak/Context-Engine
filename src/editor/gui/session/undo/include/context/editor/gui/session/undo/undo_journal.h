// GUI session undo/redo over the file-write journal (M5-F7, R-HUX-001 / R-FILE-006 / R-FILE-007 /
// L-20 / L-21 / L-30): familiar Ctrl+Z / Ctrl+Y session undo with gesture-batch auto-checkpointing,
// scoped to the M5 shipped editing surface (the F3 inspector override writes). This is a SHORT-HORIZON
// session convenience, NOT an engine undo subsystem — durable/long-range history stays git (R-FILE-007
// / L-21); this layers on the ONE write path.
//
// The load-bearing safety property (R-HUX-001): undo/redo is replayed through the SAME write path as
// any other mutation — the serialized write queue, `--if-match` CAS, and the L-30 rebase-or-drop
// policy — so an undo can NEVER clobber a concurrent writer (human or AI). Each replay routes through
// inspector::commit_override_write (the one L-30 engine) plus an up-front collision guard: if the
// field moved since we wrote it, the undo drops LOUDLY (reusing `cas.mismatch`), it never restores
// stale bytes over a co-writer's change. A "restore previous bytes" undo is exactly what R-HUX-001
// forbids.
//
// The journal owns NO disk / no filesync dependency (mirroring the inspector panel): it commits
// through the inspector::OverrideWriteGateway seam and (de)serializes to a plain JSON tree the CEF
// host persists to the gitignored `.editor/session.json`. Headless + fault-injectable WITHOUT booting
// CEF (drive an in-memory gateway; R-QA-010 concurrency seams).

#pragma once

#include "context/editor/gui/panels/inspector/inspector_panel.h" // OverrideWriteGateway, CommitResult

#include "context/editor/gui/uitree/panel.h"

#include "context/editor/serializer/json_tree.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace context::editor::gui::session::undo
{

namespace inspector = context::editor::gui::panels::inspector;
namespace serializer = context::editor::serializer;
namespace uitree = context::editor::gui::uitree;

// One reversible field edit captured at gesture-commit time — the atom of the session journal. It
// records the L-35 addressing (root_scene + id-path + entity-relative pointer) plus the `before`
// (undo target) and `after` (redo target / the value the field is EXPECTED to currently hold) values.
// Deliberately self-contained (no InspectorModel handle) so a checkpoint survives selection changes
// and JSON round-trips to `.editor/session.json`.
struct FieldEdit
{
    std::string root_scene;               // the addressing (root) scene the override targets (L-35)
    std::vector<std::string> id_path;     // the L-35 id-path to the composed entity
    std::string pointer;                  // the entity-relative JSON pointer written
    serializer::JsonValue before;         // the value before the edit — the undo target
    serializer::JsonValue after;          // the value after the edit — the redo target / collision base
};

// A gesture checkpoint (L-20 gesture-batch auto-checkpointing): ONE undo step per gesture, not per
// keystroke. Usually a single field edit (the inspector commits one field per gesture), but a batched
// gesture may carry several — undo reverts them in reverse order, each with independent field-path
// L-30 collision handling.
struct Checkpoint
{
    std::string label;                    // human/AI-readable gesture label (may be empty)
    std::vector<FieldEdit> edits;
};

// The outcome of an undo/redo replay over one checkpoint: the aggregate status + the per-field
// results (each reusing inspector::CommitResult — undo/redo IS a set of override writes). Aggregate =
// dropped if any field collided, else error if any refused, else rebased if any rebased, else applied;
// `none` when there was nothing to replay / no gateway.
struct ReplayResult
{
    using Status = inspector::CommitResult::Status;

    Status status = Status::none;
    std::vector<inspector::CommitResult> edits; // per-field outcomes (undo: reverse-applied order)
    std::string label;                          // the replayed checkpoint's label

    [[nodiscard]] bool ok() const noexcept
    {
        return status == Status::applied || status == Status::rebased;
    }
};

// The session undo/redo journal over the inspector override-write surface. Records gesture
// checkpoints, replays undo/redo as CAS-guarded override writes through the gateway seam (never
// clobbering a concurrent writer), and (de)serializes to the gitignored `.editor/session.json`. Total
// and deterministic; owns no disk.
class UndoJournal
{
public:
    // The command ids the CEF host binds to Ctrl+Z / Ctrl+Y (R-CLI-001: every affordance has a
    // keyboard/CLI path). Stable + greppable.
    static constexpr const char* kUndoCommand = "session.undo";
    static constexpr const char* kRedoCommand = "session.redo";
    // The R-EDIT-001 contribution id this session surface registers under.
    static constexpr const char* kContributionId = "builtin.session.undo";
    // The `to_json` schema version (bumped if the on-disk journal shape changes).
    static constexpr int kJournalVersion = 1;

    UndoJournal() = default;
    explicit UndoJournal(const inspector::OverrideWriteGateway* gateway) : gateway_(gateway) {}
    void set_gateway(const inspector::OverrideWriteGateway* gateway) noexcept { gateway_ = gateway; }

    // --- recording (L-20 gesture-batch auto-checkpointing) ------------------------------------------
    // Push a fully-formed gesture checkpoint (empty checkpoints are ignored). Recording ANY new
    // gesture invalidates the redo stack (standard undo/redo semantics).
    void record(Checkpoint checkpoint);
    // Open a multi-edit gesture batch; subsequent capture() calls append to it until end_gesture().
    void begin_gesture(std::string label = {});
    // Append one edit to the open gesture batch, or — when no batch is open — auto-checkpoint it as a
    // lone single-edit gesture (the inspector's common one-field-per-gesture case).
    void capture(FieldEdit edit);
    // Close the open gesture batch, recording it as ONE checkpoint (no-op when nothing was captured).
    void end_gesture();

    [[nodiscard]] bool can_undo() const noexcept { return !undo_.empty(); }
    [[nodiscard]] bool can_redo() const noexcept { return !redo_.empty(); }
    [[nodiscard]] std::size_t undo_depth() const noexcept { return undo_.size(); }
    [[nodiscard]] std::size_t redo_depth() const noexcept { return redo_.size(); }

    // Undo the most recent checkpoint (Ctrl+Z): replay each field's `before` as a CAS-guarded override
    // write through the gateway, reverting in reverse order. A field a concurrent writer touched since
    // is DROPPED loudly (never clobbered, R-HUX-001). Only a cleanly-reverted checkpoint (every field
    // applied/rebased) moves onto the redo stack. `none` when there is nothing to undo / no gateway.
    ReplayResult undo();
    // Redo the most recently undone checkpoint (Ctrl+Y): replay each field's `after`, forward order,
    // same CAS + drop-on-collision guarantees. Only a cleanly re-applied checkpoint returns to undo.
    ReplayResult redo();
    [[nodiscard]] const ReplayResult& last_replay() const noexcept { return last_; }

    // --- `.editor/session.json` persistence (R-FILE-006 gitignored session state) -------------------
    // Serialize the undo + redo stacks to a canonical-serializable JSON tree (the host writes it to
    // `.editor/session.json`). The live gateway is NOT serialized — it is re-attached on load.
    [[nodiscard]] serializer::JsonValue to_json() const;
    // Replace the stacks from a previously-serialized tree. Total + robust: a malformed / wrong-shape
    // tree leaves the journal EMPTY and returns false (a corrupt session file never throws or crashes
    // the editor). Any open gesture is discarded.
    bool load_json(const serializer::JsonValue& doc);

    // --- headless a11y command surface (R-A11Y-001 / R-CLI-001) -------------------------------------
    // The headless uitree Panel exposing the undo/redo availability + a keyboard-reachable command per
    // available action. a11y-conformant by construction (uitree::audit_a11y returns no violations) and
    // deterministic. Commands are exposed ONLY when reachable (undo/redo available), so there is never
    // an unreachable-command violation.
    [[nodiscard]] uitree::Panel build_panel() const;

private:
    // Replay one field edit as a CAS-guarded override write. `redo=false` reverts to `before`
    // (expecting the field to currently hold `after`); `redo=true` re-applies `after` (expecting
    // `before`). The up-front expected-value check is the R-HUX-001 no-clobber guard.
    [[nodiscard]] inspector::CommitResult replay_edit(const FieldEdit& edit, bool redo) const;

    const inspector::OverrideWriteGateway* gateway_ = nullptr;
    std::vector<Checkpoint> undo_;
    std::vector<Checkpoint> redo_;
    std::optional<Checkpoint> open_gesture_;
    ReplayResult last_;
};

} // namespace context::editor::gui::session::undo
