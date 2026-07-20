// Tiny zero-dependency test harness for the editor-shell ctest executables (mirrors the sibling
// modules' tests/*_test.h — the repo carries no C++ test framework, so each test is a plain
// executable that CHECK()s its invariants and returns non-zero on any failure).

#pragma once

#include "context/render/rhi.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

namespace shelltest
{
inline int g_failures = 0;

// A unique temp project dir per run. A stale directory from a previous failed run otherwise makes
// the "fresh project" assertions read as a bug in the code under test. `prefix` separates the
// suites so two test executables running concurrently cannot collide.
[[nodiscard]] inline std::filesystem::path make_temp_project(const char* prefix, const char* tag)
{
    static int counter = 0;
    std::error_code ec;
    std::filesystem::path root =
        std::filesystem::temp_directory_path(ec) /
        (std::string(prefix) + "-" + tag + "-" + std::to_string(++counter));
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return root;
}

// Named cleanup(), not remove_all(): the latter is found by ADL on fs::path and collides with
// std::filesystem::remove_all.
inline void cleanup(const std::filesystem::path& path)
{
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

[[nodiscard]] inline bool mentions(const std::string& haystack, const char* needle)
{
    return haystack.find(needle) != std::string::npos;
}

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}

// Float comparison for the UV arithmetic. The extrapolated layer UVs are computed in float, so an
// exact compare would be asserting the FPU's rounding rather than the geometry.
[[nodiscard]] inline bool near_eq(float a, float b, float tolerance = 1e-4f)
{
    const float delta = a - b;
    return (delta < 0.0f ? -delta : delta) <= tolerance;
}

[[nodiscard]] inline bool extent_eq(context::render::Extent2D a, context::render::Extent2D b)
{
    return a.width == b.width && a.height == b.height;
}

[[nodiscard]] inline bool rect_eq(const context::render::Rect2D& a,
                                  const context::render::Rect2D& b)
{
    return a.origin.x == b.origin.x && a.origin.y == b.origin.y && extent_eq(a.size, b.size);
}

[[nodiscard]] inline context::render::Rect2D rect(std::uint32_t x, std::uint32_t y,
                                                  std::uint32_t w, std::uint32_t h)
{
    return context::render::Rect2D{context::render::Origin2D{x, y},
                                   context::render::Extent2D{w, h}};
}

} // namespace shelltest

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            shelltest::fail(__FILE__, __LINE__, #cond);                                            \
    } while (false)

#define SHELL_TEST_MAIN_END() return shelltest::g_failures == 0 ? 0 : 1
