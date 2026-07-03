// Entity: a stable, generation-checked handle into the World (L-60).

#pragma once

#include <cstdint>
#include <functional>

namespace context::kernel
{

// A stable entity id. `index` addresses a slot in the World's entity table; `generation` is bumped
// every time that slot is recycled, so a handle to a destroyed entity never silently aliases a
// freshly-created one. generation == 0 is reserved for the invalid/null entity.
struct Entity
{
    std::uint32_t index = 0;
    std::uint32_t generation = 0;

    // A default-constructed Entity is the invalid one (generation 0).
    [[nodiscard]] constexpr bool valid() const noexcept { return generation != 0; }

    [[nodiscard]] friend constexpr bool operator==(Entity a, Entity b) noexcept
    {
        return a.index == b.index && a.generation == b.generation;
    }
    [[nodiscard]] friend constexpr bool operator!=(Entity a, Entity b) noexcept
    {
        return !(a == b);
    }
};

} // namespace context::kernel

// Entity is hashable so it can be used as a key (e.g. in downstream package indices).
template <>
struct std::hash<context::kernel::Entity>
{
    [[nodiscard]] std::size_t operator()(context::kernel::Entity e) const noexcept
    {
        return (static_cast<std::size_t>(e.generation) << 32) ^ static_cast<std::size_t>(e.index);
    }
};
