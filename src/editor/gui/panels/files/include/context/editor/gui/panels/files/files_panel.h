// The Files observer panel (M9 e1, D10 read half / R-A11Y-001 / R-HUX-011): projects the project's
// file tree into a headless context_gui_uitree Panel, publishes `subject: "file"` selections through
// the SAME daemon selection surface c1 added (`editor select` / the `session` topic's
// `selection-changed` fact), and renders the c1/D3 `selection-focus` fact when it names `file`.
// Since M9 e2 it is also the D10 WRITE half: rename / move / DELETE, authored through the ONE L-30
// write path and never by the panel itself. The whole panel is CI-assertable WITHOUT booting CEF,
// exactly like SceneTreePanel.
//
// THE WRITE HALF FOLLOWS THE SELECTION HALF'S RULE EXACTLY, and that is the point rather than a
// coincidence: `rename()` / `move()` / `remove()` decide NOTHING. They are write REQUESTS through
// the FileWriteGateway seam below (the Shell's WireFileWriteGateway drives the daemon's
// `editor file-move` / `editor file-delete` / `editor file-restore`), and what the panel renders
// afterwards is what the write path ANSWERED. A refusal renders a refusal -- loudly, in the status
// line, on top of the `editor.ui.write-notice` the Shell publishes from the same listener -- because
// on a destructive operation a silent failure is the worst outcome available (design 10's
// non-negotiable UX invariant: destructive/lossy moments are LOUD, never silent).
//
// THE PANEL DOES NOT OWN THE UNDO STEP EITHER. A landed operation is announced through
// `add_write_listener`; the Shell records the session-undo checkpoint from there, so the journal
// entry is minted by the same code that already owns undo for every other authored mutation rather
// than by a second, panel-private history.
//
// THIS PANEL DOES NOT OWN SELECTION (M9 e08b's rule, restated for a second subject — see
// scene_tree_panel.h for the full rationale, which applies here unchanged):
//
//   * `select()` / `clear_selection()` are WRITE REQUESTS through the SelectionGateway seam below.
//     They decide NOTHING: what they render afterwards is the selection the DAEMON reports back, and
//     a refusal renders nothing at all.
//   * `apply_selection()` is the ONLY mutator of the rendered selection, fed from the daemon's reply
//     to our own write and from the `selection-changed{subject:"file"}` fact another client caused.
//
// A DISTINCT INTERFACE FROM `scenetree::SelectionGateway`, deliberately — see files_panel.h's Shell
// counterpart (session_feed.h) for why one class cannot implement two identically-shaped pure
// virtuals with different per-subject bodies. The two interfaces are structurally identical (a
// single `request_selection` seam) because both panels are single-select observers of a DAEMON
// selection; they stay separate types so the c1 subject each writes through can never be confused at
// a call site.
//
// c1/D1: selecting a file does NOT clear the entity selection (selections of different subjects
// coexist) — this panel is a SUBSCRIBER of the `file` subject only, filtered upstream by the Shell's
// SessionFeed exactly the way the Scene tree is filtered to `entity` (session_feed.h § THE SUBJECT
// FILTER).

#pragma once

#include "context/editor/gui/panels/files/files_model.h"

#include "context/editor/gui/uitree/panel.h"

