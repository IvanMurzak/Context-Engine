// AudioEngine unit tests (M6 P6, R-SYS-006 / L-46): the miniaudio null-backend device lifecycle + the
// float mixer. Coverage (R-QA-013 happy path, edge cases, AND failure paths):
//   - init on the NULL backend (never hardware);
//   - bus-graph validation (empty / duplicate id / out-of-range parent / cycle / negative gain refused);
//   - event validation (negative gain / inverted spatial range / out-of-range bus / non-positive life);
//   - mixing energy: per-bus resolved gain scales output, spatial distance attenuates it;
//   - R-SYS-006 frame-rate independence: the same voice mixed as one block vs many blocks over the same
//     wall-clock duration sounds the same total energy and culls at the same time;
//   - the device thread runs (frames_mixed advances) and stops cleanly.

#include "context/packages/audio/audio_engine.h"
#include "context/packages/audio/errors.h"

#include "audio_test.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

namespace audio = context::packages::audio;

namespace
{
double energy(const std::vector<float>& buf)
{
    double e = 0.0;
    for (const float s : buf)
        e += static_cast<double>(s) * static_cast<double>(s);
    return e;
}

// Error codes are compared by CONTENT (the catalog identity), not pointer — the returned `const char*`
// and the package's k*Code constant need not share an address across translation units (the sibling
// packages' same_code helper).
[[nodiscard]] bool same_code(const char* got, const char* expected)
{
    return got != nullptr && std::strcmp(got, expected) == 0;
}
} // namespace

