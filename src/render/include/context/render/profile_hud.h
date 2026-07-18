// The L-47 always-on profiler HUD overlay (a15, R-OBS-002). A lightweight, opt-in overlay that
// renders the current frame's profiling counters (frame time / fps / per-lane cost / worst system /
// GC pause) as a few text rows a render backend draws on top of the scene. It is GPU-FREE: it
// produces the ROWS (text + tint) from a plain model; a backend rasterizes them. So it builds and
// unit-tests under every toolchain, and the real GPU overlay draw is the wgpu backend's job (see
// docs/profiling.md § HUD).
//
// Zero cost when disabled (the L-47 "always-on but cheap" contract): update() early-returns before
// reading the model or touching its buffer when the HUD is off — no allocation, no formatting, no
// work — and set_enabled(false) releases the row buffer. frames_built() proves the build path ran
// only while enabled.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace context::render
{

// One overlay row: text + a coarse tint so a backend can color a breach (e.g. a GC over budget) red
// without parsing the text.
enum class HudTint : std::uint8_t
{
    Normal = 0,
    Warn = 1, // a budget breach / dropped data
};

struct HudLine
{
    std::string text;
    HudTint tint = HudTint::Normal;
};

// The per-frame numbers the HUD renders — a flattened view a caller fills from a
// runtime::profile::ProfileSnapshot (the render module does not depend on the runtime profiler; the
// mapping happens at the call site, keeping render's layering clean).
struct ProfileHudModel
{
    double frame_ms = 0.0;    // total CPU time across the frame's systems
    std::uint64_t tick_hz = 0; // fixed-timestep rate (for the fps line)
    double native_ms = 0.0;
    double script_ms = 0.0;
    double wasm_ms = 0.0;
    std::string worst_system;  // the costliest system this frame (empty = none)
    double worst_ms = 0.0;
    bool gc_available = false;
    double gc_pause_ms = 0.0;  // worst mid-tick GC pause
    bool gc_within_budget = true;
};

class ProfileHud
{
public:
    ProfileHud() = default;

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    // Toggle the HUD. Disabling releases the row buffer so the disabled state holds no rows.
    void set_enabled(bool on);

    // Rebuild the overlay rows from `model` and return them. When DISABLED this returns immediately
    // with the (empty) row list WITHOUT reading `model` — the zero-cost-when-disabled path.
    const std::vector<HudLine>& update(const ProfileHudModel& model);

    [[nodiscard]] const std::vector<HudLine>& lines() const noexcept { return lines_; }
    // How many times the build path actually ran (0 while the HUD has only ever been disabled) —
    // the observable proof of the zero-cost-disabled contract.
    [[nodiscard]] std::uint64_t frames_built() const noexcept { return frames_built_; }

private:
    bool enabled_ = false;
    std::vector<HudLine> lines_;
    std::uint64_t frames_built_ = 0;
};

} // namespace context::render
