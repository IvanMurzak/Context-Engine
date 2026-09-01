// The LIVE files feed for the Files panel (M9 e1/e2, D10) — the projection from the daemon's
// `editor.files` composed read onto the headless `FilesPanel` model, plus the `PanelProvider` that
// publishes it through the Shell's panel host. Mirrors scenetree_feed.h — see that header for the
// full rationale, which applies here unchanged (WHERE THIS SITS, READ CADENCE, TOLERANCE); this file
// states only what differs.
//
// WHAT DIFFERS FROM THE SCENE TREE. `editor.files` takes NO params (it lists the WHOLE project, not
// one scene), so this feed's fetch cadence needs no `scene_path` gate — it is due on construction
// and on every `derivation.settled`, unconditionally.
//
// SINCE e2 IT ALSO OWNS THE WRITE FAN-OUT, and that is why the three sinks below exist rather than
// the composition root hanging three listeners on the panel itself. The feed installs exactly ONE
// write listener on its panel and routes each outcome from there:
//
//   * APPLIED -> the checkpoint sink (the session journal records a reversible step) AND a
//     refetch, because the write went straight out over the gateway and the tree the panel is
//     rendering is now stale by exactly the row the human just acted on. That is the same
//     READ-YOUR-WRITES hole `undo_feed.h § take_replay_landed` documents for the Inspector, and it
//     is closed here for the same reason: a panel still showing a file it just deleted is the most
//     confusing possible answer to a destructive action.
//   * REFUSED -> the notice sink (the LOUD `editor.ui.write-notice`), and NO refetch: nothing was
//     written, so there is nothing to re-read.
//
// ONE listener rather than three subscribers is what stops the journal and the notice disagreeing
// about what happened — the same argument `bind_write_notice_relay` makes for keeping all three
// loss sites on one relay.

#pragma once

#include "context/editor/contract/json.h"
#include "context/editor/gui/panels/files/files_model.h"
#include "context/editor/gui/panels/files/files_panel.h"
#include "context/editor/gui/session/undo/undo_journal.h" // undo::FileEdit (the checkpoint sink)
#include "context/editor/shell/panel_host.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>

namespace context::editor::shell::panels
{

namespace files = gui::panels::files;
namespace undo = gui::session::undo;

// --------------------------------------------------------------------------------- pure parsers

// Project one `editor.files` reply's `files` member back into the panel model. nullopt when `wire`
// is not an object or carries no `roots` array — the one shape that says nothing about the tree.
// Unparseable NODES within a recognized container are SKIPPED, never fatal (the ProblemsFeed
// tolerance discipline scenetree_feed.h documents).
[[nodiscard]] std::optional<files::FilesModel> parse_files(const contract::Json& wire);

// ---------------------------------------------------------------- the node-id -> identity mapping

// The prefix `FilesPanel::build_panel` gives every FILE tree row (`files.item.<identity>`). Named
// here so the one place that depends on it is greppable from both sides (the
// kSceneTreeRowPrefix / kProblemsRowPrefix pattern).
inline constexpr const char* kFilesRowPrefix = "files.item.";

// Resolve an activated NODE id to the selection identity `FilesPanel::select` expects. nullopt for a
// node that is not a file tree row.
[[nodiscard]] std::optional<std::string> files_row_identity(const std::string& node_id);

// ------------------------------------------------------------------------------------ the feed

// Owns a FilesPanel and drives it from the live daemon read + the derivation topic, touching the
// PanelHost on every change so the next `panel.render` is seen as fresh.
class FilesFeed
{
public:
    // Non-owning: `host` must outlive the feed. `panel_id` is passed rather than hardcoded — the
    // feed is a MECHANISM the composition root points at a roster id.
    //
    // `selection_gateway` (M9 e1) is what the panel WRITES selection through — the Shell's live
    // SessionFeed's `files::SelectionGateway` adapter in the real editor, a recording double in the
    // T1 suite, nullptr when no daemon session is wired. It must outlive the feed.
    FilesFeed(PanelHost& host, std::string panel_id, files::SelectionGateway* selection_gateway = nullptr);

    FilesFeed(const FilesFeed&) = delete;
    FilesFeed& operator=(const FilesFeed&) = delete;
    FilesFeed(FilesFeed&&) = delete;
    FilesFeed& operator=(FilesFeed&&) = delete;

