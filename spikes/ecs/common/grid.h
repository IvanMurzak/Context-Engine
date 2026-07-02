// spikes/ecs — shared uniform spatial grid (implementation-neutral).
//
// Rebuilt every frame from ECS position data; stores (x, y, opaque-handle) entries so the
// distance test itself is ECS-independent and only actual HITS pay a per-ECS random-access
// component lookup. Cell size == kAoeRadius, so a circle query touches at most 3x3 cells.
// No wraparound in queries (events near the border simply see fewer cells) — consistent
// across implementations, which is all the comparison needs.

#pragma once

#include "workload.h"

#include <cstdint>
#include <vector>

namespace spike {

class Grid
{
public:
    static constexpr float kCell = kAoeRadius;
    static constexpr int kDim = static_cast<int>(kWorldSize / kCell); // 128

    struct Entry { float x, y; std::uint64_t handle; };

    Grid() : cells_(static_cast<std::size_t>(kDim) * kDim) {}

    void clear()
    {
        for (auto& c : cells_) c.clear(); // keeps capacity — steady-state rebuild cost
    }

    void insert(float x, float y, std::uint64_t handle)
    {
        cells_[cellIndex(x, y)].push_back(Entry{x, y, handle});
    }

    // Calls f(handle) for every entry within `radius` of (cx, cy).
    template <typename F>
    void queryCircle(float cx, float cy, float radius, F&& f) const
    {
        const float r2 = radius * radius;
        const int x0 = clampCell(static_cast<int>((cx - radius) / kCell));
        const int x1 = clampCell(static_cast<int>((cx + radius) / kCell));
        const int y0 = clampCell(static_cast<int>((cy - radius) / kCell));
        const int y1 = clampCell(static_cast<int>((cy + radius) / kCell));
        for (int gy = y0; gy <= y1; ++gy)
        {
            for (int gx = x0; gx <= x1; ++gx)
            {
                const auto& cell = cells_[static_cast<std::size_t>(gy) * kDim + gx];
                for (const Entry& e : cell)
                {
                    const float dx = e.x - cx;
                    const float dy = e.y - cy;
                    if (dx * dx + dy * dy <= r2) f(e.handle);
                }
            }
        }
    }

private:
    static int clampCell(int c)
    {
        if (c < 0) return 0;
        if (c >= kDim) return kDim - 1;
        return c;
    }

    static std::size_t cellIndex(float x, float y)
    {
        const int gx = clampCell(static_cast<int>(x / kCell));
        const int gy = clampCell(static_cast<int>(y / kCell));
        return static_cast<std::size_t>(gy) * kDim + gx;
    }

    std::vector<std::vector<Entry>> cells_;
};

} // namespace spike
