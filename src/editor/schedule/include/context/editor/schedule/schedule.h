// The safe parallel scheduler (R-SIM-006 / R-LANG-011 / L-38): from the systems' STATIC declared
// access sets it derives a parallel schedule DAG ONCE, caches it, and rebuilds it ONLY on a
// composition change (a system added / removed / re-registered) — NEVER per frame. Each tick then:
//   1. runs the NATIVE/engine systems in the cached parallel batches — non-conflicting systems in one
//      batch run concurrently (fork-join over a bounded worker set); conflicting writes serialize into
//      later batches, in registration order (deterministic, L-54);
//   2. runs the TS systems SEQUENTIALLY on the single JS-VM lane (R-LANG-011: one JS VM = one thread).
//      TS systems are EXCLUDED from the DAG entirely — declared-access parallelism applies to
//      native/WASM/engine systems only.
// The native phase fully completes (joins) before the TS phase begins, so a native system and a TS
// system that both mutate the World in the same tick never race. CI thread/UB sanitizers (TSan+UBSan)
// are the authoritative correctness gate for the parallel native phase.
//
// This module orchestrates OPAQUE runnables: a system is a name + lane + declared AccessSet + a
// `std::function<void()>`. HOW a native system safely reaches only its declared World access (the
// batched per-archetype view handout of R-LANG-012, and the debug-Proxy that throws on undeclared
// access of #88 item 2) is deliberately out of scope here — release hands declared views only, which
// is enough for the parallel-safety guarantee this scheduler makes. See README § Deliberately out of
// scope.

#pragma once

