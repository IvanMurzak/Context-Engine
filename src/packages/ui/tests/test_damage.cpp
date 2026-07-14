// Damage coalescing (M7 T1, R-UI-005 damage_repaint): the DamageList merge semantics and the tree's
// dirty accumulation — region mutations produce coalesced region damage, structural changes produce
// full-surface damage.

#include "context/packages/ui/damage.h"
#include "context/packages/ui/ui_tree.h"

#include "ui_test.h"

using namespace context::packages::ui;

namespace
{
[[nodiscard]] bool same_rect(const Rect& a, const Rect& b) noexcept
{
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

[[nodiscard]] bool has_rect(const DamageList& d, const Rect& r) noexcept
{
    for (const Rect& x : d.regions)
        if (same_rect(x, r))
            return true;
    return false;
}
} // namespace

int main()
{
    // --- DamageList: overlapping regions coalesce into their union --------------------------------
    {
        DamageList d;
        d.add(Rect{0, 0, 10, 10});
        d.add(Rect{5, 5, 10, 10});   // overlaps the first
        d.add(Rect{100, 100, 5, 5}); // disjoint
        d.add(Rect{0, 0, 0, 0});     // empty — ignored on add
        CHECK(d.regions.size() == 3);
        d.coalesce();
        CHECK(d.regions.size() == 2);
        CHECK(has_rect(d, Rect{0, 0, 15, 15}));    // union of the two overlappers
        CHECK(has_rect(d, Rect{100, 100, 5, 5}));  // the disjoint one survives
    }

    // --- a chain of overlaps collapses to a single region ----------------------------------------
    {
        DamageList d;
        d.add(Rect{0, 0, 10, 10});
        d.add(Rect{9, 0, 10, 10});  // overlaps #1
        d.add(Rect{18, 0, 10, 10}); // overlaps #2
        d.coalesce();
        CHECK(d.regions.size() == 1);
        CHECK(same_rect(d.regions[0], Rect{0, 0, 28, 10}));
    }

    // --- edge-touching rects do NOT intersect (half-open) → stay separate -------------------------
    {
        DamageList d;
        d.add(Rect{0, 0, 10, 10});
        d.add(Rect{10, 0, 10, 10}); // shares the x=10 edge only
        d.coalesce();
        CHECK(d.regions.size() == 2);
    }

    // --- empty is truly empty; add-after-full is a no-op -----------------------------------------
    {
        DamageList d;
        CHECK(d.empty());
        d.add(Rect{0, 0, 0, 0});
        CHECK(d.empty()); // an empty rect added nothing

        d.mark_full();
        CHECK(!d.empty());
        CHECK(d.full);
        d.add(Rect{0, 0, 5, 5}); // ignored while full
        CHECK(d.regions.empty());
        d.coalesce(); // clears the (subsumed) regions, stays full
        CHECK(d.full);
        CHECK(d.regions.empty());

        d.clear();
        CHECK(d.empty());
        CHECK(!d.full);
    }

    // --- the tree: structural change ⇒ full damage -----------------------------------------------
    {
        UiTree tree;
        const NodeId a = tree.create_node(Role::Panel, tree.root()); // structural
        DamageList built = tree.take_damage();
        CHECK(built.full);       // building the tree damaged the whole surface
        CHECK(!built.empty());
        CHECK(tree.take_damage().empty()); // take reset the pending damage

        // give nodes bounds, then clear so we isolate region damage
        tree.set_bounds(a, Rect{0, 0, 10, 10});
        const NodeId b = tree.create_node(Role::Panel, tree.root()); // structural again
        (void)tree.take_damage();                                    // clear
        tree.set_bounds(b, Rect{5, 5, 10, 10});
        (void)tree.take_damage(); // clear

        // --- region mutations coalesce; a remove flips it back to full ---------------------------
        tree.set_style(a, Style{}); // dirties a's bounds
        tree.set_style(b, Style{}); // dirties b's bounds (overlaps a)
        DamageList region = tree.take_damage();
        CHECK(!region.full);
        CHECK(region.regions.size() == 1); // a ∪ b
        CHECK(same_rect(region.regions[0], Rect{0, 0, 15, 15}));

        tree.set_text(a, "hi"); // a region mutation on a's bounds
        DamageList text_dmg = tree.take_damage();
        CHECK(!text_dmg.full);
        CHECK(text_dmg.regions.size() == 1);
        CHECK(same_rect(text_dmg.regions[0], Rect{0, 0, 10, 10}));

        CHECK(tree.remove_node(b)); // structural
        DamageList removed = tree.take_damage();
        CHECK(removed.full);
    }

    UI_TEST_MAIN_END();
}
