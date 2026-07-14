// The retained runtime UI tree (M7 T1, R-UI-002/006, locks D2/D6). Owns the UiNode store, the
// build/mutate API, event dispatch (target-then-bubble), a minimal focus model, and the dirty/damage
// accumulation a backend consumes. Pure stdlib, headless, presentation-only — it lives OUTSIDE the sim
// World and registers no sim component (D6): at this tier it does not depend on the sim at all.
//
// Node addressing is by NodeId handle (an index into the store). Removed nodes are tombstoned, never
// compacted, so a handle is stable for the node's lifetime. The tree is created with a Root node
// (root()); every other node is created under a parent.

#pragma once

#include "context/packages/ui/damage.h"
#include "context/packages/ui/events.h"
#include "context/packages/ui/ui_node.h"

#include <cstddef>
#include <string>
#include <vector>

namespace context::packages::ui
{

class UiTree
{
public:
    // Constructs a tree with a single Root node.
    UiTree();

    [[nodiscard]] NodeId root() const noexcept { return kRoot; }

    // --- build / mutate ---------------------------------------------------------------------------

    // Create a node of `role` appended as the last child of `parent`. Returns kInvalidNode if `parent`
    // is not a live node. A create is a structural change ⇒ full damage.
    NodeId create_node(Role role, NodeId parent);

    // Remove a node and all its descendants (the root cannot be removed). Returns false for an invalid/
    // dead id or the root. Structural change ⇒ full damage.
    bool remove_node(NodeId id);

    // Move `id` (and its subtree) to be the last child of `new_parent`. Refuses to move the root, a
    // dead node, or into itself/its own descendant (would cut the subtree loose). Structural ⇒ full damage.
    bool reparent(NodeId id, NodeId new_parent);

    [[nodiscard]] UiNode* node(NodeId id) noexcept;
    [[nodiscard]] const UiNode* node(NodeId id) const noexcept;

    // Style / content mutators. Each marks the node's current bounds dirty (region damage) — a repaint
    // of the changed area, not the whole surface. Return false for an invalid/dead id.
    bool set_style(NodeId id, const Style& style);
    bool set_text(NodeId id, std::string text);
    bool set_bounds(NodeId id, const Rect& bounds); // layout writes this (T2); also usable in tests
    bool set_visible(NodeId id, bool visible);

    // --- events -----------------------------------------------------------------------------------

    // Register a handler for `type` on `id`. Multiple handlers per (node, type) fire in registration
    // order. No-op for an invalid/dead id.
    void add_handler(NodeId id, EventType type, Handler handler);

    // Dispatch `ev` to ev.target, then bubble toward the root: each visited node's handlers for
    // ev.type run with ev.current set to that node; propagation stops when a handler sets ev.handled or
    // the root is passed. No-op if ev.target is invalid/dead.
    void dispatch(Event& ev);

    // Minimal focus model: move focus to `id` (kInvalidNode clears focus). Emits FocusLost to the
    // previously focused node and FocusGained to `id` (each a dispatched, bubbling event). Idempotent
    // when `id` is already focused. Returns false for an invalid/dead non-null id.
    bool set_focus(NodeId id);
    [[nodiscard]] NodeId focused() const noexcept { return focused_; }

    // --- damage -----------------------------------------------------------------------------------

    // Take the accumulated damage (coalesced) and reset the tree's pending damage to empty.
    [[nodiscard]] DamageList take_damage();

    // Force a whole-surface repaint on the next take (e.g. a viewport resize the caller owns).
    void mark_full_damage() noexcept { pending_.mark_full(); }

    // --- introspection ----------------------------------------------------------------------------

    // Number of LIVE nodes (includes the root).
    [[nodiscard]] std::size_t node_count() const noexcept;

    // Number of live proper descendants of `id` (0 for a leaf; 0 for an invalid/dead id).
    [[nodiscard]] std::size_t descendant_count(NodeId id) const noexcept;

private:
    static constexpr NodeId kRoot = 0;

    struct HandlerEntry
    {
        EventType type;
        Handler fn;
    };

    [[nodiscard]] bool is_live(NodeId id) const noexcept;
    void kill_subtree(NodeId id); // marks id + descendants dead, detaching handlers
    void detach_from_parent(NodeId id);
    [[nodiscard]] bool is_descendant_of(NodeId id, NodeId ancestor) const noexcept;

    std::vector<UiNode> nodes_;                       // node store; NodeId == index (tombstoned, not reused)
    std::vector<std::vector<HandlerEntry>> handlers_; // parallel to nodes_: handlers per node
    DamageList pending_;                              // damage accumulated since the last take
    NodeId focused_ = kInvalidNode;
};

} // namespace context::packages::ui
