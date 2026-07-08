// R-QA-013 keystone integration test (M3 headline DoD): gameplay authored in TypeScript mutates the
// SHARED World through the zero-copy (query, executor) path.
//
// Flow: register a declarative `demo:mover` component -> create entities with known records ->
// generate its derived TS accessor (accessor_codegen.h) -> bundle an AUTHORED TS system that imports
// the accessor (esbuild --bundle --format=iife) -> eval the bundle in the task-2a V8 host -> resolve
// the executor and run it ONCE via run_system (select -> gather -> runSystemView -> scatter) -> observe
// the mutation in the derived World, and assert the view was detached/neutered at exit (R-LANG-009).
//
// CI-ONLY for its V8 dependency path: it links context_js, whose rusty_v8 prebuilt only links on the
// 3-OS CI build legs; the local Strawberry-GCC Windows dev gate builds the js STUB. So it mirrors the
// established CONTEXT_JS_HAS_V8 split (test_ts_in_v8.cpp): the full in-V8 keystone on the CI legs, and
// on the local stub toolchain a reduced assertion that the generated accessor still BUNDLES through
// esbuild (a real local check of the codegen's TS validity) and the V8 backend reports unavailable —
// so `ctest --preset dev` stays green either way.

#include "context/editor/component/component_registry.h"
#include "context/editor/system/accessor_codegen.h"
#include "context/editor/system/system.h"
#include "context/kernel/entity.h"
#include "context/kernel/world.h"
#include "context/runtime/js/js_host.h"
#include "context/runtime/ts/ts_toolchain.h"

#include "system_test.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace csys = context::editor::system;
namespace ccomp = context::editor::component;
namespace ck = context::kernel;
namespace cjs = context::runtime::js;
namespace cts = context::runtime::ts;

#ifndef CONTEXT_ESBUILD_PATH
#error "CONTEXT_ESBUILD_PATH must be defined by CMake (the staged esbuild binary path)"
#endif
#ifndef CONTEXT_SYSTEM_EXAMPLES_DIR
#error "CONTEXT_SYSTEM_EXAMPLES_DIR must be defined by CMake (the authored .ts examples dir)"
#endif

namespace
{
const std::string kEsbuild = CONTEXT_ESBUILD_PATH;
const std::string kExamples = CONTEXT_SYSTEM_EXAMPLES_DIR;

// The mover component the keystone drives: pos (f32x3) + max_speed (f32) + hits (u32).
ccomp::ComponentTypeSchema mover_schema()
{
    const std::string def = R"({
        "$id": "demo:mover", "version": 1,
        "fields": [
            {"name": "pos", "x-ctx-storage": "f32x3"},
            {"name": "max_speed", "x-ctx-storage": "f32"},
            {"name": "hits", "x-ctx-storage": "u32"}
        ]
    })";
    std::vector<std::string> problems;
    std::optional<ccomp::ComponentTypeSchema> t = ccomp::compile_component_type(def, problems);
    CHECK(t.has_value());
    CHECK(problems.empty());
    return t.value_or(ccomp::ComponentTypeSchema{});
}

