// M8.5 exit criterion 5 — `m85-exit-5-density-commitment` (ROADMAP §1-M8.5 exit / a23-m85-exit;
// R-FILE-011 / R-QA-012, a21): "the R-FILE-011 ticks/sec/instance + instances-per-box orchestration-
// density targets are committed and benchmarked." The COMMITTED-and-HONEST half of the clause, asserted
// as a permanent gate — the milestone-closing mirror of the m8-exit-5 seam-8 OPS1-honesty audit
// (which reads bench/build-time-budget.json the same way):
//
//   * the R-FILE-011 orchestration-density targets ARE committed (bench/density-targets.json carries the
//     ticks/sec/instance single-instance floor AND the instances-per-box floor at the 60 Hz sustain
//     floor, tagged the M8.5 a21 milestone with its R-QA-009 methodology + the bench subject);
//   * the commitment is HONEST per the ops1 state — the numbers declare themselves
//     advisory_until_provisioned and name their unprovisioned perf-isolated runner class
//     (perf-linux-bare-metal), exactly as a21/a12 already treat it (never a blocking number a shared GH
//     runner cannot honestly measure — R-QA-012 advisory-until-provisioned);
//   * the committed artifacts that BENCHMARK the targets exist (bench/density.py controller +
//     docs/density-targets.md normative definitions) — rots-if-removed.
//
// The "benchmarked" half itself runs on every PR in the sibling BLOCKING `density-bench` CI job
// (bench/density.py measure over the packed subject; the perf NUMBERS advisory until the runner class is
// provisioned) — this gate does not re-run the bench, it pins the commitment + honesty so a regression
// that drops the targets, silently flips them blocking, or deletes the harness turns the milestone gate
// red. Pure file reads, GPU-free; runs in the blocking "M8.5 exit gate" build-job step on all three OS legs.

#include "m85_exit_test.h"

#include "context/common/subprocess.h"

#include <filesystem>
#include <string>

#ifndef CONTEXT_DENSITY_TARGETS
#error "CONTEXT_DENSITY_TARGETS (path to bench/density-targets.json) must be defined by the build."
#endif
#ifndef CONTEXT_DENSITY_SCRIPT
#error "CONTEXT_DENSITY_SCRIPT (path to bench/density.py) must be defined by the build."
#endif
#ifndef CONTEXT_DENSITY_DOC
#error "CONTEXT_DENSITY_DOC (path to docs/density-targets.md) must be defined by the build."
#endif

namespace sp = context::common::subprocess;

namespace
{

[[nodiscard]] bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

} // namespace

using context::tests::m85::report;

int main()
{
    // --- the committed R-FILE-011 orchestration-density targets ------------------------------------
    const std::string targets = sp::read_file(CONTEXT_DENSITY_TARGETS);
    CHECK(!targets.empty());

    // Committed: both R-FILE-011 density floors are present, each with a numeric floor, plus the 60 Hz
    // sustain floor every instance must hold for a rung to count toward instances-per-box.
    CHECK(contains(targets, "\"requirement\": \"R-FILE-011\""));
    CHECK(contains(targets, "\"ticks_per_sec_per_instance\""));
    CHECK(contains(targets, "\"instances_per_box\""));
    CHECK(contains(targets, "\"floor\""));
    CHECK(contains(targets, "\"sustain_floor_ticks_per_sec\""));
    // The M8.5 a21 milestone tag + the R-QA-009 measurement methodology + the packed bench subject.
    CHECK(contains(targets, "M8.5 a21"));
    CHECK(contains(targets, "R-QA-009"));
    CHECK(contains(targets, "bench_subject"));

    // Honest per the ops1 state (R-QA-012 advisory-until-provisioned): the numbers declare themselves
    // advisory and name their UNPROVISIONED perf-isolated runner class — never a blocking number a
    // shared GH runner cannot honestly measure (the a21/a12 precedent; the m8-exit-5 seam-8 mirror).
    CHECK(contains(targets, "\"advisory_until_provisioned\": true"));
    CHECK(contains(targets, "perf-linux-bare-metal"));
    // The acknowledged-gaps honesty note vs GPU-vectorized simulators (ARCHITECTURE 1.1 wedge pitch).
    CHECK(contains(targets, "honesty_note"));

    // --- the committed artifacts that BENCHMARK the targets exist (rots-if-removed) ----------------
    // The density controller (bench/density.py — the sibling blocking `density-bench` CI job runs its
    // `measure` step per-PR) and the normative definitions doc.
    CHECK(std::filesystem::exists(CONTEXT_DENSITY_SCRIPT));
    CHECK(std::filesystem::exists(CONTEXT_DENSITY_DOC));

    return report("m85-exit-5-density-commitment",
                  "the R-FILE-011 orchestration-density targets are committed (ticks/sec/instance + "
                  "instances-per-box floors) + honestly advisory-until-provisioned, and the benchmark "
                  "harness/doc are present (the density-bench CI job runs them per-PR)");
}
