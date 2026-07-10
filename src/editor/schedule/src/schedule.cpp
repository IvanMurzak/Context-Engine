// The safe parallel scheduler — DAG construction, small-system batching, and fork-join execution.
// See schedule.h for the contract.

#include "context/editor/schedule/schedule.h"

#include <algorithm>
#include <exception>
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

// Run one system: a run_cmd system records into its per-system buffer when one is available (the
// World-taking tick); otherwise fall back to `run`; a system with neither (or a run_cmd-only system
// in the World-free tick) is skipped. `buffers` is indexed by SystemId (nullable).
void run_one(const SystemRegistry& reg, SystemId id, kernel::CommandBuffer* buffers)
{
    const SystemDesc& sys = reg.at(id);
    if (buffers != nullptr && sys.run_cmd)
    {
        sys.run_cmd(buffers[id]);
    }
    else if (sys.run)
    {
        sys.run();
    }
}

// Run every system in a chunk sequentially (a chunk is a coalesced worker task). All ids are active
// native systems by construction.
void run_chunk(const SystemRegistry& reg, const std::vector<SystemId>& chunk,
               kernel::CommandBuffer* buffers)
{
    for (const SystemId id : chunk)
    {
        run_one(reg, id, buffers);
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
        // Complexity: this back-scan makes build_schedule O(n^2) in the native-system count. That is
        // fine here — the DAG is built ONCE per composition change (never per tick), and system counts
        // are small; revisit (e.g. index conflicts by component) only if compositions reach high
        // hundreds of native systems.
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

        // Pre-plan each native batch's worker chunks alongside the DAG. The chunk plan is a pure
        // function of (batch, composition, policy) — all invariant until the next composition change —
        // so planning it here, not per tick, upholds the "computed once, not per frame" law for the
        // batching/chunking half as well as the DAG. cached_chunks_ stays index-aligned with the
        // batches.
        cached_chunks_.clear();
        cached_chunks_.reserve(cached_->native_batches().size());
        for (const std::vector<SystemId>& batch : cached_->native_batches())
        {
            cached_chunks_.push_back(plan_batch_chunks(batch, reg, policy_));
        }

        ++rebuild_count_;
    }
    return *cached_;
}

void ParallelScheduler::run_native_batch(const SystemRegistry& reg,
                                         const std::vector<std::vector<SystemId>>& chunks,
                                         kernel::CommandBuffer* buffers)
{
    if (chunks.size() <= 1)
    {
        if (!chunks.empty())
        {
            run_chunk(reg, chunks.front(), buffers);
        }
        return;
    }

    // Fork-join: one helper thread per chunk except the first, which runs inline on the calling
    // thread; join every helper before returning. join() is a clean happens-before edge, so with the
    // batch's disjoint declared writes there is no data race (the TSan/UBSan CI gate proves it).
    //
    // Exception safety: a system's run() may throw. An exception escaping a std::thread's body calls
    // std::terminate, and a joinable thread destroyed during stack unwinding does too. So each helper
    // captures its failure into a per-helper exception_ptr slot, EVERY spawned helper is joined on
    // every path (including when emplace_back itself throws under thread-resource exhaustion, or the
    // inline chunk throws), and the first captured failure is rethrown to the caller after the join
    // barrier — turning a would-be process abort into an ordinary propagated exception.
    const std::size_t helper_count = chunks.size() - 1;
    std::vector<std::exception_ptr> errors(helper_count);
    std::vector<std::thread> helpers;
    helpers.reserve(helper_count);

    const auto join_all = [&helpers]
    {
        for (std::thread& t : helpers)
        {
            if (t.joinable())
            {
                t.join();
            }
        }
    };

    try
    {
        for (std::size_t c = 1; c < chunks.size(); ++c)
        {
            helpers.emplace_back(
                [&reg, &chunks, &errors, buffers, c]
                {
                    try
                    {
                        run_chunk(reg, chunks[c], buffers);
                    }
                    catch (...)
                    {
                        errors[c - 1] = std::current_exception();
                    }
                });
        }
        run_chunk(reg, chunks.front(), buffers); // the inline chunk, on the calling thread
    }
    catch (...)
    {
        // The inline chunk (or a helper spawn) threw: still join every already-spawned helper so none
        // is destroyed while joinable, then propagate.
        join_all();
        throw;
    }

    join_all();

    // Surface the first helper failure (in chunk order) to the caller.
    for (const std::exception_ptr& e : errors)
    {
        if (e)
        {
            std::rethrow_exception(e);
        }
    }
}

void ParallelScheduler::run_tick(const SystemRegistry& reg)
{
    const Schedule& sched = schedule(reg); // (re)builds the DAG + chunk plan iff composition changed

    // Native phase: parallel batches, in sequence (batch k+1 only after batch k joins). Each batch's
    // worker chunks were pre-planned in schedule() (cached_chunks_ is index-aligned with the batches) —
    // a tick runs them, never re-plans them.
    for (const std::vector<std::vector<SystemId>>& chunks : cached_chunks_)
    {
        run_native_batch(reg, chunks, nullptr);
    }

    // TS phase: the single JS-VM lane, sequential, AFTER the native phase has fully joined — so a
    // native system and a TS system that both mutate the World this tick never overlap.
    for (const SystemId id : sched.ts_lane())
    {
        run_one(reg, id, nullptr);
    }
}

void ParallelScheduler::run_tick(const SystemRegistry& reg, kernel::World& world)
{
    const Schedule& sched = schedule(reg); // (re)builds the DAG + chunk plan iff composition changed

    // Per-system command buffers, indexed by SystemId. Grown lazily; every buffer is empty here
    // (applied or discarded at the end of every deferred tick), so this is allocation-free once the
    // registry size has been seen.
    if (cmd_buffers_.size() < reg.size())
    {
        cmd_buffers_.resize(reg.size());
    }
    kernel::CommandBuffer* buffers = cmd_buffers_.data();

    // Discard every recorded-but-unapplied buffer — a throwing system must not leave its (or its
    // siblings') structural commands to leak into the NEXT tick as stale state.
    const auto discard_all = [this]() noexcept
    {
        for (kernel::CommandBuffer& buf : cmd_buffers_)
        {
            buf.clear();
        }
    };

    try
    {
        // Native phase: after each batch fully joins, apply that batch's recorded buffers in
        // ascending registration order (the batch vector is id-ascending by construction) — the
        // deterministic L-54 ordering, independent of which helper thread finished first. Dependent
        // (conflicting) systems sit in strictly later batches, so they observe these changes; the
        // batch's own systems ran over the stable pre-batch World (deferred-invisibility).
        for (std::size_t b = 0; b < cached_chunks_.size(); ++b)
        {
            run_native_batch(reg, cached_chunks_[b], buffers);
            for (const SystemId id : sched.native_batches()[b])
            {
                if (!buffers[id].empty())
                {
                    buffers[id].apply(world);
                }
            }
        }

        // TS phase: sequential single lane; apply each TS system's buffer immediately after it
        // returns, BEFORE the next TS system runs (on a sequential lane every later system is a
        // potential dependent).
        for (const SystemId id : sched.ts_lane())
        {
            run_one(reg, id, buffers);
            if (!buffers[id].empty())
            {
                buffers[id].apply(world);
            }
        }
    }
    catch (...)
    {
        discard_all();
        throw;
    }
}

} // namespace context::editor::schedule
