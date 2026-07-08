// The safe parallel scheduler — DAG construction, small-system batching, and fork-join execution.
// See schedule.h for the contract.

#include "context/editor/schedule/schedule.h"

#include <algorithm>
#include <thread>
#include <utility>

namespace context::editor::schedule
{
namespace
{

// Resolve the effective worker fan-out: an explicit policy value, else the host's hardware
// concurrency, clamped to at least 1 (so a batch always has one lane — the calling thread).
[[nodiscard]] unsigned resolve_worker_count(const SchedulePolicy& policy) noexcept
{
    unsigned wc = policy.worker_count;
    if (wc == 0)
    {
        wc = std::thread::hardware_concurrency();
    }
    return wc == 0 ? 1U : wc;
}

[[nodiscard]] std::size_t resolve_small_target(const SchedulePolicy& policy) noexcept
{
    return policy.small_batch_target == 0 ? std::size_t{8} : policy.small_batch_target;
}

// Run every system in a chunk sequentially (a chunk is a coalesced worker task). All ids are active
// native systems by construction; an empty `run` is skipped.
void run_chunk(const SystemRegistry& reg, const std::vector<SystemId>& chunk)
{
    for (const SystemId id : chunk)
    {
        const SystemDesc& sys = reg.at(id);
        if (sys.run)
        {
            sys.run();
        }
    }
}

} // namespace

SystemId SystemRegistry::add(SystemDesc desc)
{
    const SystemId id = systems_.size();
    systems_.push_back(std::move(desc));
    active_.push_back(true);
    ++generation_;
    return id;
}

bool SystemRegistry::remove(SystemId id)
{
    if (id >= active_.size() || !active_[id])
    {
        return false;
    }
    active_[id] = false;
    ++generation_;
    return true;
}

bool SystemRegistry::is_active(SystemId id) const noexcept
{
    return id < active_.size() && active_[id];
}

Schedule build_schedule(const SystemRegistry& reg)
{
    Schedule out;
    out.generation_ = reg.generation();

    // Native systems already placed, with the batch (level) each landed in. Kept in registration order
    // so the "look back at earlier conflicting systems" scan is a simple forward walk.
    struct Placed
    {
        SystemId id;
        std::size_t level;
        const AccessSet* access;
    };
    std::vector<Placed> placed;
    placed.reserve(reg.size());

    for (SystemId id = 0; id < reg.size(); ++id)
    {
        if (!reg.is_active(id))
        {
            continue;
        }
        const SystemDesc& sys = reg.at(id);
        if (sys.lane == Lane::Ts)
        {
            // TS systems are EXCLUDED from the DAG — the single JS-VM lane, registration order.
            out.ts_lane_.push_back(id);
            continue;
        }

        // As-soon-as-possible level assignment over the conflict DAG: this system's level is one past
        // the highest level of any EARLIER native system it conflicts with (0 if it conflicts with
        // none). Two systems that end up at the same level therefore never conflict — if they did, the
        // later one would have been pushed to at least (earlier.level + 1). Conflicting systems keep
        // registration order (a conflicting later system sits in a strictly later batch).
        std::size_t level = 0;
        for (const Placed& p : placed)
        {
            if (conflicts(sys.access, *p.access) && p.level + 1 > level)
            {
                level = p.level + 1;
            }
        }
        placed.push_back(Placed{id, level, &sys.access});

        if (level >= out.native_batches_.size())
        {
            out.native_batches_.resize(level + 1);
        }
        out.native_batches_[level].push_back(id); // ascending id order preserved by the id-ordered loop
    }

    return out;
}

std::vector<std::vector<SystemId>> plan_batch_chunks(const std::vector<SystemId>& batch,
                                                     const SystemRegistry& reg,
                                                     const SchedulePolicy& policy)
{
    const unsigned worker_count = resolve_worker_count(policy);
    const std::size_t small_target = resolve_small_target(policy);

    // Pass 1 — small-system coalescing: fold runs of consecutive `Small`-cost systems into one chunk
    // (up to `small_target` per chunk) so scheduling overhead cannot dominate tiny systems; a `Normal`
    // system stands alone.
    std::vector<std::vector<SystemId>> fine;
    for (const SystemId id : batch)
    {
        const bool small = reg.at(id).cost == CostHint::Small;
        const bool can_extend = small && !fine.empty() && !fine.back().empty()
            && reg.at(fine.back().front()).cost == CostHint::Small
            && fine.back().size() < small_target;
        if (can_extend)
        {
            fine.back().push_back(id);
        }
        else
        {
            fine.push_back({id});
        }
    }

    // Pass 2 — worker-count cap: never hand out more chunks than workers. If the coalesced chunk count
    // still exceeds the cap, round-robin the fine chunks into `worker_count` buckets (each bucket stays
    // ascending in id, and every system runs exactly once).
    const std::size_t cap = std::max<std::size_t>(1, worker_count);
    if (fine.size() <= cap)
    {
        return fine;
    }

    std::vector<std::vector<SystemId>> buckets(cap);
    for (std::size_t k = 0; k < fine.size(); ++k)
    {
        std::vector<SystemId>& dst = buckets[k % cap];
        dst.insert(dst.end(), fine[k].begin(), fine[k].end());
    }
    buckets.erase(std::remove_if(buckets.begin(), buckets.end(),
                                 [](const std::vector<SystemId>& b) { return b.empty(); }),
                  buckets.end());
    return buckets;
}

ParallelScheduler::ParallelScheduler(SchedulePolicy policy) : policy_(policy) {}

const Schedule& ParallelScheduler::schedule(const SystemRegistry& reg)
{
    if (!cached_ || cached_->generation() != reg.generation())
    {
        cached_ = build_schedule(reg); // rebuild ONLY on a composition change — never per tick
        ++rebuild_count_;
    }
    return *cached_;
}

void ParallelScheduler::run_native_batch(const SystemRegistry& reg,
                                         const std::vector<SystemId>& batch)
{
    if (batch.empty())
    {
        return;
    }
    const std::vector<std::vector<SystemId>> chunks = plan_batch_chunks(batch, reg, policy_);
    if (chunks.size() <= 1)
    {
        if (!chunks.empty())
        {
            run_chunk(reg, chunks.front());
        }
        return;
    }

    // Fork-join: one helper thread per chunk except the first, which runs inline on the calling
    // thread; join every helper before returning. join() is a clean happens-before edge, so with the
    // batch's disjoint declared writes there is no data race (the TSan/UBSan CI gate proves it).
    std::vector<std::thread> helpers;
    helpers.reserve(chunks.size() - 1);
    for (std::size_t c = 1; c < chunks.size(); ++c)
    {
        helpers.emplace_back([&reg, &chunks, c] { run_chunk(reg, chunks[c]); });
    }
    run_chunk(reg, chunks.front());
    for (std::thread& t : helpers)
    {
        t.join();
    }
}

void ParallelScheduler::run_tick(const SystemRegistry& reg)
{
    const Schedule& sched = schedule(reg);

    // Native phase: parallel batches, in sequence (batch k+1 only after batch k joins).
    for (const std::vector<SystemId>& batch : sched.native_batches())
    {
        run_native_batch(reg, batch);
    }

    // TS phase: the single JS-VM lane, sequential, AFTER the native phase has fully joined — so a
    // native system and a TS system that both mutate the World this tick never overlap.
    for (const SystemId id : sched.ts_lane())
    {
        const SystemDesc& sys = reg.at(id);
        if (sys.run)
        {
            sys.run();
        }
    }
}

} // namespace context::editor::schedule
