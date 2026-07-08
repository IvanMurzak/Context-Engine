// R-QA-013 keystone (M3 task-4a headline DoD): a C++ (native) system AND a TypeScript system both
// mutate the SHARED World, scheduled safely in the SAME tick by the parallel scheduler.
//
// Flow: register three declarative components — `demo:posx` + `demo:vel` (mutated by two C++ native
// systems) and `demo:health` (mutated by an authored TS system). Two native systems declare DISJOINT
// writes, so the DAG places them in ONE batch that runs concurrently (fork-join); the TS system is
// EXCLUDED from the DAG and runs on the single JS-VM lane, AFTER the native batch joins. One
// ParallelScheduler::run_tick then drives: [native batch: posx∥vel] → [TS lane: health via run_system].
// All three mutations are observed in the derived World; the TS view is neutered at exit (R-LANG-009).
//
// CI-ONLY for its V8 dependency path: it links context_js, whose rusty_v8 prebuilt only links on the
// 3-OS CI build legs; the local Strawberry-GCC Windows dev gate builds the js STUB. So it mirrors the
// established CONTEXT_JS_HAS_V8 split (editor/system's test_system_in_v8): the full C++-and-TS keystone
// on the CI legs, and on the local stub toolchain a reduced assertion that (a) the native batch still
// runs the two C++ systems in parallel and mutates the World, (b) the derived health accessor bundles
// through esbuild, and (c) the V8 backend reports unavailable — so `ctest --preset dev` stays green.
//
// The parallel-safety of the scheduler's OWN fork-join is proven, fully sanitizer-instrumented, by the
// V8-free test_schedule.cpp; this keystone additionally proves the cross-lane (C++ + TS) tick under the
// V8 host (its rusty_v8 worker-pool races are covered by the reused runtime/js TSan suppressions).

#include "context/editor/component/component_registry.h"
#include "context/editor/schedule/schedule.h"
#include "context/editor/system/accessor_codegen.h"
#include "context/editor/system/system.h"
#include "context/kernel/entity.h"
#include "context/kernel/world.h"
#include "context/runtime/js/js_host.h"
#include "context/runtime/ts/ts_toolchain.h"

#include "schedule_test.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace csch = context::editor::schedule;
namespace csys = context::editor::system;
namespace ccomp = context::editor::component;
namespace ck = context::kernel;
namespace cjs = context::runtime::js;
namespace cts = context::runtime::ts;

#ifndef CONTEXT_ESBUILD_PATH
#error "CONTEXT_ESBUILD_PATH must be defined by CMake (the staged esbuild binary path)"
#endif
#ifndef CONTEXT_SCHEDULE_EXAMPLES_DIR
#error "CONTEXT_SCHEDULE_EXAMPLES_DIR must be defined by CMake (the authored .ts examples dir)"
#endif

namespace
{
const std::string kEsbuild = CONTEXT_ESBUILD_PATH;
const std::string kExamples = CONTEXT_SCHEDULE_EXAMPLES_DIR;

ccomp::ComponentTypeSchema compile(const std::string& def)
{
    std::vector<std::string> problems;
    std::optional<ccomp::ComponentTypeSchema> t = ccomp::compile_component_type(def, problems);
    CHECK(t.has_value());
    CHECK(problems.empty());
    return t.value_or(ccomp::ComponentTypeSchema{});
}

ccomp::ComponentTypeSchema posx_schema() // a single f32 x-position
{
    return compile(R"({ "$id": "demo:posx", "version": 1,
        "fields": [{"name": "x", "x-ctx-storage": "f32"}] })");
}
ccomp::ComponentTypeSchema vel_schema() // a single f32 velocity
{
    return compile(R"({ "$id": "demo:vel", "version": 1,
        "fields": [{"name": "v", "x-ctx-storage": "f32"}] })");
}
ccomp::ComponentTypeSchema health_schema() // a single u32 hp
{
    return compile(R"({ "$id": "demo:health", "version": 1,
        "fields": [{"name": "hp", "x-ctx-storage": "u32"}] })");
}

