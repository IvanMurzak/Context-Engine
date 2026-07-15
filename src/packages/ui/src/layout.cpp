// Headless layout + hit-testing + focus order (see layout.h). compute_layout writes each node's
// computed bounds top-down (driving a1's damage on any change); hit_test walks the tree back-to-front
// respecting visibility/opacity; focus_order is a document-order DFS over visible focusable nodes.

#include "context/packages/ui/layout.h"

#include "context/packages/ui/ui_tree.h"

namespace context::packages::ui
{
namespace
{

// The content box: a node's bounds inset by its style padding, clamped non-negative.
[[nodiscard]] Rect content_box(const Rect& r, float pad) noexcept
{
    if (pad <= 0.0f)
        return r;
    const float w = r.w - 2.0f * pad;
    const float h = r.h - 2.0f * pad;
    return Rect{r.x + pad, r.y + pad, w > 0.0f ? w : 0.0f, h > 0.0f ? h : 0.0f};
}

// A node is hittable/focusable only while visible: shown AND not fully transparent.
[[nodiscard]] bool visible(const UiNode& n) noexcept
{
    return n.style.visible && n.style.opacity > 0.0f;
}

// Write bounds only when they actually change, so an unchanged reflow produces zero damage.
void set_bounds_if_changed(UiTree& tree, NodeId id, const Rect& r)
{
    const UiNode* n = tree.node(id);
    if (n == nullptr || n->bounds == r)
        return;
    tree.set_bounds(id, r);
}

// The rect of an Absolute node within its parent's content box `c`. Opposing anchors ⇒ stretch (that
// axis's size is ignored, offset is the symmetric margin); one anchor ⇒ pinned to that edge; neither
// ⇒ centered on that axis (offset shifts the center).
[[nodiscard]] Rect place_absolute(const Layout& l, const Rect& c) noexcept
{
    const bool anchor_l = has(l.anchor, Anchor::Left);
    const bool anchor_r = has(l.anchor, Anchor::Right);
    const bool anchor_t = has(l.anchor, Anchor::Top);
    const bool anchor_b = has(l.anchor, Anchor::Bottom);

    float x = 0.0f;
    float w = l.size.x;
    if (anchor_l && anchor_r)
    {
        x = c.x + l.offset.x;
        w = c.w - 2.0f * l.offset.x;
    }
    else if (anchor_l)
        x = c.x + l.offset.x;
    else if (anchor_r)
        x = c.x + c.w - l.offset.x - l.size.x;
    else
        x = c.x + (c.w - l.size.x) * 0.5f + l.offset.x;

    float y = 0.0f;
    float h = l.size.y;
    if (anchor_t && anchor_b)
    {
        y = c.y + l.offset.y;
        h = c.h - 2.0f * l.offset.y;
    }
    else if (anchor_t)
        y = c.y + l.offset.y;
    else if (anchor_b)
        y = c.y + c.h - l.offset.y - l.size.y;
    else
        y = c.y + (c.h - l.size.y) * 0.5f + l.offset.y;

    return Rect{x, y, w > 0.0f ? w : 0.0f, h > 0.0f ? h : 0.0f};
}

// Lay out `id`'s children within its (already-set) content box, then recurse into each child.
void layout_subtree(UiTree& tree, NodeId id)
{
    std::vector<NodeId> kids;
    Rect content;
    Flow flow = Flow::None;
    float gap = 0.0f;
    {
        const UiNode* n = tree.node(id);
        if (n == nullptr)
            return;
        kids = n->children;
        content = content_box(n->bounds, n->style.padding);
        flow = n->layout.flow;
        gap = n->layout.gap;
    }

    float cursor = (flow == Flow::Row) ? content.x : content.y; // main-axis pen for flow children
    for (const NodeId kid : kids)
    {
        Layout l;
        {
            const UiNode* c = tree.node(kid);
            if (c == nullptr)
                continue;
            l = c->layout;
        }

        Rect r;
        if (l.position == Positioning::Absolute)
        {
            r = place_absolute(l, content); // out of flow — does not advance the cursor
        }
        else if (flow == Flow::Row)
        {
            const float h = (l.size.y > 0.0f) ? l.size.y : content.h; // cross-axis stretch if unset
            r = Rect{cursor, content.y, l.size.x, h};
            cursor += l.size.x + gap;
        }
        else if (flow == Flow::Column)
        {
            const float w = (l.size.x > 0.0f) ? l.size.x : content.w; // cross-axis stretch if unset
            r = Rect{content.x, cursor, w, l.size.y};
            cursor += l.size.y + gap;
        }
        else // Flow::None: a Flow child places its own box at the content origin + its offset
        {
            r = Rect{content.x + l.offset.x, content.y + l.offset.y, l.size.x, l.size.y};
        }

        set_bounds_if_changed(tree, kid, r);
    }

    for (const NodeId kid : kids)
        layout_subtree(tree, kid);
}

// Top-most hit within the subtree rooted at `id`: children back-to-front (last painted = on top),
// then the node itself. A not-visible node culls its whole subtree.
[[nodiscard]] NodeId hit_subtree(const UiTree& tree, NodeId id, float x, float y)
{
    const UiNode* n = tree.node(id);
    if (n == nullptr || !visible(*n))
        return kInvalidNode;

    for (auto it = n->children.rbegin(); it != n->children.rend(); ++it)
    {
        const NodeId hit = hit_subtree(tree, *it, x, y);
        if (hit != kInvalidNode)
            return hit;
    }

    return n->bounds.contains(x, y) ? id : kInvalidNode;
}

// Document-order DFS collecting visible focusable nodes; a not-visible node culls its subtree.
void collect_focus(const UiTree& tree, NodeId id, std::vector<NodeId>& out)
{
    const UiNode* n = tree.node(id);
    if (n == nullptr || !visible(*n))
        return;
    if (is_focusable(n->role))
        out.push_back(id);
    for (const NodeId kid : n->children)
        collect_focus(tree, kid, out);
}

} // namespace

void compute_layout(UiTree& tree, const Rect& viewport)
{
    const NodeId root = tree.root();
    set_bounds_if_changed(tree, root, viewport);
    layout_subtree(tree, root);
}

NodeId hit_test(const UiTree& tree, float x, float y)
{
    return hit_subtree(tree, tree.root(), x, y);
}

std::vector<NodeId> focus_order(const UiTree& tree)
{
    std::vector<NodeId> out;
    collect_focus(tree, tree.root(), out);
    return out;
}

bool is_focusable(Role role) noexcept
{
    switch (role)
    {
    case Role::Button:
    case Role::Slider:
    case Role::Checkbox:
    case Role::TextInput:
    case Role::ListItem:
        return true;
    case Role::Root:
    case Role::Panel:
    case Role::Group:
    case Role::Label:
    case Role::Image:
    case Role::ProgressBar:
    case Role::List:
        return false;
    }
    return false; // keeps -Wreturn-type quiet; the exhaustive switch above has no default so a new
                  // Role trips -Wswitch (a deliberate tripwire, matching role_name's style)
}

} // namespace context::packages::ui
