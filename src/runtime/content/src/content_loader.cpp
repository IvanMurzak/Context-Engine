// RuntimeContentLoader (see content_loader.h): async load/instantiate/unload of content units by GUID
// with residency accounting, handle invalidation (shared with hot reload), and a feed-independent
// world hash. Deterministic + headless — no threads, no wall-clock; the async surface is a request
// queue a scheduler pumps.

#include "context/runtime/content/content_loader.h"

#include "content_detail.h"

#include "context/editor/serializer/canonical.h"

#include <utility>

namespace context::runtime::content
{

namespace serializer = context::editor::serializer;

void RuntimeContentLoader::request_load(std::uint64_t unit_id)
{
    if (!source_.contains(unit_id))
        return; // an unknown unit never enters the queue — the directory is the source of truth
    queue_.push_back(Op{OpKind::load, unit_id});
}

void RuntimeContentLoader::request_unload(std::uint64_t unit_id)
{
    queue_.push_back(Op{OpKind::unload, unit_id});
}

std::size_t RuntimeContentLoader::pump(std::size_t max_ops)
{
    std::size_t done = 0;
    while (done < max_ops && !queue_.empty())
    {
        const Op op = queue_.front();
        queue_.pop_front();
        if (op.kind == OpKind::load)
            (void)load_now(op.unit_id);
        else
            (void)unload(op.unit_id);
        ++done;
    }
    return done;
}

bool RuntimeContentLoader::load_now(std::uint64_t unit_id, std::string* error)
{
    LoadedUnit unit;
    std::string err;
    if (!source_.load(unit_id, unit, &err))
    {
        last_error_ = err;
        if (error != nullptr)
            *error = err;
        return false;
    }

    const auto it = resident_.find(unit_id);
    if (it != resident_.end())
    {
        // Already resident — refresh its content in place (same logical residency, same generation).
        resident_bytes_ -= it->second.unit.resident_bytes;
        it->second.unit = std::move(unit);
        resident_bytes_ += it->second.unit.resident_bytes;
        return true;
    }

    Resident r;
    r.generation = ++generation_seq_;
    resident_bytes_ += unit.resident_bytes;
    r.unit = std::move(unit);
    resident_.emplace(unit_id, std::move(r));
    return true;
}

bool RuntimeContentLoader::unload(std::uint64_t unit_id)
{
    const auto it = resident_.find(unit_id);
    if (it == resident_.end())
        return false;
    resident_bytes_ -= it->second.unit.resident_bytes;
    resident_.erase(it);
    return true;
}

bool RuntimeContentLoader::invalidate(std::uint64_t unit_id, std::string* error)
{
    const auto it = resident_.find(unit_id);
    if (it == resident_.end())
    {
        last_error_ = "content.not_resident";
        if (error != nullptr)
            *error = last_error_;
        return false;
    }

    LoadedUnit unit;
    std::string err;
    if (!source_.load(unit_id, unit, &err))
    {
        last_error_ = err;
        if (error != nullptr)
            *error = err;
        return false;
    }
    // Re-derivation: swap content AND bump the generation so any prior Handle goes stale (L-24).
    resident_bytes_ -= it->second.unit.resident_bytes;
    resident_bytes_ += unit.resident_bytes;
    it->second.unit = std::move(unit);
    it->second.generation = ++generation_seq_;
    return true;
}

bool RuntimeContentLoader::is_resident(std::uint64_t unit_id) const
{
    return resident_.find(unit_id) != resident_.end();
}

std::size_t RuntimeContentLoader::resident_entity_count() const noexcept
{
    std::size_t total = 0;
    for (const auto& [id, r] : resident_)
        total += r.unit.entities.size();
    return total;
}

std::vector<std::uint64_t> RuntimeContentLoader::resident_unit_ids() const
{
    std::vector<std::uint64_t> ids;
    ids.reserve(resident_.size());
    for (const auto& [id, r] : resident_)
        ids.push_back(id);
    return ids; // std::map iterates ascending — already sorted
}

const LoadedUnit* RuntimeContentLoader::resident_unit(std::uint64_t unit_id) const
{
    const auto it = resident_.find(unit_id);
    return it == resident_.end() ? nullptr : &it->second.unit;
}

Handle RuntimeContentLoader::handle_for(std::uint64_t unit_id) const
{
    const auto it = resident_.find(unit_id);
    if (it == resident_.end())
        return Handle{};
    return Handle{unit_id, it->second.generation, true};
}

const LoadedUnit* RuntimeContentLoader::resolve(const Handle& handle) const
{
    if (!handle.valid)
        return nullptr;
    const auto it = resident_.find(handle.unit_id);
    if (it == resident_.end() || it->second.generation != handle.generation)
        return nullptr; // unloaded, reloaded, or invalidated since the handle was minted
    return &it->second.unit;
}

std::uint64_t RuntimeContentLoader::world_hash() const
{
    std::uint64_t h = detail::kFnvOffset;
    for (const auto& [id, r] : resident_) // ascending unit id (std::map) — deterministic
    {
        detail::mix_u64(h, id);
        const LoadedUnit& unit = r.unit;
        if (unit.is_sidecar)
        {
            detail::mix_u64(h, 1); // a sidecar marker keeps a sidecar id distinct from a unit id
            detail::mix_bytes(h, unit.sidecar_bytes);
            continue;
        }
        detail::mix_u64(h, unit.entities.size());
        for (const UnitEntity& e : unit.entities)
        {
            detail::mix_u64(h, e.identity);
            std::string canon;
            // serialize_canonical normalizes any representational difference between the two feeds
            // (parsed-from-pack vs live-from-editor) to the ONE canonical byte form — so equal content
            // hashes equal regardless of feed. It only fails on a non-finite double (impossible for a
            // well-formed composed value); a failure folds a sentinel rather than silently matching.
            if (serializer::serialize_canonical(e.value, canon))
                detail::mix_bytes(h, canon);
            else
                detail::mix_u64(h, 0xDEADBEEFULL);
        }
    }
    return h;
}

} // namespace context::runtime::content
