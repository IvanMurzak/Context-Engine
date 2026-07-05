// Hierarchical state hash + trace serialization (see state_hash.h).

#include "context/runtime/session/state_hash.h"

#include "context/runtime/session/hash.h"
#include "context/runtime/session/json_build.h"

#include <algorithm>
#include <cstddef>
#include <numeric>

namespace context::runtime::session
{

namespace
{
// One archetype column, resolved to its stable name + hasher (or an unregistered marker).
struct ResolvedColumn
{
    std::size_t column = 0;         // index into ArchetypeView::types()/component()
    std::string name;               // stable component name ("?<id>" if unregistered)
    const SimComponentType* type = nullptr;
};

ArchetypeHash hash_archetype(const kernel::World::ArchetypeView& view,
                             const SimComponentRegistry& registry)
{
    std::vector<ResolvedColumn> cols;
    cols.reserve(view.types().size());
    for (std::size_t c = 0; c < view.types().size(); ++c)
    {
        const SimComponentType* type = registry.by_id(view.types()[c]);
        // An unregistered component still gets a STABLE marker keyed by its id, so it neither
        // collides with a sibling unregistered column nor destabilizes the signature.
        std::string name = type != nullptr ? type->name : "?" + std::to_string(view.types()[c]);
        cols.push_back(ResolvedColumn{c, std::move(name), type});
    }
    // Canonical column order: by NAME (independent of the ComponentId assignment order).
    std::sort(cols.begin(), cols.end(),
              [](const ResolvedColumn& a, const ResolvedColumn& b) { return a.name < b.name; });

    std::string signature;
    for (const ResolvedColumn& col : cols)
    {
        if (!signature.empty())
            signature += '+';
        signature += col.name;
    }

    // Canonical entity order: by (index, generation).
    std::vector<std::size_t> rows(view.entities().size());
    std::iota(rows.begin(), rows.end(), std::size_t{0});
    std::sort(rows.begin(), rows.end(),
              [&](std::size_t a, std::size_t b)
              {
                  const kernel::Entity& ea = view.entities()[a];
                  const kernel::Entity& eb = view.entities()[b];
                  if (ea.index != eb.index)
                      return ea.index < eb.index;
                  return ea.generation < eb.generation;
              });

    Fnv1a h;
    h.update_bytes(signature);
    for (std::size_t r : rows)
    {
        const kernel::Entity& e = view.entities()[r];
        h.update_u64(e.index);
        h.update_u64(e.generation);
        for (const ResolvedColumn& col : cols)
        {
            if (col.type != nullptr)
                col.type->hash(view.component(col.column, r), h);
            else
                h.update_u64(0xDEADBEEFULL); // fixed marker for unregistered component bytes
        }
    }

    ArchetypeHash out;
    out.signature = std::move(signature);
    out.hash = h.digest();
    out.entity_count = view.entities().size();
    return out;
}
} // namespace

StateHash hash_world(const kernel::World& world, const SimComponentRegistry& registry)
{
    StateHash result;
    world.for_each_archetype([&](const kernel::World::ArchetypeView& view)
                             { result.archetypes.push_back(hash_archetype(view, registry)); });

    // Canonical archetype order (signature-sorted), so the root is independent of the World's
    // internal id-keyed archetype iteration order.
    std::sort(result.archetypes.begin(), result.archetypes.end(),
              [](const ArchetypeHash& a, const ArchetypeHash& b) { return a.signature < b.signature; });

    Fnv1a root;
    for (const ArchetypeHash& a : result.archetypes)
    {
        root.update_bytes(a.signature);
        root.update_u64(a.hash);
        root.update_u64(a.entity_count);
    }
    result.root = root.digest();
    return result;
}

namespace
{
serializer::JsonValue archetypes_to_json(const std::vector<ArchetypeHash>& archetypes)
{
    serializer::JsonValue arr = jb::array();
    for (const ArchetypeHash& a : archetypes)
    {
        serializer::JsonValue entry = jb::object();
        jb::set(entry, "signature", jb::str(a.signature));
        jb::set(entry, "hash", jb::uinteger(a.hash));
        jb::set(entry, "entityCount", jb::uinteger(a.entity_count));
        jb::push(arr, std::move(entry));
    }
    return arr;
}
} // namespace

serializer::JsonValue state_hash_to_json(const StateHash& hash)
{
    serializer::JsonValue out = jb::object();
    jb::set(out, "root", jb::uinteger(hash.root));
    jb::set(out, "archetypes", archetypes_to_json(hash.archetypes));
    return out;
}

serializer::JsonValue hash_tree_to_json(const HashTree& tree)
{
    serializer::JsonValue out = jb::object();
    jb::set(out, "tick", jb::uinteger(tree.tick));
    jb::set(out, "root", jb::uinteger(tree.root));

    serializer::JsonValue systems = jb::array();
    for (const SystemHash& s : tree.per_system)
    {
        serializer::JsonValue entry = jb::object();
        jb::set(entry, "system", jb::str(s.system));
        jb::set(entry, "hash", jb::uinteger(s.hash));
        jb::push(systems, std::move(entry));
    }
    jb::set(out, "perSystem", std::move(systems));
    jb::set(out, "perArchetype", archetypes_to_json(tree.per_archetype));
    return out;
}

serializer::JsonValue hash_trace_to_json(const HashTrace& trace)
{
    serializer::JsonValue arr = jb::array();
    for (const HashTree& tree : trace)
        jb::push(arr, hash_tree_to_json(tree));
    return arr;
}

std::vector<std::uint64_t> trace_roots(const HashTrace& trace)
{
    std::vector<std::uint64_t> roots;
    roots.reserve(trace.size());
    for (const HashTree& tree : trace)
        roots.push_back(tree.root);
    return roots;
}

} // namespace context::runtime::session
