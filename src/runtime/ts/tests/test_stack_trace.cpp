// R-QA-013 tests for the TS-resolved stack-trace remapper (stack_trace.h). Zero-dependency harness
// (plain executable, CHECK()s invariants, non-zero on failure), mirroring test_source_map.cpp.
//
// A LOCAL gate: stack_trace links no V8 — the runtime/js host produces the raw V8 stack string on a
// throw (CI-only for its V8 link), but parsing + remapping it is pure C++, so a CANNED V8 stack + a
// known Source Map exercise the whole "JS stack -> TS positions" path under the local dev gate.
// This is the "headless CI still needs symbolicated traces" half of R-OBS-005.

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "context/runtime/ts/source_map.h"
#include "context/runtime/ts/stack_trace.h"

namespace sttest
{
int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
} // namespace sttest

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            sttest::fail(__FILE__, __LINE__, #cond);                                               \
    } while (false)

namespace cts = context::runtime::ts;

namespace
{
// Same hand-encoded map as test_source_map: generated (0-based) (0,0) -> game.ts (0,0) name "boom";
// generated (1,2) -> game.ts (5,4). Stack positions below are 1-based (as V8 emits), so
// generated frame line 1 col 1 == 0-based (0,0), and line 2 col 3 == 0-based (1,2).
const char* kMap = R"({"version":3,"sources":["game.ts"],"names":["boom"],)"
                   R"("mappings":"AAAAA,IAAU;EAKI"})";

bool contains(const std::string& hay, const char* needle)
{
    return hay.find(needle) != std::string::npos;
}
} // namespace

static void test_parse_frames()
{
    const char* stack = "Error: boom\n"
                        "    at boom (bundle.js:1:1)\n"
                        "    at run (bundle.js:2:3)\n"
                        "    at bundle.js:1:1\n"
                        "    at [native code]\n";
    std::vector<cts::StackFrame> frames = cts::parse_v8_stack(stack);
    // The header line and the "[native code]" frame (no :line:col) are skipped; 3 real frames.
    CHECK(frames.size() == 3);
    if (frames.size() == 3)
    {
        CHECK(frames[0].function == "boom");
        CHECK(frames[0].file == "bundle.js");
        CHECK(frames[0].line == 1);
        CHECK(frames[0].column == 1);
        CHECK(frames[1].function == "run");
        CHECK(frames[1].line == 2);
        CHECK(frames[1].column == 3);
        CHECK(frames[2].function.empty()); // bare "at <loc>" frame
        CHECK(frames[2].file == "bundle.js");
    }
}

static void test_parenthesised_path_uses_outer_paren()
{
    // A location whose file path itself contains parentheses (e.g. a Windows folder "me (dev)") must
    // split at the OUTER '(' that opens the location tail, not the innermost one — otherwise the file
    // is truncated. Regression for the rfind('(') -> find('(') fix in parseFrameBody.
    std::vector<cts::StackFrame> frames =
        cts::parse_v8_stack("    at run (C:\\Users\\me (dev)\\bundle.js:2:3)\n");
    CHECK(frames.size() == 1);
    if (frames.size() == 1)
    {
        CHECK(frames[0].function == "run");
        CHECK(frames[0].file == "C:\\Users\\me (dev)\\bundle.js");
        CHECK(frames[0].line == 2);
        CHECK(frames[0].column == 3);
    }
}

