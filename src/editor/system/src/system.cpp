// The (query, executor) system model over the kernel World — see system.h. Selects entities by
// declared component, gathers their records into zero-copy VM views, runs the JS executor once through
// the R-LANG-009 runSystemView detach gate, and scatters the mutated records back.

#include "context/editor/system/system.h"

#include <algorithm>
#include <cstring>

namespace context::editor::system
{
namespace
{

namespace cjs = runtime::js;

// Normalize a query id list to a sorted, duplicate-free vector so the archetype subset test (and the
// selection order) are deterministic regardless of the caller's id order.
[[nodiscard]] std::vector<kernel::ComponentId>
normalized_query(const std::vector<kernel::ComponentId>& query)
{
    std::vector<kernel::ComponentId> q = query;
    std::sort(q.begin(), q.end());
    q.erase(std::unique(q.begin(), q.end()), q.end());
    return q;
}

} // namespace

std::vector<kernel::Entity> select_entities(const kernel::World& world,
                                            const std::vector<kernel::ComponentId>& query)
{
    std::vector<kernel::Entity> out;
    const std::vector<kernel::ComponentId> q = normalized_query(query);
    if (q.empty())
    {
        return out;
    }

    world.for_each_archetype(
        [&](const kernel::World::ArchetypeView& view)
        {
            // Both `types()` (the archetype key) and `q` are sorted ascending, so a single linear
            // std::includes decides "archetype carries every queried component".
            const std::vector<kernel::ComponentId>& types = view.types();
            if (std::includes(types.begin(), types.end(), q.begin(), q.end()))
            {
                const std::vector<kernel::Entity>& ents = view.entities();
                out.insert(out.end(), ents.begin(), ents.end());
            }
        });
    return out;
}

void gather_column(const kernel::World& world, const std::vector<kernel::Entity>& entities,
                   kernel::ComponentId id, std::size_t record_size, unsigned char* dst)
{
    if (record_size == 0)
    {
        return;
    }
    for (std::size_t row = 0; row < entities.size(); ++row)
    {
        unsigned char* slot = dst + row * record_size;
        const void* rec = world.get_raw(entities[row], id);
        if (rec != nullptr)
        {
            std::memcpy(slot, rec, record_size);
        }
        else
        {
            std::memset(slot, 0, record_size); // caller error: keep the packed buffer well-defined
        }
    }
}

void scatter_column(kernel::World& world, const std::vector<kernel::Entity>& entities,
                    kernel::ComponentId id, std::size_t record_size, const unsigned char* src)
{
    if (record_size == 0)
    {
        return;
    }
    for (std::size_t row = 0; row < entities.size(); ++row)
    {
        void* rec = world.get_raw(entities[row], id);
        if (rec != nullptr) // skip an entity that no longer has the component (no resurrection)
        {
            std::memcpy(rec, src + row * record_size, record_size);
        }
    }
}

bool run_system(kernel::World& world, cjs::JsEngine& engine, cjs::FunctionHandle executor,
                const std::vector<const component::RegisteredComponentType*>& query, std::string& err)
{
    if (executor == cjs::kInvalidFunction)
    {
        err = "run_system: invalid executor function handle";
        return false;
    }
    if (query.size() > cjs::kMaxSystemViews)
    {
        err = "run_system: query has more components than kMaxSystemViews view bindings";
        return false;
    }

    std::vector<kernel::ComponentId> ids;
    ids.reserve(query.size());
    for (const component::RegisteredComponentType* t : query)
    {
        if (t == nullptr)
        {
            err = "run_system: null component type in query";
            return false;
        }
        ids.push_back(t->component_id);
    }

    const std::vector<kernel::Entity> entities = select_entities(world, ids);
    const std::size_t n = entities.size();

    // Allocate + gather one VM-allocated column per queried component; bind each as a zero-copy u8
    // view spanning the whole packed column (byteOffset 0, count = n * record_size). A u8 view is
    // always element-aligned, so no per-field alignment constraint reaches runSystemView; the derived
    // TS accessors reinterpret the bytes through a DataView.
    std::vector<cjs::VmBuffer> buffers;
    std::vector<cjs::ViewBinding> bindings;
    buffers.reserve(query.size());
    bindings.reserve(query.size());

    bool gathered_ok = true;
    for (const component::RegisteredComponentType* t : query)
    {
        const std::size_t rec = t->schema.size;
        const std::size_t bytes = n * rec;
        cjs::VmBuffer buf;
        // Allocate at least one byte: a zero-entity column still needs a valid backing store to bind a
        // zero-length view over (so the executor observes "no entities" rather than being skipped).
        if (!engine.allocVmBuffer(bytes == 0 ? 1 : bytes, buf, err))
        {
            gathered_ok = false;
            break;
        }
        buffers.push_back(buf);
        if (bytes > 0)
        {
            gather_column(world, entities, t->component_id, rec,
                          static_cast<unsigned char*>(buf.data));
        }
        bindings.push_back(cjs::ViewBinding{buf.handle, cjs::ViewElement::u8, 0, bytes});
    }

    bool run_ok = false;
    if (gathered_ok)
    {
        run_ok = engine.runSystemView(executor, bindings.data(), bindings.size(), err);

        // Scatter back whatever the executor wrote — even when it threw (run_ok == false): the views
        // were live, so partial mutations that landed before the throw are real. On a pre-exec bind
        // error the gathered bytes are unchanged, so scatter is a harmless identity write.
        for (std::size_t k = 0; k < query.size(); ++k)
        {
            const std::size_t rec = query[k]->schema.size;
            if (n * rec > 0)
            {
                scatter_column(world, entities, query[k]->component_id, rec,
                               static_cast<const unsigned char*>(buffers[k].data));
            }
        }
    }

    std::string free_err;
    for (const cjs::VmBuffer& buf : buffers)
    {
        (void)engine.freeVmBuffer(buf.handle, free_err); // best-effort teardown; run result stands
    }

    return gathered_ok && run_ok;
}

} // namespace context::editor::system