std::string read_file(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Bundle the authored health system with its generated accessor into a self-executing JS module (same
// shape as editor/system's keystone). Returns the JS, or empty + fills `err` on failure.
std::string bundle_health_system(const std::string& accessor_ts, std::string& err)
{
    std::error_code ec;
    // Cast the steady_clock rep to a concrete integer before std::to_string: the rep is
    // implementation-defined, and std::to_string on it is *ambiguous* under macOS libc++.
    const auto now =
        static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path(ec) / ("context-schedule-keystone-" + std::to_string(now));
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    if (ec)
    {
        err = "could not create temp bundle dir: " + ec.message();
        return {};
    }
    {
        std::ofstream(dir / "accessor.ts", std::ios::binary) << accessor_ts;
        const std::string authored = read_file(std::filesystem::path(kExamples) / "health_system.ts");
        if (authored.empty())
        {
            err = "could not read authored health_system.ts";
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

// Seed a World with `n` entities carrying posx=x0, vel=v0, health=hp0. Returns the entity list + the
// three registered component types + their field offsets by out-params.
struct Setup
{
    ck::World world;
    ccomp::ComponentTypeRegistry reg;
    std::vector<ck::Entity> ents;
    ck::ComponentId posx_id = 0;
    ck::ComponentId vel_id = 0;
    ck::ComponentId health_id = 0;
    std::size_t posx_off = 0;
    std::size_t vel_off = 0;
    std::size_t health_off = 0;
    const ccomp::RegisteredComponentType* health_type = nullptr;
};

float read_f32(const ck::World& w, ck::Entity e, ck::ComponentId id, std::size_t off)
{
    const auto* r = static_cast<const unsigned char*>(w.get_raw(e, id));
    float v = 0.0F;
    if (r != nullptr)
    {
        std::memcpy(&v, r + off, sizeof(v));
    }
    return v;
}
[[maybe_unused]] std::uint32_t read_u32(const ck::World& w, ck::Entity e, ck::ComponentId id,
                                        std::size_t off)
{
    const auto* r = static_cast<const unsigned char*>(w.get_raw(e, id));
    std::uint32_t v = 0;
    if (r != nullptr)
    {
        std::memcpy(&v, r + off, sizeof(v));
    }
    return v;
}

// A native system that adds `d` to the f32 field at `off` of component `id` on every entity in `ents`.
csch::SystemFn advance_f32(ck::World& world, std::vector<ck::Entity> ents, ck::ComponentId id,
                           std::size_t off, float d)
{
    return [&world, ents = std::move(ents), id, off, d]
    {
        for (const ck::Entity e : ents)
        {
            auto* r = static_cast<unsigned char*>(world.get_raw(e, id));
            if (r != nullptr)
            {
                float v = 0.0F;
                std::memcpy(&v, r + off, sizeof(v));
                v += d;
                std::memcpy(r + off, &v, sizeof(v));
            }
        }
    };
}

// Register the two native systems (posx, vel) + a TS system placeholder into `reg`. The TS system's
// run closure is supplied by the caller (real run_system in V8 mode, a no-op locally).
void register_systems(csch::SystemRegistry& reg, Setup& s, csch::SystemFn ts_run)
{
    reg.add([&]
            {
                csch::SystemDesc d;
                d.name = "advancePosX";
                d.lane = csch::Lane::Native;
                d.access = csch::AccessSet::make({}, {s.posx_id});
                d.run = advance_f32(s.world, s.ents, s.posx_id, s.posx_off, 1.0F);
                return d;
            }());
    reg.add([&]
            {
                csch::SystemDesc d;
                d.name = "advanceVel";
                d.lane = csch::Lane::Native;
                d.access = csch::AccessSet::make({}, {s.vel_id});
                d.run = advance_f32(s.world, s.ents, s.vel_id, s.vel_off, 0.5F);
                return d;
            }());
    csch::SystemDesc td;
    td.name = "tsHealth";
    td.lane = csch::Lane::Ts;
    td.access = csch::AccessSet::make({}, {s.health_id}); // ignored for the TS lane
    td.run = std::move(ts_run);
    reg.add(std::move(td));
}

void build_setup(Setup& s)
{
    const ccomp::RegisteredComponentType& posx = s.reg.register_type(posx_schema());
    const ccomp::RegisteredComponentType& vel = s.reg.register_type(vel_schema());
    const ccomp::RegisteredComponentType& health = s.reg.register_type(health_schema());
    s.posx_id = posx.component_id;
    s.vel_id = vel.component_id;
    s.health_id = health.component_id;
    s.posx_off = posx.schema.field("x")->offset;
    s.vel_off = vel.schema.field("v")->offset;
    s.health_off = health.schema.field("hp")->offset;
    s.health_type = &health;

    for (int i = 0; i < 8; ++i)
    {
        const ck::Entity e = s.world.create();
        (void)s.reg.add_default(s.world, e, posx);
        (void)s.reg.add_default(s.world, e, vel);
        auto* hr = static_cast<unsigned char*>(s.reg.add_default(s.world, e, health));
        std::uint32_t hp = static_cast<std::uint32_t>(i); // distinct seed per entity
        std::memcpy(hr + s.health_off, &hp, sizeof(hp));
        s.ents.push_back(e);
    }
}

// Assert the schedule shape both modes rely on: the two native systems share ONE batch (disjoint
// writes → concurrent), the TS system is on the single lane (excluded from the DAG).
void check_schedule_shape(const csch::SystemRegistry& reg)
{
    const csch::Schedule sched = csch::build_schedule(reg);
    CHECK(sched.native_batches().size() == 1);
    CHECK(sched.native_batches().front().size() == 2); // posx ∥ vel in one parallel batch
    CHECK(sched.ts_lane().size() == 1);                // health on the single JS-VM lane
}

} // namespace

#ifdef CONTEXT_JS_HAS_V8

int main()
{
    Setup s;
    build_setup(s);

    // Derive the health accessor + bundle the authored TS system that imports it.
    std::string err;
    const std::string accessor = csys::generate_component_accessor_ts(s.health_type->schema);
    const std::string js = bundle_health_system(accessor, err);
    CHECK(!js.empty());
    if (js.empty())
    {
        std::fprintf(stderr, "bundle error: %s\n", err.c_str());
        return 1;
    }

    // Evaluate the bundle in the V8 host (installs globalThis.__healthSystem).
    std::unique_ptr<cjs::JsEngine> engine = cjs::createV8Engine(err);
    CHECK(engine != nullptr);
    if (!engine)
    {
        return 1;
    }
    CHECK(engine->eval(js, nullptr, err));
    const cjs::FunctionHandle exec = engine->getFunction("__healthSystem");
    CHECK(exec != cjs::kInvalidFunction);

    // The TS lane's runnable: run the (query, executor) health system through run_system on the JS lane.
    bool ts_ok = false;
    std::string ts_err;
    csch::SystemRegistry reg;
    register_systems(reg, s, [&]
                     {
                         ts_ok =
                             csys::run_system(s.world, *engine, exec, {s.health_type}, ts_err);
                     });
    check_schedule_shape(reg);

    // One tick: [native batch: advancePosX ∥ advanceVel] → [TS lane: health]. Both a C++ system and a
    // TS system mutate the shared World, scheduled safely, in a single tick.
    csch::ParallelScheduler sched({/*worker_count*/ 4, 0});
    sched.run_tick(reg);
    CHECK(ts_ok);
    if (!ts_ok)
    {
        std::fprintf(stderr, "run_system error: %s\n", ts_err.c_str());
    }

    // All three mutations landed in the derived World.
    for (std::size_t i = 0; i < s.ents.size(); ++i)
    {
        CHECK(read_f32(s.world, s.ents[i], s.posx_id, s.posx_off) == 1.0F);      // native: 0 + 1
        CHECK(read_f32(s.world, s.ents[i], s.vel_id, s.vel_off) == 0.5F);        // native: 0 + 0.5
        CHECK(read_u32(s.world, s.ents[i], s.health_id, s.health_off) == i + 1); // TS: seed i + 1
    }

    // R-LANG-009 gate preserved: the view the TS system retained on globalThis is neutered post-run.
    double n = -1.0;
    CHECK(engine->eval("globalThis.__lastHealthView.byteLength", &n, err));
    CHECK(n == 0.0);

    if (scheduletest::g_failures == 0)
    {
        std::printf("schedule-in-v8: a C++ system and a TS system both mutated the shared World, "
                    "scheduled safely in one tick; TS view detached at exit\n");
    }
    SCHEDULE_TEST_MAIN_END();
}

#else // !CONTEXT_JS_HAS_V8 — local Strawberry-GCC dev gate (js stub). esbuild still works here.

int main()
{
    Setup s;
    build_setup(s);

    // The TS lane cannot run without V8 locally, so its runnable is a no-op here; the native batch (two
    // C++ systems in parallel) still runs and mutates the World — the locally-exercisable half.
    csch::SystemRegistry reg;
    register_systems(reg, s, [] {});
    check_schedule_shape(reg);

    csch::ParallelScheduler sched({/*worker_count*/ 4, 0});
    sched.run_tick(reg);
    for (const ck::Entity e : s.ents)
    {
        CHECK(read_f32(s.world, e, s.posx_id, s.posx_off) == 1.0F); // C++ native systems ran in parallel
        CHECK(read_f32(s.world, e, s.vel_id, s.vel_off) == 0.5F);
    }

    // The derived health accessor is valid TS that esbuild bundles together with the authored system (a
    // real local check of codegen validity), and the V8 backend correctly reports unavailable.
    std::string err;
    const std::string accessor = csys::generate_component_accessor_ts(s.health_type->schema);
    CHECK(!accessor.empty());
    const std::string js = bundle_health_system(accessor, err);
    CHECK(!js.empty());
    if (js.empty())
    {
        std::fprintf(stderr, "bundle error: %s\n", err.c_str());
        return 1;
    }
    CHECK(js.find("__healthSystem") != std::string::npos);

    CHECK(!cjs::v8BackendAvailable());
    std::unique_ptr<cjs::JsEngine> engine = cjs::createV8Engine(err);
    CHECK(engine == nullptr);

    if (scheduletest::g_failures == 0)
    {
        std::printf("schedule-in-v8: native C++ batch mutated the World in parallel; health accessor "
                    "bundles through esbuild; V8 backend not built on this toolchain (stub) — CI build "
                    "legs run the real C++-and-TS keystone\n");
    }
    SCHEDULE_TEST_MAIN_END();
}

#endif // CONTEXT_JS_HAS_V8
