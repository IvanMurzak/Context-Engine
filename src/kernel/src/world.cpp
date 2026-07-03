// World implementation: custom archetype/SoA storage (L-60).

#include "context/kernel/world.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <new>
#include <unordered_map>
#include <vector>

namespace context::kernel
{

namespace detail
{
ComponentId next_component_id() noexcept
{
    static ComponentId counter = 0;
    return counter++;
}
} // namespace detail

namespace
{

// One SoA column: a contiguous, over-alignment-correct byte buffer of `count` component objects.
// Move-only; owns its buffer. Element lifetime is managed through the type-erased ComponentOps.
class Column
{
public:
    Column() = default;
    explicit Column(const ComponentOps* ops) noexcept : ops_(ops) {}

    Column(const Column&) = delete;
    Column& operator=(const Column&) = delete;

    Column(Column&& o) noexcept
        : ops_(o.ops_), data_(o.data_), count_(o.count_), capacity_(o.capacity_)
    {
        o.data_ = nullptr;
        o.count_ = 0;
        o.capacity_ = 0;
    }

    Column& operator=(Column&& o) noexcept
    {
        if (this != &o)
        {
            release();
            ops_ = o.ops_;
            data_ = o.data_;
            count_ = o.count_;
            capacity_ = o.capacity_;
            o.data_ = nullptr;
            o.count_ = 0;
            o.capacity_ = 0;
        }
        return *this;
    }

    ~Column() { release(); }

    [[nodiscard]] void* elem(std::size_t i) const noexcept { return data_ + i * ops_->size; }

    // Move-construct a component at index `i` (raw, uninitialized storage) from `src`, growing the
    // buffer if needed. `i` is always the current count (append), so count advances to i + 1.
    void append_move(std::size_t i, void* src)
    {
        ensure_capacity(i + 1);
        ops_->move_construct(elem(i), src);
        count_ = i + 1;
    }

    // Swap-remove element `row`: destroy it, move the last element into its place, shrink by one.
    void swap_remove(std::size_t row) noexcept
    {
        const std::size_t last = count_ - 1;
        ops_->destroy(elem(row));
        if (row != last)
        {
            ops_->move_construct(elem(row), elem(last));
            ops_->destroy(elem(last));
        }
        count_ = last;
    }

    void destroy_at(std::size_t i) noexcept { ops_->destroy(elem(i)); }

private:
    void ensure_capacity(std::size_t n)
    {
        if (n <= capacity_)
            return;
        std::size_t new_cap = capacity_ != 0 ? capacity_ * 2 : 4;
        if (new_cap < n)
            new_cap = n;
        auto* nd = static_cast<std::byte*>(
            ::operator new(new_cap * ops_->size, std::align_val_t{ops_->align}));
        for (std::size_t i = 0; i < count_; ++i)
        {
            ops_->move_construct(nd + i * ops_->size, elem(i));
            ops_->destroy(elem(i));
        }
        if (data_ != nullptr)
            ::operator delete(data_, std::align_val_t{ops_->align});
        data_ = nd;
        capacity_ = new_cap;
    }

    void release() noexcept
    {
        if (data_ != nullptr)
        {
            for (std::size_t i = 0; i < count_; ++i)
                ops_->destroy(elem(i));
            ::operator delete(data_, std::align_val_t{ops_->align});
            data_ = nullptr;
        }
        count_ = 0;
        capacity_ = 0;
    }

    const ComponentOps* ops_ = nullptr;
    std::byte* data_ = nullptr;
    std::size_t count_ = 0;
    std::size_t capacity_ = 0;
};

// An archetype groups every entity that has exactly the component set `types` (sorted). `columns`
// is parallel to `types`; `entities[row]` is the entity occupying storage row `row` in every column.
struct Archetype
{
    std::vector<ComponentId> types;
    std::vector<Column> columns;
    std::vector<Entity> entities;

    [[nodiscard]] bool has(ComponentId id) const noexcept
    {
        return std::binary_search(types.begin(), types.end(), id);
    }

