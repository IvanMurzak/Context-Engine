// The M4 T7 golden-scene corpus (ROADMAP §1 M4 exit; R-REND-002 T1 semantics; issue #141): render
// the committed corpus scenes offscreen through ANY rhi.h device — the native wgpu backend, the
// browser's WebGPU via emscripten, or the fake test backend — and hand back the raw pixels for the
// SSIM visual-equivalence gate (tools/golden_compare.py vs the baselines under goldens/).
//
// KERNEL-FREE by design (like web_main.cpp): this header covers the two kernel-free corpus scenes
//   * triangle3d — the 3D clip-space render pipeline (offscreen_scene.h's reference triangle);
//   * sprite2d   — the R-2D-001 ortho sprite path (sprite_offscreen.h's reference scene);
// so the web build renders the same corpus with no kernel/extract surface. The kernel-backed lit
// scene (lit3d) is the sibling golden_lit.h (native-only until the web lit proof lands). Each scene
// render is THE proof's factored render path (render_offscreen_triangle_pixels /
// render_sprite_scene_pixels), so a committed golden is the proof's frame by construction.
//
// Consumers need the sprite package on the include path (native: link context_render_sprite; web:
// the sprite include dir — both already true for every harness that includes this header).

#pragma once

#include "context/render/offscreen_scene.h"
#include "context/render/rhi.h"
#include "context/render/sprite/sprite_offscreen.h"

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace context::render::golden
{

// One rendered corpus frame: tight row-major RGBA8, rows top-first.
struct GoldenImage
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
};

// The kernel-free corpus scene ids this header renders (golden_lit.h adds "lit3d").
inline const std::vector<std::string>& kernel_free_scene_ids()
{
    static const std::vector<std::string> ids{"triangle3d", "sprite2d"};
    return ids;
}

// Render corpus scene `scene` through `device` into `out`. Returns false on an unknown scene id or
// a failed readback (diagnostic on stderr via the underlying proof path). `device` must be a live
// (GPU or fake) device; adapter presence / headless SKIP is the caller's concern.
inline bool render_golden_scene(IDevice& device, const std::string& scene, GoldenImage& out)
{
    if (scene == "triangle3d")
    {
        out.width = offscreen_triangle_size();
        out.height = offscreen_triangle_size();
        return render_offscreen_triangle_pixels(device, out.rgba);
    }
    if (scene == "sprite2d")
    {
        out.width = sprite::sprite_target_size();
        out.height = sprite::sprite_target_size();
        return sprite::render_sprite_scene_pixels(device, out.rgba);
    }
    return false;
}

// Write `image` as a binary PPM (P6, maxval 255, alpha dropped) — the corpus interchange format
// (stdlib-parseable by tools/golden_compare.py; no image dependency anywhere in the chain). C++
// streams, not C stdio (MSVC /W4 /WX rejects fopen as C4996). Returns false on any IO failure.
inline bool write_ppm(const GoldenImage& image, const std::string& path)
{
    if (image.width == 0 || image.height == 0 ||
        image.rgba.size() != static_cast<std::size_t>(image.width) * image.height * 4u)
    {
        return false;
    }
    std::ofstream file(path, std::ios::binary);
    if (!file)
    {
        return false;
    }
    file << "P6\n" << image.width << " " << image.height << "\n255\n";
    std::vector<char> row(static_cast<std::size_t>(image.width) * 3u);
    for (std::uint32_t y = 0; y < image.height; ++y)
    {
        const std::uint8_t* src =
            image.rgba.data() + static_cast<std::size_t>(y) * image.width * 4u;
        for (std::uint32_t x = 0; x < image.width; ++x)
        {
            row[static_cast<std::size_t>(x) * 3u + 0u] = static_cast<char>(src[x * 4u + 0u]);
            row[static_cast<std::size_t>(x) * 3u + 1u] = static_cast<char>(src[x * 4u + 1u]);
            row[static_cast<std::size_t>(x) * 3u + 2u] = static_cast<char>(src[x * 4u + 2u]);
        }
        file.write(row.data(), static_cast<std::streamsize>(row.size()));
    }
    return static_cast<bool>(file);
}

} // namespace context::render::golden