static void test_remap_to_ts()
{
    std::optional<cts::SourceMap> map = cts::SourceMap::parse(kMap);
    CHECK(map.has_value());
    if (!map.has_value())
    {
        return;
    }
    const char* stack = "Error: boom\n"
                        "    at boom (bundle.js:1:1)\n"
                        "    at run (bundle.js:2:3)\n"
                        "    at bundle.js:1:1\n";
    std::vector<cts::StackFrame> frames = cts::parse_v8_stack(stack);
    cts::remap_stack(frames, *map);
    CHECK(frames.size() == 3);
    if (frames.size() == 3)
    {
        // (1,1) -> game.ts (0,0) rendered 1-based -> game.ts:1:1; function label preserved.
        CHECK(frames[0].resolved);
        CHECK(frames[0].file == "game.ts");
        CHECK(frames[0].line == 1);
        CHECK(frames[0].column == 1);
        CHECK(frames[0].function == "boom");
        // (2,3) 1-based == generated (1,2) 0-based -> game.ts 0-based (5,14) [origColumn is
        // cumulative across the map] rendered 1-based -> game.ts:6:15.
        CHECK(frames[1].resolved);
        CHECK(frames[1].file == "game.ts");
        CHECK(frames[1].line == 6);
        CHECK(frames[1].column == 15);
        CHECK(frames[1].function == "run");
        // The bare frame (1,1) resolves too; its empty function label picks up the mapped name.
        CHECK(frames[2].resolved);
        CHECK(frames[2].file == "game.ts");
        CHECK(frames[2].function == "boom");
    }
}

static void test_unmappable_frame_passes_through()
{
    std::optional<cts::SourceMap> map = cts::SourceMap::parse(kMap);
    CHECK(map.has_value());
    if (!map.has_value())
    {
        return;
    }
    // Generated line 9 has no mapping row -> the frame is left unchanged (pass-through, not dropped).
    std::vector<cts::StackFrame> frames = cts::parse_v8_stack("    at foo (bundle.js:9:9)\n");
    cts::remap_stack(frames, *map);
    CHECK(frames.size() == 1);
    if (frames.size() == 1)
    {
        CHECK(!frames[0].resolved);
        CHECK(frames[0].file == "bundle.js");
        CHECK(frames[0].line == 9);
        CHECK(frames[0].column == 9);
    }
}

static void test_render_and_convenience()
{
    std::optional<cts::SourceMap> map = cts::SourceMap::parse(kMap);
    CHECK(map.has_value());
    if (!map.has_value())
    {
        return;
    }
    const char* stack = "Error: boom\n"
                        "    at boom (bundle.js:1:1)\n"
                        "    at run (bundle.js:2:3)\n";
    const std::string trace = cts::resolve_ts_stack(stack, *map, "Error: boom");
    // The rendered trace carries the message header + TS-resolved frames.
    CHECK(contains(trace, "Error: boom"));
    CHECK(contains(trace, "game.ts:1:1"));
    CHECK(contains(trace, "game.ts:6:15"));
    CHECK(contains(trace, "boom (game.ts:1:1)"));
    // The transpiled bundle position must NOT leak into a resolved trace.
    CHECK(!contains(trace, "bundle.js"));
}

static void test_empty_and_headerless()
{
    std::optional<cts::SourceMap> map = cts::SourceMap::parse(kMap);
    CHECK(map.has_value());
    if (!map.has_value())
    {
        return;
    }
    // A stack that is only a message (no frames) renders to just the header.
    CHECK(cts::parse_v8_stack("Error: boom").empty());
    const std::string trace = cts::resolve_ts_stack("Error: boom", *map, "Error: boom");
    CHECK(trace == "Error: boom");
    // No message header -> only the frame lines.
    const std::string noHeader = cts::resolve_ts_stack("    at boom (bundle.js:1:1)\n", *map);
    CHECK(contains(noHeader, "at boom (game.ts:1:1)"));
    CHECK(noHeader.rfind("    at ", 0) == 0); // starts with the frame indent, no header line
}

int main()
{
    test_parse_frames();
    test_parenthesised_path_uses_outer_paren();
    test_remap_to_ts();
    test_unmappable_frame_passes_through();
    test_render_and_convenience();
    test_empty_and_headerless();
    if (sttest::g_failures != 0)
    {
        std::fprintf(stderr, "test_stack_trace: %d CHECK(s) failed\n", sttest::g_failures);
        return 1;
    }
    std::printf("test_stack_trace: OK\n");
    return 0;
}
