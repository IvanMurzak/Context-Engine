// UI quad draw-order + batching + damage draw-set selection — see context/render/ui/batch.h.

#include "context/render/ui/batch.h"

#include <algorithm>

namespace context::render::ui
{

std::vector<std::uint32_t> sort_ui_draw_order(const UiRenderSnapshot& snapshot)
{
    std::vector<std::uint32_t> order(snapshot.quads.size());
    for (std::uint32_t i = 0; i < order.size(); ++i)
    {
        order[i] = i;
    }
    // Stable sort by paint order: preserves the extract's pre-order tie-break for equal keys.
    std::stable_sort(order.begin(), order.end(), [&snapshot](std::uint32_t a, std::uint32_t b)
                     { return snapshot.quads[a].order < snapshot.quads[b].order; });
    return order;
}

std::vector<UiBatch> build_ui_batches(const UiRenderSnapshot& snapshot)
{
    std::vector<UiBatch> batches;
    const std::vector<std::uint32_t> order = sort_ui_draw_order(snapshot);
    bool have_current = false;
    bool current_opaque = true;
    for (const std::uint32_t idx : order)
    {
        const bool opaque = quad_is_opaque(snapshot.quads[idx]);
        if (!have_current || opaque != current_opaque)
        {
            UiBatch batch;
            batch.opaque = opaque;
            batches.push_back(std::move(batch));
            have_current = true;
            current_opaque = opaque;
        }
        batches.back().quad_indices.push_back(idx);
    }
    return batches;
}

std::vector<std::uint32_t> select_draw_set(const UiRenderSnapshot& snapshot,
                                           const packages::ui::RepaintPlan& plan)
{
    const std::vector<std::uint32_t> order = sort_ui_draw_order(snapshot);
    if (plan.full_repaint)
    {
        return order; // repaint everything, in draw order
    }
    std::vector<std::uint32_t> selected;
    for (const std::uint32_t idx : order)
    {
        const packages::ui::Rect& rect = snapshot.quads[idx].rect;
        const bool touched = std::any_of(plan.regions.begin(), plan.regions.end(),
                                         [&rect](const packages::ui::Rect& r)
                                         { return rect.intersects(r); });
        if (touched)
        {
            selected.push_back(idx);
        }
    }
    return selected;
}

} // namespace context::render::ui
