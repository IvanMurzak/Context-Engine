// R-QA-013 integration test (M7 T4 / a4, R-UI-001/006): the authored `context.ui` TypeScript HUD,
// transpiled + bundled by esbuild, RUNS in the shipped in-process V8 host and drives a headless
// context_ui UiTree through the doubles-only authoring binding shim (script_bindings.h). This is the
// DoD's headline proof — "authored TS builds the tree, dispatch an event, assert state readback":
//   1) bind the doubles-only `__ui_*` host primitives to a UiScriptContext (tree + numeric StateStore);
//   2) eval the bundled ui_hud.ts -> buildHud() constructs a HUD tree (panel + score label + health bar
//      + play button) and binds the label read-only to the score state;
//   3) dispatch a pointer-down on the button -> the C++ tree handler calls back into the VM (__ui_invoke)
//      -> the authored onClick scores points (the UI->state action path);
//   4) assert the state readback + the read-only data binding reflect the new score/health.
//
// It is CI-ONLY for its V8 dependency path (it links context_js, whose rusty_v8 prebuilt only links on
// the 3-OS CI build legs; the local Strawberry-GCC Windows dev gate builds the js STUB). So it mirrors
// test_ts_in_v8.cpp's CONTEXT_JS_HAS_V8 split: the full flow on the CI legs, and on the local stub
// toolchain a reduced assertion (esbuild still bundles the authored TS; the V8 backend correctly reports
// unavailable) so the local `ctest --preset dev` stays green.

#include "context/packages/ui/script_bindings.h"
#include "context/packages/ui/ui_tree.h"
#include "context/runtime/js/js_host.h"
#include "context/runtime/ts/ts_toolchain.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace uihud
{
int g_failures = 0;

inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
} // namespace uihud

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            uihud::fail(__FILE__, __LINE__, #cond);                                                \
    } while (false)

namespace cjs = context::runtime::js;
namespace cts = context::runtime::ts;
namespace cui = context::packages::ui;

#ifndef CONTEXT_ESBUILD_PATH
#error "CONTEXT_ESBUILD_PATH must be defined by CMake (the staged esbuild binary path)"
#endif
#ifndef CONTEXT_TS_EXAMPLES_DIR
#error "CONTEXT_TS_EXAMPLES_DIR must be defined by CMake (the authored .ts examples dir)"
#endif

namespace
{
const std::string kEsbuild = CONTEXT_ESBUILD_PATH;
const std::string kExamples = CONTEXT_TS_EXAMPLES_DIR;

// The score state key ui_hud.ts uses (kept in lockstep with examples/ui_hud.ts HudState). Shared by
// both the V8 and the stub branch; the V8-only keys (health / node-id stashes) are locals in the V8
// main() so the local GCC stub build carries no namespace-scope const the Clang CI legs would flag
// -Wunused-const-variable (the profile's GCC-vs-Clang unused-entity blind spot).
constexpr std::int32_t kScoreKey = 100;

// Bundle the authored HUD entrypoint (ui_hud.ts, importing context_ui.ts) to a self-executing module.
std::string bundleHud(std::string& err)
{
    std::unique_ptr<cts::TsToolchain> tc = cts::createEsbuildToolchain(kEsbuild, err);
    if (!tc)
    {
        return {};
    }
    cts::TranspileOptions opts;
    opts.bundle = true;
    opts.format = cts::ModuleFormat::Iife;
    cts::TranspileResult r = tc->transpile(kExamples + "/ui_hud.ts", opts);
    if (!r.ok)
    {
        err = r.diagnostics.empty() ? "bundle failed" : r.diagnostics.front().message;
        return {};
    }
    return r.js;
}
} // namespace

#ifdef CONTEXT_JS_HAS_V8

namespace
{
// Bind the doubles-only `__ui_*` host primitives to `ctx` and wire the dispatch->handler bridge to the
// VM's __ui_invoke callback. UiHostFunction is byte-identical to cjs::HostFunction (same signature), so
// the table entries bind directly. This install glue is the "registration" seam a4 owns; it lives with
// the test (js-coupled) so context_ui_script stays V8-free.
bool install_ui_bindings(cjs::JsEngine& engine, cui::UiScriptContext& ctx, std::string& err)
{
    for (const cui::UiHostBinding& b : cui::ui_host_bindings())
    {
        if (!engine.bindHostFunction(b.name, b.fn, &ctx, err))
        {
            return false;
        }
    }
    // The C++ tree handler closure calls ctx.invoke_handler -> this invoker -> the VM's __ui_invoke, so
    // an authored onClick runs in-VM on dispatch. Resolve __ui_invoke lazily (defined by the bundle).
    cjs::JsEngine* enginePtr = &engine;
    ctx.set_invoker([enginePtr](std::uint32_t handler_id, cui::Event& ev) {
        cjs::FunctionHandle invoke = enginePtr->getFunction("__ui_invoke");
        if (invoke == cjs::kInvalidFunction)
        {
            return;
        }
        const double args[4] = {static_cast<double>(handler_id),
                                static_cast<double>(static_cast<int>(ev.type)),
                                static_cast<double>(ev.pointer_x),
                                static_cast<double>(ev.pointer_y)};
        double result = 0.0;
        std::string ierr;
        enginePtr->callFunction(invoke, args, 4, &result, ierr);
    });
    return true;
}

// Read a NodeId a headless driver stashed into a state key (doubles-only: ids travel through state).
cui::NodeId node_from_state(const cui::UiScriptContext& ctx, std::int32_t key)
{
    return static_cast<cui::NodeId>(static_cast<std::uint32_t>(ctx.read_state(key)));
}
} // namespace

