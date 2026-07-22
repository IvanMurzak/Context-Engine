// The scene-tree observer panel (M5-F2, R-EDIT-001 / R-BRIDGE-008 / L-35 / R-A11Y-001 / R-HUX-011):
// projects the composed derived-world hierarchy into a headless context_gui_uitree Panel, RENDERS
// the current selection (emitting a selection event other panels consume), and re-renders on the
// R-BRIDGE-008 derivation.settled event while respecting the stability field. Read-only observer: no
// writes into the world, no new error-catalog codes. The whole panel is CI-assertable WITHOUT
// booting CEF.
//
// M9 e08b — THE PANEL NO LONGER OWNS SELECTION. Selection is DAEMON state (e08a, design 05 §4 / D7
// tier 1: `editor select` + the `session` topic's `selection-changed` fact), so this panel is a
// SUBSCRIBER and a WRITER, never a custodian:
//
//   * `select()` / `clear_selection()` are WRITE REQUESTS through the SelectionGateway seam below.
//     They decide NOTHING: what they render afterwards is the selection the DAEMON reports back, and
//     a refusal renders nothing at all. There is no optimistic local move, so a request the daemon
//     never applied can never be visible here.
//   * `apply_selection()` is the ONLY mutator of the rendered selection. It is fed from two places
//     that are deliberately the same kind of thing — the daemon's reply to our own write, and the
//     daemon's `selection-changed` fact caused by ANOTHER client.
//
// WHY THE WRITER RENDERS FROM THE REPLY AND NOT FROM ITS OWN ECHO. e08a's `origin` rule says a
// consumer DROPS a fact stamped with its own client id (docs/editor-session-state.md), so the fact a
// write produces is precisely the one this Shell will not apply — the reply is what carries that
// change home. Getting this backwards yields a panel whose own selections never appear while other
// clients' do, which is why the T2 drill asserts both directions against a live daemon.
//
// A selection made by a SECOND client (the CLI, a scripted agent, another window) therefore reaches
// this panel through the same door as its own — there is no local path at all. With no gateway bound
// (the a11y harness's default-constructed panel) `select()` simply reports false: a panel with no
// daemon cannot change a selection it does not own, and saying so is more honest than a local copy
// that quietly disagrees with the daemon the moment one appears.

#pragma once

#include "context/editor/gui/panels/scenetree/scene_tree_model.h"

#include "context/editor/gui/uitree/panel.h"