#include "context/editor/schedule/access.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace context::editor::schedule
{

// A stable handle to a registered system (its registration index). Stays valid across removals
// (removal tombstones the slot rather than shifting indices), so a SystemId never dangles onto a
// different system.
using SystemId = std::size_t;

// Which scheduler lane a system runs on. Native/engine (and, later, WASM) systems are placed in the
// parallel DAG by their declared access; TS systems are excluded and run on the single JS-VM lane.
enum class Lane
{
    Native,
    Ts,
};

// Batching hint for the small-system coalescing policy. A `Small` system is cheap enough that giving
// it its own worker task would let scheduling overhead dominate its run cost, so the policy coalesces
// runs of small systems into a shared task. `Normal` systems get their own task.
enum class CostHint
{
    Normal,
    Small,
};

// The runnable a system executes each tick. A native system's closure mutates the World's declared
// component records (no structural change — that is deferred to the #88-item-3 command buffer); a TS
// system's closure drives the (query, executor) run_system on the JS lane. The scheduler never
// inspects the body — it only decides WHERE (which batch / the TS lane) and WHEN it runs.
using SystemFn = std::function<void()>;

// One registered system.
struct SystemDesc
{
    std::string name;
    Lane lane = Lane::Native;
    AccessSet access; // consulted for DAG placement only when lane == Native
    CostHint cost = CostHint::Normal;
    SystemFn run;
};

// The parallel-execution / batching policy.
struct SchedulePolicy
{
    // Worker fan-out cap for a native batch. 0 ⇒ resolve to std::thread::hardware_concurrency()
    // (clamped to >= 1) at run time. A batch never spawns more than (worker_count - 1) helper threads
    // (one chunk always runs inline on the calling thread), so a bounded pool of threads services any
    // batch size.
    unsigned worker_count = 0;

    // Small-system coalescing: at most this many consecutive `Small`-cost systems are merged into one
    // worker task. 0 ⇒ a sensible default (8). A `Normal` system is never coalesced with neighbours.
    std::size_t small_batch_target = 0;
};

// The registry of registered systems — the "composition". Mutating it (add / remove) bumps the
// composition generation, which is what invalidates a cached Schedule. SystemIds are stable across
// removals.
//
// Precondition (not enforced): the registry must NOT be mutated (add / remove) from inside a system's
// `run` body while a ParallelScheduler::run_tick over it is in flight. A composition change reallocates
// `systems_` / `active_`, which the worker threads are concurrently reading via `at` / `is_active` — a
// data race and iterator/reference invalidation. Compose the registry between ticks, never during one.
class SystemRegistry
{
public:
    // Register a system; returns its stable SystemId and bumps the composition generation.
    SystemId add(SystemDesc desc);

    // Tombstone a previously-registered system (a composition change). Idempotent; bumps the
    // generation only when it actually deactivates a live system. A removed SystemId is never reused.
    bool remove(SystemId id);

    [[nodiscard]] bool is_active(SystemId id) const noexcept;

    // The composition generation — increments on every add and every effective remove. A Schedule
    // built for generation G stays valid until this changes.
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

    [[nodiscard]] std::size_t size() const noexcept { return systems_.size(); }
    [[nodiscard]] const SystemDesc& at(SystemId id) const { return systems_.at(id); }

private:
    friend class ParallelScheduler;
    std::vector<SystemDesc> systems_;
    std::vector<bool> active_;
    std::uint64_t generation_ = 0;
};

// The derived, cached schedule: the native parallel batches + the excluded TS single lane. Immutable
// once built; the ParallelScheduler rebuilds a fresh one on a composition change.
class Schedule
{
public:
    // Native parallel batches ("levels"): each inner vector holds SystemIds that are pairwise
    // non-conflicting and may run concurrently; batches run in sequence (batch k+1 only after batch k
    // joins). Within a batch the ids are in ascending registration order; a system that conflicts with
    // an earlier one lands in a strictly later batch, so conflicting writes preserve registration
    // order (deterministic, L-54).
    [[nodiscard]] const std::vector<std::vector<SystemId>>& native_batches() const noexcept
    {
        return native_batches_;
    }

    // The TS systems, in registration order — the single JS-VM lane, EXCLUDED from the DAG above.
    [[nodiscard]] const std::vector<SystemId>& ts_lane() const noexcept { return ts_lane_; }

    // The composition generation this schedule was derived from.
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

private:
    friend class ParallelScheduler;
    friend Schedule build_schedule(const SystemRegistry& reg);
    std::vector<std::vector<SystemId>> native_batches_;
    std::vector<SystemId> ts_lane_;
    std::uint64_t generation_ = 0;
};

// Split one native batch into coalesced worker chunks per the policy (small-system batching + the
// worker-count cap). Exposed for direct testing of the batching policy. Every SystemId in `batch`
// appears in exactly one chunk, order-preserving; the number of chunks never exceeds
// max(1, worker_count).
[[nodiscard]] std::vector<std::vector<SystemId>>
plan_batch_chunks(const std::vector<SystemId>& batch, const SystemRegistry& reg,
                  const SchedulePolicy& policy);

// The scheduler: owns the cached Schedule and runs ticks. Build the schedule ONCE (lazily on the first
// tick / schedule() call), reuse it every subsequent tick, and rebuild ONLY when the registry's
// composition generation has changed.
class ParallelScheduler
{
public:
    explicit ParallelScheduler(SchedulePolicy policy = {});

    // Return the schedule for the registry's CURRENT composition, building (or rebuilding) it iff the
    // composition generation changed since the cached one. Pure with respect to the World — building a
    // schedule never runs a system.
    const Schedule& schedule(const SystemRegistry& reg);

    // Run exactly one tick: the native parallel batches (fork-join, batching policy applied), then the
    // TS single lane sequentially. Reuses the cached schedule AND the cached per-batch chunk plan; a
    // tick NEVER rebuilds the DAG or re-plans chunks (only a composition change does). A system whose
    // `run` is empty is skipped. An exception thrown by a system's `run` (on any lane, including a
    // helper thread) propagates out of run_tick after the batch's fork-join barrier — it never calls
    // std::terminate. Precondition: no system's `run` mutates `reg`'s composition mid-tick (see
    // SystemRegistry).
    void run_tick(const SystemRegistry& reg);

    // Total number of times the DAG has been (re)built over this scheduler's lifetime. A run of N
    // ticks with no composition change leaves this at 1 — the "computed once, not per frame" proof.
    [[nodiscard]] std::uint64_t rebuild_count() const noexcept { return rebuild_count_; }

    [[nodiscard]] const SchedulePolicy& policy() const noexcept { return policy_; }

private:
    // Run one native batch from its PRE-PLANNED worker chunks (planned once in schedule(), not per
    // tick). Fork-join over the chunks; any exception a system throws is captured and rethrown after
    // every helper joins.
    void run_native_batch(const SystemRegistry& reg,
                          const std::vector<std::vector<SystemId>>& chunks);

    SchedulePolicy policy_;
    std::optional<Schedule> cached_;
    // Per native batch → its worker chunk plan, index-aligned with cached_->native_batches(). Computed
    // alongside the DAG in schedule() (a pure function of batch + composition + policy, invariant until
    // the next composition change) so run_tick reuses it instead of re-planning every tick.
    std::vector<std::vector<std::vector<SystemId>>> cached_chunks_;
    std::uint64_t rebuild_count_ = 0;
};

// Build the parallel schedule DAG from a registry's current active systems. Pure/free function (the
// ParallelScheduler caches the result); exposed for tests that assert DAG structure directly.
[[nodiscard]] Schedule build_schedule(const SystemRegistry& reg);

} // namespace context::editor::schedule
