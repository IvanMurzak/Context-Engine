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
#include <functional>
#include <memory>
#include <string>
#include <vector>

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

// Backend-agnostic: the ViewBinding element-width arithmetic is exercised on EVERY toolchain,
// including the local Strawberry-GCC stub gate (view_element.cpp is compiled into both builds).
static void test_view_element_width()
{
    CHECK(cjs::view_element_width(cjs::ViewElement::u8) == 1);
    CHECK(cjs::view_element_width(cjs::ViewElement::i8) == 1);
    CHECK(cjs::view_element_width(cjs::ViewElement::u16) == 2);
    CHECK(cjs::view_element_width(cjs::ViewElement::i16) == 2);
    CHECK(cjs::view_element_width(cjs::ViewElement::u32) == 4);
    CHECK(cjs::view_element_width(cjs::ViewElement::i32) == 4);
    CHECK(cjs::view_element_width(cjs::ViewElement::f32) == 4);
    CHECK(cjs::view_element_width(cjs::ViewElement::f64) == 8);
    CHECK(cjs::view_element_width(cjs::ViewElement::i64) == 8);
    CHECK(cjs::view_element_width(cjs::ViewElement::u64) == 8);
}

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

// --- R-LANG-009 zero-copy view lifetime & invalidation protocol ---------------------------------

// A zero-copy f32 view over a VmBuffer, mutated by a JS system, read back by the host — proving the
// JS typed array ALIASES the stable backing store both ways (no copy).
static void test_view_zero_copy_roundtrip()
{
    std::string err;
    std::unique_ptr<cjs::JsEngine> engine = cjs::createV8Engine(err);
    CHECK(engine != nullptr);
    if (!engine)
    {
        return;
    }
    cjs::VmBuffer buf;
    CHECK(engine->allocVmBuffer(4 * sizeof(float), buf, err)); // 4 f32 lanes
    auto* f = static_cast<float*>(buf.data);
    f[0] = 3.0f;
    f[1] = 0.0f;
    f[2] = 0.0f;
    f[3] = 7.0f;

    // A system reads lane 0, doubles it into lane 1, and stamps lane 2 — all through the view.
    CHECK(engine->eval("function sys(v){ v[1] = v[0] * 2; v[2] = 42; }", nullptr, err));
    cjs::FunctionHandle sys = engine->getFunction("sys");
    CHECK(sys != cjs::kInvalidFunction);

    const cjs::ViewBinding vb{buf.handle, cjs::ViewElement::f32, 0, 4};
    CHECK(engine->runSystemView(sys, &vb, 1, err));

    // The host reads JS's writes back through the SAME memory — zero-copy.
    CHECK(f[1] == 6.0f);
    CHECK(f[2] == 42.0f);
    CHECK(f[0] == 3.0f); // untouched lane preserved
    CHECK(f[3] == 7.0f);
    CHECK(engine->freeVmBuffer(buf.handle, err));
}

