// World: the data-oriented, custom archetype/SoA ECS core (L-60, R-SIM-003).
//
// Entities are stable, generation-checked ids (entity.h). Components are stored column-wise (SoA)
// grouped by archetype — the exact set of component types an entity has. Adding or removing a
// component migrates the entity to a different archetype; storage for a component type is one
// contiguous, cache-friendly column, which is what makes zero-copy views possible downstream
// (R-LANG-008). The concrete storage/allocator seam is deliberately owned in-kernel so the five
// locked in-storage protocols (R-LANG-008/009/010/012, L-39) have a home to grow into.

#pragma once

#include "component.h"
#include "entity.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace context::kernel
{

class World
{
public:
    World();
    ~World();

    World(World&&) noexcept;
    World& operator=(World&&) noexcept;
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    // --- entity lifecycle ---------------------------------------------------------------------

    // Create a fresh entity with no components. The returned id is stable: it never collides with a
    // live entity, and a handle to a destroyed entity stays distinguishable via its generation.
    [[nodiscard]] Entity create();

    // Destroy an entity and all its components. A no-op for an already-dead / invalid handle.
    void destroy(Entity e);

    [[nodiscard]] bool is_alive(Entity e) const;

    // Number of currently-live entities.
    [[nodiscard]] std::size_t alive_count() const;

    // --- components ---------------------------------------------------------------------------

    // Add (or overwrite) component T on e and return a reference to the stored value. Adding a new
    // component migrates e to the archetype that includes T.
    template <class T>
    T& add(Entity e, T value)
    {
        register_component_ops(component_id<T>(), ops_for<T>());
        void* slot = add_component(e, component_id<T>(), ops_for<T>(), &value);
        return *static_cast<T*>(slot);
    }

    // Remove component T from e. Returns false if e did not have T (or is dead).
    template <class T>
    bool remove(Entity e)
    {
        return remove_component(e, component_id<T>());
    }

    template <class T>
    [[nodiscard]] T* get(Entity e)
    {
        return static_cast<T*>(locate_component(e, component_id<T>()));
    }

    template <class T>
    [[nodiscard]] const T* get(Entity e) const
    {
        return static_cast<const T*>(locate_component(e, component_id<T>()));
    }

    template <class T>
    [[nodiscard]] bool has(Entity e) const
    {
        return has_component(e, component_id<T>());
    }

    // --- data-driven (runtime-typed) components (R-LANG-010) ----------------------------------
    //
    // The non-template storage API a runtime-registered component type (derived from a declarative
    // schema, no C++ type) uses: it reaches the SAME archetype/SoA storage that add<T>/get<T> above
    // reach, addressed by a caller-supplied ComponentId + ComponentOps rather than a compile-time
    // type. `ops` is typically kernel::pod_ops(size, align) — a trivially-relocatable POD record
    // (component.h). This is the seam the declarative component compiler builds on, so defining a
    // new component type requires no native engine rebuild (L-60).

    // Add (or overwrite) component `id` on `e`, copying ops.size bytes from `src` into storage and
    // returning the stored record (nullptr if `e` is dead). `src == nullptr` adds a zero-initialized
    // record. For POD ops the copy is a memcpy; for ops with a real move_construct, `src` is consumed
    // as if moved-from.
    void* add_raw(Entity e, ComponentId id, const ComponentOps& ops, const void* src);

    // Locate component `id` on `e` (the raw record), or nullptr when absent / `e` is dead.
    [[nodiscard]] void* get_raw(Entity e, ComponentId id);
    [[nodiscard]] const void* get_raw(Entity e, ComponentId id) const;

    // Whether `e` currently has component `id`.
    [[nodiscard]] bool has_raw(Entity e, ComponentId id) const;

    // Remove component `id` from `e`; false if `e` did not have it (or is dead).
    bool remove_raw(Entity e, ComponentId id);

    // --- queries ------------------------------------------------------------------------------

    // Invoke fn(Entity, Cs&...) for every live entity whose archetype contains all of Cs. Iteration
    // walks each matching archetype's contiguous columns, so it is cache-friendly (SoA). Structural
    // mutation of the World from inside fn is not supported (it may invalidate the columns fn holds).
    template <class... Cs, class F>
    void each(F&& fn)
    {
        static_assert(sizeof...(Cs) >= 1, "each<>() requires at least one component type");
        (register_component_ops(component_id<Cs>(), ops_for<Cs>()), ...);
        const ComponentId ids[] = {component_id<Cs>()...};
        each_impl<Cs...>(std::forward<F>(fn), ids, std::index_sequence_for<Cs...>{});
    }

    // --- introspection (generic state hashing / serialization, R-QA-005) ----------------------

    // A read-only view of one archetype's live storage, handed to for_each_archetype's visitor.
    // `types()` is the archetype's sorted component-id set; `entities()[row]` occupies storage
    // `row` in every column; `component(col, row)` is a raw pointer to the component of type
    // `types()[col]` for the entity at `row`. Pointers are valid only for the visitor call.
    class ArchetypeView
    {
    public:
        [[nodiscard]] const std::vector<ComponentId>& types() const noexcept { return *types_; }
        [[nodiscard]] const std::vector<Entity>& entities() const noexcept { return *entities_; }
        // Raw pointer to the (col, row) component; col indexes types(), row indexes entities().
        [[nodiscard]] const void* component(std::size_t col, std::size_t row) const noexcept;

    private:
        friend class World;
        const std::vector<ComponentId>* types_ = nullptr;
        const std::vector<Entity>* entities_ = nullptr;
        const void* archetype_ = nullptr; // opaque Archetype* (defined in world.cpp)
    };

    // Visit every archetype that currently holds at least one entity, in the World's canonical
    // component-id-sorted order (the archetype map key order). The visitor receives a read-only
    // ArchetypeView and MUST NOT structurally mutate the World. This is the generic walk state
    // hashing + serialization use when the concrete component types are not known at compile time
    // (R-QA-005 hierarchical state hash). Component identity is by ComponentId here; a higher layer
    // that needs stable cross-process names maps ids to registered names itself.
    void for_each_archetype(const std::function<void(const ArchetypeView&)>& fn) const;

private:
    template <class... Cs, class F, std::size_t... Is>
    void each_impl(F&& fn, const ComponentId* ids, std::index_sequence<Is...>)
    {
        for_each(ids, sizeof...(Cs),
                 [&](Entity e, void** cols) { fn(e, *static_cast<Cs*>(cols[Is])...); });
    }

    void register_component_ops(ComponentId id, const ComponentOps& ops);
    void* add_component(Entity e, ComponentId id, const ComponentOps& ops, void* src_move);
    bool remove_component(Entity e, ComponentId id);
    [[nodiscard]] void* locate_component(Entity e, ComponentId id) const;
    [[nodiscard]] bool has_component(Entity e, ComponentId id) const;
    void for_each(const ComponentId* ids, std::size_t count,
                  const std::function<void(Entity, void**)>& fn);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace context::kernel
