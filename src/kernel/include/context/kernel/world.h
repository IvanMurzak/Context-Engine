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
