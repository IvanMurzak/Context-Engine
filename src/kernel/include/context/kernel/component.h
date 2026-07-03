// Component identity + type-erased storage operations for the archetype/SoA World (L-60).

#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>

namespace context::kernel
{

// A runtime component id. Ids are assigned lazily, once per distinct C++ type, in first-touch order.
// Runtime ids (rather than compile-time indices) are what let the custom World register component
// layouts at runtime — the property that ruled EnTT out and ruled the custom core in (L-60).
using ComponentId = std::uint32_t;

// Type-erased lifetime operations for one component type. The archetype columns are raw byte
// buffers; these hooks move-construct and destroy elements so migration and column growth never
// need to know the concrete type.
struct ComponentOps
{
    std::size_t size = 0;
    std::size_t align = 0;
    // Move-construct a new object at `dst` from the object at `src`, leaving `src` in a
    // destructible moved-from state.
    void (*move_construct)(void* dst, void* src) noexcept = nullptr;
    // Run the destructor of the object at `p`.
    void (*destroy)(void* p) noexcept = nullptr;
};

namespace detail
{
// Monotonic id source, defined in world.cpp so every translation unit shares one counter.
ComponentId next_component_id() noexcept;
} // namespace detail

// Stable per-type component id. The static local guarantees one id per type for the process.
template <class T>
[[nodiscard]] ComponentId component_id() noexcept
{
    static const ComponentId id = detail::next_component_id();
    return id;
}

// The ComponentOps for T, materialized once as a static and referenced by pointer everywhere.
template <class T>
[[nodiscard]] const ComponentOps& ops_for() noexcept
{
    static const ComponentOps ops{
        sizeof(T),
        alignof(T),
        [](void* dst, void* src) noexcept { ::new (dst) T(std::move(*static_cast<T*>(src))); },
        [](void* p) noexcept { static_cast<T*>(p)->~T(); },
    };
    return ops;
}

} // namespace context::kernel