int main()
{
    // --- init on the null backend ----------------------------------------------------------------
    {
        audio::AudioEngine e;
        CHECK(e.init() == nullptr);
        CHECK(e.sample_rate() > 0);
    }

    // --- bus-graph validation (fail-closed) ------------------------------------------------------
    {
        audio::AudioEngine e;
        CHECK(e.init() == nullptr);
        CHECK(same_code(e.configure_buses({}), audio::kInvalidBusCode));           // empty
        CHECK(same_code(e.configure_buses({{"a", 1.0F, -1}, {"a", 1.0F, -1}}),
                        audio::kInvalidBusCode));                                 // duplicate id
        CHECK(same_code(e.configure_buses({{"a", 1.0F, 5}}), audio::kInvalidBusCode)); // parent OOB
        CHECK(same_code(e.configure_buses({{"a", 1.0F, 1}, {"b", 1.0F, 0}}),
                        audio::kInvalidBusCode));                                 // a<->b cycle
        CHECK(same_code(e.configure_buses({{"a", 1.0F, 0}}), audio::kInvalidBusCode)); // self-parent
        CHECK(same_code(e.configure_buses({{"a", -1.0F, -1}}), audio::kInvalidBusCode)); // negative gain
        // A valid nested graph resolves.
        CHECK(e.configure_buses({{"master", 1.0F, -1}, {"sfx", 0.5F, 0}, {"music", 0.7F, 0}}) ==
              nullptr);
    }

    // --- event validation (fail-closed) ----------------------------------------------------------
    {
        audio::AudioEngine e;
        CHECK(e.init() == nullptr);
        CHECK(e.configure_buses({{"master", 1.0F, -1}}) == nullptr);

        audio::EventDesc ev;
        ev.bus = 0;
        ev.gain = 1.0F;
        CHECK(e.trigger(ev, {}) == nullptr);
        CHECK(e.active_voice_count() == 1);

        audio::EventDesc neg = ev;
        neg.gain = -0.1F;
        CHECK(same_code(e.trigger(neg, {}), audio::kInvalidEventCode));

        audio::EventDesc oob = ev;
        oob.bus = 9;
        CHECK(same_code(e.trigger(oob, {}), audio::kInvalidEventCode));

        audio::EventDesc inverted = ev;
        inverted.spatial = true;
        inverted.min_distance = 10.0F;
        inverted.max_distance = 2.0F;
        CHECK(same_code(e.trigger(inverted, {}), audio::kInvalidEventCode));

        audio::EventDesc dead = ev;
        dead.life_seconds = 0.0F;
        CHECK(same_code(e.trigger(dead, {}), audio::kInvalidEventCode));

        CHECK(e.active_voice_count() == 1); // no rejected event added a voice
    }

    // --- mixing energy: a lower bus gain yields less output ---------------------------------------
    {
        const auto energy_for = [](float bus_gain) {
            audio::AudioEngine e;
            CHECK(e.init() == nullptr);
            CHECK(e.configure_buses({{"m", bus_gain, -1}}) == nullptr);
            audio::EventDesc ev;
            ev.bus = 0;
            ev.gain = 1.0F;
            ev.life_seconds = 1.0F;
            CHECK(e.trigger(ev, {}) == nullptr);
            std::vector<float> buf;
            e.render_for_test(buf, 4800, 48000); // 0.1 s
            return energy(buf);
        };
        const double full = energy_for(1.0F);
        const double quarter = energy_for(0.25F);
        CHECK(full > 0.0);
        CHECK(quarter > 0.0);
        CHECK(quarter < full); // gain^2 scaling
    }

    // --- spatial attenuation: nearer is louder ---------------------------------------------------
    {
        const auto energy_at = [](float dist) {
            audio::AudioEngine e;
            CHECK(e.init() == nullptr);
            CHECK(e.configure_buses({{"m", 1.0F, -1}}) == nullptr);
            e.set_listener({0.0F, 0.0F, 0.0F});
            audio::EventDesc ev;
            ev.bus = 0;
            ev.gain = 1.0F;
            ev.spatial = true;
            ev.min_distance = 1.0F;
            ev.max_distance = 10.0F;
            ev.life_seconds = 1.0F;
            CHECK(e.trigger(ev, {dist, 0.0F, 0.0F}) == nullptr);
            std::vector<float> buf;
            e.render_for_test(buf, 4800, 48000);
            return energy(buf);
        };
        const double near = energy_at(1.0F);  // at min distance => full
        const double mid = energy_at(5.0F);   // partial attenuation
        const double far = energy_at(10.0F);  // at max distance => silent
        CHECK(near > mid);
        CHECK(mid > far);
    }

    // --- R-SYS-006 frame-rate independence -------------------------------------------------------
    // The same voice (0.3 s life) rendered over the SAME 0.5 s wall-clock duration sounds the same
    // total energy and culls, whether mixed as one big block or many small ones.
    {
        const auto run = [](std::uint32_t block_frames, int blocks) {
            audio::AudioEngine e;
            CHECK(e.init() == nullptr);
            CHECK(e.configure_buses({{"m", 1.0F, -1}}) == nullptr);
            audio::EventDesc ev;
            ev.bus = 0;
            ev.gain = 1.0F;
            ev.life_seconds = 0.3F;
            CHECK(e.trigger(ev, {}) == nullptr);
            double e_total = 0.0;
            std::vector<float> buf;
            for (int i = 0; i < blocks; ++i)
            {
                e.render_for_test(buf, block_frames, 48000);
                e_total += energy(buf);
            }
            return std::pair<double, std::size_t>{e_total, e.active_voice_count()};
        };
        const auto one_block = run(24000, 1);  // 0.5 s in one block
        const auto many_blocks = run(2400, 10); // 0.5 s in ten blocks
        CHECK(one_block.second == 0);  // the 0.3 s voice culled after 0.5 s in both schedules
        CHECK(many_blocks.second == 0);
        CHECK(one_block.first > 0.0);
        const double rel =
            std::abs(one_block.first - many_blocks.first) / one_block.first;
        CHECK(rel < 0.02); // same sounded duration => matching energy regardless of block size
    }

    // --- the device thread runs on the null backend + stops cleanly ------------------------------
    {
        audio::AudioEngine e;
        CHECK(e.init() == nullptr);
        CHECK(e.configure_buses({{"m", 1.0F, -1}}) == nullptr);
        CHECK(e.start() == nullptr);
        // Spin (bounded) until the audio thread has mixed at least one buffer.
        bool ran = false;
        for (int i = 0; i < 1000 && !ran; ++i)
        {
            if (e.frames_mixed() > 0)
                ran = true;
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(ran); // miniaudio's own low-latency thread fired the callback
        e.stop();
        const std::uint64_t after_stop = e.frames_mixed();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        CHECK(e.frames_mixed() == after_stop); // no callbacks after stop()
    }

    AUDIO_TEST_MAIN_END();
}
