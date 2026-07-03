// Resource handles: typed, generation-checked handles into a resource pool (R-KERNEL-001).
//
// A Handle<T> is a small value (index + generation) that names a resource in a ResourcePool<T>.
// The pool bumps the slot generation on erase, so a handle to a freed resource is detected as stale
// (get() returns nullptr) rather than silently aliasing whatever later reused the slot — the same
// safety property Entity has, generalized to arbitrary engine resources.

#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace context::kernel
{

template <class T>
struct Handle
{
    std::uint32_t index = 0;
    std::uint32_t generation = 0; // 0 == the null/invalid handle

    [[nodiscard]] constexpr bool valid() const noexcept { return generation != 0; }

    [[nodiscard]] friend constexpr bool operator==(Handle a, Handle b) noexcept
    {
        return a.index == b.index && a.generation == b.generation;
    }
    [[nodiscard]] friend constexpr bool operator!=(Handle a, Handle b) noexcept
    {
        return !(a == b);
    }
};

template <class T>
class ResourcePool
{
public:
    // Store a resource and return a stable handle to it.
    Handle<T> insert(T value)
    {
        std::uint32_t idx;
        if (!free_list_.empty())
        {
            idx = free_list_.back();
            free_list_.pop_back();
            Slot& slot = slots_[idx];
            slot.value = std::move(value);
            slot.occupied = true;
            ++live_;
            return Handle<T>{idx, slot.generation};
        }
        idx = static_cast<std::uint32_t>(slots_.size());
        slots_.push_back(Slot{std::move(value), 1u, true});
        ++live_;
        return Handle<T>{idx, 1u};
    }

    // Fetch the resource, or nullptr if the handle is invalid or stale (its slot was erased).
    [[nodiscard]] T* get(Handle<T> h) noexcept
    {
        Slot* slot = resolve(h);
        return slot != nullptr ? &slot->value : nullptr;
    }

    [[nodiscard]] const T* get(Handle<T> h) const noexcept
    {
        const Slot* slot = resolve(h);
        return slot != nullptr ? &slot->value : nullptr;
    }

    [[nodiscard]] bool contains(Handle<T> h) const noexcept { return resolve(h) != nullptr; }

    // Free the resource. Returns false if the handle was already stale/invalid. Bumps the slot
    // generation so every outstanding copy of the handle becomes stale.
    bool erase(Handle<T> h) noexcept
    {
        Slot* slot = resolve(h);
        if (slot == nullptr)
            return false;
        slot->value = T{};
        slot->occupied = false;
        ++slot->generation;
        if (slot->generation == 0) // never hand out the invalid generation
            slot->generation = 1;
        free_list_.push_back(h.index);
        --live_;
        return true;
    }

    // Number of live (non-erased) resources.
    [[nodiscard]] std::size_t size() const noexcept { return live_; }
    [[nodiscard]] bool empty() const noexcept { return live_ == 0; }

private:
    struct Slot
    {
        T value{};
        std::uint32_t generation = 1;
        bool occupied = false;
    };

    [[nodiscard]] Slot* resolve(Handle<T> h) noexcept
    {
        if (!h.valid() || h.index >= slots_.size())
            return nullptr;
        Slot& slot = slots_[h.index];
        if (!slot.occupied || slot.generation != h.generation)
            return nullptr;
        return &slot;
    }

    [[nodiscard]] const Slot* resolve(Handle<T> h) const noexcept
    {
        if (!h.valid() || h.index >= slots_.size())
            return nullptr;
        const Slot& slot = slots_[h.index];
        if (!slot.occupied || slot.generation != h.generation)
            return nullptr;
        return &slot;
    }

    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_list_;
    std::size_t live_ = 0;
};

} // namespace context::kernel
