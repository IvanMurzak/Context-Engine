// The sim->render extract step (L-39, R-REND-003) — see extract.h.

#include "context/render/extract.h"

#include "context/kernel/component.h"
#include "context/kernel/world.h"

#include <cstddef>

namespace context::render
{

void extract_render_world(const kernel::World& world, std::uint64_t sim_tick, RenderSnapshot& out)
{
    out.clear();
    out.sim_tick = sim_tick;

    // Component ids are process-global per C++ type (component.h): matching by id here finds exactly
    // the entities the sim World stored via add<Transform>() / add<Renderable>().
    const kernel::ComponentId transform_id = kernel::component_id<Transform>();
    const kernel::ComponentId renderable_id = kernel::component_id<Renderable>();

    // Read-only archetype walk: each ArchetypeView exposes the sorted component-id set, the live
    // entities, and raw (col, row) component pointers. The visitor must not mutate the World.
    world.for_each_archetype(
        [&](const kernel::World::ArchetypeView& view)
        {
            const std::vector<kernel::ComponentId>& types = view.types();

            constexpr std::size_t kNone = static_cast<std::size_t>(-1);
            std::size_t transform_col = kNone;
            std::size_t renderable_col = kNone;
            for (std::size_t col = 0; col < types.size(); ++col)
            {
                if (types[col] == transform_id)
                {
                    transform_col = col;
                }
                else if (types[col] == renderable_id)
                {
                    renderable_col = col;
                }
            }

            // Not a drawable archetype unless it carries BOTH components.
            if (transform_col == kNone || renderable_col == kNone)
            {
                return;
            }

            const std::vector<kernel::Entity>& entities = view.entities();
            out.items.reserve(out.items.size() + entities.size());
            for (std::size_t row = 0; row < entities.size(); ++row)
            {
                const auto* transform =
                    static_cast<const Transform*>(view.component(transform_col, row));
                const auto* renderable =
                    static_cast<const Renderable*>(view.component(renderable_col, row));
                out.items.push_back(RenderItem{entities[row], *transform, *renderable});
            }
        });
}

} // namespace context::render
