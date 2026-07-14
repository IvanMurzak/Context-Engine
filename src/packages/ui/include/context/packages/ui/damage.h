// Dirty/damage tracking computed IN the tree (M7 T1, R-UI-005 damage_repaint). A DamageList is the set
// of dirty rectangles a mutation produced, plus a `full` flag for whole-surface repaint (a structural
// change, or a request that cannot be expressed as regions). `coalesce()` merges overlapping regions to
// a minimal set so a damage-capable backend repaints dirty regions only; a backend WITHOUT damage
// support falls back to full repaint (provider.h negotiate_repaint).

#pragma once

#include "context/packages/ui/ui_node.h"

#include <vector>

namespace context::packages::ui
{

// The damage accumulated since the last take. Backend-agnostic: any provider consumes it (the D1 seam).
struct DamageList
{
    std::vector<Rect> regions;
    bool full = false;

    // Add a dirty region. Empty rects are ignored (nothing to repaint). No-op once `full` is set —
    // whole-surface repaint already subsumes any region.
    void add(const Rect& r);

    // Request a whole-surface repaint (structural change / un-region-able damage). Clears regions.
    void mark_full() noexcept;

    // No damage at all (neither a full request nor any region).
    [[nodiscard]] bool empty() const noexcept { return !full && regions.empty(); }

    // Merge intersecting regions into their bounding union until a fixpoint — the minimal covering set a
    // damage-capable backend repaints. No-op when `full` (regions are subsumed and cleared).
    void coalesce();

    // Reset to no-damage.
    void clear() noexcept;
};

} // namespace context::packages::ui
