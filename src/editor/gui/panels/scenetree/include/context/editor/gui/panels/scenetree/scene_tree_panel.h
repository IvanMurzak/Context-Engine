// The scene-tree observer panel (M5-F2, R-EDIT-001 / R-BRIDGE-008 / L-35 / R-A11Y-001 / R-HUX-011):
// projects the composed derived-world hierarchy into a headless context_gui_uitree Panel, drives
// selection (emitting a selection event other panels consume), and re-renders on the R-BRIDGE-008
// derivation.settled event while respecting the stability field. Read-only observer: no writes into
// the world, no new error-catalog codes. The whole panel is CI-assertable WITHOUT booting CEF.

#pragma once

#include "context/editor/gui/panels/scenetree/scene_tree_model.h"

#include "context/editor/gui/uitree/panel.h"

#include "context/editor/bridge/event_stream.h" // bridge::Stability

#include <cstdint>
#include <functional>
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

class SceneTreePanel
{
public:
    // The R-EDIT-001 contribution id this built-in panel registers under.
    static constexpr const char* kContributionId = "builtin.scene-tree";

    SceneTreePanel() = default;

    // Replace the rendered derived world (e.g. from a fresh bridge query snapshot). Preserves the
    // current selection when its identity still exists in the new model; clears it (notifying
    // listeners) otherwise.
    void set_model(SceneTreeModel model);
    [[nodiscard]] const SceneTreeModel& model() const noexcept { return model_; }

    // R-BRIDGE-008: consume a derivation.settled event — advance the tracked generation, record the
    // stability the settled generation reports, and re-project. `set_model` refreshes the tree from a
    // snapshot; this updates the generation/stability the status line and re-render reflect, so a
    // settling world is visibly distinguished from a stable one.
    void on_derivation_settled(std::uint64_t generation, bridge::Stability stability);

    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] bridge::Stability stability() const noexcept { return stability_; }

    // Selection. `select` sets the current selection to the node `identity` (must exist in the model;
    // an unknown identity is ignored and returns false), notifies every registered listener, and
    // returns true. `clear_selection` resets to empty and notifies.
    bool select(const std::string& identity);
    void clear_selection();
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

    SceneTreeModel model_;
    SceneSelection selection_;
    std::uint64_t generation_ = 0;
    bridge::Stability stability_ = bridge::Stability::stable;
    std::vector<SelectionListener> listeners_;
};

} // namespace context::editor::gui::panels::scenetree
