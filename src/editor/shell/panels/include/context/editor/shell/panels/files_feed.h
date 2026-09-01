// The LIVE files feed for the Files panel (M9 e1, D10 read half) — the projection from the daemon's
// `editor.files` composed read onto the headless `FilesPanel` model, plus the `PanelProvider` that
// publishes it through the Shell's panel host. Mirrors scenetree_feed.h — see that header for the
// full rationale, which applies here unchanged (WHERE THIS SITS, READ CADENCE, TOLERANCE); this file
// states only what differs.
//
// WHAT DIFFERS FROM THE SCENE TREE. `editor.files` takes NO params (it lists the WHOLE project, not
// one scene), so this feed's fetch cadence needs no `scene_path` gate — it is due on construction
// and on every `derivation.settled`, unconditionally.

#pragma once

#include "context/editor/contract/json.h"
#include "context/editor/gui/panels/files/files_model.h"
#include "context/editor/gui/panels/files/files_panel.h"
#include "context/editor/shell/panel_host.h"

#include <cstddef>
#include <optional>
#include <string>

namespace context::editor::shell::panels
{

namespace files = gui::panels::files;

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

    [[nodiscard]] files::FilesPanel& panel() noexcept { return panel_; }
    [[nodiscard]] const files::FilesPanel& panel() const noexcept { return panel_; }

    [[nodiscard]] std::size_t results_applied() const noexcept { return results_applied_; }
    [[nodiscard]] std::size_t events_applied() const noexcept { return events_applied_; }

    // The provider to bind on the PanelHost. Captures `this` — the feed must OUTLIVE the binding.
    // Files is a read-only observer with SELECTION (`files.select`): no gestures, no persisted
    // state, both REPORTED absent rather than stubbed.
    [[nodiscard]] PanelProvider make_provider();

private:
    PanelHost& host_;
    std::string panel_id_;
    files::FilesPanel panel_;
    bool fetch_due_ = true; // born due: the first pump performs the initial hydration
    std::size_t results_applied_ = 0;
    std::size_t events_applied_ = 0;
};

} // namespace context::editor::shell::panels