int main()
{
    // 1) authored TS -> JS (esbuild).
    std::string err;
    const std::string js = bundleHud(err);
    CHECK(!js.empty());
    if (js.empty())
    {
        std::fprintf(stderr, "bundle error: %s\n", err.c_str());
        return 1;
    }

    // 2) the headless tree + numeric state the authored HUD drives, bound to the V8 host.
    cui::UiTree tree;
    cui::StateStore state;
    cui::UiScriptContext ctx(tree, state);

    std::unique_ptr<cjs::JsEngine> engine = cjs::createV8Engine(err);
    CHECK(engine != nullptr);
    if (!engine)
    {
        return 1;
    }
    CHECK(install_ui_bindings(*engine, ctx, err));

    // 3) eval the bundle: buildHud() runs at module top level and constructs the HUD tree.
    CHECK(engine->eval(js, nullptr, err));

    // The V8-only state keys (in lockstep with examples/ui_hud.ts HudState) — locals so the stub build
    // carries no unused namespace-scope const the Clang legs would flag.
    constexpr std::int32_t kHealthKey = 200;
    constexpr std::int32_t kButtonNodeKey = 900;
    constexpr std::int32_t kScoreNodeKey = 901;

    // The tree was built FROM TS: root + panel + score label + health bar + play button = 5 live nodes.
    CHECK(tree.node_count() == 5);
    const cui::NodeId button = node_from_state(ctx, kButtonNodeKey);
    const cui::NodeId score = node_from_state(ctx, kScoreNodeKey);
    CHECK(button != cui::kInvalidNode);
    CHECK(score != cui::kInvalidNode);
    CHECK(tree.node(button) != nullptr && tree.node(button)->role == cui::Role::Button);
    CHECK(tree.node(score) != nullptr && tree.node(score)->role == cui::Role::Label);

    // Initial bound state: score 0, health 100 (written by the authored buildHud).
    CHECK(ctx.read_state(kScoreKey) == 0.0);
    CHECK(ctx.read_state(kHealthKey) == 100.0);
    CHECK(ctx.is_bound(score));
    CHECK(ctx.bound_value(score) == 0.0);

    // 4) dispatch a pointer-down on the play button: the C++ handler calls back into the VM, the
    //    authored onClick scores 10 points and costs 5 health (the UI->state action path).
    cui::Event ev;
    ev.type = cui::EventType::PointerDown;
    ev.target = button;
    ev.pointer_x = 24.0f;
    ev.pointer_y = 60.0f;
    tree.dispatch(ev);

    // State readback: the action mutated the store, and the read-only data binding reflects the score.
    CHECK(ctx.read_state(kScoreKey) == 10.0);
    CHECK(ctx.read_state(kHealthKey) == 95.0);
    CHECK(ctx.bound_value(score) == 10.0);

    // A second click accumulates (proves the real store + repeated in-VM handler dispatch).
    cui::Event ev2;
    ev2.type = cui::EventType::PointerDown;
    ev2.target = button;
    tree.dispatch(ev2);
    CHECK(ctx.read_state(kScoreKey) == 20.0);
    CHECK(ctx.read_state(kHealthKey) == 90.0);
    CHECK(ctx.bound_value(score) == 20.0);

    if (uihud::g_failures == 0)
    {
        std::printf("ui-ts-authoring: authored context.ui HUD built the tree in V8; dispatch->onClick "
                    "state readback OK (score=20, health=90)\n");
    }
    return uihud::g_failures == 0 ? 0 : 1;
}

#else // !CONTEXT_JS_HAS_V8 — local Strawberry-GCC dev gate (js stub). esbuild still bundles here.

int main()
{
    // The toolchain half IS locally exercisable even where V8 cannot link: prove the authored context.ui
    // HUD still bundles to JS (esbuild runs here), the bundle wires the doubles-only host seam + the
    // __ui_invoke callback, and the V8 backend correctly reports unavailable (so the local non-dependency
    // gate is green; the CI build legs run the real in-V8 flow above).
    std::string err;
    const std::string js = bundleHud(err);
    CHECK(!js.empty());
    if (js.empty())
    {
        std::fprintf(stderr, "bundle error: %s\n", err.c_str());
        return 1;
    }
    // The bundle references the doubles-only primitives + the callback the host relies on.
    CHECK(js.find("__ui_create") != std::string::npos);
    CHECK(js.find("__ui_on") != std::string::npos);
    CHECK(js.find("__ui_invoke") != std::string::npos);
    CHECK(js.find("buildHud") != std::string::npos);

    CHECK(!cjs::v8BackendAvailable());
    std::unique_ptr<cjs::JsEngine> engine = cjs::createV8Engine(err);
    CHECK(engine == nullptr);

    // The C++ binding shim itself is fully exercised WITHOUT V8 by ui-test_script_bindings (the local
    // gate). Prove it composes here too: a UiScriptContext builds + drives a tree with no engine.
    cui::UiTree tree;
    cui::StateStore state;
    cui::UiScriptContext ctx(tree, state);
    const cui::NodeId panel = ctx.create(tree.root(), cui::Role::Panel);
    const cui::NodeId label = ctx.create(panel, cui::Role::Label);
    ctx.write_state(kScoreKey, 0.0);
    CHECK(ctx.bind_value(label, kScoreKey));
    ctx.add_state(kScoreKey, 10.0);
    CHECK(ctx.bound_value(label) == 10.0);

    if (uihud::g_failures == 0)
    {
        std::printf("ui-ts-authoring: toolchain green; V8 backend not built on this toolchain (stub) — "
                    "CI build legs run the real in-V8 HUD flow\n");
    }
    return uihud::g_failures == 0 ? 0 : 1;
}

#endif // CONTEXT_JS_HAS_V8
