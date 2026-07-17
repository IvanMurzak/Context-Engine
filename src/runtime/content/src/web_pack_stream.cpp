// WebPackStreamer (see web_pack_stream.h) — the a11 chunked-pack streaming loader.

#include "context/runtime/content/web_pack_stream.h"

#include "context/runtime/content/content_loader.h"
#include "context/runtime/content/pack_source.h"

#include <deque>
#include <string>
#include <vector>

namespace context::runtime::content
{

WebStreamResult stream_pack_chunked(const ChunkFetcher& fetcher, std::uint64_t chunk_size,
                                    std::uint64_t memory_budget)
{
    WebStreamResult result;

    // --- Phase 1: fetch the whole pack in byte-range chunks (the streamed transfer) ------------------
    const std::uint64_t total = fetcher.total_size();
    if (total == 0)
    {
        result.error = "empty pack (fetcher reported total_size == 0)";
        return result;
    }
    // chunk_size 0 means "one fetch of the whole archive"; otherwise pull ⌈total / chunk_size⌉ ranges.
    const std::uint64_t step = (chunk_size == 0) ? total : chunk_size;
    std::string assembled;
    assembled.reserve(static_cast<std::size_t>(total));
    std::string piece;
    for (std::uint64_t offset = 0; offset < total; offset += step)
    {
        const std::uint64_t length = (offset + step <= total) ? step : (total - offset);
        piece.clear();
        if (!fetcher.fetch_range(offset, length, piece))
        {
            result.error = "range fetch failed at offset " + std::to_string(offset);
            return result;
        }
        if (piece.size() != static_cast<std::size_t>(length))
        {
            result.error = "range fetch returned " + std::to_string(piece.size()) +
                           " bytes, expected " + std::to_string(length) + " at offset " +
                           std::to_string(offset);
            return result;
        }
        assembled.append(piece);
        result.bytes_fetched += length;
        ++result.chunk_count;
    }

    // --- Phase 2: parse the assembled archive (self-verifying — a truncated/corrupt pack fails here) --
    PackContentSource source(std::move(assembled));
    if (!source.ok())
    {
        result.error = "pack parse failed: " + source.error();
        return result;
    }
    result.engine_version = source.engine_version();
    result.unit_count = source.directory().size();

    // --- Phase 3: stream every unit into residency, evicting oldest-first to hold the memory budget --
    // The archive bytes stay held once (source_); only the resident UNITS are the budget-bounded working
    // set (a02 README: the memory-budget scheduler holds a working set far smaller than the archive). A
    // simple oldest-first (FIFO) eviction is enough to prove the ceiling holds over a streamed load; the
    // proximity-driven StreamingScheduler is the richer policy the native gate covers separately.
    RuntimeContentLoader loader(source);
    std::deque<std::uint64_t> resident_order; // load order, for oldest-first eviction
    for (const UnitDescriptor& desc : source.directory())
    {
        std::string load_error;
        if (!loader.load_now(desc.unit_id, &load_error))
        {
            result.error = "unit load failed for " + std::to_string(desc.unit_id) + ": " + load_error;
            return result;
        }
        resident_order.push_back(desc.unit_id);

        // Reconcile to the budget: evict the oldest-loaded units until within budget, but never evict
        // the unit just loaded (keep at least the newest). A budget smaller than the single largest unit
        // cannot be honored — report that fail-closed rather than silently overshooting.
        if (memory_budget != 0)
        {
            while (loader.resident_bytes() > memory_budget && resident_order.size() > 1)
            {
                const std::uint64_t evict = resident_order.front();
                resident_order.pop_front();
                loader.unload(evict);
            }
            if (loader.resident_bytes() > memory_budget)
            {
                result.error = "memory budget " + std::to_string(memory_budget) +
                               " is smaller than a single unit (" +
                               std::to_string(loader.resident_bytes()) + " bytes) — cannot stream";
                return result;
            }
        }
        if (loader.resident_bytes() > result.peak_resident_bytes)
            result.peak_resident_bytes = loader.resident_bytes();
    }

    result.resident_unit_count = loader.resident_unit_count();
    result.world_hash = loader.world_hash();
    result.ok = true;
    return result;
}

} // namespace context::runtime::content
