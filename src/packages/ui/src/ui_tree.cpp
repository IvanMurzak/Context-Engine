// UiTree — retained runtime UI tree: build/mutate, dispatch, focus, damage (see ui_tree.h).

#include "context/packages/ui/ui_tree.h"

#include <algorithm>
#include <utility>

namespace context::packages::ui
{

UiTree::UiTree()
{
    UiNode root;
    root.id = kRoot;
    root.parent = kInvalidNode;
    root.role = Role::Root;
    root.alive = true;
    nodes_.push_back(std::move(root));
    handlers_.emplace_back();
}

bool UiTree::is_live(NodeId id) const noexcept
{
    return id != kInvalidNode && id < nodes_.size() && nodes_[id].alive;
}

UiNode* UiTree::node(NodeId id) noexcept
{
    return is_live(id) ? &nodes_[id] : nullptr;
}

const UiNode* UiTree::node(NodeId id) const noexcept
{
    return is_live(id) ? &nodes_[id] : nullptr;
}

NodeId UiTree::create_node(Role role, NodeId parent)
{
    if (!is_live(parent))
        return kInvalidNode;

    const NodeId id = static_cast<NodeId>(nodes_.size());
    UiNode n;
    n.id = id;
    n.parent = parent;
    n.role = role;
    n.alive = true;
    nodes_.push_back(std::move(n));
    handlers_.emplace_back();

    nodes_[parent].children.push_back(id); // parent index stays valid across the append
    pending_.mark_full();                  // structural change
    return id;
}

void UiTree::detach_from_parent(NodeId id)
{
    if (id == kInvalidNode || id >= nodes_.size())
        return;
    const NodeId p = nodes_[id].parent;
    if (p != kInvalidNode && p < nodes_.size())
    {
        std::vector<NodeId>& kids = nodes_[p].children;
        kids.erase(std::remove(kids.begin(), kids.end(), id), kids.end());
    }
    nodes_[id].parent = kInvalidNode;
}

void UiTree::kill_subtree(NodeId id)
{
    if (id == kInvalidNode || id >= nodes_.size())
        return;
    if (!nodes_[id].alive)
        return;

    const std::vector<NodeId> kids = nodes_[id].children; // copy: cleared below, then recursed
    nodes_[id].alive = false;
    nodes_[id].children.clear();
    handlers_[id].clear();
    if (focused_ == id)
        focused_ = kInvalidNode;

    for (const NodeId c : kids)
        kill_subtree(c);
}

bool UiTree::remove_node(NodeId id)
{
    if (!is_live(id) || id == kRoot)
        return false;
    detach_from_parent(id);
    kill_subtree(id);
    pending_.mark_full(); // structural change
    return true;
}

bool UiTree::is_descendant_of(NodeId id, NodeId ancestor) const noexcept
{
    // True iff `id` sits within the subtree rooted at `ancestor` (ancestor is an ancestor of id, or ==).
    NodeId cur = id;
    while (cur != kInvalidNode && cur < nodes_.size())
    {
        if (cur == ancestor)
            return true;
        cur = nodes_[cur].parent;
    }
    return false;
}

bool UiTree::reparent(NodeId id, NodeId new_parent)
{
    if (!is_live(id) || id == kRoot)
        return false;
    if (!is_live(new_parent))
        return false;
    if (id == new_parent)
        return false;
    if (is_descendant_of(new_parent, id)) // would cut the subtree loose
        return false;

    detach_from_parent(id);
    nodes_[id].parent = new_parent;
    nodes_[new_parent].children.push_back(id);
    pending_.mark_full(); // structural change
    return true;
}

bool UiTree::set_style(NodeId id, const Style& style)
{
    UiNode* n = node(id);
    if (n == nullptr)
        return false;
    n->style = style;
    pending_.add(n->bounds);
    return true;
}

bool UiTree::set_layout(NodeId id, const Layout& layout)
{
    UiNode* n = node(id);
    if (n == nullptr)
        return false;
    n->layout = layout;
    // A layout-input change dirties the node's current area; the actual reflow (and the damage its
    // moved rects produce) happens when compute_layout runs and calls set_bounds.
    pending_.add(n->bounds);
    return true;
}

bool UiTree::set_text(NodeId id, std::string text)
{
    UiNode* n = node(id);
    if (n == nullptr)
        return false;
    n->text = std::move(text);
    pending_.add(n->bounds);
    return true;
}

bool UiTree::set_bounds(NodeId id, const Rect& bounds)
{
    UiNode* n = node(id);
    if (n == nullptr)
        return false;
    pending_.add(n->bounds); // old area
    n->bounds = bounds;
    pending_.add(n->bounds); // new area
    return true;
}

bool UiTree::set_visible(NodeId id, bool visible)
{
    UiNode* n = node(id);
    if (n == nullptr)
        return false;
    n->style.visible = visible;
    pending_.add(n->bounds);
    return true;
}

void UiTree::add_handler(NodeId id, EventType type, Handler handler)
{
    if (!is_live(id))
        return;
    handlers_[id].push_back(HandlerEntry{type, std::move(handler)});
}

void UiTree::dispatch(Event& ev)
{
    if (!is_live(ev.target))
        return;

    NodeId cur = ev.target;
    while (cur != kInvalidNode && is_live(cur))
    {
        ev.current = cur;
        // Snapshot the handler list so a handler that mutates the tree/handlers can't invalidate us.
        const std::vector<HandlerEntry> snapshot = handlers_[cur];
        for (const HandlerEntry& h : snapshot)
            if (h.type == ev.type && h.fn)
                h.fn(ev);

        if (ev.handled)
            break;
        cur = is_live(cur) ? nodes_[cur].parent : kInvalidNode; // bubble toward the root
    }
}

bool UiTree::set_focus(NodeId id)
{
    if (id != kInvalidNode && !is_live(id))
        return false;
    if (focused_ == id)
        return true;

    const NodeId previous = focused_;
    focused_ = id; // update first so a handler observing focused() sees the new state

    if (previous != kInvalidNode && is_live(previous))
    {
        Event lost;
        lost.type = EventType::FocusLost;
        lost.target = previous;
        dispatch(lost);
    }
    if (id != kInvalidNode)
    {
        Event gained;
        gained.type = EventType::FocusGained;
        gained.target = id;
        dispatch(gained);
    }
    return true;
}

DamageList UiTree::take_damage()
{
    DamageList out = pending_;
    out.coalesce();
    pending_.clear();
    return out;
}

std::size_t UiTree::node_count() const noexcept
{
    std::size_t count = 0;
    for (const UiNode& n : nodes_)
        if (n.alive)
            ++count;
    return count;
}

std::size_t UiTree::descendant_count(NodeId id) const noexcept
{
    if (!is_live(id))
        return 0;
    std::size_t count = 0;
    std::vector<NodeId> stack = nodes_[id].children;
    while (!stack.empty())
    {
        const NodeId cur = stack.back();
        stack.pop_back();
        if (!is_live(cur))
            continue;
        ++count;
        for (const NodeId child : nodes_[cur].children)
            stack.push_back(child);
    }
    return count;
}

} // namespace context::packages::ui
