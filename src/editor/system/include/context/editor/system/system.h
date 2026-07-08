// The (query, executor) system model over the kernel World (R-LANG-009 / R-LANG-010 item 1,
// R-LANG-002) — the M3 keystone that lets gameplay authored in TypeScript mutate the shared World.
//
// A system is a (query, executor) pair: the query is a set of declared component types, and the
// executor is a resolved JS function that runs ONCE over zero-copy views of those components. Running
// a system:
//   1. selects every live entity whose archetype carries ALL queried components (canonical, ordered);
//   2. gathers each queried component's records for those entities into a contiguous VM-allocated
//      backing store (one VmBuffer per component column, packed row-major);
//   3. binds each column as a zero-copy Uint8Array view and invokes the executor exactly once via
//      JsEngine::runSystemView — so the executor's derived TS accessors (accessor_codegen.h) read/write
//      the gathered bytes directly, and every view is detached/neutered at system exit (the R-LANG-009
//      hard gate is preserved — this tier does NOT weaken it);
//   4. scatters the (possibly mutated) records back into the World's storage.
//
// The gather/scatter copy is the deliberate bridge until the World's hot columns are themselves
// VM-allocated (the R-LANG-008/L-60 "VM-interior World columns" seam, split out on #88) — at which
// point the views alias the columns directly and this copy disappears. Structural changes (add/remove
// component, entity create/destroy) MUST NOT happen inside the executor: memory may move under a live
// view. Deferring them to an end-of-system command buffer is a separate split-out task (#88 item 3);
// this tier runs read/write systems over a stable archetype set.

#pragma once

#include "context/editor/component/component_registry.h"
#include "context/kernel/entity.h"
#include "context/kernel/world.h"
#include "context/runtime/js/js_engine.h"

#include <cstddef>
#include <string>
#include <vector>

namespace context::editor::system
{

// Select every live entity whose archetype contains ALL component ids in `query`, in the World's
// canonical archetype order (component-id-sorted archetype key, then storage-row order) — a
// deterministic, platform-independent ordering (L-54). An empty `query` selects nothing (a system
// with no component query has no per-entity work here; a global tick uses runSystemView with zero
// bindings directly). Duplicate ids in `query` are treated as one.
[[nodiscard]] std::vector<kernel::Entity>
select_entities(const kernel::World& world, const std::vector<kernel::ComponentId>& query);

// Gather the `entities`' records of component `id` (`record_size` bytes each) into `dst`
// (>= entities.size() * record_size bytes), packed row-major in `entities` order. Every entity MUST
// currently have component `id` (select_entities guarantees this for a query that includes `id`); a
// missing record is a caller error and zero-fills that row's bytes. `record_size == 0` is a no-op.
void gather_column(const kernel::World& world, const std::vector<kernel::Entity>& entities,
                   kernel::ComponentId id, std::size_t record_size, unsigned char* dst);

// Scatter `src` (>= entities.size() * record_size bytes, packed row-major) back into the `entities`'
// records of component `id`. The inverse of gather_column; a missing record for an entity skips that
// row (no resurrection of a component the entity lost). `record_size == 0` is a no-op.
void scatter_column(kernel::World& world, const std::vector<kernel::Entity>& entities,
                    kernel::ComponentId id, std::size_t record_size, const unsigned char* src);

// Run the (query, executor) system: select entities carrying every component in `query`, gather each
// queried column into a zero-copy VM view, invoke `executor` ONCE with the views passed positionally
// in `query` order, then scatter the columns back. The executor's derived TS accessors (one per view)
// read/write the gathered records; runSystemView detaches every view at exit. Returns false and fills
// `err` on a bad executor handle, an allocation/bind/detach failure, or an executor throw (the partial
// mutations that landed before the throw are still scattered back — the executor ran over live views).
//
// A `query` with zero matched entities still invokes `executor` (with zero-length views), so a system
// observes "no entities" rather than being skipped. `query` must not exceed JsEngine::kMaxSystemViews
// components (one view per column); exceeding it is reported, not silently clamped.
//
// Precondition: `query` must not list the same component type twice. Unlike select_entities (which
// dedupes ids for the archetype subset test), run_system binds one column/view PER `query` entry
// positionally, so a duplicate would gather two independent columns from the same storage and scatter
// them back in `query` order — last-index-wins, silently discarding an earlier duplicate view's writes.
// Callers pass each declared component at most once (not validated here; a duplicate is a caller error).
[[nodiscard]] bool run_system(kernel::World& world, runtime::js::JsEngine& engine,
                              runtime::js::FunctionHandle executor,
                              const std::vector<const component::RegisteredComponentType*>& query,
                              std::string& err);

} // namespace context::editor::system
