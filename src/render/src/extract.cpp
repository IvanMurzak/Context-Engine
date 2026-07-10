// The sim->render extract step (L-39, R-REND-003) — see extract.h.

#include "context/render/extract.h"

#include "context/kernel/component.h"
#include "context/kernel/world.h"

#include <cmath>
#include <cstddef>

namespace context::render
{
namespace
{

constexpr std::size_t kNone = static_cast<std::size_t>(-1);

// Find the column index of `id` in an archetype's sorted component-id set, or kNone.
std::size_t find_column(const std::vector<kernel::ComponentId>& types, kernel::ComponentId id)
{
    for (std::size_t col = 0; col < types.size(); ++col)
    {
        if (types[col] == id)
        {
            return col;
        }
    }
    return kNone;
}

} // namespace

void extract_render_world(const kernel::World& world, std::uint64_t sim_tick, RenderSnapshot& out)
{
    out.clear();
    out.sim_tick = sim_tick;

    // Component ids are process-global per C++ type (component.h): matching by id here finds exactly
    // the entities the sim World stored via add<Transform>() / add<Renderable>() / add<PbrMaterial>()
    // / add<DirectionalLight>() / add<PointLight>().
    const kernel::ComponentId transform_id = kernel::component_id<Transform>();
    const kernel::ComponentId renderable_id = kernel::component_id<Renderable>();
    const kernel::ComponentId material_id = kernel::component_id<PbrMaterial>();
    const kernel::ComponentId dir_light_id = kernel::component_id<DirectionalLight>();
    const kernel::ComponentId point_light_id = kernel::component_id<PointLight>();

    // Read-only archetype walk: each ArchetypeView exposes the sorted component-id set, the live
    // entities, and raw (col, row) component pointers. The visitor must not mutate the World.
    world.for_each_archetype(
        [&](const kernel::World::ArchetypeView& view)
        {
            const std::vector<kernel::ComponentId>& types = view.types();

            const std::size_t transform_col = find_column(types, transform_id);
            const std::size_t renderable_col = find_column(types, renderable_id);
            const std::size_t material_col = find_column(types, material_id);
            const std::size_t dir_light_col = find_column(types, dir_light_id);
            const std::size_t point_light_col = find_column(types, point_light_id);

            const std::vector<kernel::Entity>& entities = view.entities();

            // Drawables: entities carrying BOTH Transform and Renderable. A PbrMaterial rides along
            // when present; a drawable without one gets the default material (R-REND-004 baseline).
            if (transform_col != kNone && renderable_col != kNone)
            {
                out.items.reserve(out.items.size() + entities.size());
                for (std::size_t row = 0; row < entities.size(); ++row)
                {
                    const auto* transform =
                        static_cast<const Transform*>(view.component(transform_col, row));
                    const auto* renderable =
                        static_cast<const Renderable*>(view.component(renderable_col, row));
                    RenderItem item{entities[row], *transform, *renderable, PbrMaterial{}};
                    if (material_col != kNone)
                    {
                        item.material =
                            *static_cast<const PbrMaterial*>(view.component(material_col, row));
                    }
                    out.items.push_back(item);
                }
            }

            // Directional lights: self-contained (direction lives in the component). A zero-length
            // authored direction is MALFORMED — no meaningful light frame can be derived — so the
            // extract skips it (the render side only ever sees unit directions).
            if (dir_light_col != kNone)
            {
                for (std::size_t row = 0; row < entities.size(); ++row)
                {
                    const auto* light =
                        static_cast<const DirectionalLight*>(view.component(dir_light_col, row));
                    const float dx = light->direction[0];
                    const float dy = light->direction[1];
                    const float dz = light->direction[2];
                    const float len_sq = dx * dx + dy * dy + dz * dz;
                    if (!(len_sq > 1.0e-12f)) // also rejects NaN
                    {
                        continue;
                    }
                    DirectionalLightItem item{entities[row], *light};
                    const float inv_len = 1.0f / std::sqrt(len_sq);
                    item.light.direction[0] = dx * inv_len;
                    item.light.direction[1] = dy * inv_len;
                    item.light.direction[2] = dz * inv_len;
                    out.directional_lights.push_back(item);
                }
            }

            // Point lights: position comes from the entity's Transform, so a PointLight WITHOUT a
            // Transform has no position — skipped (the absent-component edge, mirroring drawables).
            if (point_light_col != kNone && transform_col != kNone)
            {
                for (std::size_t row = 0; row < entities.size(); ++row)
                {
                    const auto* light =
                        static_cast<const PointLight*>(view.component(point_light_col, row));
                    const auto* transform =
                        static_cast<const Transform*>(view.component(transform_col, row));
                    PointLightItem item{entities[row], *light,
                                        {transform->position[0], transform->position[1],
                                         transform->position[2]}};
                    out.point_lights.push_back(item);
                }
            }
        });
}

} // namespace context::render
