// The UI extract step — see context/render/ui/snapshot.h.

#include "context/render/ui/snapshot.h"

#include "context/packages/ui/ui_tree.h"
#include "context/render/ui/composite.h"

namespace context::render::ui
{

namespace
{

using packages::ui::Color;
using packages::ui::NodeId;
using packages::ui::Rect;
using packages::ui::UiNode;
using packages::ui::UiTree;

// Pre-order (painter's algorithm) walk: emit this node's quad (when it is a visible drawable), then
// recurse into its children with the composited opacity. An invisible / fully-transparent-opacity node
// prunes its whole subtree. Depth is bounded by the authored tree depth (shallow by construction).
void extract_node(const UiTree& tree, NodeId id, float parent_opacity, UiRenderSnapshot& out)
{
    const UiNode* node = tree.node(id);
    if (node == nullptr || !node->alive)
    {
        return;
    }
    if (!node->style.visible)
    {
        return; // invisible ⇒ prune node AND subtree (matches hit_test / focus_order semantics)
    }
    const float opacity = effective_opacity(parent_opacity, node->style.opacity);
    if (opacity <= 0.0f)
    {
        return; // fully transparent ⇒ nothing this node or its subtree contributes
    }

    // A drawable contributes one solid quad: a non-empty box with a non-transparent background. Root /
    // pure containers (transparent background) draw nothing but still parent visible children.
    if (node->style.background.a > 0)
    {
        const Rect rect = apply_transform(node->bounds, node->style.transform);
        if (!rect.empty())
        {
            UiQuad quad;
            quad.rect = rect;
            quad.color = node->style.background;
            quad.opacity = opacity;
            quad.node = id;
            quad.order = static_cast<std::uint32_t>(out.quads.size());
            out.quads.push_back(quad);
        }
    }

    for (const NodeId child : node->children)
    {
        extract_node(tree, child, opacity, out);
    }
}

} // namespace

void extract_ui(const UiTree& tree, const Rect& viewport, UiRenderSnapshot& out)
{
    out.clear();
    out.surface = viewport;
    ++out.generation;
    extract_node(tree, tree.root(), 1.0f, out);
}

} // namespace context::render::ui
