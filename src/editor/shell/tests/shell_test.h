// Tiny zero-dependency test harness for the editor-shell ctest executables (mirrors the sibling
// modules' tests/*_test.h — the repo carries no C++ test framework, so each test is a plain
// executable that CHECK()s its invariants and returns non-zero on any failure).

#pragma once

#include "context/render/rhi.h"

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace shelltest
{
inline int g_failures = 0;

// A unique temp project dir per run. A stale directory from a previous failed run otherwise makes
// the "fresh project" assertions read as a bug in the code under test. `prefix` separates the
// suites so two test executables running concurrently cannot collide.
//
// UNIQUE PER PROCESS, not merely per suite. `prefix`+`tag`+counter is DETERMINISTIC, and this
// function begins by `remove_all`ing the path it is about to use — so two concurrent runs of the
// same executable (the CI Windows legs ride self-hosted runners sharing one box and one TEMP, so a
// PR rollup and a `main` push overlap routinely) would delete each other's fixture mid-test. On a
// security suite that failure is the worst kind: every "refused" assertion stays trivially true
// while the non-vacuity positives fail, which reads exactly like a real containment regression.
[[nodiscard]] inline std::filesystem::path make_temp_project(const char* prefix, const char* tag)
{
    static int counter = 0;
    // One stamp per process, taken once. The tick count is materialised into a CONCRETE integer
    // BEFORE `std::to_string`: a chrono rep is implementation-defined, and Apple libc++ finds the
    // overload ambiguous on one where GCC and MSVC do not (test.md § Suite 1, macOS libc++ note).
    static const long long run_ticks = static_cast<long long>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    static const std::string run_stamp = std::to_string(run_ticks);
    std::error_code ec;
    std::filesystem::path root =
        std::filesystem::temp_directory_path(ec) /
        (std::string(prefix) + "-" + tag + "-" + run_stamp + "-" + std::to_string(++counter));
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

// How many times `needle` occurs. Shared because a security-policy assertion often needs a COUNT
// rather than a presence: `!mentions(csp, "frame-src context-ext: data:")` passes against
// `frame-src data: context-ext:`, whereas `count_occurrences(csp, "data:") == 1` pins that the one
// permitted `data:` source is still the only one, whatever the directive order.
[[nodiscard]] inline std::size_t count_occurrences(const std::string& haystack, const char* needle)
{
    if (needle == nullptr || *needle == '\0')
    {
        return 0;
    }
    std::size_t count = 0;
    for (std::size_t at = haystack.find(needle); at != std::string::npos;
         at = haystack.find(needle, at + 1))
    {
        ++count;
    }
    return count;
}

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}

// Write a fixture file, creating its parent directories, and ASSERT it landed.
//
// The existence assertion is the load-bearing half, which is why this lives in the shared harness
// rather than being re-typed per suite: on an adversarial suite a fixture that silently failed to
// write makes every "refused" assertion trivially true, so the suite keeps passing while asserting
// nothing at all.
inline void write_file(const std::filesystem::path& path, const std::string& content)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    {
        // C++ streams, never std::fopen: MSVC /W4 /WX rejects the C stdio family as C4996 and the
        // local GCC gate cannot see it (conventions.md § Coding conventions).
        std::ofstream out(path, std::ios::binary);
        out << content;
    }
    if (!std::filesystem::exists(path, ec))
    {
        fail(__FILE__, __LINE__, "shelltest::write_file fixture did not land on disk");
    }
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