std::string read_file(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Bundle the authored movement system with its generated accessor into a self-executing JS module.
// Writes the generated `accessor.ts` next to a copy of the authored `movement_system.ts` in a fresh
// temp dir (so the system's `import "./accessor"` resolves), then esbuild-bundles it. Returns the JS,
// or empty + fills `err` on failure.
std::string bundle_system(const std::string& accessor_ts, std::string& err)
{
    std::error_code ec;
    // Cast the steady_clock rep to a concrete integer before std::to_string: the rep is
    // implementation-defined, and std::to_string on it is *ambiguous* under macOS libc++ (compiles
    // under GCC/MSVC) — the profile test.md § macOS libc++ overload-resolution blind spot.
    const auto now =
        static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path(ec) / ("context-system-keystone-" + std::to_string(now));
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    if (ec)
    {
        err = "could not create temp bundle dir: " + ec.message();
        return {};
    }

    {
        std::ofstream(dir / "accessor.ts", std::ios::binary) << accessor_ts;
        const std::string authored = read_file(std::filesystem::path(kExamples) / "movement_system.ts");
        if (authored.empty())
        {
            err = "could not read authored movement_system.ts";
            return {};
        }
        std::ofstream(dir / "system.ts", std::ios::binary) << authored;
    }

    std::unique_ptr<cts::TsToolchain> tc = cts::createEsbuildToolchain(kEsbuild, err);
    if (!tc)
    {
        return {};
    }
    cts::TranspileOptions opts;
    opts.bundle = true;
    opts.format = cts::ModuleFormat::Iife;
    const cts::TranspileResult r = tc->transpile((dir / "system.ts").string(), opts);
    std::filesystem::remove_all(dir, ec); // best-effort cleanup
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
// Read a f32 field lane / a u32 field from an entity's live record (through the raw World storage).
float read_f32(const ck::World& w, ck::Entity e, ck::ComponentId id, std::size_t off)
{
    const auto* r = static_cast<const unsigned char*>(w.get_raw(e, id));
    float v = 0.0F;
    std::memcpy(&v, r + off, sizeof(v));
    return v;
}
std::uint32_t read_u32(const ck::World& w, ck::Entity e, ck::ComponentId id, std::size_t off)
{
    const auto* r = static_cast<const unsigned char*>(w.get_raw(e, id));
    std::uint32_t v = 0;
    std::memcpy(&v, r + off, sizeof(v));
    return v;
}
} // namespace

int main()
{
    // 1) Register the declarative component + populate a World with known mover records.
    ck::World world;
    ccomp::ComponentTypeRegistry reg;
    const ccomp::RegisteredComponentType& mover = reg.register_type(mover_schema());
    const ccomp::ComponentField* pos = mover.schema.field("pos");
    const ccomp::ComponentField* speed = mover.schema.field("max_speed");
    const ccomp::ComponentField* hits = mover.schema.field("hits");
    CHECK(pos != nullptr && speed != nullptr && hits != nullptr);

    struct Seed
    {
        float x;
        float speed;
        std::uint32_t hits;
    };
    const Seed seeds[3] = {{1.0F, 0.5F, 0}, {10.0F, 2.0F, 7}, {-3.0F, 4.0F, 100}};
    std::vector<ck::Entity> ents;
    for (const Seed& s : seeds)
    {
        const ck::Entity e = world.create();
        auto* r = static_cast<unsigned char*>(reg.add_default(world, e, mover));
        CHECK(r != nullptr);
        std::memcpy(r + pos->offset, &s.x, sizeof(s.x));       // pos.x (lane 0)
        std::memcpy(r + speed->offset, &s.speed, sizeof(s.speed));
        std::memcpy(r + hits->offset, &s.hits, sizeof(s.hits));
        ents.push_back(e);
    }

    // 2) Derive the accessor TS + bundle the authored system that imports it.
    std::string err;
    const std::string accessor = csys::generate_component_accessor_ts(mover.schema);
    const std::string js = bundle_system(accessor, err);
    CHECK(!js.empty());
    if (js.empty())
    {
        std::fprintf(stderr, "bundle error: %s\n", err.c_str());
        return 1;
    }

    // 3) Evaluate the bundle in the V8 host (installs globalThis.__moveSystem).
    std::unique_ptr<cjs::JsEngine> engine = cjs::createV8Engine(err);
    CHECK(engine != nullptr);
    if (!engine)
    {
        return 1;
    }
    CHECK(engine->eval(js, nullptr, err));

    // 4) Resolve the executor + run the (query, executor) system ONCE over the mover column.
    const cjs::FunctionHandle exec = engine->getFunction("__moveSystem");
    CHECK(exec != cjs::kInvalidFunction);
    const bool ran = csys::run_system(world, *engine, exec, {&mover}, err);
    CHECK(ran);
    if (!ran)
    {
        std::fprintf(stderr, "run_system error: %s\n", err.c_str());
    }

    // 5) The mutation is observable in the DERIVED World: each mover's x advanced by its speed and its
    //    hit counter ticked — proving authored TS mutated the shared World through the zero-copy path.
    for (std::size_t i = 0; i < ents.size(); ++i)
    {
        const float x = read_f32(world, ents[i], mover.component_id, pos->offset);
        const std::uint32_t h = read_u32(world, ents[i], mover.component_id, hits->offset);
        CHECK(x == seeds[i].x + seeds[i].speed);
        CHECK(h == seeds[i].hits + 1);
        // Untouched lanes of pos (y, z) stayed zero — the system only wrote lane 0.
        const float y = read_f32(world, ents[i], mover.component_id, pos->offset + sizeof(float));
        CHECK(y == 0.0F);
    }

    // 6) R-LANG-009 gate preserved: the view the system retained on globalThis is neutered post-run.
    double n = -1.0;
    CHECK(engine->eval("globalThis.__lastMoverView.byteLength", &n, err));
    CHECK(n == 0.0);

    if (systemtest::g_failures == 0)
    {
        std::printf("system-in-v8: authored TS mutated the shared World through zero-copy views; "
                    "view detached at exit\n");
    }
    SYSTEM_TEST_MAIN_END();
}

#else // !CONTEXT_JS_HAS_V8 — local Strawberry-GCC dev gate (js stub). esbuild still works here.

int main()
{
    // The codegen + toolchain half IS locally exercisable: prove the DERIVED accessor is valid TS that
    // esbuild bundles together with the authored system (a real local check of codegen validity), and
    // that the V8 backend correctly reports unavailable (so the local non-dependency gate is green; the
    // CI build legs run the real in-V8 keystone above).
    const ccomp::ComponentTypeSchema mover = mover_schema();
    const std::string accessor = csys::generate_component_accessor_ts(mover);
    CHECK(!accessor.empty());

    std::string err;
    const std::string js = bundle_system(accessor, err);
    CHECK(!js.empty());
    if (js.empty())
    {
        std::fprintf(stderr, "bundle error: %s\n", err.c_str());
        return 1;
    }
    CHECK(js.find("__moveSystem") != std::string::npos);

    CHECK(!cjs::v8BackendAvailable());
    std::unique_ptr<cjs::JsEngine> engine = cjs::createV8Engine(err);
    CHECK(engine == nullptr);

    if (systemtest::g_failures == 0)
    {
        std::printf("system-in-v8: derived accessor bundles through esbuild; V8 backend not built on "
                    "this toolchain (stub) — CI build legs run the real in-V8 keystone\n");
    }
    SYSTEM_TEST_MAIN_END();
}

#endif // CONTEXT_JS_HAS_V8