// The view is detached/neutered at system exit: a reference JS retained past the call is dead, a new
// view over its ArrayBuffer throws, and the host's backing store is intact (detach kills the JS
// view, never the store).
static void test_view_detach_neuters_retained()
{
    std::string err;
    std::unique_ptr<cjs::JsEngine> engine = cjs::createV8Engine(err);
    CHECK(engine != nullptr);
    if (!engine)
    {
        return;
    }
    cjs::VmBuffer buf;
    CHECK(engine->allocVmBuffer(4, buf, err));
    auto* bytes = static_cast<unsigned char*>(buf.data);
    bytes[0] = 11;

    // The system stashes the view + its buffer on globalThis and writes a marker, then returns
    // (retaining a live reference the engine must neuter).
    CHECK(engine->eval("function leaky(v){ globalThis.__v = v; globalThis.__b = v.buffer; v[3] = 99; }",
                       nullptr, err));
    cjs::FunctionHandle leaky = engine->getFunction("leaky");
    CHECK(leaky != cjs::kInvalidFunction);

    const cjs::ViewBinding vb{buf.handle, cjs::ViewElement::u8, 0, 4};
    CHECK(engine->runSystemView(leaky, &vb, 1, err));

    // Host sees the write (it landed before detach) and the store is still readable after detach.
    CHECK(bytes[3] == 99);
    CHECK(bytes[0] == 11);

    // The retained typed array is neutered: length + byteLength 0, element read undefined.
    double n = -1.0;
    CHECK(engine->eval("globalThis.__v.length", &n, err));
    CHECK(n == 0.0);
    n = -1.0;
    CHECK(engine->eval("globalThis.__v.byteLength", &n, err));
    CHECK(n == 0.0);
    n = -1.0;
    CHECK(engine->eval("(globalThis.__v[3] === undefined) ? 1 : 0", &n, err));
    CHECK(n == 1.0);

    // Constructing a NEW view over the detached buffer throws (buffer is neutered).
    n = -1.0;
    CHECK(engine->eval("(function(){ try { new Uint8Array(globalThis.__b); return 0; } "
                       "catch (e) { return 1; } })()",
                       &n, err));
    CHECK(n == 1.0);
    CHECK(engine->freeVmBuffer(buf.handle, err));
}

// A (Position, Velocity)-style view-SET: two columns bound in one system invocation, both aliasing
// their VmBuffers — the R-LANG-012 batched-handout shape.
static void test_view_multi_binding_set()
{
    std::string err;
    std::unique_ptr<cjs::JsEngine> engine = cjs::createV8Engine(err);
    CHECK(engine != nullptr);
    if (!engine)
    {
        return;
    }
    cjs::VmBuffer pos;
    cjs::VmBuffer vel;
    CHECK(engine->allocVmBuffer(4 * sizeof(float), pos, err)); // 2 entities * (x,y)
    CHECK(engine->allocVmBuffer(4 * sizeof(float), vel, err));
    auto* p = static_cast<float*>(pos.data);
    auto* v = static_cast<float*>(vel.data);
    p[0] = 1.0f;
    p[1] = 2.0f;
    p[2] = 3.0f;
    p[3] = 4.0f;
    v[0] = 10.0f;
    v[1] = 20.0f;
    v[2] = 30.0f;
    v[3] = 40.0f;

    CHECK(engine->eval("function move(pos, vel){ for (let i = 0; i < pos.length; i++) "
                       "pos[i] += vel[i]; }",
                       nullptr, err));
    cjs::FunctionHandle move = engine->getFunction("move");
    CHECK(move != cjs::kInvalidFunction);

    const cjs::ViewBinding vbs[2] = {
        {pos.handle, cjs::ViewElement::f32, 0, 4},
        {vel.handle, cjs::ViewElement::f32, 0, 4},
    };
    CHECK(engine->runSystemView(move, vbs, 2, err));

    CHECK(p[0] == 11.0f);
    CHECK(p[1] == 22.0f);
    CHECK(p[2] == 33.0f);
    CHECK(p[3] == 44.0f);
    CHECK(engine->freeVmBuffer(pos.handle, err));
    CHECK(engine->freeVmBuffer(vel.handle, err));
}

