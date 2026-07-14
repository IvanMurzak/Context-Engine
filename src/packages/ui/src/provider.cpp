// negotiate_repaint — the UI-Provider capability negotiation / fallback table (see provider.h).

#include "context/packages/ui/provider.h"

namespace context::packages::ui
{

RepaintPlan negotiate_repaint(const Capabilities& caps, const DamageList& damage, const Rect& viewport)
{
    RepaintPlan plan;

    // Fallback: a backend WITHOUT damage support repaints everything; a `full` damage request likewise.
    if (!caps.damage_repaint || damage.full)
    {
        plan.full_repaint = true;
        plan.regions.push_back(viewport);
        return plan;
    }

    // Damage-capable backend: incremental repaint over the coalesced dirty regions (possibly none).
    plan.full_repaint = false;
    DamageList coalesced = damage;
    coalesced.coalesce();
    plan.regions = coalesced.regions;
    return plan;
}

} // namespace context::packages::ui
