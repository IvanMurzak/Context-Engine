// The PHYSICS-DETERMINISM SPIKE PROOF (R-SIM-005 / L-54, M6-F0a — issue #170).
//
// F0a's make-or-break question: can a physics-shaped step hash byte-identically across the wedge
// matrix (Linux-x64 / Win-x64 / macOS-ARM64) BEFORE any physics package is written? This gate is the
// evidence behind the recorded physics-core DECISION (docs/physics-determinism-decision.md): a
// FIXED-POINT core. It runs a trivial physics-shaped simulation — semi-implicit-Euler integration of
// several rigid bodies under constant gravity, with fixed-point drag and integer-restitution
// wall/floor collisions — entirely in the INTEGER / fixed-point domain (no float, no libm, no FMA),
// then folds each tick's state through the SAME big-endian FNV-1a primitive the real hierarchical
// state hash uses (hash.h). Because the trajectory is pure integer arithmetic and the fold is
// fixed-endian, the golden digests below are PORTABLE — if any matrix platform computes a different
// hash, THAT leg goes red, which is precisely the cross-platform determinism guarantee.
//
// Registered as the `determinism-physics-wedge` ctest, so the blocking CI "Determinism gate" step
// (-R "^determinism-") runs it on all three build-matrix legs — the wedge-matrix proof lands with no
// new CI job. This mirrors determinism_gate.cpp (the integer-only session harness) but exercises the
// fixed-point ARITHMETIC (mul / shift / compare) a physics core needs, not just component storage.
//
// Updating the golden values: they change only when the fixed-point scenario changes on PURPOSE.
// Re-derive by running this gate — it prints the observed values — then paste them below.

#include "context/runtime/session/hash.h"
#include "session_test.h"

#include <array>
#include <cstdint>
#include <cstdio>

using context::runtime::session::Fnv1a;

namespace
{
// Fixed-point scalars are int64 in Q<kFrac> sub-units. Every operation below is integer add / mul /
// shift / compare — NEVER float — so a body's whole trajectory is bit-identical on x86-64 and arm64.
constexpr std::int64_t kFrac = 16;
constexpr std::int64_t kOne = std::int64_t{1} << kFrac;

// Fixed-point multiply: (a * b) >> kFrac, with the product taken in int64 (operands stay well within
// 2^31 sub-units for this scenario, so the 64-bit product never overflows). Deterministic rounding:
// arithmetic right shift truncates toward negative infinity IDENTICALLY on every target (it is a
// pure integer shift, not an FP rounding mode), which is exactly why fixed-point is portable.
constexpr std::int64_t fx_mul(std::int64_t a, std::int64_t b) noexcept
{
    return (a * b) >> kFrac;
}

struct Body
{
    std::int64_t px = 0, py = 0; // position, fixed-point sub-units
    std::int64_t vx = 0, vy = 0; // velocity, fixed-point sub-units per tick
};

constexpr std::int64_t kFloorY = 0;              // floor plane (py may not go below this)
constexpr std::int64_t kWallX = 40 * kOne;       // right wall
constexpr std::int64_t kGravity = kOne / 10;     // 0.1 unit / tick^2 downward
constexpr std::int64_t kDrag = (kOne * 98) / 100;      // 0.98 horizontal drag per tick
constexpr std::int64_t kRestitution = (kOne * 3) / 4;  // 0.75 bounce coefficient

// One physics-shaped step over a body: semi-implicit Euler (update v, then p) + fixed-point drag +
// integer-restitution floor/wall collisions. Pure integer arithmetic end to end.
void step_body(Body& b) noexcept
{
    // Integrate velocity (gravity) then position.
    b.vy -= kGravity;
    b.vx = fx_mul(b.vx, kDrag);
    b.px += b.vx;
    b.py += b.vy;

    // Floor collision: reflect position across the floor, invert + damp vertical velocity.
    if (b.py < kFloorY)
    {
        b.py = kFloorY + (kFloorY - b.py);
        b.vy = fx_mul(-b.vy, kRestitution);
    }
    // Wall collisions (left at x=0, right at kWallX): reflect + damp horizontal velocity.
    if (b.px < 0)
    {
        b.px = -b.px;
        b.vx = fx_mul(-b.vx, kRestitution);
    }
    else if (b.px > kWallX)
    {
        b.px = kWallX - (b.px - kWallX);
        b.vx = fx_mul(-b.vx, kRestitution);
    }
}

// Fold one body's fixed-point fields into the hash in a fixed field order (big-endian, hash.h).
void hash_body(const Body& b, Fnv1a& h) noexcept
{
    h.update_i64(b.px);
    h.update_i64(b.py);
    h.update_i64(b.vx);
    h.update_i64(b.vy);
}

constexpr std::uint64_t kTicks = 96;

// The initial scene: four bodies with distinct fixed-point positions + velocities so the run
// exercises drag, gravity, and all three collision surfaces.
std::array<Body, 4> initial_scene()
{
    return {{
        Body{5 * kOne, 20 * kOne, kOne, 0},
        Body{12 * kOne, 8 * kOne, -2 * kOne, kOne / 2},
        Body{30 * kOne, 15 * kOne, 3 * kOne, -kOne},
        Body{38 * kOne, 3 * kOne, kOne / 4, 2 * kOne},
    }};
}

// Run the fixed scenario; return the end-of-run state hash and a fold of every per-tick root (so a
// mid-run divergence that self-heals by the last tick still fails).
struct Result
{
    std::uint64_t final_state = 0;
    std::uint64_t trace_fold = 0;
};

Result run_fixture()
{
    std::array<Body, 4> bodies = initial_scene();
    Fnv1a trace;
    for (std::uint64_t t = 0; t < kTicks; ++t)
    {
        for (Body& b : bodies)
            step_body(b);
        // Per-tick root: hash all bodies' fields in order.
        Fnv1a tick;
        for (const Body& b : bodies)
            hash_body(b, tick);
        trace.update_u64(tick.digest());
    }
    Fnv1a final_h;
    for (const Body& b : bodies)
        hash_body(b, final_h);
    return {final_h.digest(), trace.digest()};
}

// The golden digests, derived on the reference build and asserted identical on every matrix platform.
constexpr std::uint64_t kGoldenFinalState = 0x98D2D5873CA89D73ULL;
constexpr std::uint64_t kGoldenTraceFold = 0xDD9DC8456413FCA4ULL;
} // namespace

int main()
{
    const Result a = run_fixture();

    // Print the observed digests so a deliberate golden update (or a platform mismatch in CI) is
    // legible directly from the test log.
    std::printf("[determinism-physics] ticks=%llu bodies=4 finalState=0x%016llX traceFold=0x%016llX\n",
                static_cast<unsigned long long>(kTicks),
                static_cast<unsigned long long>(a.final_state),
                static_cast<unsigned long long>(a.trace_fold));

    // --- within-run determinism: a second identical run reproduces the digests exactly ----------
    const Result b = run_fixture();
    CHECK(b.final_state == a.final_state);
    CHECK(b.trace_fold == a.trace_fold);

    // --- the CROSS-PLATFORM golden assertion: identical on Linux-x64 / Win-x64 / macOS-ARM64 -----
    CHECK(a.final_state == kGoldenFinalState);
    CHECK(a.trace_fold == kGoldenTraceFold);

    SESSION_TEST_MAIN_END();
}