    // Index of `id` in `types` (== its column index). Precondition: has(id).
    [[nodiscard]] std::size_t column_index(ComponentId id) const noexcept
    {
        return static_cast<std::size_t>(
            std::lower_bound(types.begin(), types.end(), id) - types.begin());
    }
};

struct EntityRecord
{
    Archetype* archetype = nullptr;
    std::uint32_t row = 0;
    std::uint32_t generation = 0;
    bool alive = false;
};

} // namespace

struct World::Impl
{
    std::vector<EntityRecord> records;
    std::vector<std::uint32_t> free_list;
    // Destruction-order invariant: `ops` MUST be declared before `archetypes`.
    // Every Column stores a `const ComponentOps*` into this map (get_or_create_archetype),
    // and ~Archetype's column teardown dereferences those pointers to destroy components.
    // Members die in reverse declaration order, so `archetypes` (and its Columns) must be
    // destroyed BEFORE `ops`; swapping these two reintroduces the ~Impl heap-use-after-free.
    std::unordered_map<ComponentId, ComponentOps> ops;
    std::map<std::vector<ComponentId>, std::unique_ptr<Archetype>> archetypes;
    std::size_t alive = 0;

    Archetype* get_or_create_archetype(const std::vector<ComponentId>& types)
    {
        auto it = archetypes.find(types);
        if (it != archetypes.end())
            return it->second.get();
        auto arch = std::make_unique<Archetype>();
        arch->types = types;
        arch->columns.reserve(types.size());
        for (ComponentId cid : types)
            arch->columns.emplace_back(&ops.at(cid));
        Archetype* raw = arch.get();
        archetypes.emplace(types, std::move(arch));
        return raw;
    }

    // Append entity to an archetype's entity list (columns filled by the caller). Returns the row.
    std::uint32_t add_entity_row(Archetype* arch, Entity e)
    {
        const auto row = static_cast<std::uint32_t>(arch->entities.size());
        arch->entities.push_back(e);
        return row;
    }

