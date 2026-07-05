// Simulation components + the type-erased sim-component registry (R-QA-005 / R-SIM-003, L-60).
//
// The headless session's world state is ECS components in the kernel World. For the state hash and
// the session serializer to walk that state WITHOUT compile-time knowledge of each component type,
// every simulation component is registered here with a STABLE NAME (unlike the runtime ComponentId,
// which is assigned in first-touch order and therefore not stable across processes) and a fixed,
// ORDERED list of named integer fields.
//
// Integer-only, by law: every sim component is a POD of std::int64_t fields ONLY — no float, no
// padding. Float state would hash differently across the x86-64 / arm64 determinism matrix (and any
// float arithmetic in a system would diverge across platforms). The simulation therefore works in a
// fixed-point / integer domain; the state hash folds each field as a fixed-width big-endian integer
// (hash.h), so a world's digest is bit-identical on every platform in the matrix.

#pragma once

#include "context/kernel/component.h"
#include "context/kernel/entity.h"
#include "context/kernel/world.h"
#include "context/runtime/session/hash.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace context::runtime::session
{

// --- the engine's built-in simulation components (each a POD of int64 fields only) -----------------

// A 2D position, integer world units (fixed-point domain — no float, R-QA-005).
struct Position
{
    std::int64_t x = 0;
    std::int64_t y = 0;
};

// A 2D velocity applied to Position each tick (integer units per tick).
struct Velocity
{
    std::int64_t x = 0;
    std::int64_t y = 0;
};

// A player/agent health value — marks the input-controlled "actor" (Position+Health), distinct from
// spawned movers (Position+Velocity), so a system can address each archetype independently.
struct Health
{
    std::int64_t hp = 0;
};

// The world-singleton input state the input system writes each tick from injected events + actions.
// `move_x/move_y/buttons` are the mapped gameplay-action channel; `ui` is the mapped UI-action
// channel; `event_fold` is a running fold of raw synthetic input EVENTS (device/code/value) so
// injected events also move the state hash. One entity carries this component (the singleton).
struct InputState
{
    std::int64_t move_x = 0;
    std::int64_t move_y = 0;
    std::int64_t buttons = 0;
    std::int64_t ui = 0;
    std::int64_t event_fold = 0;
};

// One (field-name, value) pair read out of a component instance.
struct SimField
{
    std::string name;
    std::int64_t value = 0;
};

// A registered simulation component type: a stable name, the runtime ComponentId, and the ordered
// field-name list. The component's in-memory representation is exactly `fields.size()` contiguous
// int64 values (asserted at registration), so read / write / hash are layout-generic memcpy walks.
struct SimComponentType
{
    std::string name;
    kernel::ComponentId id = 0;
    std::vector<std::string> fields;

    [[nodiscard]] std::size_t field_count() const noexcept { return fields.size(); }

    // Read the component at `data` (int64[field_count]) into named fields, in declared order.
    [[nodiscard]] std::vector<SimField> read(const void* data) const;
    // Fold the component's fields into `h` in declared order (fixed-width big-endian — hash.h).
    void hash(const void* data, Fnv1a& h) const;
    // Overwrite the component at `data` with `values` (indexed parallel to `fields`; extra values
    // ignored, missing values left as previously stored).
    void write(void* data, const std::vector<std::int64_t>& values) const;

    // Add a default (all-zero) instance of this component onto `e` in `w`; returns the component
    // storage to write into. Set at registration from the concrete type.
    void* (*add_fn)(kernel::World& w, kernel::Entity e) = nullptr;
    // Locate the component on `e` (const), or nullptr.
    const void* (*locate_fn)(const kernel::World& w, kernel::Entity e) = nullptr;
};

// The sim-component registry: id/name lookup over a registered set. The engine's built-in set is a
// process-wide singleton (builtin_components()); package-contributed components would register
// through the same add() when the declarative component compiler lands (R-LANG-010).
class SimComponentRegistry
{
public:
    template <class T>
    void register_component(std::string name, std::vector<std::string> fields)
    {
        SimComponentType t;
        t.name = std::move(name);
        t.id = kernel::component_id<T>();
        t.fields = std::move(fields);
        t.add_fn = [](kernel::World& w, kernel::Entity e) -> void*
        { return static_cast<void*>(&w.add<T>(e, T{})); };
        t.locate_fn = [](const kernel::World& w, kernel::Entity e) -> const void*
        { return w.template get<T>(e); };
        // A sim component is exactly field_count int64 values — the layout the memcpy walks assume.
        register_checked(std::move(t), sizeof(T));
    }

    [[nodiscard]] const SimComponentType* by_id(kernel::ComponentId id) const;
    [[nodiscard]] const SimComponentType* by_name(std::string_view name) const;
    [[nodiscard]] const std::vector<SimComponentType>& all() const noexcept { return types_; }

private:
    // Push `t`, aborting (dev guard) if `type_size` is not field_count * sizeof(int64).
    void register_checked(SimComponentType t, std::size_t type_size);
    std::vector<SimComponentType> types_;
};

// The engine's built-in simulation components (position, velocity, health, input_state), each at a
// stable name. Built once; process-wide.
[[nodiscard]] const SimComponentRegistry& builtin_components();

} // namespace context::runtime::session
