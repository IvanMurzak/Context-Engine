// Dynamic AABB tree implementation of the R-SIM-007 broad-phase spatial index (see spatial_index.h).

#include "context/packages/spatial/spatial_index.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace context::packages::spatial
{
namespace
{

// Node-pool sentinel: "no node" / end of the free list.
constexpr std::int32_t kNull = -1;

// Fat-box margin (world units) added around a leaf's tight box. Small movements that keep the tight
// box inside the fat box skip reinsertion (the O(1) update path). A positive margin also guarantees
// every fat box has non-zero extent, so the surface-area heuristic never divides a point.
constexpr float kFatMargin = 0.1f;

[[nodiscard]] Aabb merge(const Aabb& a, const Aabb& b) noexcept
{
    Aabb r;
    r.min.x = std::min(a.min.x, b.min.x);
    r.min.y = std::min(a.min.y, b.min.y);
    r.min.z = std::min(a.min.z, b.min.z);
    r.max.x = std::max(a.max.x, b.max.x);
    r.max.y = std::max(a.max.y, b.max.y);
    r.max.z = std::max(a.max.z, b.max.z);
    return r;
}

// Surface area of an AABB: 2*(dx*dy + dy*dz + dz*dx). The cost function the branch-and-bound leaf
// placement minimizes (a smaller total tree surface area means tighter, cheaper-to-prune nodes).
[[nodiscard]] float surface_area(const Aabb& b) noexcept
{
    const float dx = b.max.x - b.min.x;
    const float dy = b.max.y - b.min.y;
    const float dz = b.max.z - b.min.z;
    return 2.0f * (dx * dy + dy * dz + dz * dx);
}

// Repair a possibly-inverted box (min above max on some axis) by sorting each axis, then enlarge by
// the fat margin. Used for the structural (fat) box of a leaf.
[[nodiscard]] Aabb fatten(const Aabb& tight) noexcept
{
    Aabb r;
    r.min.x = std::min(tight.min.x, tight.max.x) - kFatMargin;
    r.min.y = std::min(tight.min.y, tight.max.y) - kFatMargin;
    r.min.z = std::min(tight.min.z, tight.max.z) - kFatMargin;
    r.max.x = std::max(tight.min.x, tight.max.x) + kFatMargin;
    r.max.y = std::max(tight.min.y, tight.max.y) + kFatMargin;
    r.max.z = std::max(tight.min.z, tight.max.z) + kFatMargin;
    return r;
}

// Componentwise min<=max normalization of a caller-supplied tight box (no margin).
[[nodiscard]] Aabb normalize(const Aabb& in) noexcept
{
    Aabb r;
    r.min.x = std::min(in.min.x, in.max.x);
    r.min.y = std::min(in.min.y, in.max.y);
    r.min.z = std::min(in.min.z, in.max.z);
    r.max.x = std::max(in.min.x, in.max.x);
    r.max.y = std::max(in.min.y, in.max.y);
    r.max.z = std::max(in.min.z, in.max.z);
    return r;
}

struct Node
{
    Aabb box;   // fat box (leaf) or union of children's fat boxes (internal node)
    Aabb tight; // leaf only: the exact bounds used for query membership
    kernel::Entity entity{};
    std::int32_t parent = kNull;
    std::int32_t child1 = kNull;
    std::int32_t child2 = kNull;
    // Tree height: 0 = leaf, 1 + max(child heights) = internal, -1 = node is on the free list (then
    // `parent` doubles as the next-free index).
    std::int32_t height = 0;

    [[nodiscard]] bool is_leaf() const noexcept { return child1 == kNull; }
};

} // namespace

struct SpatialIndex::Impl
{
    std::vector<Node> nodes;
    std::int32_t root = kNull;
    std::int32_t free_list = kNull;
    std::size_t leaf_count = 0;
    std::unordered_map<kernel::Entity, std::int32_t> entity_to_node;
    mutable std::size_t last_query_cost = 0;

    // Index the node pool without implicit signed/unsigned conversions (MSVC /W4 clean).
    [[nodiscard]] Node& node(std::int32_t i) noexcept { return nodes[static_cast<std::size_t>(i)]; }
    [[nodiscard]] const Node& node(std::int32_t i) const noexcept
    {
        return nodes[static_cast<std::size_t>(i)];
    }

    [[nodiscard]] std::int32_t allocate_node()
    {
        if (free_list == kNull)
        {
            nodes.emplace_back();
            return static_cast<std::int32_t>(nodes.size() - 1);
        }
        const std::int32_t i = free_list;
        free_list = nodes[static_cast<std::size_t>(i)].parent;
        Node& n = node(i);
        n = Node{};
        return i;
    }

    void free_node(std::int32_t i)
    {
        Node& n = node(i);
        n.parent = free_list;
        n.height = -1;
        free_list = i;
    }

    // Rebalance the subtree rooted at `ia` with a single rotation if its children's heights differ by
    // more than one (the Box2D b2DynamicTree rotation). Returns the new subtree root index.
    std::int32_t balance(std::int32_t ia)
    {
        Node& a = node(ia);
        if (a.is_leaf() || a.height < 2)
        {
            return ia;
        }

        const std::int32_t ib = a.child1;
        const std::int32_t ic = a.child2;
        Node& b = node(ib);
        Node& c = node(ic);
        const std::int32_t balance_factor = c.height - b.height;

        // Rotate C up.
        if (balance_factor > 1)
        {
            const std::int32_t if_ = c.child1;
            const std::int32_t ig = c.child2;
            Node& f = node(if_);
            Node& g = node(ig);

            c.child1 = ia;
            c.parent = a.parent;
            a.parent = ic;

            if (c.parent != kNull)
            {
                if (node(c.parent).child1 == ia)
                    node(c.parent).child1 = ic;
                else
                    node(c.parent).child2 = ic;
            }
            else
            {
                root = ic;
            }

            if (f.height > g.height)
            {
                c.child2 = if_;
                a.child2 = ig;
                g.parent = ia;
                a.box = merge(b.box, g.box);
                c.box = merge(a.box, f.box);
                a.height = 1 + std::max(b.height, g.height);
                c.height = 1 + std::max(a.height, f.height);
            }
            else
            {
                c.child2 = ig;
                a.child2 = if_;
                f.parent = ia;
                a.box = merge(b.box, f.box);
                c.box = merge(a.box, g.box);
                a.height = 1 + std::max(b.height, f.height);
                c.height = 1 + std::max(a.height, g.height);
            }
            return ic;
        }

        // Rotate B up.
        if (balance_factor < -1)
        {
            const std::int32_t id = b.child1;
            const std::int32_t ie = b.child2;
            Node& d = node(id);
            Node& e = node(ie);

            b.child1 = ia;
            b.parent = a.parent;
            a.parent = ib;

            if (b.parent != kNull)
            {
                if (node(b.parent).child1 == ia)
                    node(b.parent).child1 = ib;
                else
                    node(b.parent).child2 = ib;
            }
            else
            {
                root = ib;
            }

            if (d.height > e.height)
            {
                b.child2 = id;
                a.child1 = ie;
                e.parent = ia;
                a.box = merge(c.box, e.box);
                b.box = merge(a.box, d.box);
                a.height = 1 + std::max(c.height, e.height);
                b.height = 1 + std::max(a.height, d.height);
            }
            else
            {
                b.child2 = ie;
                a.child1 = id;
                d.parent = ia;
                a.box = merge(c.box, d.box);
                b.box = merge(a.box, e.box);
                a.height = 1 + std::max(c.height, d.height);
                b.height = 1 + std::max(a.height, e.height);
            }
            return ib;
        }

        return ia;
    }

    // Walk from `start` to the root, re-fitting each ancestor's box + height and rebalancing.
    void refit_and_balance(std::int32_t start)
    {
        std::int32_t i = start;
        while (i != kNull)
        {
            i = balance(i);
            const std::int32_t c1 = node(i).child1;
            const std::int32_t c2 = node(i).child2;
            node(i).height = 1 + std::max(node(c1).height, node(c2).height);
            node(i).box = merge(node(c1).box, node(c2).box);
            i = node(i).parent;
        }
    }

    void insert_leaf(std::int32_t leaf)
    {
        if (root == kNull)
        {
            root = leaf;
            node(leaf).parent = kNull;
            return;
        }

        // Descend from the root to the best sibling via surface-area branch-and-bound.
        const Aabb leaf_box = node(leaf).box;
        std::int32_t index = root;
        while (!node(index).is_leaf())
        {
            const std::int32_t c1 = node(index).child1;
            const std::int32_t c2 = node(index).child2;

            const float area = surface_area(node(index).box);
            const Aabb combined = merge(node(index).box, leaf_box);
            const float combined_area = surface_area(combined);

            // Cost of creating a new parent here for `leaf` and the current node.
            const float cost = 2.0f * combined_area;
            // Minimum cost of pushing `leaf` further down (inherited by every ancestor).
            const float inheritance_cost = 2.0f * (combined_area - area);

            const float cost1 = descend_cost(c1, leaf_box, inheritance_cost);
            const float cost2 = descend_cost(c2, leaf_box, inheritance_cost);

            if (cost < cost1 && cost < cost2)
            {
                break;
            }
            index = (cost1 < cost2) ? c1 : c2;
        }

        const std::int32_t sibling = index;

        // Splice a new internal parent above `sibling`, with `sibling` and `leaf` as its children.
        const std::int32_t old_parent = node(sibling).parent;
        const std::int32_t new_parent = allocate_node();
        node(new_parent).parent = old_parent;
        node(new_parent).box = merge(leaf_box, node(sibling).box);
        node(new_parent).height = node(sibling).height + 1;
        node(new_parent).child1 = sibling;
        node(new_parent).child2 = leaf;
        node(sibling).parent = new_parent;
        node(leaf).parent = new_parent;

        if (old_parent != kNull)
        {
            if (node(old_parent).child1 == sibling)
                node(old_parent).child1 = new_parent;
            else
                node(old_parent).child2 = new_parent;
        }
        else
        {
            root = new_parent;
        }

        refit_and_balance(node(leaf).parent);
    }

    // Cost of inserting `leaf_box` into the subtree rooted at `child`.
    [[nodiscard]] float descend_cost(std::int32_t child, const Aabb& leaf_box,
                                     float inheritance_cost) const noexcept
    {
        const Aabb combined = merge(leaf_box, node(child).box);
        if (node(child).is_leaf())
        {
            return surface_area(combined) + inheritance_cost;
        }
        // A non-leaf pays only the INCREASE in its box's area (the box is not replaced, just grown).
        const float old_area = surface_area(node(child).box);
        const float new_area = surface_area(combined);
        return (new_area - old_area) + inheritance_cost;
    }

    void remove_leaf(std::int32_t leaf)
    {
        if (leaf == root)
        {
            root = kNull;
            return;
        }

        const std::int32_t parent = node(leaf).parent;
        const std::int32_t grand_parent = node(parent).parent;
        const std::int32_t sibling =
            (node(parent).child1 == leaf) ? node(parent).child2 : node(parent).child1;

        if (grand_parent != kNull)
        {
            // Replace `parent` with `sibling` under `grand_parent`, then refit up.
            if (node(grand_parent).child1 == parent)
                node(grand_parent).child1 = sibling;
            else
                node(grand_parent).child2 = sibling;
            node(sibling).parent = grand_parent;
            free_node(parent);
            refit_and_balance(grand_parent);
        }
        else
        {
            root = sibling;
            node(sibling).parent = kNull;
            free_node(parent);
        }
    }

    // Depth-first structural check of the subtree rooted at `i`; accumulates the visited leaf count.
    [[nodiscard]] bool validate_node(std::int32_t i, std::int32_t expected_parent,
                                     std::size_t& leaves_seen) const
    {
        if (i == kNull)
        {
            return true;
        }
        if (i < 0 || static_cast<std::size_t>(i) >= nodes.size())
        {
            return false;
        }
        const Node& n = node(i);
        if (n.parent != expected_parent)
        {
            return false;
        }
        if (n.is_leaf())
        {
            if (n.child2 != kNull || n.height != 0)
            {
                return false;
            }
            if (!n.box.contains(n.tight))
            {
                return false;
            }
            ++leaves_seen;
            return true;
        }
        if (n.child1 == kNull || n.child2 == kNull)
        {
            return false;
        }
        const Node& c1 = node(n.child1);
        const Node& c2 = node(n.child2);
        const std::int32_t expect_height = 1 + std::max(c1.height, c2.height);
        if (n.height != expect_height)
        {
            return false;
        }
        if (!n.box.contains(c1.box) || !n.box.contains(c2.box))
        {
            return false;
        }
        return validate_node(n.child1, i, leaves_seen) && validate_node(n.child2, i, leaves_seen);
    }
};

// --- Aabb geometry --------------------------------------------------------------------------------

Aabb Aabb::from_center_half_extents(const Vec3& center, const Vec3& half_extents) noexcept
{
    const float hx = std::max(0.0f, half_extents.x);
    const float hy = std::max(0.0f, half_extents.y);
    const float hz = std::max(0.0f, half_extents.z);
    Aabb r;
    r.min = Vec3{center.x - hx, center.y - hy, center.z - hz};
    r.max = Vec3{center.x + hx, center.y + hy, center.z + hz};
    return r;
}

bool Aabb::overlaps(const Aabb& other) const noexcept
{
    return min.x <= other.max.x && max.x >= other.min.x && //
           min.y <= other.max.y && max.y >= other.min.y && //
           min.z <= other.max.z && max.z >= other.min.z;
}

bool Aabb::contains(const Aabb& other) const noexcept
{
    return min.x <= other.min.x && max.x >= other.max.x && //
           min.y <= other.min.y && max.y >= other.max.y && //
           min.z <= other.min.z && max.z >= other.max.z;
}

bool Aabb::overlaps_sphere(const Vec3& c, float radius) const noexcept
{
    if (radius < 0.0f)
    {
        return false;
    }
    // Normalize each axis first: the Aabb contract lets callers pass either corner ordering, but
    // std::clamp requires lo <= hi (an inverted box would be undefined behavior). Repair per-axis.
    const float lo_x = std::min(min.x, max.x), hi_x = std::max(min.x, max.x);
    const float lo_y = std::min(min.y, max.y), hi_y = std::max(min.y, max.y);
    const float lo_z = std::min(min.z, max.z), hi_z = std::max(min.z, max.z);
    // Closest point on the box to the sphere center, then its squared distance.
    const float cx = std::clamp(c.x, lo_x, hi_x);
    const float cy = std::clamp(c.y, lo_y, hi_y);
    const float cz = std::clamp(c.z, lo_z, hi_z);
    const float dx = c.x - cx;
    const float dy = c.y - cy;
    const float dz = c.z - cz;
    const float d2 = dx * dx + dy * dy + dz * dz;
    return d2 <= radius * radius;
}

Vec3 Aabb::center() const noexcept
{
    return Vec3{0.5f * (min.x + max.x), 0.5f * (min.y + max.y), 0.5f * (min.z + max.z)};
}

// --- SpatialIndex ---------------------------------------------------------------------------------

SpatialIndex::SpatialIndex() : impl_(std::make_unique<Impl>()) {}
SpatialIndex::~SpatialIndex() = default;
SpatialIndex::SpatialIndex(SpatialIndex&&) noexcept = default;
SpatialIndex& SpatialIndex::operator=(SpatialIndex&&) noexcept = default;

bool SpatialIndex::insert(kernel::Entity e, const Aabb& bounds)
{
    if (!e.valid())
    {
        return false;
    }
    const auto it = impl_->entity_to_node.find(e);
    if (it != impl_->entity_to_node.end())
    {
        update(e, bounds);
        return false;
    }

    const std::int32_t leaf = impl_->allocate_node();
    Node& n = impl_->node(leaf);
    n.entity = e;
    n.tight = normalize(bounds);
    n.box = fatten(bounds);
    n.child1 = kNull;
    n.child2 = kNull;
    n.height = 0;

    impl_->insert_leaf(leaf);
    impl_->entity_to_node.emplace(e, leaf);
    ++impl_->leaf_count;
    return true;
}

bool SpatialIndex::update(kernel::Entity e, const Aabb& bounds)
{
    const auto it = impl_->entity_to_node.find(e);
    if (it == impl_->entity_to_node.end())
    {
        return false;
    }
    const std::int32_t leaf = it->second;
    const Aabb tight = normalize(bounds);
    impl_->node(leaf).tight = tight;

    // Fast path: the new tight box still fits inside the leaf's fat box — no structural change.
    if (impl_->node(leaf).box.contains(tight))
    {
        return true;
    }

    impl_->remove_leaf(leaf);
    Node& n = impl_->node(leaf);
    n.box = fatten(bounds);
    n.tight = tight;
    n.child1 = kNull;
    n.child2 = kNull;
    n.height = 0;
    impl_->insert_leaf(leaf);
    return true;
}

bool SpatialIndex::remove(kernel::Entity e)
{
    const auto it = impl_->entity_to_node.find(e);
    if (it == impl_->entity_to_node.end())
    {
        return false;
    }
    const std::int32_t leaf = it->second;
    impl_->remove_leaf(leaf);
    impl_->free_node(leaf);
    impl_->entity_to_node.erase(it);
    --impl_->leaf_count;
    return true;
}

void SpatialIndex::clear() noexcept
{
    impl_->nodes.clear();
    impl_->entity_to_node.clear();
    impl_->root = kNull;
    impl_->free_list = kNull;
    impl_->leaf_count = 0;
    impl_->last_query_cost = 0;
}

void SpatialIndex::query_aabb(const Aabb& box, std::vector<kernel::Entity>& out) const
{
    impl_->last_query_cost = 0;
    if (impl_->root == kNull)
    {
        return;
    }
    // Explicit stack (a deep tree could otherwise overflow the call stack).
    std::vector<std::int32_t> stack;
    stack.push_back(impl_->root);
    while (!stack.empty())
    {
        const std::int32_t idx = stack.back();
        stack.pop_back();
        ++impl_->last_query_cost;

        const Node& n = impl_->node(idx);
        if (!n.box.overlaps(box))
        {
            continue;
        }
        if (n.is_leaf())
        {
            if (n.tight.overlaps(box))
            {
                out.push_back(n.entity);
            }
        }
        else
        {
            stack.push_back(n.child1);
            stack.push_back(n.child2);
        }
    }
}

void SpatialIndex::query_radius(const Vec3& center, float radius,
                                std::vector<kernel::Entity>& out) const
{
    impl_->last_query_cost = 0;
    if (impl_->root == kNull || radius < 0.0f)
    {
        return;
    }
    std::vector<std::int32_t> stack;
    stack.push_back(impl_->root);
    while (!stack.empty())
    {
        const std::int32_t idx = stack.back();
        stack.pop_back();
        ++impl_->last_query_cost;

        const Node& n = impl_->node(idx);
        if (!n.box.overlaps_sphere(center, radius))
        {
            continue;
        }
        if (n.is_leaf())
        {
            if (n.tight.overlaps_sphere(center, radius))
            {
                out.push_back(n.entity);
            }
        }
        else
        {
            stack.push_back(n.child1);
            stack.push_back(n.child2);
        }
    }
}

std::vector<kernel::Entity> SpatialIndex::query_aabb(const Aabb& box) const
{
    std::vector<kernel::Entity> out;
    query_aabb(box, out);
    return out;
}

std::vector<kernel::Entity> SpatialIndex::query_radius(const Vec3& center, float radius) const
{
    std::vector<kernel::Entity> out;
    query_radius(center, radius, out);
    return out;
}

bool SpatialIndex::contains(kernel::Entity e) const noexcept
{
    return impl_->entity_to_node.find(e) != impl_->entity_to_node.end();
}

std::size_t SpatialIndex::size() const noexcept
{
    return impl_->leaf_count;
}

int SpatialIndex::height() const noexcept
{
    if (impl_->root == kNull)
    {
        return -1;
    }
    return static_cast<int>(impl_->node(impl_->root).height);
}

std::size_t SpatialIndex::node_count() const noexcept
{
    return impl_->leaf_count == 0 ? std::size_t{0} : (2 * impl_->leaf_count - 1);
}

std::size_t SpatialIndex::last_query_visited_nodes() const noexcept
{
    return impl_->last_query_cost;
}

bool SpatialIndex::validate() const
{
    std::size_t leaves_seen = 0;
    if (!impl_->validate_node(impl_->root, kNull, leaves_seen))
    {
        return false;
    }
    if (leaves_seen != impl_->leaf_count)
    {
        return false;
    }
    if (impl_->entity_to_node.size() != impl_->leaf_count)
    {
        return false;
    }
    return true;
}

} // namespace context::packages::spatial
