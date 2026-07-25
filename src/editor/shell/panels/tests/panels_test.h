// Tiny zero-dependency test harness for the context_editor_panels ctest executables (mirrors the
// sibling modules' tests/*_test.h — the repo carries no C++ test framework, so each test is a plain
// executable that CHECK()s its invariants and returns non-zero on any failure).
//
// Deliberately NOT a reuse of ../../tests/shell_test.h: that header includes context/render/rhi.h
// for its compositor fixtures, and nothing in this suite builds against the render tier.

#pragma once

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace panelstest
{
inline int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}

[[nodiscard]] inline bool mentions(const std::string& haystack, const char* needle)
{
    return haystack.find(needle) != std::string::npos;
}

// A unique throwaway project directory. Deliberately NOT `../../tests/shell_test.h`'s (which pulls
// context/render/rhi.h, per this header's note) — but the PROPERTIES are copied from it on purpose:
// the per-process tick stamp AND the per-call counter together are what keep two concurrent runs of
// the same executable, on the CI Windows legs that share one box and one TEMP, from `remove_all`ing
// each other's fixture mid-test.
[[nodiscard]] inline std::filesystem::path make_temp_project(const char* prefix, const char* tag)
{
    namespace fs = std::filesystem;
    static int counter = 0;
    // The tick count is materialised into a CONCRETE long long BEFORE std::to_string: a chrono rep is
    // implementation-defined and Apple libc++ finds the overload ambiguous on one where GCC and MSVC
    // do not (test.md § Suite 1, the macOS libc++ note).
    static const long long run_ticks = static_cast<long long>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    static const std::string run_stamp = std::to_string(run_ticks);
    std::error_code ec;
    fs::path root = fs::temp_directory_path(ec) / (std::string(prefix) + "-" + tag + "-" + run_stamp +
                                                   "-" + std::to_string(++counter));
    fs::create_directories(root, ec);
    return root;
}

// Named `cleanup` rather than calling `fs::remove_all` at the call site: ADL on `fs::path` makes the
// unqualified spelling ambiguous, and a test that swallows the error_code should say so once.
inline void cleanup(const std::filesystem::path& path)
{
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

[[nodiscard]] inline std::string read_file(const std::filesystem::path& path)
{
    // std::ifstream, not std::fopen: MSVC /W4 /WX rejects the C stdio family as C4996 and the local
    // GCC gate cannot see it (conventions.md § Coding conventions).
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

} // namespace panelstest

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            panelstest::fail(__FILE__, __LINE__, #cond);                                           \
    } while (false)

#define PANELS_TEST_MAIN_END() return panelstest::g_failures == 0 ? 0 : 1
