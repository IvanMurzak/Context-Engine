// Headless introspection shims over the runtime UI tree (M7 T5 / a5, R-UI-006). Pure stdlib free
// functions over the public UiTree/UiNode surface: a deterministic pre-order walk of the live nodes, a
// name -> node lookup, and the string<->Role bridge. They back the headless `context ui …` drive/assert
// verbs (dump / query / send / assert) — the CLI walks + addresses a tree with these, so the same
// introspection the CLI uses is unit-tested here, with no GPU and no contract-layer link (the CLI
// serializes the walk to the R-CLI-008 envelope; this package stays pure stdlib, D6 presentation).

#pragma once

#include "context/packages/ui/ui_node.h"

#include <string>
#include <vector>

namespace context::packages::ui
{

class UiTree; // operated on by-reference; the .cpp uses the full definition

// Every LIVE node in document order (pre-order DFS: parent before its children, children in order),
// starting at the root. Deterministic and stable — the addressing/enumeration order the dump verb and
// the samples corpus rely on. Tombstoned (removed) nodes are skipped.
[[nodiscard]] std::vector<NodeId> live_nodes_preorder(const UiTree& tree);

// The first live node (in pre-order) whose UiNode::name equals `name`, or kInvalidNode when none —
// the author-facing address `ui query` / `ui send` / `ui assert` resolve a node by. An empty `name`
// never matches (the root and unnamed nodes carry an empty name; address them by role/structure, not
// by name).
[[nodiscard]] NodeId find_by_name(const UiTree& tree, const std::string& name);

// The Role for a lowercase role name — the inverse of ui_node.h's role_name(). Returns false for an
// unknown name (so a scene loader / assert rejects an out-of-vocabulary role rather than defaulting).
[[nodiscard]] bool role_from_name(const std::string& name, Role& out) noexcept;

} // namespace context::packages::ui
