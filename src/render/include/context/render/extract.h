// The sim->render extract step (L-39, R-REND-003).
//
// Reads render-relevant components out of the sim World into a RenderSnapshot. This is a READ-ONLY
// observer of the World (R-REND-003 one-way data flow): it takes `const World&`, walks archetypes
// through the World's read-only for_each_archetype seam, and never mutates game state. Headless
// engines simply never call it (R-HEAD-002/003).

#pragma once

#include "context/render/render_world.h"

namespace context::kernel
{
class World;
}

namespace context::render
{

// Extract every entity that carries BOTH a Transform and a Renderable into `out.items` (out is
// cleared first), stamping `out.sim_tick`. Entities missing either component are skipped — the
// drawable set, not the whole World.
//
// This foundation does a full archetype walk. The visible-set bound (L-39: extract queries the
// R-SIM-007 broad-phase spatial index so its cost scales with the visible set, not O(N)) wires in a
// later wave; the walk is structured so that bound is an additive change to which archetypes/entities
// are visited, not a reshaping of the extract.
void extract_render_world(const kernel::World& world, std::uint64_t sim_tick, RenderSnapshot& out);

} // namespace context::render
