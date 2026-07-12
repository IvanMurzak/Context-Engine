// R-QA-013 test for the M6 X1 pooled/no-alloc hot API (hot_api.h — the query half of R-SIM-008).
// Full in-V8 battery on the CI legs (CONTEXT_JS_HAS_V8): idempotent install, pool identity reuse +
// steady-state zero misses + exhaustion fallback, in-place vector math (incl. aliasing), the
// Q16.16 fixed mirror cross-checked BIT-FOR-BIT against the REAL C++ packages/simmath core, and
// pooled query cursors over real runSystemView zero-copy views. On the local stub toolchain
// (Strawberry GCC — the rusty_v8 prebuilt cannot link) it asserts the JS source's structure and
// that the V8 backend correctly reports unavailable, so `ctest --preset dev` stays green.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "context/packages/simmath/fixed.h"
#include "context/runtime/js/js_host.h"
#include "context/runtime/ts/hot_api.h"

namespace
{
int g_failures = 0;

void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
} // namespace

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            fail(__FILE__, __LINE__, #cond);                                                       \
    } while (false)

namespace cjs = context::runtime::js;
namespace cts = context::runtime::ts;
namespace simmath = context::packages::simmath;

namespace
{
// The JS source ships these API markers regardless of backend — a structural local gate.
void test_source_structure()
{
    const std::string_view src = cts::hot_api_js();
    CHECK(!src.empty());
    CHECK(src.find("__hotApi") != std::string_view::npos);
    CHECK(src.find("ctx.pool") != std::string_view::npos);
    CHECK(src.find("ctx.math") != std::string_view::npos);
    CHECK(src.find("ctx.fixed") != std::string_view::npos);
    CHECK(src.find("ctx.query") != std::string_view::npos);
    CHECK(src.find("globalThis.ctx") != std::string_view::npos);
}
} // namespace

#ifdef CONTEXT_JS_HAS_V8

