// RuntimeContentLoader (R-ASSET-005): the RuntimeKernel primitive that exposes async
// load / instantiate / unload of content units by GUID over a ContentSource (a pack or the live
// editor feed). Async is modelled as a request queue + a bounded pump() — request_load/request_unload
// enqueue, pump(n) services up to n requests (the streaming step a scheduler drives). Synchronous
// load_now/unload exist for direct callers + tests.
//
// A resident unit is materialized content addressed by its composed identity (L-37). Residency is
// isolated: unloading one unit never disturbs another (the R-ASSET-005 / M2-proven property). The
// loader accounts resident_bytes so the StreamingScheduler can hold a working set within a budget.
//
// Handles + invalidation (L-22 / L-24, R-PLAY-003 hot reload): a Handle names a resident unit at a
// generation; resolve() returns null once that generation is stale. invalidate(guid) re-materializes
// the unit from the source (a re-derivation) and bumps its generation — the SAME handle-invalidation
// path hot reload uses, so a live re-derive and a streamed reload share one mechanism.

#pragma once

#include "context/runtime/content/content_source.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace context::runtime::content
{

// An opaque reference to a resident unit at a point-in-time generation. Becomes stale (resolve()
// returns null) when the unit is unloaded or re-derived (invalidate()).
struct Handle
{
    std::uint64_t unit_id = 0;
    std::uint64_t generation = 0;
    bool valid = false;
};

class RuntimeContentLoader
{
public:
    explicit RuntimeContentLoader(const ContentSource& source) : source_(source) {}

    // --- async request surface (a scheduler drives these) -------------------------------------
    // Enqueue a load / unload. Idempotent at service time (loading a resident unit refreshes it;
    // unloading an absent unit is a no-op). Requests for units unknown to the source are dropped.
    void request_load(std::uint64_t unit_id);
    void request_unload(std::uint64_t unit_id);
    [[nodiscard]] std::size_t pending_requests() const noexcept { return queue_.size(); }

    // Service up to `max_ops` queued requests in FIFO order; returns the number serviced. A load that
    // fails to materialize (corrupt chunk) is counted as serviced and recorded in last_error().
    std::size_t pump(std::size_t max_ops = static_cast<std::size_t>(-1));

    // --- synchronous surface ------------------------------------------------------------------
    // Materialize a unit now (instantiate it into residency). Returns false + sets last_error() for an
    // unknown id or a corrupt chunk. Re-loading a resident unit refreshes it WITHOUT bumping its
    // generation (that is invalidate()'s job) — it stays the same logical residency.
    bool load_now(std::uint64_t unit_id, std::string* error = nullptr);
    // Drop a unit from residency; returns false if it was not resident. Releases its bytes + entities.
    bool unload(std::uint64_t unit_id);
    // Re-derive a resident unit from the source and BUMP its generation (hot reload / L-24 handle
    // invalidation). Returns false if the unit is not resident or re-materialization failed. Any
    // Handle minted before this call resolves to null afterward.
    bool invalidate(std::uint64_t unit_id, std::string* error = nullptr);

    // --- residency queries --------------------------------------------------------------------
    [[nodiscard]] bool is_resident(std::uint64_t unit_id) const;
    [[nodiscard]] std::size_t resident_unit_count() const noexcept { return resident_.size(); }
    [[nodiscard]] std::size_t resident_entity_count() const noexcept;
    [[nodiscard]] std::uint64_t resident_bytes() const noexcept { return resident_bytes_; }
    [[nodiscard]] std::size_t known_unit_count() const noexcept { return source_.directory().size(); }
    // The source directory (the cheap per-unit metadata index) — a scheduler reads unit byte sizes
    // from it to plan a memory-budgeted working set without decoding chunks.
    [[nodiscard]] const std::vector<UnitDescriptor>& directory() const { return source_.directory(); }
    // Resident unit ids ascending — a stable residency fingerprint for isolation asserts.
    [[nodiscard]] std::vector<std::uint64_t> resident_unit_ids() const;
    // The materialized unit, or nullptr when not resident (a read-only view into residency).
    [[nodiscard]] const LoadedUnit* resident_unit(std::uint64_t unit_id) const;

    // --- handles ------------------------------------------------------------------------------
    // Mint a handle to a resident unit at its current generation; an invalid handle when not resident.
    [[nodiscard]] Handle handle_for(std::uint64_t unit_id) const;
    // Resolve a handle to its unit, or nullptr if the unit is not resident OR the handle is stale
    // (its generation no longer matches — the unit was unloaded and reloaded, or invalidated).
    [[nodiscard]] const LoadedUnit* resolve(const Handle& handle) const;

    // --- state hash ---------------------------------------------------------------------------
    // A feed-independent FNV-1a-64 fold over the resident world: per resident unit (ascending id) its
    // id, entity count, and each entity's composed identity + canonical(value); a sidecar folds its
    // bytes. Two feeds that carry the same content units produce the SAME hash — the parity property.
    [[nodiscard]] std::uint64_t world_hash() const;

    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }

private:
    struct Resident
    {
        LoadedUnit unit;
        std::uint64_t generation = 0;
    };

    enum class OpKind
    {
        load,
        unload
    };
    struct Op
    {
        OpKind kind;
        std::uint64_t unit_id;
    };

    const ContentSource& source_;
    std::map<std::uint64_t, Resident> resident_; // ordered by unit id (deterministic iteration/hash)
    std::deque<Op> queue_;
    std::uint64_t resident_bytes_ = 0;
    std::uint64_t generation_seq_ = 0; // monotonic generation minter (per (re)materialization)
    std::string last_error_;
};

} // namespace context::runtime::content
