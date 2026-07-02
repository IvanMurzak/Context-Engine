// spikes/ecs — shared workload contract.
//
// The SAME toy simulation is implemented three ways (EnTT, flecs, custom archetype/SoA
// sketch). Everything order-sensitive lives HERE so all three implementations execute
// bit-identical per-entity math and converge on the same end-of-run checksum regardless
// of each ECS's internal iteration order.
//
// Simulation (per frame, phases run in this exact order):
//   1. move   — pos += vel * dt, wrap into [0, kWorldSize)          (R: Velocity, W: Position)
//   2. grid   — rebuild the uniform spatial grid from positions      (R: Position)
//   3. aoe    — kAoePerFrame area-damage events query the grid;      (R: grid, W: Health)
//               hits take kAoeDamage immediately and enqueue a
//               deferred "burn" request (structural add — deferred)
//   4. burn   — entities WITH Burning: hp -= kBurnDamage, ticks--;   (W: Health, Burning)
//               ticks==0 enqueues a deferred Burning remove
//   5. kill   — entities with hp <= 0 enqueue a deferred destroy;    (R: Health, LogicalId)
//               churn: logical ids [f*kChurn, (f+1)*kChurn) enqueue
//               a deferred destroy (entity add/remove churn)
//   6. flush  — apply deferred ops in this exact order:
//               (a) churn destroys (skip already-dead),
//               (b) kill-list destroys (skip already-dead),
//               (c) burn requests: skip dead; refresh ticks if already Burning,
//                   else add Burning{kBurnTicks},
//               (d) burn removes: remove Burning only if ticks is STILL 0 (an entity
//                   re-burned in (c) had its ticks refreshed and stays burning),
//               (e) spawn kChurn new entities (fresh sequential logical ids).
//
// Per-entity operations are order-independent across entities (move is per-entity pure;
// all damage constants are exactly-representable floats, so any order of subtraction is
// bit-identical), which is what makes the cross-implementation checksum meaningful.
//
// Structural changes are ALWAYS deferred to phase 6 — mirroring the R-LANG-009 lock
// (command buffers at end-of-system; memory never moves under a live view).

#pragma once

#include <bit>
#include <cstdint>

namespace spike {

// ---- workload constants ------------------------------------------------------------------
inline constexpr std::uint32_t kInitialEntities = 1'000'000;
inline constexpr int kFrames = 32;
inline constexpr std::uint32_t kChurnPerFrame = 10'000; // destroys AND spawns per frame
inline constexpr float kWorldSize = 1024.0f;            // positions live in [0, kWorldSize)^2
inline constexpr float kDt = 1.0f / 60.0f;
inline constexpr int kAoePerFrame = 256;
inline constexpr float kAoeRadius = 8.0f;
inline constexpr float kAoeDamage = 10.0f;
inline constexpr float kBurnDamage = 1.0f;
inline constexpr int kBurnTicks = 3;
inline constexpr float kInitialHp = 100.0f;

inline constexpr int kSimRuns = 5;        // median-of-5 full simulation runs
inline constexpr int kMicroIterReps = 15; // 1M-iteration microbench repetitions
inline constexpr int kMicroIterWarmup = 3;
inline constexpr int kRandomAccessReps = 5;
inline constexpr std::uint32_t kCreateDestroyCount = 100'000;

// ---- component payloads (plain data; identical layout in all three impls) ---------------
struct Position { float x, y; };
struct Velocity { float x, y; };
struct Health   { float hp; };
struct Burning  { int ticks; };
struct LogicalId{ std::uint32_t id; };

// ---- deterministic PRNG ------------------------------------------------------------------
constexpr std::uint64_t splitmix64(std::uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// Top 24 bits -> [0, 1) float; 24-bit mantissa-safe so the mapping is exact.
constexpr float unitFloat(std::uint64_t h)
{
    return static_cast<float>(h >> 40) * (1.0f / 16777216.0f);
}

// ---- deterministic spawn state -----------------------------------------------------------
struct SpawnState { Position pos; Velocity vel; Health hp; };

inline SpawnState spawnStateFor(std::uint32_t id)
{
    const std::uint64_t base = static_cast<std::uint64_t>(id) * 4u;
    SpawnState s;
    s.pos.x = unitFloat(splitmix64(base + 0)) * kWorldSize;
    s.pos.y = unitFloat(splitmix64(base + 1)) * kWorldSize;
    s.vel.x = (unitFloat(splitmix64(base + 2)) - 0.5f) * 64.0f; // [-32, 32)
    s.vel.y = (unitFloat(splitmix64(base + 3)) - 0.5f) * 64.0f;
    s.hp.hp = kInitialHp;
    return s;
}

// ---- deterministic AoE event positions ---------------------------------------------------
inline void aoeCenterFor(int frame, int event, float& cx, float& cy)
{
    const std::uint64_t base = 0xA0E00000ull + static_cast<std::uint64_t>(frame) * 2048u
                             + static_cast<std::uint64_t>(event) * 2u;
    cx = unitFloat(splitmix64(base + 0)) * kWorldSize;
    cy = unitFloat(splitmix64(base + 1)) * kWorldSize;
}

// ---- the one true move step (identical code in all three impls) --------------------------
inline void moveOne(float& x, float& y, float vx, float vy)
{
    x += vx * kDt;
    y += vy * kDt;
    if (x < 0.0f) x += kWorldSize; else if (x >= kWorldSize) x -= kWorldSize;
    if (y < 0.0f) y += kWorldSize; else if (y >= kWorldSize) y -= kWorldSize;
}

// ---- order-independent world checksum ----------------------------------------------------
// Combines per-entity hashes with + and ^ so ECS iteration order cannot affect the result.
struct Checksum
{
    std::uint64_t sum = 0;
    std::uint64_t xr = 0;
    std::uint64_t count = 0;

    void add(std::uint32_t id, float x, float y, float hp, std::int32_t burnTicks)
    {
        std::uint64_t h = splitmix64((static_cast<std::uint64_t>(id) << 32)
                                     ^ std::bit_cast<std::uint32_t>(x));
        h = splitmix64(h ^ std::bit_cast<std::uint32_t>(y));
        h = splitmix64(h ^ std::bit_cast<std::uint32_t>(hp));
        h = splitmix64(h ^ static_cast<std::uint32_t>(burnTicks));
        sum += h;
        xr ^= h;
        ++count;
    }

    bool operator==(const Checksum&) const = default;
};

} // namespace spike
