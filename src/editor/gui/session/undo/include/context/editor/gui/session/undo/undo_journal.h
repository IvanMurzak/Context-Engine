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
// through the inspector::OverrideWriteGateway seam and (de)serializes to a plain JSON tree its HOST
// persists. Headless + fault-injectable WITHOUT booting CEF (drive an in-memory gateway; R-QA-010
// concurrency seams).
//
// ⚠ WHERE THAT TREE LANDS: the gitignored `.editor/editor-state.json`, NOT `.editor/session.json`.
// Since M9 e09c the host is the Shell's `shell/panels/undo_feed.{h,cpp}`, which carries the tree as
// an opaque canonical string through `EditorStateStore::set_undo` — the ONE seam it reaches disk
// through. `.editor/session.json` belongs to the DAEMON, which is its single writer (C-F3, design
// 03 §1; kernel_server.h says the same from the other side), so persisting the journal there would
// put a second writer on that file. This header said `session.json` until e09c gave the journal a
// real host; do not restore that wording.

#pragma once

#include "context/editor/gui/panels/files/files_panel.h" // e2: FileWriteGateway / FileWriteResult
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
namespace files = context::editor::gui::panels::files;
namespace serializer = context::editor::serializer;
namespace uitree = context::editor::gui::uitree;

// One reversible field edit captured at gesture-commit time — the atom of the session journal. It
// records the L-35 addressing (root_scene + id-path + entity-relative pointer) plus the `before`
// (undo target) and `after` (redo target / the value the field is EXPECTED to currently hold) values.
// Deliberately self-contained (no InspectorModel handle) so a checkpoint survives selection changes
// and the JSON round-trip its host persists (see the header note: `.editor/editor-state.json`).
struct FieldEdit
{
    std::string root_scene;               // the addressing (root) scene the override targets (L-35)
    std::vector<std::string> id_path;     // the L-35 id-path to the composed entity
    std::string pointer;                  // the entity-relative JSON pointer written
    serializer::JsonValue before;         // the value before the edit — the undo target
    serializer::JsonValue after;          // the value after the edit — the redo target / collision base
};

// One reversible FILE operation (M9 e2) — the journal's SECOND atom, beside FieldEdit.
//
// WHY A SECOND ATOM RATHER THAN A FIELD EDIT IN DISGUISE. A FieldEdit is addressed by
// (root_scene, id_path, pointer) and carries before/after VALUES; a file operation has none of
// those — its subject is a path and its inverse is another file operation. Modelling it as a
// FieldEdit would mean inventing a pointer nothing can read and a value nothing can write.
//
// WHAT MAKES THE DELETE REVERSIBLE. `restore_token` is the handle `editor file-delete` returned: the
// deleted bytes are quarantined under `.editor/trash/<token>/`, so the undo restores the SAME bytes
// rather than bytes this journal re-serialized. That is why the checkpoint stays small enough to
// live in `.editor/editor-state.json` and why it is binary-safe (an arbitrary asset has no
// byte-faithful JSON string form).
struct FileEdit
{
    enum class Op
    {
        move,   // rename IS move: undo moves `to` back to `from`, redo re-applies `from` -> `to`
        remove, // undo RESTORES via `restore_token`; redo deletes `from` again
    };

    Op op = Op::move;
    std::string from;          // the path before the operation (the deleted path for `remove`)
    std::string to;            // the path after (`move` only; empty for `remove`)
    std::string restore_token; // `remove` only: the quarantine handle the undo restores through
};

// A gesture checkpoint (L-20 gesture-batch auto-checkpointing): ONE undo step per gesture, not per
// keystroke. Usually a single field edit (the inspector commits one field per gesture), but a batched
// gesture may carry several — undo reverts them in reverse order, each with independent field-path
// L-30 collision handling.
struct Checkpoint
{
    std::string label;                    // human/AI-readable gesture label (may be empty)
    std::vector<FieldEdit> edits;
    // M9 e2: the file operations this step performed. A checkpoint carries field edits OR file
    // edits (the two surfaces that author today never batch together), but the shape allows both so
    // a future gesture that does can be ONE undo step rather than two.
    std::vector<FileEdit> file_edits;

    [[nodiscard]] bool empty() const noexcept { return edits.empty() && file_edits.empty(); }
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
// clobbering a concurrent writer), and (de)serializes to a JSON tree its host persists (since e09c:
// the gitignored `.editor/editor-state.json`). Total and deterministic; owns no disk.
class UndoJournal
{
public:
    // The command ids the CEF host binds to Ctrl+Z / Ctrl+Y (R-CLI-001: every affordance has a
    // keyboard/CLI path). Stable + greppable.
    static constexpr const char* kUndoCommand = "session.undo";
    static constexpr const char* kRedoCommand = "session.redo";
    // The R-EDIT-001 contribution id this session surface registers under.
    static constexpr const char* kContributionId = "builtin.session.undo";
    // The `CommitResult::code` a replay reports when the field could not be READ at all (no
    // connection, a refused read, an entity that no longer resolves). Deliberately NOT
    // `cas.mismatch`: nothing was written and no concurrent writer was observed — see `replay_edit`.
    //
    // The prefix names the LAYER, not the `edit` RPC, and that is deliberate: this code never
    // reaches an Envelope or `exit_code_for`, so an `edit.*` spelling would read like a wire code
    // the contract catalog should know and does not. `WireOverrideWriteGateway::kNoDaemonCode`
    // (`shell.no_daemon`) is the same shape for the same reason — a host-minted, uncatalogued code.
    static constexpr const char* kReadUnavailableCode = "undo.read_unavailable";
    // The e2 twin for the FILE half: the replay had no file write path to go through. Same shape
    // and same reason as the code above — host-minted, uncatalogued, and NOT `cas.mismatch`,
    // because nothing was written and no concurrent writer was observed.
    static constexpr const char* kFileWriteUnavailableCode = "undo.file_write_unavailable";
    // The `to_json` schema version (bumped if the on-disk journal shape changes).
    static constexpr int kJournalVersion = 1;

