// R-QA-013 tests for the in-process V8 JS host (issue #76). Zero-dependency harness (the repo
// carries no C++ test framework — each test is a plain executable that CHECK()s its invariants
// and returns non-zero on any failure), mirroring the sibling modules' tests/*_test.h.
//
// When the V8 backend is compiled in (CONTEXT_JS_HAS_V8 — the 3-OS CI build legs), this runs
// the full battery: trivial eval + numeric result, compile/runtime error paths, host->JS and
// JS->host boundary calls, the shape-B VM-allocated backing-store seam (host read/write
// visible), the CDP inspector seam presence, and repeated create/dispose lifecycle (ASan/LSan
// clean under the CI sanitize legs). On a toolchain that cannot link the rusty_v8 prebuilt
// (the local Strawberry-GCC Windows `dev` gate), it asserts the stub reports unavailability —
// keeping the local non-dependency gate green.

#include <cstdio>
#include <memory>
#include <string>

#include "context/runtime/js/js_host.h"

namespace jstest
{
int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
}  // namespace jstest

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            jstest::fail(__FILE__, __LINE__, #cond);                                               \
    } while (false)

namespace cjs = context::runtime::js;

#ifdef CONTEXT_JS_HAS_V8

// A JS->host callback: returns the sum of its args plus a host-side bias read through `user`.
static double hostSum(void* user, const double* args, std::size_t nargs)
{
    double acc = *static_cast<double*>(user);
    for (std::size_t i = 0; i < nargs; ++i)
    {
        acc += args[i];
    }
    return acc;
}

static void test_trivial_eval()
{
    std::string err;
    std::unique_ptr<cjs::JsEngine> engine = cjs::createV8Engine(err);
    CHECK(engine != nullptr);
    if (!engine)
    {
        return;
    }
    CHECK(engine->name() == "v8");
    CHECK(!engine->version().empty());

    double result = 0.0;
    CHECK(engine->eval("40 + 2", &result, err));
    CHECK(result == 42.0);

    // Compile error path: fills err, returns false.
    err.clear();
    CHECK(!engine->eval("this is not valid js @@@", nullptr, err));
    CHECK(!err.empty());

    // Runtime throw path: fills err, returns false.
    err.clear();
    CHECK(!engine->eval("throw new Error('boom')", nullptr, err));
    CHECK(!err.empty());
}

static void test_host_to_js()
{
    std::string err;
    std::unique_ptr<cjs::JsEngine> engine = cjs::createV8Engine(err);
    CHECK(engine != nullptr);
    if (!engine)
    {
        return;
    }
    CHECK(engine->eval("function triple(x) { return x * 3; }", nullptr, err));
    cjs::FunctionHandle fn = engine->getFunction("triple");
    CHECK(fn != cjs::kInvalidFunction);

    const double args[1] = {14.0};
    double out = 0.0;
    CHECK(engine->callFunction(fn, args, 1, &out, err));
    CHECK(out == 42.0);

    CHECK(engine->getFunction("does_not_exist") == cjs::kInvalidFunction);
}

static void test_js_to_host()
{
    std::string err;
    std::unique_ptr<cjs::JsEngine> engine = cjs::createV8Engine(err);
    CHECK(engine != nullptr);
    if (!engine)
    {
        return;
    }
    double bias = 2.0;
    CHECK(engine->bindHostFunction("hostSum", &hostSum, &bias, err));

    // JS calls into the host binding; the returned value flows back to the JS completion.
    double out = 0.0;
    CHECK(engine->eval("hostSum(20, 20)", &out, err));
    CHECK(out == 42.0);  // 20 + 20 + bias(2)
}

static void test_shape_b_vm_buffer()
{
    std::string err;
    std::unique_ptr<cjs::JsEngine> engine = cjs::createV8Engine(err);
    CHECK(engine != nullptr);
    if (!engine)
    {
        return;
    }
    cjs::VmBuffer buf;
    CHECK(engine->allocVmBuffer(64, buf, err));
    CHECK(buf.handle != cjs::kInvalidVmBuffer);
    CHECK(buf.data != nullptr);
    CHECK(buf.size == 64);

    // Host writes a pattern into the VM-allocated (sandbox-interior) store, then reads it back
    // — proving the buffer is host read/write visible (task 3 layers zero-copy views on top).
    auto* bytes = static_cast<unsigned char*>(buf.data);
    for (std::size_t i = 0; i < buf.size; ++i)
    {
        bytes[i] = static_cast<unsigned char>(i & 0xFF);
    }
    bool readback_ok = true;
    for (std::size_t i = 0; i < buf.size; ++i)
    {
        if (bytes[i] != static_cast<unsigned char>(i & 0xFF))
        {
            readback_ok = false;
        }
    }
    CHECK(readback_ok);

    CHECK(engine->freeVmBuffer(buf.handle, err));
    // Double free / bad handle is rejected.
    CHECK(!engine->freeVmBuffer(buf.handle, err));
    CHECK(!engine->freeVmBuffer(cjs::kInvalidVmBuffer, err));
}

static void test_inspector_seam_present()
{
    std::string err;
    std::unique_ptr<cjs::JsEngine> engine = cjs::createV8Engine(err);
    CHECK(engine != nullptr);
    if (!engine)
    {
        return;
    }
    CHECK(engine->inspectorSeamPresent());
}

static void test_lifecycle_no_leak()
{
    // Repeated create/dispose across the shared process-global platform — the ASan/LSan
    // cleanliness proof (CI sanitize legs run this under -fsanitize=address,undefined + LSan).
    for (int i = 0; i < 3; ++i)
    {
        std::string err;
        std::unique_ptr<cjs::JsEngine> engine = cjs::createV8Engine(err);
        CHECK(engine != nullptr);
        if (!engine)
        {
            continue;
        }
        double result = 0.0;
        CHECK(engine->eval("1 + 1", &result, err));
        CHECK(result == 2.0);

        cjs::VmBuffer buf;
        CHECK(engine->allocVmBuffer(128, buf, err));
        // Intentionally NOT freed here — teardown must reclaim outstanding VM buffers cleanly.
    }
}

int main()
{
    CHECK(cjs::v8BackendAvailable());
    test_trivial_eval();
    test_host_to_js();
    test_js_to_host();
    test_shape_b_vm_buffer();
    test_inspector_seam_present();
    test_lifecycle_no_leak();
    if (jstest::g_failures == 0)
    {
        std::printf("js host: all checks passed\n");
    }
    return jstest::g_failures == 0 ? 0 : 1;
}

#else  // !CONTEXT_JS_HAS_V8 — stub toolchain (local Strawberry-GCC Windows dev gate)

int main()
{
    CHECK(!cjs::v8BackendAvailable());
    std::string err;
    std::unique_ptr<cjs::JsEngine> engine = cjs::createV8Engine(err);
    CHECK(engine == nullptr);
    CHECK(!err.empty());
    if (jstest::g_failures == 0)
    {
        std::printf("js host: V8 backend not built on this toolchain (stub) — non-dependency "
                    "gate green; CI build legs are authoritative\n");
    }
    return jstest::g_failures == 0 ? 0 : 1;
}

#endif  // CONTEXT_JS_HAS_V8