    // Adopt one `editor.files` reply. Accepts the envelope (`{ok, data: {files, …}}`), the bare
    // `data`, or the bare `files` object. Returns true when a model was actually adopted (the host
    // was touched).
    bool apply_result(const contract::Json& reply);

    // Consume one subscription event. `derivation.settled` marks a refetch due (a write elsewhere —
    // including this window's own — can create/move/delete a file). Returns true when the panel's
    // rendered surface changed. Unknown topics are ignored.
    bool apply_event(const std::string& topic, const contract::Json& payload,
                     std::uint64_t generation);

    // The pump's contract, identical to SceneTreeFeed's: `fetch_due()` says a live re-read is
    // wanted; the pump calls `mark_fetched()` BEFORE issuing the RPC (claiming the fetch).
    [[nodiscard]] bool fetch_due() const noexcept { return fetch_due_; }
    void mark_fetched() noexcept { fetch_due_ = false; }

    // --- M9 e2: the write half --------------------------------------------------------------------

    // Bind the file write path the panel authors through (the Shell's WireFileWriteGateway).
    // `nullptr` detaches; an unbound panel exposes NO authoring command at all, so a human is never
    // offered a delete that would quietly do nothing (files_panel.h § set_write_gateway).
    void bind_write_gateway(files::FileWriteGateway* gateway);
    [[nodiscard]] bool has_write_gateway() const noexcept { return write_gateway_bound_; }

    // Where a LANDED file operation becomes an undo step. Erased through a std::function for the
    // reason `InspectorFeed::bind_checkpoint_sink` states: this feed must not name the UndoFeed that
    // is declared after it. `label` is the human-readable step name Session History renders.
    using CheckpointSink = std::function<void(undo::FileEdit, std::string)>;
    void bind_checkpoint_sink(CheckpointSink sink);
    [[nodiscard]] bool has_checkpoint_sink() const noexcept { return static_cast<bool>(checkpoints_); }

    // Where a REFUSED file operation becomes a loud notice — the same seam InspectorFeed/UndoFeed
    // take, with the same erasure (this feed must not name `shell::WriteNoticeRelay`). `verb` is the
    // Shell's own prose ("rename", "delete", "restore"), not a pinned token.
    using NoticeSink = std::function<void(const char* verb, const files::FileWriteResult& result)>;
    void bind_notice_sink(NoticeSink sink);
    [[nodiscard]] bool has_notice_sink() const noexcept { return static_cast<bool>(notices_); }

    // How many file operations this feed observed LAND, and how many it observed REFUSED. The
    // counters are what make "the human was told" assertable rather than assumed.
    [[nodiscard]] std::size_t writes_landed() const noexcept { return writes_landed_; }
    [[nodiscard]] std::size_t writes_refused() const noexcept { return writes_refused_; }

    [[nodiscard]] files::FilesPanel& panel() noexcept { return panel_; }
    [[nodiscard]] const files::FilesPanel& panel() const noexcept { return panel_; }

    [[nodiscard]] std::size_t results_applied() const noexcept { return results_applied_; }
    [[nodiscard]] std::size_t events_applied() const noexcept { return events_applied_; }

    // The provider to bind on the PanelHost. Captures `this` — the feed must OUTLIVE the binding.
    // Files carries SELECTION (`files.select`) and, since e2, the three authoring commands; no
    // gestures and no persisted state of its own, both REPORTED absent rather than stubbed (the
    // undo STEP a file operation produces is persisted by the journal, which owns that seam).
    [[nodiscard]] PanelProvider make_provider();

private:
    // The ONE write listener's body: fan a panel write outcome out to the journal / the notice relay
    // / the refetch. Installed on the panel by the constructor.
    void on_panel_write(files::FileWriteVerb verb, const files::FileWriteResult& result);

    PanelHost& host_;
    std::string panel_id_;
    files::FilesPanel panel_;
    bool fetch_due_ = true; // born due: the first pump performs the initial hydration
    bool write_gateway_bound_ = false;
    CheckpointSink checkpoints_;
    NoticeSink notices_;
    std::size_t results_applied_ = 0;
    std::size_t events_applied_ = 0;
    std::size_t writes_landed_ = 0;
    std::size_t writes_refused_ = 0;
};

} // namespace context::editor::shell::panels
