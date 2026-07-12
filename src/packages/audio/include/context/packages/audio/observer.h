// AudioObserver — the PRESENTATION observer that drives the AudioEngine from simulation state
// (M6 P6, R-SIM-001), OFF the deterministic sim path.
//
// It READS entity positions from a `const kernel::World&` (so it structurally CANNOT write sim state),
// sets the engine's listener from a designated listener entity, and triggers/positions spatialized
// voices. It registers NO sim component, writes NO World state, and folds into NO state hash — the
// R-SIM-001 downstream-observer rule (the same shape as the particles cosmetic observer). The one-way
// int->float position conversion feeds the float mixer and is never fed back into the sim.

#pragma once

#include "context/kernel/entity.h"
#include "context/kernel/world.h"
#include "context/packages/audio/audio_engine.h"

#include <cstddef>

namespace context::packages::audio
{

class AudioObserver
{
public:
    // Read every positioned sim entity in `world` (a const archetype walk) — a pure observation pass
    // that records how many carry a position, without touching the World. Returns (and caches) that
    // count. A did-real-work signal for tests; never mutates sim state.
    std::size_t observe(const kernel::World& world);

    // Set the engine listener to the position of `listener` (its session::Position, read from the
    // const `world` and converted to float). Returns kInvalidEntityCode if the entity is dead (nothing
    // read); otherwise sets the listener (origin when the entity carries no position) and returns
    // nullptr.
    [[nodiscard]] const char* update_listener(const kernel::World& world, kernel::Entity listener,
                                              AudioEngine& engine);

    // Trigger `event` at the position of `source` (its session::Position, read from the const `world`
    // and converted to float). Returns kInvalidEntityCode if the entity is dead; otherwise forwards to
    // engine.trigger (which validates the event) and returns its result.
    [[nodiscard]] const char* trigger_at(const kernel::World& world, kernel::Entity source,
                                         const EventDesc& event, AudioEngine& engine);

    // How many positioned sim entities the last observe() saw.
    [[nodiscard]] std::size_t observed_count() const noexcept { return observed_; }

private:
    std::size_t observed_ = 0;
};

} // namespace context::packages::audio
