// Provider capability negotiation / fallback table (M7 T1, R-UI-002/005, lock D1): negotiate_repaint
// turns (capabilities, damage) into a repaint plan — a backend with NO damage support falls back to a
// full repaint; a damage-capable backend repaints only the coalesced dirty regions.

#include "context/packages/ui/provider.h"
#include "context/packages/ui/ui_tree.h"

#include "ui_test.h"

using namespace context::packages::ui;

namespace
{
[[nodiscard]] bool same_rect(const Rect& a, const Rect& b) noexcept
{
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

// A minimal concrete provider proving the contract is implementable and reports capabilities. It counts
// how much it was asked to repaint (regions for an incremental plan; the whole viewport for a full one).
class GpuLikeProvider final : public UiProvider
{
public:
    [[nodiscard]] Capabilities capabilities() const override
    {
        Capabilities caps;
        caps.gpu_driver = true;
        caps.damage_repaint = true;
        caps.composited_transforms = true;
        return caps; // text features stay false — honest per-provider truth
    }

    void present(const UiTree&, const RepaintPlan& plan) override
    {
        ++frames;
        last_full = plan.full_repaint;
        last_regions = plan.regions.size();
    }

    int frames = 0;
    bool last_full = false;
    std::size_t last_regions = 0;
};
} // namespace

int main()
{
    const Rect viewport{0, 0, 1920, 1080};

    // --- Capabilities default to all-false --------------------------------------------------------
    {
        Capabilities caps;
        CHECK(!caps.gpu_driver && !caps.damage_repaint && !caps.composited_transforms);
        CHECK(!caps.text_shaping && !caps.bidi && !caps.ime);
    }

    // --- fallback: NO damage support ⇒ FULL repaint (regardless of the damage list) ---------------
    {
        Capabilities caps; // damage_repaint = false
        DamageList damage;
        damage.add(Rect{10, 10, 20, 20}); // even a small region…
        const RepaintPlan plan = negotiate_repaint(caps, damage, viewport);
        CHECK(plan.full_repaint);
        CHECK(plan.regions.size() == 1);
        CHECK(same_rect(plan.regions[0], viewport)); // …forces a whole-viewport repaint
    }

    // --- damage-capable backend + region damage ⇒ incremental coalesced repaint -------------------
    {
        Capabilities caps;
        caps.damage_repaint = true;
        DamageList damage;
        damage.add(Rect{0, 0, 10, 10});
        damage.add(Rect{5, 5, 10, 10}); // overlaps ⇒ coalesces
        damage.add(Rect{200, 200, 4, 4});
        const RepaintPlan plan = negotiate_repaint(caps, damage, viewport);
        CHECK(!plan.full_repaint);
        CHECK(plan.regions.size() == 2); // {0,0,15,15} + {200,200,4,4}
    }

    // --- damage-capable backend + FULL damage ⇒ full repaint --------------------------------------
    {
        Capabilities caps;
        caps.damage_repaint = true;
        DamageList damage;
        damage.mark_full();
        const RepaintPlan plan = negotiate_repaint(caps, damage, viewport);
        CHECK(plan.full_repaint);
        CHECK(plan.regions.size() == 1);
        CHECK(same_rect(plan.regions[0], viewport));
    }

    // --- damage-capable backend + NO damage ⇒ nothing to repaint ----------------------------------
    {
        Capabilities caps;
        caps.damage_repaint = true;
        const DamageList damage; // empty
        const RepaintPlan plan = negotiate_repaint(caps, damage, viewport);
        CHECK(!plan.full_repaint);
        CHECK(plan.regions.empty());
    }

    // --- a concrete provider consumes the plan through the contract -------------------------------
    {
        UiTree tree;
        GpuLikeProvider provider;
        CHECK(provider.capabilities().gpu_driver);
        CHECK(provider.capabilities().damage_repaint);

        DamageList damage;
        damage.add(Rect{1, 1, 2, 2});
        const RepaintPlan plan = negotiate_repaint(provider.capabilities(), damage, viewport);
        provider.present(tree, plan);
        CHECK(provider.frames == 1);
        CHECK(!provider.last_full);
        CHECK(provider.last_regions == 1);
    }

    UI_TEST_MAIN_END();
}
