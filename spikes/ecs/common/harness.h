// spikes/ecs — the shared benchmark harness.
//
// Each implementation provides a Sim type with this duck-typed interface and calls
// runHarness<Sim>("name"). The harness owns run structure, timing, statistics, the
// determinism cross-check (order-independent world checksum) and the baseline
// microbenchmarks, so all three executables measure exactly the same thing.
//
//   Sim();                                            // empty world
//   void setup();                                     // spawn kInitialEntities
//   void phaseMove(); void phaseGrid();
//   void phaseAoe(int frame); void phaseBurn();
//   void phaseKill(int frame); void phaseFlush(int frame);
//   Checksum checksum();                              // over all alive entities
//   std::uint64_t aliveCount();
//   void microIterate();                              // one move pass over all entities
//   void collectHandles(std::vector<std::uint64_t>&); // opaque handles of alive entities
//   double sumHealthRandom(const std::vector<std::uint64_t>&); // random-access hp reads
//   void benchCreate(std::uint32_t n);                // create n entities (4 components)
//   void benchDestroy();                              // destroy those n entities

#pragma once

#include "bench.h"
#include "workload.h"

#include <cinttypes>
#include <cstdio>
#include <vector>

namespace spike {

// Deterministic Fisher-Yates so every implementation random-accesses in the same order.
inline void deterministicShuffle(std::vector<std::uint64_t>& v)
{
    std::uint64_t state = 0x5EEDF00Dull;
    for (std::size_t i = v.size(); i > 1; --i)
    {
        state = splitmix64(state);
        const std::size_t j = static_cast<std::size_t>(state % i);
        std::swap(v[i - 1], v[j]);
    }
}

template <typename SimT>
int runHarness(const char* impl)
{
    std::printf("[spike-ecs] impl=%s entities=%u frames=%d churn=%u aoe=%d runs=%d\n",
                impl, kInitialEntities, kFrames, kChurnPerFrame, kAoePerFrame, kSimRuns);

    struct RunSample
    {
        double setup = 0, move = 0, grid = 0, aoe = 0, burn = 0, kill = 0, flush = 0;
        double frameTotal = 0;
    };

    // ---- full simulation: median of kSimRuns runs ------------------------------------
    std::vector<RunSample> runs;
    Checksum firstChecksum{};
    std::uint64_t alive = 0;
    for (int run = 0; run < kSimRuns; ++run)
    {
        SimT sim;
        RunSample rs;
        Timer t;
        sim.setup();
        rs.setup = t.elapsedMs();

        for (int frame = 0; frame < kFrames; ++frame)
        {
            t.reset(); sim.phaseMove();        rs.move  += t.elapsedMs();
            t.reset(); sim.phaseGrid();        rs.grid  += t.elapsedMs();
            t.reset(); sim.phaseAoe(frame);    rs.aoe   += t.elapsedMs();
            t.reset(); sim.phaseBurn();        rs.burn  += t.elapsedMs();
            t.reset(); sim.phaseKill(frame);   rs.kill  += t.elapsedMs();
            t.reset(); sim.phaseFlush(frame);  rs.flush += t.elapsedMs();
        }
        rs.frameTotal = rs.move + rs.grid + rs.aoe + rs.burn + rs.kill + rs.flush;

        const Checksum cs = sim.checksum();
        alive = sim.aliveCount();
        if (run == 0)
        {
            firstChecksum = cs;
        }
        else if (!(cs == firstChecksum))
        {
            std::printf("[spike-ecs] impl=%s ERROR: checksum diverged between runs\n", impl);
            return 1;
        }
        runs.push_back(rs);
    }

    std::printf("[spike-ecs] impl=%s checksum sum=%016" PRIx64 " xor=%016" PRIx64
                " count=%" PRIu64 " alive=%" PRIu64 "\n",
                impl, firstChecksum.sum, firstChecksum.xr, firstChecksum.count, alive);

    auto collect = [&](double RunSample::*field) {
        std::vector<double> v;
        for (const auto& r : runs) v.push_back(r.*field);
        return computeStats(v);
    };
    printMetric(impl, "sim.setup_1M", collect(&RunSample::setup), "ms");
    printMetric(impl, "sim.move_total", collect(&RunSample::move), "ms");
    printMetric(impl, "sim.grid_total", collect(&RunSample::grid), "ms");
    printMetric(impl, "sim.aoe_total", collect(&RunSample::aoe), "ms");
    printMetric(impl, "sim.burn_total", collect(&RunSample::burn), "ms");
    printMetric(impl, "sim.kill_total", collect(&RunSample::kill), "ms");
    printMetric(impl, "sim.flush_total", collect(&RunSample::flush), "ms");
    {
        Stats ft = collect(&RunSample::frameTotal);
        printMetric(impl, "sim.frames_total", ft, "ms");
        Stats perFrame{ft.median / kFrames, ft.min / kFrames, ft.max / kFrames};
        printMetric(impl, "sim.per_frame", perFrame, "ms");
    }

    // ---- baseline microbenchmarks on a fresh 1M world --------------------------------
    {
        SimT sim;
        sim.setup();

        // 1M-entity iteration (move pass), median of kMicroIterReps.
        for (int i = 0; i < kMicroIterWarmup; ++i) sim.microIterate();
        std::vector<double> iterMs;
        for (int i = 0; i < kMicroIterReps; ++i)
        {
            Timer t;
            sim.microIterate();
            iterMs.push_back(t.elapsedMs());
        }
        const Stats it = computeStats(iterMs);
        printMetric(impl, "micro.iterate_1M", it, "ms");
        const double nsPerEntity = it.median * 1e6 / kInitialEntities;
        std::printf("[spike-ecs] impl=%s metric=micro.iterate_per_entity median=%.3f unit=ns\n",
                    impl, nsPerEntity);

        // Random-access component reads over deterministically shuffled handles.
        std::vector<std::uint64_t> handles;
        sim.collectHandles(handles);
        deterministicShuffle(handles);
        std::vector<double> raMs;
        double sum = 0.0;
        for (int i = 0; i < kRandomAccessReps; ++i)
        {
            Timer t;
            sum = sim.sumHealthRandom(handles);
            raMs.push_back(t.elapsedMs());
        }
        printMetric(impl, "micro.random_access_1M", computeStats(raMs), "ms");
        std::printf("[spike-ecs] impl=%s metric=micro.random_access_sum value=%.1f\n", impl, sum);
    }

    // ---- create/destroy throughput on a fresh empty world ----------------------------
    {
        std::vector<double> createMs, destroyMs;
        for (int i = 0; i < kSimRuns; ++i)
        {
            SimT sim;
            Timer t;
            sim.benchCreate(kCreateDestroyCount);
            createMs.push_back(t.elapsedMs());
            t.reset();
            sim.benchDestroy();
            destroyMs.push_back(t.elapsedMs());
        }
        printMetric(impl, "micro.create_100k", computeStats(createMs), "ms");
        printMetric(impl, "micro.destroy_100k", computeStats(destroyMs), "ms");
    }

    std::printf("[spike-ecs] impl=%s done\n", impl);
    return 0;
}

} // namespace spike