#include "context/editor/bridge/event_stream.h" // bridge::Stability

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace context::editor::gui::panels::files
{

// The selection event other panels could consume (mirrors scenetree::SceneSelection). An empty
// `identity` means the selection was cleared.
struct FileSelection
{
    std::string identity;
};

// The command a focusable tree row binds so selection has a keyboard path (R-A11Y-001 / R-CLI-001).
inline constexpr const char* kSelectCommand = "files.select";

// The M9 e2 authoring commands. Every affordance has a keyboard/CLI path (R-CLI-001), and each is
// exposed ONLY when it is actually reachable (a bound write gateway AND a selected file row) so
// audit_a11y never sees a command with nothing behind it.
inline constexpr const char* kRenameCommand = "files.rename";
inline constexpr const char* kMoveCommand = "files.move";
inline constexpr const char* kDeleteCommand = "files.delete";

// Which authoring operation a result describes. Free of any wire spelling: the Shell maps it to the
// verb it sends and to the human-readable `action` on a write notice.
enum class FileWriteVerb
{
    move,    // rename IS move -- one engine operation, one journal entry shape
    remove,  // the destructive one
    restore, // the inverse of `remove`, replayed by session undo
};

// What one requested file operation ANSWERED. `refused` is a first-class outcome here, not an error
// path bolted on: every field a human needs in order to understand a refused destructive operation
// travels on it.
struct FileWriteResult
{
    enum class Status
    {
        none,    // nothing was requested (or no gateway is bound to request through)
        applied, // the write path performed it
        refused, // the write path said no, and NOTHING was written
    };

    Status status = Status::none;
    std::string code;    // the R-CLI-008 catalog code on a refusal ("" when applied)
    std::string message; // the human/AI-readable detail the write path produced
    std::string path;    // the primary path the operation targeted
    std::string other_path;    // move: the destination ("" otherwise)
    std::string restore_token; // remove: the handle an undo restores through

    [[nodiscard]] bool ok() const noexcept { return status == Status::applied; }
};

// The seam the panel WRITES file operations through -- the ONE L-30 write path, reached exactly the
// way the Inspector reaches its own (a boundary-clean pure-virtual the Shell implements over the
// wire). The panel opens no file and links no filesync/assetdb type, so hosting it never moves the
// D10 shell-boundary FORBIDDEN list.
//
// An implementation NEVER throws and never reports success it did not achieve: an unbound / dead
// connection answers `refused` with a code, which is what makes the failure path renderable instead
// of invisible.
class FileWriteGateway
{
public:
    virtual ~FileWriteGateway() = default;

    // Move (or rename -- the same operation) `from` to `to`, sidecar and GUID identity travelling
    // with it. An occupied destination is REFUSED, never overwritten.
    [[nodiscard]] virtual FileWriteResult move_file(const std::string& from,
                                                    const std::string& to) = 0;
    // Delete `path` and its sidecar. On success the result carries the `restore_token` that makes it
    // reversible.
    [[nodiscard]] virtual FileWriteResult delete_file(const std::string& path) = 0;
    // Restore a deletion by the token `delete_file` returned (the undo replay path).
    [[nodiscard]] virtual FileWriteResult restore_file(const std::string& restore_token) = 0;
};

// The seam the panel WRITES selection through (M9 e1, mirrors scenetree::SelectionGateway). The real
// implementation is the Shell's SessionFeed (session_feed.h), driving `editor select
// {subject:"file"}` over the wire with the SAME origin echo-suppression c1 established.
class SelectionGateway
{
public:
    virtual ~SelectionGateway() = default;

    // Request that `ids` BECOME the `file` selection. An empty `ids` clears it. Returns THE
    // SELECTION THE DAEMON NOW HOLDS (never a bool — the panel renders the daemon's answer, not its
    // own request), `nullopt` when the request did not land at all (no daemon, a refused scope, a
    // transport fault) — the panel then changes nothing.
    [[nodiscard]] virtual std::optional<std::vector<std::string>>
    request_selection(const std::vector<std::string>& ids) = 0;
};

class FilesPanel
{
public:
    // The R-EDIT-001 contribution id this built-in panel registers under.
    static constexpr const char* kContributionId = "builtin.files";

    // The LOCAL refusal codes (M9 e2). Deliberately NOT R-CLI-008 catalog codes and deliberately not
    // `internal.error`: nothing failed on the write path — the panel refused before asking, because
    // the request could not be honoured as stated. `panel_host.h` and
    // `WireOverrideWriteGateway::kNoDaemonCode` state the same rule: a host-side condition does not
    // get to pollute the published catalog.
    static constexpr const char* kNoWritePathCode = "files.no_write_path";
    static constexpr const char* kInvalidRequestCode = "files.invalid_request";

    // `gateway` may be null: the panel then renders daemon selection it is given but can request no
    // change of its own (the a11y harness's default-constructed panel). Non-owning — the gateway
    // must outlive the panel.
    explicit FilesPanel(SelectionGateway* gateway = nullptr) noexcept : gateway_(gateway) {}

    // Bind the write path (M9 e2). `nullptr` detaches, and an UNBOUND panel is not a silent no-op:
    // it exposes NO authoring command at all, so a human is never offered a delete that would
    // quietly do nothing. Non-owning — the gateway must outlive the panel, the same contract
    // SelectionGateway carries.
    void set_write_gateway(FileWriteGateway* gateway) noexcept { writes_ = gateway; }
    [[nodiscard]] bool can_write() const noexcept { return writes_ != nullptr; }

    // Replace the rendered file tree (e.g. from a fresh `editor.files` read). The selection is not
    // touched — it belongs to the daemon, and a path vanishing from THIS panel's view is not the
    // daemon deselecting it (a refresh can arrive before a rename's fact does).
    void set_model(FilesModel model);
    [[nodiscard]] const FilesModel& model() const noexcept { return model_; }

    // R-BRIDGE-008: consume a derivation.settled event — advance the tracked generation, record the
    // stability the settled generation reports, and re-project. Mirrors SceneTreePanel; the file tree
    // is not itself derived-world state, but it can change for the same reasons a settle exists to
    // announce (a file the derivation pipeline just wrote/moved), so the panel re-fetches on the same
    // signal.
    void on_derivation_settled(std::uint64_t generation, bridge::Stability stability);

    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] bridge::Stability stability() const noexcept { return stability_; }

    // --- selection: WRITE requests (the panel changes nothing itself) ---------------------------
    [[nodiscard]] bool select(const std::string& identity);
    [[nodiscard]] bool clear_selection();

    // --- selection: the daemon's fact (the ONLY mutator) ----------------------------------------
    // The FIRST id is what this single-select panel renders; an empty list clears it. An id with no
    // row in the current model is still adopted (the daemon's truth does not depend on what this
    // panel has loaded) — it simply renders no "(selected)" marker until a model containing it
    // arrives.
    bool apply_selection(const std::vector<std::string>& ids);

    [[nodiscard]] const FileSelection& selection() const noexcept { return selection_; }

    using SelectionListener = std::function<void(const FileSelection&)>;
    void add_selection_listener(SelectionListener listener);

    // --- M9 e2: the authoring surface (WRITE requests; the panel decides nothing) ----------------
    //
    // Each returns true when the write path APPLIED the operation. False covers both "refused" and
    // "not attempted" — `last_write()` distinguishes them, and the write listeners are told either
    // way, because a refusal the human is not told about is the failure mode this panel exists to
    // avoid.

    // Rename in place: `new_name` is a BASENAME, resolved against the row's own directory. A name
    // carrying a '/' is refused LOCALLY (that is a move, and silently reinterpreting it would move
    // the human's file somewhere they did not name) — as is an empty one, or a rename to the name
    // it already has.
    bool rename(const std::string& identity, const std::string& new_name);
    // Move to an explicit project-relative destination path.
    bool move(const std::string& identity, const std::string& destination);
    // DELETE the row's file and its sidecar. Reversible: a landed delete's `restore_token` reaches
    // the write listeners, which is how the session journal makes it undoable.
    bool remove(const std::string& identity);
    // Restore a previously deleted file by its token.
    //
    // NO PRODUCTION CALLER TODAY, stated plainly because the opposite is easy to assume: session
    // undo replays a delete by calling `FileWriteGateway::restore_file` on the gateway DIRECTLY
    // (undo_journal.cpp § replay_file_edit), exactly as the Inspector's replays bypass its panel —
    // so a replay does NOT get this panel's fan-out (status line, notice sink, refetch). This
    // remains here as the panel-side half of the seam, for the eventual human-facing "undo the
    // delete I just did" affordance; until something exposes it, it is exercised only by the write
    // suites. Do not reason about replay behaviour from this method.
    bool restore(const std::string& restore_token);

    // The most recent write outcome, rendered in the status line and readable by a test.
    [[nodiscard]] const FileWriteResult& last_write() const noexcept { return last_write_; }

    // Every attempted write, applied or refused, in the order they happened. The Shell binds this
    // ONCE and fans out from there: a landed operation becomes an undo checkpoint, a refused one
    // becomes an `editor.ui.write-notice`. Keeping BOTH on one listener is what stops the two
    // reactions drifting onto different notions of what happened.
    using WriteListener = std::function<void(FileWriteVerb, const FileWriteResult&)>;
    void add_write_listener(WriteListener listener);

    // c1/D3: whether the daemon's `selection-focus` currently names `file` — the Shell's SessionFeed
    // drives this from the `selection-focus` fact (session_feed.h), exactly as it re-points the
    // Inspector. Rendered in the panel's status line so the panel's own state proves the wiring
    // ("the panel renders focus state").
    void set_focused(bool focused);
    [[nodiscard]] bool focused() const noexcept { return focused_; }

    // Build the headless uitree Panel for the current model + selection + focus + generation/
    // stability. Deterministic: identical state produces a byte-identical Panel (uitree::render_html).
    // a11y-conformant by construction — uitree::audit_a11y returns no violations for any model.
    [[nodiscard]] uitree::Panel build_panel() const;

private:
    void notify() const;
    [[nodiscard]] bool write_selection(const std::vector<std::string>& ids);
    // The ONE place a write outcome is adopted + announced, so `last_write_` and the listeners can
    // never disagree about what the write path said.
    bool settle_write(FileWriteVerb verb, FileWriteResult result);
    // A locally-refused request (no gateway, unknown row, malformed name): still a REFUSAL, still
    // announced, never a silent return.
    bool refuse_locally(FileWriteVerb verb, std::string code, std::string message,
                        std::string path);

    SelectionGateway* gateway_ = nullptr;
    FileWriteGateway* writes_ = nullptr;
    FilesModel model_;
    FileSelection selection_;
    FileWriteResult last_write_;
    std::vector<WriteListener> write_listeners_;
    bool focused_ = false;
    std::uint64_t generation_ = 0;
    bridge::Stability stability_ = bridge::Stability::stable;
    std::vector<SelectionListener> listeners_;
};

} // namespace context::editor::gui::panels::files