#include "context/editor/bridge/event_stream.h" // bridge::Stability

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace context::editor::gui::panels::scenetree
{

// The selection event other panels consume (the R-HUX-011 selection loop). `identity` is the L-35
// id-path joined (the selection key); `identity_hash` is the L-37 composed identity hash. An empty
// `identity` means the selection was cleared.
struct SceneSelection
{
    std::string identity;
    std::uint64_t identity_hash = 0;
};

// The command a focusable tree row binds so selection has a keyboard path (R-CLI-001 CLI-completeness
// as a structural accessibility property — every GUI action reachable without a pointer).
inline constexpr const char* kSelectCommand = "scenetree.select";

// The seam the panel WRITES selection through (M9 e08b, design 05 §4). Deliberately the same shape
// as the inspector's OverrideWriteGateway: a pure virtual the panel library declares and something
// else implements, so this library stays boundary-clean (no client SDK, no bridge RPC, no daemon) and
// the panel's write path is CI-assertable with a recording double.
//
// The real implementation is a WIRE gateway over `editor.select` (shell-side, e08b's SessionFeed).
// It is deliberately NOT routed through the in-process `gui/contract` shim: e08a mints `origin` ids
// per WIRE CONNECTION, so an in-process attach is permanently `origin 0` and could never be told
// apart from the daemon itself — echo suppression would break silently (docs/editor-session-state.md).
class SelectionGateway
{
public:
    virtual ~SelectionGateway() = default;

    // Request that `ids` BECOME the selection (`editor select`, mode `replace` — the only mode this
    // single-select panel can express today; add/toggle/remove are the daemon's and arrive with the
    // multi-select gesture). An EMPTY `ids` clears it.
    //
    // Returns THE SELECTION THE DAEMON NOW HOLDS — not a bool, because the panel must render the
    // daemon's answer rather than assume its own request became it. A benign no-op (re-selecting what
    // is already selected) still answers with that selection, so applying it is idempotent.
    //
    // `nullopt` means the request did not land at all: no daemon, a refused scope, a transport fault.
    // The panel then changes nothing — an unapplied request must never be visible.
    [[nodiscard]] virtual std::optional<std::vector<std::string>>
    request_selection(const std::vector<std::string>& ids) = 0;
};

class SceneTreePanel
{
public:
    // The R-EDIT-001 contribution id this built-in panel registers under.
    static constexpr const char* kContributionId = "builtin.scene-tree";

    // `gateway` may be null: the panel then renders daemon selection it is given but can request no
    // change of its own (the a11y harness's default-constructed panel). Non-owning — the gateway
    // must outlive the panel.
    explicit SceneTreePanel(SelectionGateway* gateway = nullptr) noexcept : gateway_(gateway) {}

    // Replace the rendered derived world (e.g. from a fresh bridge query snapshot).
    //
    // The selection is NOT touched: it belongs to the daemon, and a node vanishing from THIS panel's
    // view of the world is not the daemon deselecting it (the composed tree can be re-read, filtered,
    // or arrive late). Only the L-37 identity HASH is re-resolved against the new model — 0 when the
    // selected identity has no row here, exactly the model's "no hash" value. Listeners are notified
    // only when that re-resolution actually changed something.
    void set_model(SceneTreeModel model);
    [[nodiscard]] const SceneTreeModel& model() const noexcept { return model_; }

    // R-BRIDGE-008: consume a derivation.settled event — advance the tracked generation, record the
    // stability the settled generation reports, and re-project. `set_model` refreshes the tree from a
    // snapshot; this updates the generation/stability the status line and re-render reflect, so a
    // settling world is visibly distinguished from a stable one.
    void on_derivation_settled(std::uint64_t generation, bridge::Stability stability);

    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] bridge::Stability stability() const noexcept { return stability_; }

    // --- selection: WRITE requests (the panel changes nothing itself) ---------------------------
    //
    // `select` asks the daemon to make `identity` the selection, then RENDERS THE DAEMON'S ANSWER.
    // The identity must exist in the rendered model (an unknown row is a dead click, not a protocol
    // fault) and a gateway must be bound. Returns whether the request landed — false when the row is
    // unknown, no gateway is bound, or the daemon refused; in every one of those cases nothing here
    // moved. A landed request that changed nothing (re-selecting the same row) still returns true:
    // the click was acted on, it simply had nothing to do.
    [[nodiscard]] bool select(const std::string& identity);
    // Ask the daemon to clear the selection. Same contract as `select`.
    [[nodiscard]] bool clear_selection();

    // --- selection: the daemon's fact (the ONLY mutator) ----------------------------------------
    //
    // Adopt the daemon's selection — the `selection-changed` fact's `ids`, the `ids` a write's own
    // reply carried back, or an `editor selection-get` read at hydration; all three are the same
    // fact from the same authority. The FIRST id is what this single-select panel renders; the rest are carried
    // by the daemon for the clients that can express them. An id with no row in the current model is
    // still adopted (the daemon's truth does not depend on what this panel has loaded) — it simply
    // renders no "(selected)" marker until a model containing it arrives. Notifies listeners and
    // returns true only when the rendered selection actually changed.
    bool apply_selection(const std::vector<std::string>& ids);

    // The RENDERED selection — a projection of daemon state, not an authoritative copy.
    [[nodiscard]] const SceneSelection& selection() const noexcept { return selection_; }

    // Register a listener other panels use to react to selection changes (R-HUX-011).
    using SelectionListener = std::function<void(const SceneSelection&)>;
    void add_selection_listener(SelectionListener listener);

    // Build the headless uitree Panel for the current model + selection + generation/stability.
    // Deterministic: identical state produces a byte-identical Panel (uitree::render_html), so a
    // re-render on settle with an unchanged world is stable. a11y-conformant by construction —
    // uitree::audit_a11y returns no violations for any model.
    [[nodiscard]] uitree::Panel build_panel() const;

private:
    void notify() const;
    // The ONE write path both `select` and `clear_selection` take (see their contract above).
    [[nodiscard]] bool write_selection(const std::vector<std::string>& ids);

    SelectionGateway* gateway_ = nullptr;
    SceneTreeModel model_;
    // The RENDERED selection: what the daemon last said, never what this panel decided.
    SceneSelection selection_;
    std::uint64_t generation_ = 0;
    bridge::Stability stability_ = bridge::Stability::stable;
    std::vector<SelectionListener> listeners_;
};

} // namespace context::editor::gui::panels::scenetree