// Bad bindings are rejected (false + err), never a silent wrap.
static void test_view_bad_bindings()
{
    std::string err;
    std::unique_ptr<cjs::JsEngine> engine = cjs::createV8Engine(err);
    CHECK(engine != nullptr);
    if (!engine)
    {
        return;
    }
    cjs::VmBuffer buf;
    CHECK(engine->allocVmBuffer(4 * sizeof(float), buf, err));
    CHECK(engine->eval("function noop(){}", nullptr, err));
    cjs::FunctionHandle noop = engine->getFunction("noop");
    CHECK(noop != cjs::kInvalidFunction);

    // Bad VM buffer handle.
    err.clear();
    const cjs::ViewBinding badHandle{cjs::kInvalidVmBuffer, cjs::ViewElement::f32, 0, 1};
    CHECK(!engine->runSystemView(noop, &badHandle, 1, err));
    CHECK(!err.empty());

    // Misaligned byte offset for an f32 view (2 is not a multiple of 4).
    err.clear();
    const cjs::ViewBinding misaligned{buf.handle, cjs::ViewElement::f32, 2, 1};
    CHECK(!engine->runSystemView(noop, &misaligned, 1, err));
    CHECK(!err.empty());

    // Slice runs past the end of the store (5 f32 in a 4-f32 buffer).
    err.clear();
    const cjs::ViewBinding tooLong{buf.handle, cjs::ViewElement::f32, 0, 5};
    CHECK(!engine->runSystemView(noop, &tooLong, 1, err));
    CHECK(!err.empty());

    // Bad function handle.
    err.clear();
    const cjs::ViewBinding okvb{buf.handle, cjs::ViewElement::f32, 0, 1};
    CHECK(!engine->runSystemView(cjs::kInvalidFunction, &okvb, 1, err));
    CHECK(!err.empty());

    // Too many bindings.
    err.clear();
    cjs::ViewBinding many[cjs::kMaxSystemViews + 1];
    for (auto& b : many)
    {
        b = cjs::ViewBinding{buf.handle, cjs::ViewElement::f32, 0, 1};
    }
    CHECK(!engine->runSystemView(noop, many, cjs::kMaxSystemViews + 1, err));
    CHECK(!err.empty());

    CHECK(engine->freeVmBuffer(buf.handle, err));
}

// A system that THROWS mid-run still gets its views detached at exit (no retained-view leak on the
// error path) and the call reports the failure.
static void test_view_detach_on_throw()
{
    std::string err;
    std::unique_ptr<cjs::JsEngine> engine = cjs::createV8Engine(err);
    CHECK(engine != nullptr);
    if (!engine)
    {
        return;
    }
    cjs::VmBuffer buf;
    CHECK(engine->allocVmBuffer(4, buf, err));
    CHECK(engine->eval("function boom(v){ globalThis.__t = v; throw new Error('boom'); }", nullptr,
                       err));
    cjs::FunctionHandle boom = engine->getFunction("boom");
    CHECK(boom != cjs::kInvalidFunction);

    err.clear();
    const cjs::ViewBinding vb{buf.handle, cjs::ViewElement::u8, 0, 4};
    CHECK(!engine->runSystemView(boom, &vb, 1, err)); // exec threw
    CHECK(!err.empty());

    // Despite the throw, the stashed view was detached (neutered).
    double n = -1.0;
    CHECK(engine->eval("globalThis.__t.byteLength", &n, err));
    CHECK(n == 0.0);
    CHECK(engine->freeVmBuffer(buf.handle, err));
}

// Zero bindings is legal — the executor simply runs with no view arguments.
static void test_view_no_bindings()
{
    std::string err;
    std::unique_ptr<cjs::JsEngine> engine = cjs::createV8Engine(err);
    CHECK(engine != nullptr);
    if (!engine)
    {
        return;
    }
    CHECK(engine->eval("function tick(){ globalThis.__ticked = 1; }", nullptr, err));
    cjs::FunctionHandle tick = engine->getFunction("tick");
    CHECK(tick != cjs::kInvalidFunction);
    CHECK(engine->runSystemView(tick, nullptr, 0, err));
    double n = -1.0;
    CHECK(engine->eval("globalThis.__ticked", &n, err));
    CHECK(n == 1.0);
}

// --- R-OBS-005 interactive CDP inspector attach (issue #94) --------------------------------------

