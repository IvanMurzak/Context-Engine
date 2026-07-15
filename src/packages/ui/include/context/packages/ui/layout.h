// Headless layout + hit-testing + focus order for the runtime UI tree (M7 T2, R-UI-006/003). Computed
// geometry with NO renderer: an anchored/absolute + stack/flex-lite flow layout that writes each node's
// computed `bounds`, top-most point hit-testing that respects visibility/opacity, and a deterministic
// focus order. All three are free functions over the public UiTree/UiNode surface (the layout INPUT
// types live on the node in ui_node.h) — pure stdlib, so CLI/AI can drive and assert UI without a GPU.

#pragma once

#include "context/packages/ui/ui_node.h"

#include <vector>

namespace context::packages::ui
{

class UiTree; // operated on by-reference; the .cpp uses the full definition

// Compute every node's `bounds` from the tree's layout inputs, top-down from the root (whose box is
// `viewport`). Each node arranges its Flow children along its `flow` axis (Row/Column, `gap`-spaced,
// inside the padding-inset content box); Absolute children anchor to the content-box edges. A node
// whose computed rect CHANGES is written through UiTree::set_bounds, so a resize/reflow drives the a1
// damage machinery (only the moved rects are damaged). Idempotent: re-running with the same inputs +
// viewport produces no further damage.
void compute_layout(UiTree& tree, const Rect& viewport);

// The top-most node whose computed bounds contain (x, y), or kInvalidNode if none. "Top-most" is
// painter's order: a child paints over its parent, and a later sibling over an earlier one, so the
// DEEPEST last-painted node under the point wins. A node that is not visible (style.visible == false
// OR style.opacity <= 0) is not hittable AND its whole subtree is skipped. Runs against the computed
// rects, so call compute_layout first (or set bounds explicitly).
[[nodiscard]] NodeId hit_test(const UiTree& tree, float x, float y);

// The deterministic focus order: focusable nodes in document order (pre-order DFS — parent before
// children, children in order). A not-visible node's subtree is skipped (so a hidden container hides
// its focusable descendants). Independent of computed geometry — tab order is document order.
[[nodiscard]] std::vector<NodeId> focus_order(const UiTree& tree);

// Whether a role participates in focus/tab navigation — the interactive roles of the closed vocabulary
// (Button, Slider, Checkbox, TextInput, ListItem). The a11y hook (R-A11Y-002, post-core) refines this.
[[nodiscard]] bool is_focusable(Role role) noexcept;

} // namespace context::packages::ui