namespace
{
// Evaluate `code` expecting a numeric completion value.
double evalNum(cjs::JsEngine& engine, const std::string& code)
{
    double result = -424242.0;
    std::string err;
    if (!engine.eval(code, &result, err))
    {
        std::fprintf(stderr, "eval failed: %s\ncode: %s\n", err.c_str(), code.c_str());
        ++g_failures;
    }
    return result;
}

void test_install_idempotent(cjs::JsEngine& engine)
{
    std::string err;
    CHECK(cts::install_hot_api(engine, err));
    CHECK(evalNum(engine, "globalThis.ctx && ctx.__hotApi === 1 ? 1 : 0") == 1.0);
    // Second install is a no-op (the pools/cursors keep their identity).
    CHECK(evalNum(engine, "globalThis.__hot_poolMark = ctx.pool; 1") == 1.0);
    CHECK(cts::install_hot_api(engine, err));
    CHECK(evalNum(engine, "ctx.pool === globalThis.__hot_poolMark ? 1 : 0") == 1.0);
}

void test_pool(cjs::JsEngine& engine)
{
    // Identity reuse: after reset(), the same register objects are handed out again.
    CHECK(evalNum(engine, "ctx.pool.reset();"
                          "var __hot_r0 = ctx.pool.v3();"
                          "ctx.pool.reset();"
                          "ctx.pool.v3() === __hot_r0 ? 1 : 0") == 1.0);
    // Steady-state usage allocates nothing: 32 registers per width without a miss.
    CHECK(evalNum(engine, "ctx.pool.reset();"
                          "for (var i = 0; i < 32; i++) { ctx.pool.v2(); }"
                          "ctx.pool.misses") == 0.0);
    // Exhaustion falls back to allocating (correctness) and counts the miss.
    CHECK(evalNum(engine, "ctx.pool.reset();"
                          "for (var i = 0; i < 32; i++) { ctx.pool.v2(); }"
                          "var __hot_extra = ctx.pool.v2();"
                          "(__hot_extra.length === 2 && ctx.pool.misses === 1) ? 1 : 0") == 1.0);
}

void test_math(cjs::JsEngine& engine)
{
    CHECK(evalNum(engine, "ctx.pool.reset();"
                          "var a = ctx.math.v3set(ctx.pool.v3(), 1, 0, 0);"
                          "var b = ctx.math.v3set(ctx.pool.v3(), 0, 1, 0);"
                          "var c = ctx.math.v3cross(ctx.pool.v3(), a, b);"
                          "(c[0] === 0 && c[1] === 0 && c[2] === 1) ? 1 : 0") == 1.0);
    // Aliasing-safe cross: out === a still computes the true cross product.
    CHECK(evalNum(engine, "ctx.pool.reset();"
                          "var a = ctx.math.v3set(ctx.pool.v3(), 2, 3, 4);"
                          "var b = ctx.math.v3set(ctx.pool.v3(), 5, 6, 7);"
                          "ctx.math.v3cross(a, a, b);"
                          "(a[0] === -3 && a[1] === 6 && a[2] === -3) ? 1 : 0") == 1.0);
    CHECK(evalNum(engine, "ctx.pool.reset();"
                          "var a = ctx.math.v2set(ctx.pool.v2(), 3, 4);"
                          "ctx.math.v2dot(a, a)") == 25.0);
    CHECK(evalNum(engine, "ctx.math.lerp(10, 20, 0.25)") == 12.5);
    CHECK(evalNum(engine, "ctx.math.clamp(7, 0, 5)") == 5.0);
    CHECK(evalNum(engine, "ctx.pool.reset();"
                          "var s = ctx.math.v2sub(ctx.pool.v2(),"
                          "    ctx.math.v2set(ctx.pool.v2(), 9, 7),"
                          "    ctx.math.v2set(ctx.pool.v2(), 4, 2));"
                          "(s[0] === 5 && s[1] === 5) ? 1 : 0") == 1.0);
}

// The Q16.16 mirror must match the REAL C++ simmath core bit-for-bit inside the |raw| <= 2^31
// domain — floor-shift multiply and truncate-toward-zero divide included, negatives included.
void test_fixed_matches_simmath(cjs::JsEngine& engine)
{
    using simmath::Fixed;
    const std::int64_t one = simmath::kFixedOneRaw;
    const std::int64_t pairs[][2] = {
        {3 * one / 2, 5 * one / 2},          // 1.5 * 2.5
        {-3 * one / 2, 5 * one / 2},         // -1.5 * 2.5 (floor semantics on the product shift)
        {-one / 3, one / 7},                 // small negative fractions
        {2147483647LL, 2147483647LL},        // the domain corner (|raw| <= 2^31)
        {2147483647LL, -2147483647LL},       //
        {one, -one},                         //
        {12345, -98765},                     // sub-unit raws
        {-7 * one, 2 * one},                 // -7 / 2 = -3.5 exact; trunc-vs-floor probe on div
    };
    for (const auto& p : pairs)
    {
        const std::int64_t a = p[0];
        const std::int64_t b = p[1];
        const std::int64_t mulExpected = (Fixed::from_raw(a) * Fixed::from_raw(b)).raw;
        char code[160];
        std::snprintf(code, sizeof(code), "ctx.fixed.mul(%lld, %lld)",
                      static_cast<long long>(a), static_cast<long long>(b));
        CHECK(evalNum(engine, code) == static_cast<double>(mulExpected));
        if (b != 0)
        {
            const std::int64_t divExpected = (Fixed::from_raw(a) / Fixed::from_raw(b)).raw;
            std::snprintf(code, sizeof(code), "ctx.fixed.div(%lld, %lld)",
                          static_cast<long long>(a), static_cast<long long>(b));
            CHECK(evalNum(engine, code) == static_cast<double>(divExpected));
        }
    }

    // from_ratio truncates toward zero exactly like C++ (floor would give -21846 here).
    CHECK(evalNum(engine, "ctx.fixed.fromRatio(-1, 3)") ==
          static_cast<double>(Fixed::from_ratio(-1, 3).raw));
    CHECK(evalNum(engine, "ctx.fixed.fromRatio(1, 3)") ==
          static_cast<double>(Fixed::from_ratio(1, 3).raw));
    // floor vs trunc on a negative value: floor(-1.5) == -2, trunc(-1.5) == -1 (fixed.h contract).
    const Fixed negOneHalf = Fixed::from_ratio(-3, 2);
    char code[96];
    std::snprintf(code, sizeof(code), "ctx.fixed.floorInt(%lld)",
                  static_cast<long long>(negOneHalf.raw));
    CHECK(evalNum(engine, code) == static_cast<double>(negOneHalf.floor_int()));
    std::snprintf(code, sizeof(code), "ctx.fixed.truncInt(%lld)",
                  static_cast<long long>(negOneHalf.raw));
    CHECK(evalNum(engine, code) == static_cast<double>(negOneHalf.trunc_int()));
    // divInt mirrors C++ raw/n; division by zero is deterministically 0 (C++ UB made fail-safe).
    CHECK(evalNum(engine, "ctx.fixed.divInt(-458752, 3)") ==
          static_cast<double>((Fixed::from_raw(-458752) / std::int64_t{3}).raw));
    CHECK(evalNum(engine, "ctx.fixed.div(65536, 0)") == 0.0);
    CHECK(evalNum(engine, "ctx.fixed.sign(-5) === -1 && ctx.fixed.abs(-5) === 5 ? 1 : 0") == 1.0);
}

// Pooled query cursors over REAL zero-copy views: a (pos, vel)-strided f64 column integrated by a
// JS executor through ctx.query, twice — proving cursor identity reuse across invocations, zero
// steady-state misses, and that the host reads the mutation back through the stable VmBuffer.
void test_query_over_views(cjs::JsEngine& engine)
{
    constexpr std::size_t kRows = 4;
    constexpr std::size_t kStride = 2; // lane 0 = pos, lane 1 = vel
    std::string err;

    cjs::VmBuffer buffer;
    CHECK(engine.allocVmBuffer(kRows * kStride * sizeof(double), buffer, err));
    auto* data = static_cast<double*>(buffer.data);
    for (std::size_t r = 0; r < kRows; ++r)
    {
        data[r * kStride + 0] = static_cast<double>(r) * 10.0; // pos
        data[r * kStride + 1] = static_cast<double>(r) + 1.0;  // vel
    }

    CHECK(evalNum(engine,
                  "var __hot_c0 = null;"
                  "globalThis.__hot_sameCursor = -1;"
                  "function __hot_integrate(col) {"
                  "    ctx.query.reset();"
                  "    var c = ctx.query.open(col, 2);"
                  "    if (__hot_c0 === null) { __hot_c0 = c; }"
                  "    globalThis.__hot_sameCursor = (c === __hot_c0) ? 1 : 0;"
                  "    for (var r = 0; r < c.count; r++) {"
                  "        c.set(r, 0, c.get(r, 0) + c.get(r, 1));"
                  "    }"
                  "}"
                  "1") == 1.0);

    const cjs::FunctionHandle integrate = engine.getFunction("__hot_integrate");
    CHECK(integrate != cjs::kInvalidFunction);
    cjs::ViewBinding binding;
    binding.buffer = buffer.handle;
    binding.element = cjs::ViewElement::f64;
    binding.byteOffset = 0;
    binding.count = kRows * kStride;

    CHECK(engine.runSystemView(integrate, &binding, 1, err));
    CHECK(engine.runSystemView(integrate, &binding, 1, err));

    // Two integrations: pos = r*10 + 2*(r+1), read back through the stable backing store.
    for (std::size_t r = 0; r < kRows; ++r)
    {
        const double expected = static_cast<double>(r) * 10.0 + 2.0 * (static_cast<double>(r) + 1.0);
        CHECK(data[r * kStride + 0] == expected);
        CHECK(data[r * kStride + 1] == static_cast<double>(r) + 1.0);
    }
    // The second invocation reused the pooled cursor object; no pool misses accrued.
    CHECK(evalNum(engine, "globalThis.__hot_sameCursor") == 1.0);
    CHECK(evalNum(engine, "ctx.query.misses") == 0.0);
    CHECK(engine.freeVmBuffer(buffer.handle, err));
}
} // namespace

int main()
{
    test_source_structure();

    std::string err;
    std::unique_ptr<cjs::JsEngine> engine = cjs::createV8Engine(err);
    CHECK(engine != nullptr);
    if (engine == nullptr)
    {
        std::fprintf(stderr, "createV8Engine failed: %s\n", err.c_str());
        return 1;
    }
    test_install_idempotent(*engine);
    test_pool(*engine);
    test_math(*engine);
    test_fixed_matches_simmath(*engine);
    test_query_over_views(*engine);

    if (g_failures != 0)
    {
        std::fprintf(stderr, "%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("hot_api (V8): all checks passed\n");
    return 0;
}

#else // !CONTEXT_JS_HAS_V8 — the local stub toolchain (profile setup.md/test.md carve-out)

int main()
{
    test_source_structure();

    std::string err;
    CHECK(!context::runtime::js::v8BackendAvailable());
    CHECK(context::runtime::js::createV8Engine(err) == nullptr);
    CHECK(!err.empty());

    if (g_failures != 0)
    {
        std::fprintf(stderr, "%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("hot_api (stub): source-structure checks passed; V8 battery runs on the CI legs\n");
    return 0;
}

#endif
