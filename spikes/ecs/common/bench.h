// spikes/ecs — timing + statistics helpers (median-of-N with dispersion).

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

namespace spike {

class Timer
{
public:
    Timer() : start_(clock::now()) {}
    void reset() { start_ = clock::now(); }

    double elapsedMs() const
    {
        return std::chrono::duration<double, std::milli>(clock::now() - start_).count();
    }

private:
    using clock = std::chrono::steady_clock;
    clock::time_point start_;
};

struct Stats
{
    double median = 0.0;
    double min = 0.0;
    double max = 0.0;
};

inline Stats computeStats(std::vector<double> samples)
{
    Stats s;
    if (samples.empty()) return s;
    std::sort(samples.begin(), samples.end());
    const std::size_t n = samples.size();
    s.min = samples.front();
    s.max = samples.back();
    s.median = (n % 2 == 1) ? samples[n / 2]
                            : 0.5 * (samples[n / 2 - 1] + samples[n / 2]);
    return s;
}

// One machine-greppable result line per metric: "[spike-ecs] impl=<i> metric=<m> ..."
inline void printMetric(const char* impl, const char* metric, const Stats& s, const char* unit)
{
    std::printf("[spike-ecs] impl=%s metric=%s median=%.3f min=%.3f max=%.3f unit=%s\n",
                impl, metric, s.median, s.min, s.max, unit);
}

} // namespace spike
