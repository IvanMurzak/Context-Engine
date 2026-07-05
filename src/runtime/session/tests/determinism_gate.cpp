// The R-QA-005 / L-54 DETERMINISM GATE (blocking, 3-OS: Linux-x64 / Win-x64 / macOS-ARM64).
//
// A cross-platform GOLDEN-hash assertion: a fixed seed + a fixed injected input stream, stepped a
// fixed number of ticks, must produce a byte-identical hierarchical state-hash trace on EVERY
// platform in the determinism matrix. Because sim state is integer-only and hashing folds fixed-
// width big-endian integers (never host-endian bytes, never floats), the golden values below are
// portable — if any platform computes a different hash, THAT leg goes red, which is exactly the
// cross-platform determinism guarantee R-QA-005 demands. Registered as the `determinism-state-hash`
// ctest the CI "Determinism gate" step runs on all three build-matrix legs (docs/ci-fleet-manifest.json).
//
// Updating the golden values: they change only when the demo scenario / systems / hash change on
// PURPOSE. Re-derive by running this gate — it prints the observed values — then paste them below.

#include "context/runtime/session/hash.h"
#include "context/runtime/session/session.h"
#include "context/runtime/session/state_hash.h"
#include "session_test.h"

#include <cstdint>
#include <cstdio>

using namespace context::runtime::session;

namespace
{
constexpr std::uint64_t kSeed = 0x00C0FFEEULL;
constexpr std::uint64_t kTicks = 20;

// The golden digests, derived on the reference build and asserted identical on every matrix platform.
constexpr std::uint64_t kGoldenFinalRoot = 0x22008F1C6FE5F43FULL;
constexpr std::uint64_t kGoldenTraceFold = 0x926BEE919034D236ULL;

// Run the fixed determinism scenario with tracing on.
Session run_fixture()
{
    SessionConfig config;
    config.seed = kSeed;
    Session session(config);
    session.set_trace(true);
    // A fixed, mixed input stream: gameplay move actions, a UI action, and a raw input event.
    session.inject_action_at(0, ActionActivation{"move_x", "performed", 3});
    session.inject_event_at(1, InputEvent{"key", "W", 1});
    session.inject_action_at(2, ActionActivation{"ui_submit", "started", 1});
    session.inject_action_at(5, ActionActivation{"move_y", "performed", -2});
    session.inject_action_at(9, ActionActivation{"fire", "performed", 1});
    session.step(kTicks);
    return session;
}

// Fold a whole trace's per-tick roots into one digest (captures the entire trajectory, not just the
// endpoint — a mid-run divergence that self-heals by the last tick would still fail this).
std::uint64_t fold_trace(const HashTrace& trace)
{
    Fnv1a h;
    for (std::uint64_t root : trace_roots(trace))
        h.update_u64(root);
    return h.digest();
}
} // namespace

int main()
{
    Session a = run_fixture();
    const std::uint64_t final_root = a.state_hash().root;
    const std::uint64_t trace_fold = fold_trace(a.trace());

    // Print the observed digests so a deliberate golden update (or a platform mismatch in CI) is
    // legible directly from the test log.
    std::printf("[determinism] seed=0x%llX ticks=%llu finalRoot=0x%016llX traceFold=0x%016llX "
                "traceLen=%zu\n",
                static_cast<unsigned long long>(kSeed), static_cast<unsigned long long>(kTicks),
                static_cast<unsigned long long>(final_root),
                static_cast<unsigned long long>(trace_fold), a.trace().size());

    // --- within-run determinism: a second identical run reproduces the digests exactly ---------
    Session b = run_fixture();
    CHECK(b.state_hash().root == final_root);
    CHECK(fold_trace(b.trace()) == trace_fold);

    // --- structural expectations --------------------------------------------------------------
    CHECK(a.trace().size() == kTicks);
    CHECK(a.trace()[0].per_system.size() == 3); // input, control, motion

    // --- the CROSS-PLATFORM golden assertion: identical on Linux-x64 / Win-x64 / macOS-ARM64 ---
    CHECK(final_root == kGoldenFinalRoot);
    CHECK(trace_fold == kGoldenTraceFold);

    SESSION_TEST_MAIN_END();
}