namespace
{
// True if any collected CDP message contains `needle` (a coarse substring probe — the JSON here is
// V8's own compact serialization, so a method/field literal is unambiguous).
bool anyContains(const std::vector<std::string>& msgs, const char* needle)
{
    for (const std::string& m : msgs)
    {
        if (m.find(needle) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}
} // namespace

// Attach a CDP session, feed a couple of protocol commands, and confirm responses flow back through
// the sink — the inspector attach/dispatch/detach lifecycle (the seam a standard CDP client uses).
static void test_inspector_attach_lifecycle()
{
    std::string err;
    std::unique_ptr<cjs::JsEngine> engine = cjs::createV8Engine(err);
    CHECK(engine != nullptr);
    if (!engine)
    {
        return;
    }
    std::unique_ptr<cjs::InspectorSession> session = engine->attachInspector(err);
    CHECK(session != nullptr);
    if (!session)
    {
        std::fprintf(stderr, "attachInspector failed: %s\n", err.c_str());
        return;
    }

    std::vector<std::string> msgs;
    session->onMessage([&msgs](std::string_view m) { msgs.emplace_back(m); });

    // Runtime.enable + Debugger.enable each get an id-tagged response; Debugger.enable also emits a
    // scriptParsed-class stream once a script is parsed (exercised in the pause test below).
    CHECK(session->dispatch(R"({"id":1,"method":"Runtime.enable"})", err));
    CHECK(session->dispatch(R"({"id":2,"method":"Debugger.enable"})", err));
    CHECK(!msgs.empty());
    CHECK(anyContains(msgs, "\"id\":1"));
    CHECK(anyContains(msgs, "\"id\":2"));

    // An empty message is rejected before it reaches V8 (a malformed non-JSON is reported by V8 as a
    // CDP error RESPONSE, not via the return value — so only the empty case returns false here).
    err.clear();
    CHECK(!session->dispatch("", err));
    CHECK(!err.empty());

    // Detach = destroy the session (RAII); the engine (Isolate owner) outlives it.
    session.reset();
}

// The pause loop end-to-end WITHOUT source maps: a `debugger;` statement pauses execution, the pump
// dispatches Debugger.resume, and the statement AFTER the pause runs (proving resume worked). The
// source-MAPPED breakpoint (authored .ts -> generated JS) is proven in runtime/ts/test_ts_debug.
static void test_inspector_debugger_statement_pause()
{
    std::string err;
    std::unique_ptr<cjs::JsEngine> engine = cjs::createV8Engine(err);
    CHECK(engine != nullptr);
    if (!engine)
    {
        return;
    }
    std::unique_ptr<cjs::InspectorSession> session = engine->attachInspector(err);
    CHECK(session != nullptr);
    if (!session)
    {
        return;
    }

    std::vector<std::string> msgs;
    session->onMessage([&msgs](std::string_view m) { msgs.emplace_back(m); });
    CHECK(session->dispatch(R"({"id":1,"method":"Debugger.enable"})", err));

    // On pause, resume once. `resumed` guards against being called without a prior pause.
    bool paused_seen = false;
    session->onPause([&]() -> bool {
        paused_seen = true;
        std::string e;
        session->dispatch(R"({"id":100,"method":"Debugger.resume"})", e);
        return true; // issued a resume — leave the pause loop
    });

    // `debugger;` pauses (Debugger is enabled); the assignment after it runs only once resumed.
    CHECK(session->run("debugger; globalThis.__paused_ok = 7;", "test://pause.js", err));
    CHECK(paused_seen);
    CHECK(anyContains(msgs, "Debugger.paused"));

    // The post-pause statement executed => resume drove the program to completion.
    double n = 0.0;
    CHECK(engine->eval("globalThis.__paused_ok", &n, err));
    CHECK(n == 7.0);
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
    test_view_element_width();
    test_trivial_eval();
    test_host_to_js();
    test_js_to_host();
    test_shape_b_vm_buffer();
    test_view_zero_copy_roundtrip();
    test_view_detach_neuters_retained();
    test_view_multi_binding_set();
    test_view_bad_bindings();
    test_view_detach_on_throw();
    test_view_no_bindings();
    test_inspector_seam_present();
    test_inspector_attach_lifecycle();
    test_inspector_debugger_statement_pause();
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
    test_view_element_width();
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
