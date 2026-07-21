// The LIVE scene-tree feed for the Scene Tree panel (M9 e05d3, design 04 §4 / 05 §7) — the
// projection from the daemon's `editor.scene-tree` composed read onto the headless
// `SceneTreePanel` model, plus the `PanelProvider` that publishes it through the Shell's panel host.
//
// WHERE THIS SITS. The Shell is an ordinary client (D10/D18): the kernel-typed model BUILDER runs on
// the daemon (gui/panels/builders/, served by `editor.scene-tree`), and what arrives here is the
// boundary-clean panel model AS DATA. This file is deliberately the ONLY place that wire shape is
// interpreted — the parsers mirror builders::scene_tree_to_wire, and the feed tests link both halves
// so the two sides cannot drift silently.
//
// READ CADENCE (05 §7: hydrate from reads + subscriptions). The feed itself performs NO IO: it marks
// a fetch DUE — on construction (first hydration) and on every `derivation.settled` event — and the
// owner loop's pump (`pump_panel_feeds`, builtin_panels.h) performs the RPC and hands the result
// back to `apply_result`. That split is what keeps every function here pure and T1-testable on all
// three default `build` legs, exactly like the ProblemsFeed parsers.
//
// TOLERANCE IS A DESIGN CHOICE (the ProblemsFeed discipline): a malformed / unrecognized payload is
// SKIPPED (the feed reports it by returning false), never fatal — a Scene tree that is quietly
// empty is honest about a daemon that answered nothing, and the strict shape is asserted by the
// round-trip tests, not by runtime brittleness.

#pragma once

#include "context/editor/contract/json.h"
#include "context/editor/gui/panels/scenetree/scene_tree_model.h"
#include "context/editor/gui/panels/scenetree/scene_tree_panel.h"
#include "context/editor/shell/panel_host.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace context::editor::shell::panels
{

namespace scenetree = gui::panels::scenetree;

// --------------------------------------------------------------------------------- pure parsers

// A u64 carried as a lowercase-hex wire string (builders::wire's identityHash discipline — a JSON
// number is a double and would lose bits above 2^53). 0 for the empty string or any non-hex input —
// 0 is exactly the model's "no hash" value (a synthetic instance boundary), so garbage degrades to
// the honest default rather than a fabricated identity.
[[nodiscard]] std::uint64_t parse_hex_u64(const std::string& text);

// Project one `editor.scene-tree` reply's `sceneTree` member back into the panel model. nullopt when
// `wire` is not an object or carries no `roots` array — the one shape that says nothing about the
// tree. Unparseable NODES within a recognized container are SKIPPED, never fatal.
[[nodiscard]] std::optional<scenetree::SceneTreeModel>
parse_scene_tree(const contract::Json& wire);

// ---------------------------------------------------------------- the node-id -> identity mapping

// The prefix `SceneTreePanel::build_panel` gives every tree row (`scenetree.item.<identity>`). Named
// here so the one place that depends on it is greppable from both sides (the kProblemsRowPrefix
// pattern).
inline constexpr const char* kSceneTreeRowPrefix = "scenetree.item.";

// Resolve an activated NODE id to the selection identity `SceneTreePanel::select` expects. The
// hydration runtime dispatches node ids — it knows nothing about scene identities, and teaching it
// would be the panel-specific special-casing e05d3 must not introduce (the host and runtime stay
// agnostic; this function absorbs the difference). nullopt for a node that is not a tree row.
[[nodiscard]] std::optional<std::string> scenetree_row_identity(const std::string& node_id);

// ------------------------------------------------------------------------------------ the feed

// Owns a SceneTreePanel and drives it from the live daemon reads + the derivation topic, touching
// the PanelHost on every change so the next `panel.render` is seen as fresh.
class SceneTreeFeed
{
public:
    // Non-owning: `host` must outlive the feed. `panel_id` is passed rather than hardcoded — the
    // feed is a MECHANISM the composition root points at a roster id.
    SceneTreeFeed(PanelHost& host, std::string panel_id);

    SceneTreeFeed(const SceneTreeFeed&) = delete;
    SceneTreeFeed& operator=(const SceneTreeFeed&) = delete;
    SceneTreeFeed(SceneTreeFeed&&) = delete;
    SceneTreeFeed& operator=(SceneTreeFeed&&) = delete;

    // Adopt one `editor.scene-tree` reply. Accepts the envelope (`{ok, data: {sceneTree, …}}`), the
    // bare `data`, or the bare `sceneTree` object — the daemon's envelope framing is not this feed's
    // to pin. Returns true when a model was actually adopted (the host was touched).
    bool apply_result(const contract::Json& reply);

    // Consume one subscription event. `derivation.settled` advances the panel's generation/stability
    // line AND marks a refetch due (the composed world may have changed shape). Returns true when
    // the panel's rendered surface changed. Unknown topics are ignored.
    bool apply_event(const std::string& topic, const contract::Json& payload,
                     std::uint64_t generation);

    // The pump's contract: `fetch_due()` says a live re-read is wanted; the pump calls
    // `mark_fetched()` BEFORE issuing the RPC (claiming the fetch), so a failed call waits for the
    // next settle rather than hammering a dead daemon every frame.
    [[nodiscard]] bool fetch_due() const noexcept { return fetch_due_; }
    void mark_fetched() noexcept { fetch_due_ = false; }

    [[nodiscard]] scenetree::SceneTreePanel& panel() noexcept { return panel_; }
    [[nodiscard]] const scenetree::SceneTreePanel& panel() const noexcept { return panel_; }

    [[nodiscard]] std::size_t results_applied() const noexcept { return results_applied_; }
    [[nodiscard]] std::size_t events_applied() const noexcept { return events_applied_; }

    // The provider to bind on the PanelHost. Captures `this` — the feed must OUTLIVE the binding (at
    // the composition root both live for the process's lifetime). Scene tree is a read-only observer
    // with SELECTION (`scenetree.select` — the R-HUX-011 loop the Inspector feed subscribes to): no
    // gestures, no persisted state, both REPORTED absent rather than stubbed.
    [[nodiscard]] PanelProvider make_provider();

private:
    PanelHost& host_;
    std::string panel_id_;
    scenetree::SceneTreePanel panel_;
    bool fetch_due_ = true; // born due: the first pump performs the initial hydration
    std::size_t results_applied_ = 0;
    std::size_t events_applied_ = 0;
};

} // namespace context::editor::shell::panels
