// AudioEngine implementation (audio_engine.h) — the miniaudio null-backend device + float mixer.
//
// This is the ONLY place the vendored miniaudio device API is used (PIMPL). miniaudio's own audio
// thread drives data_callback; the mixer is FLOAT and self-contained here, so nothing it does touches
// the kernel World, a sim component, or the L-54 state hash (R-SIM-001 — audio is a presentation
// observer). All shared mixer state is guarded by a mutex; the audio callback try-locks (a contended
// buffer is dropped to silence rather than blocking the audio thread), the control path locks fully.

#include "context/packages/audio/audio_engine.h"

#include "context/packages/audio/errors.h"

#include <miniaudio.h> // vendored single header (declarations only; the impl TU is miniaudio_impl.cpp)

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <vector>

namespace context::packages::audio
{

namespace
{
constexpr float kPi = 3.14159265358979323846F;
constexpr float kToneHz = 440.0F; // a fixed carrier so mixed output is audible/non-vacuous

// One playing voice — pure presentation float state, never in the World, never hashed.
struct Voice
{
    float gain = 1.0F;      // event gain * resolved bus gain
    bool spatial = false;
    float min_distance = 0.0F;
    float max_distance = 0.0F;
    Vec3 position;
    float phase = 0.0F;     // carrier phase, advanced per frame
    float remaining = 0.0F; // seconds of envelope left; <= 0 => culled
};

// Distance attenuation in [0, 1] for a spatial voice; 1 for a non-spatial one.
[[nodiscard]] float attenuation(const Voice& v, const Vec3& listener)
{
    if (!v.spatial)
        return 1.0F;
    const float dx = v.position.x - listener.x;
    const float dy = v.position.y - listener.y;
    const float dz = v.position.z - listener.z;
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (d <= v.min_distance)
        return 1.0F;
    if (d >= v.max_distance)
        return 0.0F;
    const float span = v.max_distance - v.min_distance;
    return span > 0.0F ? (v.max_distance - d) / span : 0.0F;
}
} // namespace

struct AudioEngine::Impl
{
    ma_context context{};
    ma_device device{};
    bool context_inited = false;
    bool device_inited = false;
    bool started = false;
    std::uint32_t sample_rate = 48000;

    mutable std::mutex mtx;
    std::vector<Voice> voices;   // guarded by mtx
    Vec3 listener;               // guarded by mtx
    std::vector<float> bus_gain; // resolved absolute (root-to-leaf product) gain per bus, guarded by mtx

    std::atomic<std::uint64_t> frames_mixed{0};

    // Mix `frames` stereo frames into `out` (interleaved L/R, additive over a zeroed buffer) at
    // `sr` Hz, advancing envelopes by frames/sr and culling finished voices. Caller holds mtx.
    void mix_locked(float* out, std::uint32_t frames, std::uint32_t sr)
    {
        if (frames == 0 || sr == 0)
            return;
        const float dt_block = static_cast<float>(frames) / static_cast<float>(sr);
        const float phase_inc = 2.0F * kPi * kToneHz / static_cast<float>(sr);
        for (Voice& v : voices)
        {
            const float atten = attenuation(v, listener) * v.gain;
            // Equal-power stereo pan from the voice's x-offset relative to the listener.
            const float ref = v.spatial && v.max_distance > 0.0F ? v.max_distance : 1.0F;
            float pan = (v.position.x - listener.x) / ref;
            pan = std::clamp(pan, -1.0F, 1.0F);
            const float angle = (pan + 1.0F) * (kPi / 4.0F); // [0, pi/2]
            const float left = std::cos(angle);
            const float right = std::sin(angle);
            // Envelope-limit the voice WITHIN this block: it sounds only for the frames left in its
            // remaining life, then is silent (and culled below). This makes the total sounded duration
            // depend on the voice's life alone, NOT the caller's block size — the R-SYS-006
            // frame-rate-independence property (a big block cannot over-play a short voice).
            const long long life_frames =
                std::max<long long>(0, std::llround(static_cast<double>(v.remaining) * sr));
            const auto sounded =
                static_cast<std::uint32_t>(std::min<long long>(life_frames, frames));
            for (std::uint32_t f = 0; f < sounded; ++f)
            {
                const float s = std::sin(v.phase) * atten;
                out[f * 2] += s * left;
                out[f * 2 + 1] += s * right;
                v.phase += phase_inc;
                if (v.phase >= 2.0F * kPi)
                    v.phase -= 2.0F * kPi;
            }
            v.remaining -= dt_block;
        }
        voices.erase(std::remove_if(voices.begin(), voices.end(),
                                    [](const Voice& v) { return v.remaining <= 0.0F; }),
                     voices.end());
    }

