// AudioEngine — the miniaudio-backed audio device + mixer (M6 P6, R-SYS-006 / L-46) as a PRESENTATION
// subsystem, OFF the deterministic sim path (R-SIM-001).
//
// The engine owns a miniaudio device on the NULL backend (CI runners have no audio hardware, and even
// locally the tests never open a hardware device) that runs miniaudio's OWN low-latency audio thread.
// The device callback mixes the active voices — distance attenuation + equal-power stereo pan +
// per-bus gain — advancing each voice's envelope in WALL-CLOCK time (frames / sampleRate), so the mix
// is frame-rate INDEPENDENT of the sim tick rate (R-SYS-006). All mixer state is FLOAT and lives here,
// never in the kernel World and never in a sim component, so nothing this engine does can perturb the
// L-54 hierarchical state hash or taint the deterministic build.
//
// miniaudio is a private implementation detail (PIMPL) — it does not leak into this header, so no
// consumer needs the vendored include dir. Not copyable (it owns an OS device + audio thread).

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace context::packages::audio
{

// A 3D point in presentation space (floats, SI meters) — off the sim path.
struct Vec3
{
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};

// One mixing bus in the runtime bus graph: a linear gain and an optional parent (an index into the
// configure_buses() vector, or -1 for a top-level/master bus). The runtime analogue of one entry in an
// authored ctx:audio-bus graph.
struct BusConfig
{
    std::string id;
    float gain = 1.0F;
    int parent = -1; // index into the configure_buses() vector; -1 = master (top-level)
};

// The parameters of a triggered sound — the runtime analogue of a ctx:audio-event. A non-spatial
// (2D/UI) event sets spatial=false and plays at full `gain`; a spatial event is attenuated by the
// listener distance across [min_distance, max_distance] (SI meters).
struct EventDesc
{
    int bus = 0;             // index into the configured bus vector
    float gain = 1.0F;       // linear gain (>= 0)
    bool spatial = false;    // whether to attenuate by listener distance
    float min_distance = 0.0F;
    float max_distance = 0.0F;
    float life_seconds = 0.25F; // envelope length; the voice culls after this wall-clock duration
};

class AudioEngine
{
public:
    AudioEngine();
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // Initialize the miniaudio context + device on the NULL backend (never hardware). Returns nullptr
    // on success, kDeviceUnavailableCode on failure (audio disabled; the caller's sim is unaffected).
    [[nodiscard]] const char* init();

    // Install the mixing-bus graph. Validates: non-empty, unique ids, every parent in range, the
    // parent graph acyclic, and non-negative gains. Returns nullptr on success, kInvalidBusCode on
    // rejection (the previous graph is left untouched — fail-closed).
    [[nodiscard]] const char* configure_buses(const std::vector<BusConfig>& buses);

    // Trigger a sound at `position`. Validates the event (bus in range, non-negative gain, and — when
    // spatial — a consistent min<max attenuation range). Returns nullptr and adds a voice on success,
    // kInvalidEventCode on rejection (nothing triggered — fail-closed).
    [[nodiscard]] const char* trigger(const EventDesc& event, const Vec3& position);

    // Set the listener position (presentation only — off the sim path).
    void set_listener(const Vec3& position) noexcept;

    // Start / stop miniaudio's audio thread on the null backend. start() returns nullptr on success,
    // kDeviceUnavailableCode if the device is not initialized or fails to start.
    [[nodiscard]] const char* start();
    void stop() noexcept;

    // Deterministic, single-threaded render of `frames` stereo frames at `sample_rate` into `out`
    // (resized to frames*2, interleaved L/R). This is the SAME mixer the device callback runs, exposed
    // so the mixing + spatialization can be unit-tested WITHOUT the device thread and so frame-rate
    // independence (R-SYS-006) is testable across block sizes. Advances voice envelopes by
    // frames / sample_rate and culls finished voices.
    void render_for_test(std::vector<float>& out, std::uint32_t frames, std::uint32_t sample_rate);

    // Number of live voices (mutex-guarded snapshot).
    [[nodiscard]] std::size_t active_voice_count() const noexcept;

    // Total frames the device thread has mixed since start() (atomic; a liveness signal for tests).
    [[nodiscard]] std::uint64_t frames_mixed() const noexcept;

    // The device sample rate this engine mixes at (Hz).
    [[nodiscard]] std::uint32_t sample_rate() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace context::packages::audio