    UndoJournal() = default;
    explicit UndoJournal(const inspector::OverrideWriteGateway* gateway) : gateway_(gateway) {}
    void set_gateway(const inspector::OverrideWriteGateway* gateway) noexcept { gateway_ = gateway; }

    // M9 e2: the FILE write path a file checkpoint replays through — the SAME seam the Files panel
    // authors through, so an undo is not a privileged back door but the identical operation issued
    // in reverse (R-HUX-001: undo/redo is replayed through the same write path as any other
    // mutation). `nullptr` detaches; an unbound file gateway makes a file replay an honest REFUSAL
    // (nothing written, the step KEPT), never a silent no-op that would consume the human's history.
    void set_file_gateway(files::FileWriteGateway* gateway) noexcept { file_gateway_ = gateway; }

    // --- recording (L-20 gesture-batch auto-checkpointing) ------------------------------------------
    // Push a fully-formed gesture checkpoint (empty checkpoints are ignored). Recording ANY new
    // gesture invalidates the redo stack (standard undo/redo semantics).
    void record(Checkpoint checkpoint);
    // Open a multi-edit gesture batch; subsequent capture() calls append to it until end_gesture().
    void begin_gesture(std::string label = {});
    // Append one edit to the open gesture batch, or — when no batch is open — auto-checkpoint it as a
    // lone single-edit gesture (the inspector's common one-field-per-gesture case).
    void capture(FieldEdit edit);
    // The e2 twin for a FILE operation. Same batching rule; `label` names the step in Session
    // History when it auto-checkpoints (a file step's label is the only thing a human can read to
    // tell one undo entry from another, since a path is not a field).
    void capture_file(FileEdit edit, std::string label = {});
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
    //
    // Since e2 a checkpoint may hold FILE edits instead of field edits, and the two gateways are
    // bound independently — so "is there anything to replay through" is per-checkpoint, not a
    // single up-front gateway test. A step whose gateway is missing REFUSES (and is kept).
    //
    // AN ERROR KEEPS THE STEP; A DROP CONSUMES IT — the same caller contract every other user of
    // `commit_override_write` honours (inspector_panel.h; InspectorPanel::commit keeps its staged
    // gesture on a refusal for exactly this reason). A drop means the field MOVED, so this step can
    // never be replayed and re-offering it would hand the human an undo guaranteed to refuse. A
    // refusal — no daemon, an unreadable field, a write path that said no — wrote NOTHING, so the
    // step is still perfectly good; consuming it would silently cost the human their history over a
    // transient outage, which is precisely what a durable journal must not do. A partially-landed
    // batch is treated as consumed (see `nothing_landed` in the .cpp).
    ReplayResult undo();
    // Redo the most recently undone checkpoint (Ctrl+Y): replay each field's `after`, forward order,
    // same CAS + drop-on-collision guarantees, and the same keep-on-error rule as `undo` above. Only
    // a cleanly re-applied checkpoint returns to undo.
    ReplayResult redo();
    [[nodiscard]] const ReplayResult& last_replay() const noexcept { return last_; }

    // --- host-persisted session state (R-FILE-006 gitignored) ---------------------------------------
    // Serialize the undo + redo stacks to a canonical-serializable JSON tree. The HOST decides where
    // it lands; since e09c that is the Shell writing `.editor/editor-state.json` (see the file header
    // — NOT the daemon's `.editor/session.json`). The live gateway is NOT serialized — it is
    // re-attached on load.
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

    // Replay one FILE edit through the file gateway (M9 e2). NON-const and takes `edit` by
    // reference because a REDONE delete re-quarantines the bytes and hands back a token, which must
    // replace the stale one in the checkpoint — otherwise the next undo of that step would try to
    // restore through a handle nothing is filed under.
    //
    // The outcome is reported as an inspector::CommitResult so the aggregate/keep-vs-consume rules
    // stay in ONE place: `file` carries the path (a file operation has no field pointer, so
    // `pointer` stays empty), `applied` means the write path performed it, and a REFUSAL is
    // `error` — nothing was written, so the caller keeps the step, exactly as for a refused field
    // write. There is deliberately no `dropped` arm: the L-30 collision that produces one is a
    // field-value comparison, and the file operations refuse on their own never-overwrite rules
    // instead (asset.move_destination_exists / asset.restore_destination_exists), which is the same
    // guarantee expressed where the engine can actually check it.
    [[nodiscard]] inspector::CommitResult replay_file_edit(FileEdit& edit, bool redo);

    // The ONE home for the keep-vs-consume policy `undo` and `redo` share: push `cp` onto `to` when
    // the replay cleanly landed, back onto `from` when it refused without writing anything, and
    // consume it otherwise. Both callers route through here so the rule cannot drift between the
    // two near-identical mirrors.
    void settle(Checkpoint cp, const ReplayResult& r, bool all_ok, std::vector<Checkpoint>& from,
                std::vector<Checkpoint>& to);

    const inspector::OverrideWriteGateway* gateway_ = nullptr;
    files::FileWriteGateway* file_gateway_ = nullptr;
    std::vector<Checkpoint> undo_;
    std::vector<Checkpoint> redo_;
    std::optional<Checkpoint> open_gesture_;
    ReplayResult last_;
};

} // namespace context::editor::gui::session::undo