    static void data_callback(ma_device* device, void* output, const void* input, ma_uint32 frameCount)
    {
        (void)input;
        auto* self = static_cast<Impl*>(device->pUserData);
        auto* out = static_cast<float*>(output);
        std::fill(out, out + static_cast<std::size_t>(frameCount) * 2, 0.0F);
        // try_lock: never block the audio thread — a contended buffer stays silence (a dropped frame
        // is acceptable for this null-backend foundation; a lock-free ring buffer is a hardening
        // follow-up). The control path (trigger/set/configure) holds the lock only briefly.
        std::unique_lock<std::mutex> lock(self->mtx, std::try_to_lock);
        if (lock.owns_lock())
            self->mix_locked(out, frameCount, self->sample_rate);
        self->frames_mixed.fetch_add(frameCount, std::memory_order_relaxed);
    }
};

AudioEngine::AudioEngine() : impl_(std::make_unique<Impl>()) {}

AudioEngine::~AudioEngine()
{
    stop();
    if (impl_->device_inited)
        ma_device_uninit(&impl_->device);
    if (impl_->context_inited)
        ma_context_uninit(&impl_->context);
}

const char* AudioEngine::init()
{
    // Force the NULL backend explicitly: no hardware device is ever opened (CI has none; locally the
    // tests must not touch the real device). The null device still runs miniaudio's own audio thread.
    ma_backend backends[1] = {ma_backend_null};
    ma_context_config context_config = ma_context_config_init();
    if (ma_context_init(backends, 1, &context_config, &impl_->context) != MA_SUCCESS)
        return kDeviceUnavailableCode;
    impl_->context_inited = true;

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = 2;
    config.sampleRate = impl_->sample_rate;
    config.dataCallback = &Impl::data_callback;
    config.pUserData = impl_.get();
    if (ma_device_init(&impl_->context, &config, &impl_->device) != MA_SUCCESS)
        return kDeviceUnavailableCode;
    impl_->device_inited = true;
    // Reflect the device's actual sample rate (the null backend honors the request, but read it back
    // so the mixer and any frame-rate math track the real device rate).
    if (impl_->device.sampleRate != 0)
        impl_->sample_rate = impl_->device.sampleRate;
    return nullptr;
}

const char* AudioEngine::configure_buses(const std::vector<BusConfig>& buses)
{
    if (buses.empty())
        return kInvalidBusCode;
    const int n = static_cast<int>(buses.size());
    for (int i = 0; i < n; ++i)
    {
        if (buses[static_cast<std::size_t>(i)].gain < 0.0F)
            return kInvalidBusCode;
        // Unique ids.
        for (int j = i + 1; j < n; ++j)
            if (buses[static_cast<std::size_t>(i)].id == buses[static_cast<std::size_t>(j)].id)
                return kInvalidBusCode;
        // Parent in range (or -1 = master).
        const int parent = buses[static_cast<std::size_t>(i)].parent;
        if (parent < -1 || parent >= n)
            return kInvalidBusCode;
    }
    // Acyclic + resolve absolute (root-to-leaf product) gains.
    std::vector<float> resolved(buses.size(), 0.0F);
    for (int i = 0; i < n; ++i)
    {
        float gain = 1.0F;
        int cursor = i;
        for (int hops = 0; hops <= n; ++hops)
        {
            gain *= buses[static_cast<std::size_t>(cursor)].gain;
            const int parent = buses[static_cast<std::size_t>(cursor)].parent;
            if (parent == -1)
            {
                resolved[static_cast<std::size_t>(i)] = gain;
                break;
            }
            if (parent == i && hops > 0)
                return kInvalidBusCode; // returned to the start => a cycle
            cursor = parent;
            if (hops == n)
                return kInvalidBusCode; // chain longer than the bus count => a cycle
        }
    }
    std::lock_guard<std::mutex> lock(impl_->mtx);
    impl_->bus_gain = std::move(resolved);
    return nullptr;
}

const char* AudioEngine::trigger(const EventDesc& event, const Vec3& position)
{
    if (event.gain < 0.0F)
        return kInvalidEventCode;
    if (event.spatial &&
        (event.min_distance < 0.0F || event.max_distance <= event.min_distance))
        return kInvalidEventCode;
    if (event.life_seconds <= 0.0F)
        return kInvalidEventCode;

    std::lock_guard<std::mutex> lock(impl_->mtx);
    if (event.bus < 0 || event.bus >= static_cast<int>(impl_->bus_gain.size()))
        return kInvalidEventCode;
    Voice v;
    v.gain = event.gain * impl_->bus_gain[static_cast<std::size_t>(event.bus)];
    v.spatial = event.spatial;
    v.min_distance = event.min_distance;
    v.max_distance = event.max_distance;
    v.position = position;
    v.remaining = event.life_seconds;
    impl_->voices.push_back(v);
    return nullptr;
}

void AudioEngine::set_listener(const Vec3& position) noexcept
{
    std::lock_guard<std::mutex> lock(impl_->mtx);
    impl_->listener = position;
}

const char* AudioEngine::start()
{
    if (!impl_->device_inited)
        return kDeviceUnavailableCode;
    if (impl_->started)
        return nullptr;
    if (ma_device_start(&impl_->device) != MA_SUCCESS)
        return kDeviceUnavailableCode;
    impl_->started = true;
    return nullptr;
}

void AudioEngine::stop() noexcept
{
    if (impl_->started)
    {
        ma_device_stop(&impl_->device);
        impl_->started = false;
    }
}

void AudioEngine::render_for_test(std::vector<float>& out, std::uint32_t frames,
                                  std::uint32_t sample_rate)
{
    out.assign(static_cast<std::size_t>(frames) * 2, 0.0F);
    std::lock_guard<std::mutex> lock(impl_->mtx);
    impl_->mix_locked(out.data(), frames, sample_rate);
}

std::size_t AudioEngine::active_voice_count() const noexcept
{
    std::lock_guard<std::mutex> lock(impl_->mtx);
    return impl_->voices.size();
}

std::uint64_t AudioEngine::frames_mixed() const noexcept
{
    return impl_->frames_mixed.load(std::memory_order_relaxed);
}

std::uint32_t AudioEngine::sample_rate() const noexcept
{
    return impl_->sample_rate;
}

} // namespace context::packages::audio
