// Broad-phase spatial acceleration structure (R-SIM-007): an incrementally-updated dynamic AABB tree
// (a dynamic bounding-volume hierarchy) over transform-bearing entities.
//
// This is a first-party PACKAGE that composes on top of the microkernel's entity identity
// (context::kernel::Entity) — it is deliberately NOT kernel core (R-KERNEL-001 stays minimal): the
// kernel keeps only component storage, the scheduler, the module registry, the event bus, the
// resource-handle registry, and the platform seam, and this index sits on top like any other package.
//
// The tree is kept current INCREMENTALLY as bounds change (insert / update / remove are each
// O(log N)), never rebuilt per frame, and answers radius + AABB queries in O(result + log N) rather
// than O(N). One index is designed to be shared by three future consumers, each wired in a later
// wave: render culling (L-39 — the extract step queries this so its cost scales with the visible set,
// not world size), spatial queries (the R-CLI-006 radius/AABB predicates), and asset streaming
// (R-ASSET-003 proximity-driven load/unload). This task delivers the index + its query API; the
// consumers depend on it later.
//
// Design: a dynamic AABB tree (the Box2D b2DynamicTree / Bullet dbvt broad-phase pattern). Each
// entity is a leaf holding a tight bounds box plus a slightly enlarged ("fat") box; internal nodes
// hold the union of their children's fat boxes. Leaves are placed by a surface-area-heuristic
// branch-and-bound search and the tree is kept near-balanced by rotations on the walk back up, so
// height stays ~log N. Updates whose new tight box still fits inside the leaf's fat box are O(1)
// (no reinsert) — the temporal-coherence property that makes the structure cheap for entities that
// move a little each tick. Query MEMBERSHIP is decided against the exact tight box (no fat-margin
// false positives), while internal pruning uses the conservative fat union boxes.

#pragma once

#include "context/kernel/entity.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace context::packages::spatial
{

// A 3D point / vector. Plain data; component-wise semantics only (no rotation/orientation — a
// broad-phase index works over axis-aligned world bounds).
struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// An axis-aligned bounding box in world space. `min`/`max` are the componentwise lower/upper corners;
// a degenerate box with min == max is a point. Inputs are normalized on insertion (a box whose min
// exceeds its max on some axis is repaired by swapping that axis), so callers may pass either corner
// ordering.
struct Aabb
{
    Vec3 min;
    Vec3 max;

    // A zero-volume box at a single point.
    [[nodiscard]] static Aabb from_point(const Vec3& p) noexcept { return Aabb{p, p}; }

    // A box centered at `center` with the given non-negative half-extents.
    [[nodiscard]] static Aabb from_center_half_extents(const Vec3& center,
                                                       const Vec3& half_extents) noexcept;

    // True if this box and `other` share any volume (touching faces count as overlap).
    [[nodiscard]] bool overlaps(const Aabb& other) const noexcept;

    // True if this box fully contains `other`.
    [[nodiscard]] bool contains(const Aabb& other) const noexcept;

    // True if this box shares any volume with the solid sphere of `radius` around `center`. A negative
    // radius never overlaps.
    [[nodiscard]] bool overlaps_sphere(const Vec3& center, float radius) const noexcept;

    // The geometric center of the box.
    [[nodiscard]] Vec3 center() const noexcept;
};

// The broad-phase spatial index. Move-only: it owns a heavyweight node pool (like the kernel World).
class SpatialIndex
{
public:
    SpatialIndex();
    ~SpatialIndex();

    SpatialIndex(SpatialIndex&&) noexcept;
    SpatialIndex& operator=(SpatialIndex&&) noexcept;
    SpatialIndex(const SpatialIndex&) = delete;
    SpatialIndex& operator=(const SpatialIndex&) = delete;

    // --- incremental maintenance (each O(log N)) ----------------------------------------------

    // Insert entity `e` with the given tight world bounds. If `e` is already present its bounds are
    // updated in place instead. Returns true if `e` was newly inserted, false if it already existed
    // (and was updated). The invalid/null entity is rejected (returns false, no insert).
    bool insert(kernel::Entity e, const Aabb& bounds);

    // Update the bounds of an entity already in the index. Returns false if `e` is absent. When the
    // new tight box still fits inside the entity's current fat node box this is O(1) — no structural
    // change; otherwise the leaf is removed and reinserted (still O(log N)).
    bool update(kernel::Entity e, const Aabb& bounds);

    // Remove `e` from the index. Returns false if `e` was not present.
    bool remove(kernel::Entity e);

    // Remove all entities (keeps allocated capacity for reuse).
    void clear() noexcept;

    // --- queries (each O(result + log N)) -----------------------------------------------------

    // Append every entity whose tight bounds overlap `box` to `out`. Result ORDER is unspecified —
    // treat the output as a set. `out` is appended to (not cleared), so callers can accumulate.
    void query_aabb(const Aabb& box, std::vector<kernel::Entity>& out) const;

    // Append every entity whose tight bounds overlap the solid sphere (`center`, `radius`) to `out`.
    // Result order is unspecified; a negative radius yields no results.
    void query_radius(const Vec3& center, float radius, std::vector<kernel::Entity>& out) const;

    // Convenience overloads returning a fresh vector.
    [[nodiscard]] std::vector<kernel::Entity> query_aabb(const Aabb& box) const;
    [[nodiscard]] std::vector<kernel::Entity> query_radius(const Vec3& center, float radius) const;

    // --- observation --------------------------------------------------------------------------

    [[nodiscard]] bool contains(kernel::Entity e) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    // Height of the tree: -1 when empty, 0 for a single entity, otherwise 1 + the taller child. A
    // near-balanced tree keeps this ~log2(size), which is what makes queries O(result + log N).
    [[nodiscard]] int height() const noexcept;

    // Total nodes currently in the tree (leaves + internal). A full binary tree of L>=1 leaves has
    // exactly 2*L - 1 nodes; 0 when empty.
    [[nodiscard]] std::size_t node_count() const noexcept;

    // Nodes visited by the MOST RECENT query call. This is the query cost metric a sub-linear-scaling
    // test asserts on (a small fixed-region query must visit far fewer than N nodes as N grows).
    [[nodiscard]] std::size_t last_query_visited_nodes() const noexcept;

    // Verify the structure's internal invariants (parent/child links, AABB containment of children,
    // height consistency, leaf/node counts). Returns true when intact. Intended for tests.
    [[nodiscard]] bool validate() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace context::packages::spatial