    // Remove storage row `row` from `arch`, keeping columns + the entity list dense.
    void swap_remove_row(Archetype* arch, std::uint32_t row)
    {
        const auto last = static_cast<std::uint32_t>(arch->entities.size() - 1);
        for (Column& col : arch->columns)
            col.swap_remove(row);
        if (row != last)
        {
            arch->entities[row] = arch->entities[last];
            records[arch->entities[row].index].row = row;
        }
        arch->entities.pop_back();
    }
};

World::World() : impl_(std::make_unique<Impl>()) {}
World::~World() = default;
World::World(World&&) noexcept = default;
World& World::operator=(World&&) noexcept = default;

Entity World::create()
{
    std::uint32_t idx;
    if (!impl_->free_list.empty())
    {
        idx = impl_->free_list.back();
        impl_->free_list.pop_back();
    }
    else
    {
        idx = static_cast<std::uint32_t>(impl_->records.size());
        impl_->records.emplace_back();
    }

    EntityRecord& rec = impl_->records[idx];
    rec.generation += 1;
    if (rec.generation == 0) // wrapped past the invalid sentinel — skip it
        rec.generation = 1;
    rec.alive = true;

    const Entity e{idx, rec.generation};
    Archetype* empty = impl_->get_or_create_archetype({});
    rec.archetype = empty;
    rec.row = impl_->add_entity_row(empty, e);
    ++impl_->alive;
    return e;
}

void World::destroy(Entity e)
{
    if (!is_alive(e))
        return;
    EntityRecord& rec = impl_->records[e.index];
    impl_->swap_remove_row(rec.archetype, rec.row);
    rec.alive = false;
    rec.archetype = nullptr;
    impl_->free_list.push_back(e.index);
    --impl_->alive;
}

bool World::is_alive(Entity e) const
{
    if (!e.valid() || e.index >= impl_->records.size())
        return false;
    const EntityRecord& rec = impl_->records[e.index];
    return rec.alive && rec.generation == e.generation;
}

std::size_t World::alive_count() const { return impl_->alive; }

void World::register_component_ops(ComponentId id, const ComponentOps& ops)
{
    impl_->ops.try_emplace(id, ops);
}

void* World::add_component(Entity e, ComponentId id, const ComponentOps& ops, void* src_move)
{
    if (!is_alive(e))
        return nullptr;
    impl_->ops.try_emplace(id, ops);
    EntityRecord& rec = impl_->records[e.index];
    Archetype* cur = rec.archetype;

    // Already present → overwrite in place.
    if (cur->has(id))
    {
        Column& col = cur->columns[cur->column_index(id)];
        void* dst = col.elem(rec.row);
        ops.destroy(dst);
        ops.move_construct(dst, src_move);
        return dst;
    }

    std::vector<ComponentId> target_types = cur->types;
    target_types.insert(std::lower_bound(target_types.begin(), target_types.end(), id), id);
    Archetype* tgt = impl_->get_or_create_archetype(target_types);

    const std::uint32_t new_row = impl_->add_entity_row(tgt, e);
    void* result = nullptr;
    for (std::size_t k = 0; k < tgt->types.size(); ++k)
    {
        Column& tcol = tgt->columns[k];
        const ComponentId cid = tgt->types[k];
        if (cid == id)
        {
            tcol.append_move(new_row, src_move);
            result = tcol.elem(new_row);
        }
        else
        {
            Column& scol = cur->columns[cur->column_index(cid)];
            tcol.append_move(new_row, scol.elem(rec.row));
        }
    }
    impl_->swap_remove_row(cur, rec.row);
    rec.archetype = tgt;
    rec.row = new_row;
    return result;
}

bool World::remove_component(Entity e, ComponentId id)
{
    if (!is_alive(e))
        return false;
    EntityRecord& rec = impl_->records[e.index];
    Archetype* cur = rec.archetype;
    if (!cur->has(id))
        return false;

    std::vector<ComponentId> target_types = cur->types;
    auto it = std::lower_bound(target_types.begin(), target_types.end(), id);
    target_types.erase(it);
    Archetype* tgt = impl_->get_or_create_archetype(target_types);

    const std::uint32_t new_row = impl_->add_entity_row(tgt, e);
    for (std::size_t k = 0; k < tgt->types.size(); ++k)
    {
        const ComponentId cid = tgt->types[k];
        Column& scol = cur->columns[cur->column_index(cid)];
        tgt->columns[k].append_move(new_row, scol.elem(rec.row));
    }
    // The dropped component's element is destroyed by the swap-remove below.
    impl_->swap_remove_row(cur, rec.row);
    rec.archetype = tgt;
    rec.row = new_row;
    return true;
}

void* World::locate_component(Entity e, ComponentId id) const
{
    if (!is_alive(e))
        return nullptr;
    const EntityRecord& rec = impl_->records[e.index];
    Archetype* cur = rec.archetype;
    if (!cur->has(id))
        return nullptr;
    return cur->columns[cur->column_index(id)].elem(rec.row);
}

bool World::has_component(Entity e, ComponentId id) const
{
    if (!is_alive(e))
        return false;
    return impl_->records[e.index].archetype->has(id);
}

void World::for_each(const ComponentId* ids, std::size_t count,
                     const std::function<void(Entity, void**)>& fn)
{
    std::vector<Column*> cols(count, nullptr);
    std::vector<void*> ptrs(count, nullptr);
    for (auto& kv : impl_->archetypes)
    {
        Archetype* arch = kv.second.get();
        bool matches = true;
        for (std::size_t k = 0; k < count; ++k)
        {
            if (!arch->has(ids[k]))
            {
                matches = false;
                break;
            }
        }
        if (!matches)
            continue;
        for (std::size_t k = 0; k < count; ++k)
            cols[k] = &arch->columns[arch->column_index(ids[k])];
        const std::size_t n = arch->entities.size();
        for (std::size_t row = 0; row < n; ++row)
        {
            for (std::size_t k = 0; k < count; ++k)
                ptrs[k] = cols[k]->elem(row);
            fn(arch->entities[row], ptrs.data());
        }
    }
}

} // namespace context::kernel
