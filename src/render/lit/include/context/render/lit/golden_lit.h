// The kernel-backed half of the M4 T7 golden-scene corpus + the R-QA-007 min-spec floor bench loop
// (issue #141). Split from golden.h because both pull the kernel + extract (via LitOffscreen):
//   * lit3d — the representative T1 scene (PBR + directional/point lights + shadow depth pass +
//             lightmap hook, lit_offscreen.h) as a golden corpus scene. Native-only until the web
//             lit proof lands (src/render/web/README.md § follow-ups) — web corpus coverage is the
//             kernel-free pair in golden.h.
//   * bench — the min-spec floor benchmark subject (R-QA-007, measured per R-QA-009): the SAME lit
//             scene rendered frame-after-frame at the floor resolution through LitOffscreen (GPU
//             residency built once, per-frame uniform write + shadow pass + main pass + readback —
//             the R-HEAD-004 offscreen frame loop). Per-frame wall time is reported raw;
//             median-of-N/dispersion policy lives in bench/minspec_floor.py (one methodology home).

#pragma once

#include "context/render/golden.h"
#include "context/render/lit/lit_offscreen.h"
#include "context/render/rhi.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace context::render::lit
{

// Render the lit3d corpus scene (the base-config lit frame) through `device` into `out`. The same
// frame render_offscreen_lit asserts against the CPU PBR reference, so the committed golden is the
// proof's frame by construction. Returns false when the scene cannot be built or read back.
inline bool render_golden_lit3d(IDevice& device, golden::GoldenImage& out)
{
    std::unique_ptr<LitOffscreen> lit = LitOffscreen::create(device);
    if (lit == nullptr)
    {
        return false;
    }
    out.width = lit->width();
    out.height = lit->height();
    return lit->render_frame(LitSceneConfig{}, out.rgba);
}

// One min-spec bench result: raw per-frame samples (milliseconds), plus the subject's shape.
struct LitBenchResult
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t warmup_frames = 0;
    std::vector<double> samples_ms;
};

// Render `frames` timed frames (after `warmup` untimed ones) of the lit scene at `width`x`height`
// through `device`. Each frame is the full offscreen loop: uniform write -> shadow depth pass ->
// PBR main pass -> copy -> mapped readback (the readback is what makes a frame observably complete
// on every backend — the documented bench subject, not display presentation). Returns false when
// the scene cannot be built or a frame fails.
inline bool bench_lit_frames(IDevice& device, std::uint32_t width, std::uint32_t height,
                             std::uint32_t warmup, std::uint32_t frames, LitBenchResult& out)
{
    std::unique_ptr<LitOffscreen> lit = LitOffscreen::create(device, width, height);
    if (lit == nullptr || frames == 0)
    {
        return false;
    }
    out.width = width;
    out.height = height;
    out.warmup_frames = warmup;
    out.samples_ms.clear();
    out.samples_ms.reserve(frames);

    const LitSceneConfig config; // the representative base config: all lights + shadows on
    std::vector<std::uint8_t> frame;
    for (std::uint32_t i = 0; i < warmup + frames; ++i)
    {
        const auto begin = std::chrono::steady_clock::now();
        if (!lit->render_frame(config, frame))
        {
            return false;
        }
        const auto end = std::chrono::steady_clock::now();
        if (i >= warmup)
        {
            const std::chrono::duration<double, std::milli> elapsed = end - begin;
            out.samples_ms.push_back(elapsed.count());
        }
    }
    return true;
}

// Serialize a bench result as the one-line JSON contract bench/minspec_floor.py parses.
inline std::string bench_result_json(const LitBenchResult& result)
{
    std::string json = "{\"subject\":\"lit3d\",\"width\":" + std::to_string(result.width) +
                       ",\"height\":" + std::to_string(result.height) +
                       ",\"warmup_frames\":" + std::to_string(result.warmup_frames) +
                       ",\"samples_ms\":[";
    for (std::size_t i = 0; i < result.samples_ms.size(); ++i)
    {
        if (i > 0)
        {
            json += ",";
        }
        // Fixed microsecond precision keeps the line locale-free and compact.
        const double us = result.samples_ms[i] * 1000.0;
        json += std::to_string(static_cast<long long>(us + 0.5));
    }
    json += "],\"samples_unit\":\"us\"}";
    return json;
}

} // namespace context::render::lit
