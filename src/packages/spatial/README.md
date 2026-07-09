# src/packages/spatial/ — broad-phase spatial index (R-SIM-007)

The first-party **broad-phase spatial acceleration structure** — an incrementally-updated **dynamic
AABB tree** (a dynamic bounding-volume hierarchy, the Box2D `b2DynamicTree` / Bullet `dbvt` broad-phase
pattern) over transform-bearing entities. It is a **package**, not kernel core: `R-KERNEL-001` stays
minimal and this composes on top of the microkernel's entity identity like any other feature package
(the kernel gains no dependency on it).

## Why it exists

Three subsystems each independently need "find things near here, fast": render culling (**L-39** — so
the extract step scales with the visible set, not world size), spatial queries (the **R-CLI-006**
radius/AABB predicates), and asset streaming (**R-ASSET-003** proximity-driven load/unload). A single
shared broad-phase index gives all three **O(result + log N)** queries instead of an O(N) scan.

**This task (issue #115) delivers the index + its query API.** The three consumers wire in later
waves; nothing depends on them here.

## What it does

- **Incremental maintenance** — `insert` / `update` / `remove` are each **O(log N)** and keep the tree
  current as bounds change; the tree is **never rebuilt per frame**. An `update` whose new tight box
  still fits inside the leaf's slightly-enlarged "fat" box is **O(1)** (no reinsert) — the
  temporal-coherence fast path for entities that move a little each tick.
- **Queries** — `query_aabb(box)` and `query_radius(center, radius)` return every entity whose exact
  (tight) bounds overlap, in **O(result + log N)**. Result order is unspecified (treat it as a set).
  Membership is decided against the tight box (no fat-margin false positives); internal pruning uses
  the conservative fat union boxes.
- **Balance** — leaves are placed by a surface-area-heuristic branch-and-bound search and the tree is
  rebalanced by rotations on the way back up, so height stays ~`log2(N)`.

## Usage

```cpp
#include "context/packages/spatial/spatial_index.h"
using namespace context::packages::spatial;

SpatialIndex index;
index.insert(entity, Aabb::from_center_half_extents({x, y, z}, {hx, hy, hz}));
index.update(entity, newBounds);                 // O(1) for small moves, else O(log N)
auto hits = index.query_radius({px, py, pz}, r);  // O(result + log N)
```

`Entity` is `context::kernel::Entity`, so query results address directly into the kernel World.

## Design records

`R-SIM-007` (this structure), `R-KERNEL-001` (why it's a package, not kernel), `L-39` /
`R-CLI-006` / `R-ASSET-003` (its three consumers). See `engine-design/REQUIREMENTS.md` and
`DESIGN-DECISIONS.md`.
