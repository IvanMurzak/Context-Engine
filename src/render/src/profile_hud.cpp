// The L-47 profiler HUD overlay implementation (see profile_hud.h).

#include "context/render/profile_hud.h"

#include <iomanip>
#include <sstream>
#include <string>

namespace context::render
{
namespace
{
// Fixed-point millisecond text in an isolated stream (never mutates a caller's stream flags). A
// plain double, not a std::chrono rep, so std::to_string would be fine — ostringstream is used only
// for the precision control.
std::string ms(double v)
{
    std::ostringstream s;
    s << std::fixed << std::setprecision(2) << v;
    return s.str();
}
} // namespace

void ProfileHud::set_enabled(bool on)
{
    enabled_ = on;
    if (!on)
        lines_.clear(); // the disabled state holds no rows
}

const std::vector<HudLine>& ProfileHud::update(const ProfileHudModel& model)
{
    if (!enabled_)
        return lines_; // zero-cost: model untouched, no allocation, no counter bump

    lines_.clear();

    // Frame time + derived fps (fps only when the model carries a tick rate).
    {
        std::ostringstream row;
        row << "frame " << ms(model.frame_ms) << " ms";
        if (model.tick_hz > 0)
            row << "  (" << model.tick_hz << " Hz)";
        lines_.push_back(HudLine{row.str(), HudTint::Normal});
    }

    // Per-lane cost — only lanes that ran (a zero-cost lane is omitted, not shown as 0).
    {
        std::ostringstream row;
        bool any = false;
        if (model.native_ms > 0.0)
        {
            row << "native " << ms(model.native_ms) << " ms";
            any = true;
        }
        if (model.script_ms > 0.0)
        {
            if (any)
                row << " | ";
            row << "script " << ms(model.script_ms) << " ms";
            any = true;
        }
        if (model.wasm_ms > 0.0)
        {
            if (any)
                row << " | ";
            row << "wasm " << ms(model.wasm_ms) << " ms";
            any = true;
        }
        if (any)
            lines_.push_back(HudLine{row.str(), HudTint::Normal});
    }

    // The costliest system this frame.
    if (!model.worst_system.empty())
        lines_.push_back(
            HudLine{"worst " + model.worst_system + " " + ms(model.worst_ms) + " ms",
                    HudTint::Normal});

    // GC-pause line — warn-tinted on a budget breach so the overlay flags it without text parsing.
    if (model.gc_available)
    {
        const HudTint tint = model.gc_within_budget ? HudTint::Normal : HudTint::Warn;
        std::string text = "gc " + ms(model.gc_pause_ms) + " ms";
        text += model.gc_within_budget ? " (ok)" : " (OVER budget)";
        lines_.push_back(HudLine{text, tint});
    }
    else
    {
        lines_.push_back(HudLine{"gc n/a", HudTint::Normal});
    }

    ++frames_built_;
    return lines_;
}

} // namespace context::render
