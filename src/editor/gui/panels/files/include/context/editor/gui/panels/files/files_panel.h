// The Files observer panel (M9 e1, D10 read half / R-A11Y-001 / R-HUX-011): projects the project's
// file tree into a headless context_gui_uitree Panel, publishes `subject: "file"` selections through
// the SAME daemon selection surface c1 added (`editor select` / the `session` topic's
// `selection-changed` fact), and renders the c1/D3 `selection-focus` fact when it names `file`.
// Read-only observer: no writes into the project, no new error-catalog codes (rename/move/delete are
// task e2). The whole panel is CI-assertable WITHOUT booting CEF, exactly like SceneTreePanel.
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

    // `gateway` may be null: the panel then renders daemon selection it is given but can request no
    // change of its own (the a11y harness's default-constructed panel). Non-owning — the gateway
    // must outlive the panel.
    explicit FilesPanel(SelectionGateway* gateway = nullptr) noexcept : gateway_(gateway) {}

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

    SelectionGateway* gateway_ = nullptr;
    FilesModel model_;
    FileSelection selection_;
    bool focused_ = false;
    std::uint64_t generation_ = 0;
    bridge::Stability stability_ = bridge::Stability::stable;
    std::vector<SelectionListener> listeners_;
};

} // namespace context::editor::gui::panels::files
