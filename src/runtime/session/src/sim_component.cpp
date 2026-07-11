// Sim-component registry + the built-in component set (see sim_component.h).

#include "context/runtime/session/sim_component.h"

#include <cstdio>
#include <cstdlib>
#include <utility>

namespace context::runtime::session
{

std::vector<SimField> SimComponentType::read(const void* data) const
{
    std::vector<SimField> out;
    out.reserve(fields.size());
    for (std::size_t i = 0; i < fields.size(); ++i)
    {
        std::int64_t v = 0;
        std::memcpy(&v, static_cast<const std::byte*>(data) + i * sizeof(std::int64_t),
                    sizeof(std::int64_t));
        out.push_back(SimField{fields[i], v});
    }
    return out;
}

void SimComponentType::hash(const void* data, Fnv1a& h) const
{
    for (std::size_t i = 0; i < fields.size(); ++i)
    {
        std::int64_t v = 0;
        std::memcpy(&v, static_cast<const std::byte*>(data) + i * sizeof(std::int64_t),
                    sizeof(std::int64_t));
        h.update_i64(v);
    }
}

void SimComponentType::write(void* data, const std::vector<std::int64_t>& values) const
{
    const std::size_t n = values.size() < fields.size() ? values.size() : fields.size();
    for (std::size_t i = 0; i < n; ++i)
        std::memcpy(static_cast<std::byte*>(data) + i * sizeof(std::int64_t), &values[i],
                    sizeof(std::int64_t));
}

void SimComponentRegistry::register_checked(SimComponentType t, std::size_t type_size)
{
    if (type_size != t.fields.size() * sizeof(std::int64_t))
    {
        // A sim component whose layout is not exactly field_count int64s breaks the generic memcpy
        // walk — a programmer error caught by the session tests; fail loudly rather than corrupt.
        std::fprintf(stderr,
                     "sim component '%s': size %zu != %zu int64 fields * 8 — sim components must be "
                     "POD of int64 only\n",
                     t.name.c_str(), type_size, t.fields.size());
        std::abort();
    }
    for (SimComponentType& existing : types_)
        if (existing.id == t.id)
        {
            existing = std::move(t);
            return;
        }
    types_.push_back(std::move(t));
}

const SimComponentType* SimComponentRegistry::by_id(kernel::ComponentId id) const
{
    for (const SimComponentType& t : types_)
        if (t.id == id)
            return &t;
    return nullptr;
}

const SimComponentType* SimComponentRegistry::by_name(std::string_view name) const
{
    for (const SimComponentType& t : types_)
        if (t.name == name)
            return &t;
    return nullptr;
}

const SimComponentRegistry& builtin_components()
{
    static const SimComponentRegistry registry = []
    {
        SimComponentRegistry r;
        r.register_component<Position>("position", {"x", "y"});
        r.register_component<Velocity>("velocity", {"x", "y"});
        r.register_component<Health>("health", {"hp"});
        r.register_component<InputState>("input_state",
                                         {"move_x", "move_y", "buttons", "ui", "event_fold"});
        return r;
    }();
    return registry;
}

namespace detail
{
SimComponentRegistry& mutable_sim_components()
{
    // Seeded ONCE from the pristine built-in set, then extended by package registrations. A function-
    // local static: it is constructed on first use — before main() when the first package's
    // SimComponentRegistrar runs — so there is no static-init-order race with builtin_components()
    // (also a function-local static, constructed on demand by this copy). Package registrations that
    // run at static-init all land before the Session first hashes through it.
    static SimComponentRegistry registry = builtin_components();
    return registry;
}
} // namespace detail

} // namespace context::runtime::session
