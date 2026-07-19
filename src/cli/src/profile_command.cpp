// `context profile gc` implementation (see profile_command.h).

#include "context/cli/profile_command.h"

#include "context/cli/args.h" // the exported strict-u64 parser (context::cli::parse_u64)
#include "context/runtime/js/gc_errors.h"
#include "context/runtime/js/js_host.h"
#include "context/runtime/profile/gc_channel.h"
#include "context/runtime/profile/snapshot.h"
#include "context/runtime/profile/span_channel.h"
#include "context/runtime/profile/trace_export.h"
#include "context/runtime/session/session.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <optional>
#include <string>

namespace context::cli
{

using editor::contract::Envelope;
using editor::contract::Json;
namespace cjs = context::runtime::js;
namespace profile = context::runtime::profile;
namespace session = context::runtime::session;

namespace
{
const std::string* flag(const std::map<std::string, std::string>& flags, const char* name)
{
    auto it = flags.find(name);
    return it != flags.end() ? &it->second : nullptr;
}

// Unsigned flag values go through the exported context::cli::parse_u64 (args.h) — the
// digit-only, overflow-safe parser test_cli.cpp pins — rather than a third file-local copy of the
// session_command.cpp stoull variant. parse_ms stays local: no shared double parser exists yet.
std::optional<double> parse_ms(const std::string& s)
{
    try
    {
        std::size_t pos = 0;
        const double v = std::stod(s, &pos);
        if (pos != s.size())
            return std::nullopt;
        return v;
    }
    catch (...)
    {
        return std::nullopt;
    }
}

// The most samples inlined into the envelope — the aggregates always cover EVERY pause; the
// per-sample list is a bounded illustration (R-CLI-017 large-result discipline).
constexpr std::size_t kMaxEnvelopeSamples = 128;

Envelope profile_gc(const std::map<std::string, std::string>& flags)
{
    // --- resolve the workload parameters -------------------------------------------------------
    std::uint64_t ticks = 60;
    if (const std::string* t = flag(flags, "ticks"))
    {
        const std::optional<std::uint64_t> parsed = parse_u64(*t);
        if (!parsed.has_value() || *parsed == 0)
            return Envelope::failure("usage.invalid", "--ticks must be a positive integer");
        ticks = *parsed;
    }
    std::uint64_t churn = 2000;
    if (const std::string* c = flag(flags, "churn"))
    {
        const std::optional<std::uint64_t> parsed = parse_u64(*c);
        if (!parsed.has_value())
            return Envelope::failure("usage.invalid", "--churn must be an unsigned integer");
        churn = *parsed;
    }

    session::SessionConfig config; // the demo scenario at the 60 Hz fixed timestep
    session::Session sim(config);

    // The R-SIM-008 budget is defined relative to the fixed timestep: default a quarter of the
    // tick gap (~4.17 ms at 60 Hz) until the R-QA-007 min-spec floors pin a measured figure.
    double budget_ms = 0.25 * (1000.0 / static_cast<double>(sim.tick_hz()));
    if (const std::string* b = flag(flags, "budget-ms"))
    {
        const std::optional<double> parsed = parse_ms(*b);
        if (!parsed.has_value() || !std::isfinite(*parsed) || !(*parsed > 0.0))
            return Envelope::failure(cjs::kGcInvalidBudgetCode,
                                     "--budget-ms must be a finite positive duration in "
                                     "milliseconds");
        budget_ms = *parsed;
    }
    cjs::GcWindowOptions window_options;
    window_options.budget_ms = budget_ms;
    if (const std::string* tb = flag(flags, "trigger-bytes"))
    {
        // Reject 0 explicitly (mirrors the --ticks guard): trigger_bytes == 0 disables the growth
        // trigger inside gcWindow while this branch also clears force_collect, so accepting it
        // would silently run a profile that NEVER collects — the opposite of the "collect on any
        // growth" reading the describe text ("at least this many bytes") invites.
        const std::optional<std::uint64_t> parsed = parse_u64(*tb);
        if (!parsed.has_value() || *parsed == 0)
            return Envelope::failure("usage.invalid",
                                     "--trigger-bytes must be a positive integer (omit the flag "
                                     "to force-collect every window)");
        window_options.trigger_bytes = *parsed;
        window_options.force_collect = false;
    }
    else
    {
        window_options.force_collect = true; // default: collect every window
    }

    // --- boot the JS VM (fail-closed on a stub build) -------------------------------------------
    std::string err;
    std::unique_ptr<cjs::JsEngine> engine = cjs::createV8Engine(err);
    if (engine == nullptr)
        return Envelope::failure(cjs::kGcUnavailableCode,
                                 "the in-process JS VM is not built into this binary: " + err);

    // The churn function: allocate + discard short-lived objects (what a hot TS system that
    // ignores the pooled/no-alloc ctx.* APIs would do to the heap).
    {
        char code[192];
        std::snprintf(code, sizeof(code),
                      "function __profile_churn() { var a = []; for (var i = 0; i < %llu; i++) { "
                      "a.push({x: i}); } return a.length; }",
                      static_cast<unsigned long long>(churn));
        if (!engine->eval(code, nullptr, err))
            return Envelope::failure(cjs::kGcWindowFailedCode,
                                     "workload eval failed: " + err);
    }
    const cjs::FunctionHandle churn_fn = engine->getFunction("__profile_churn");
    if (churn_fn == cjs::kInvalidFunction)
        return Envelope::failure(cjs::kGcWindowFailedCode, "workload function did not resolve");

    // --- run: mid-tick churn via the observer; the GC window + drain at every tick boundary ----
    profile::GcPauseChannel channel;
    // Discard GC activity provoked by VM boot + the workload eval above: those pauses predate
    // tick 0, so leaving them in the ring would let the first inter-tick drain mis-attribute
    // them as mid-tick gameplay pauses (inflating the R-SIM-008-policed aggregate). clear()
    // wipes the boot records/aggregates but keeps the engine-drop high-water mark, so only
    // in-run drops fail the budget verdict.
    channel.drain(*engine, 0);
    channel.clear();
    std::string run_err;
    bool run_ok = true;
    std::uint64_t collected_windows = 0;
    sim.set_system_observer(
        [&](std::uint64_t, std::size_t, const std::string&, const kernel::World&)
        {
            if (!run_ok)
                return;
            std::string eval_err;
            if (!engine->callFunction(churn_fn, nullptr, 0, nullptr, eval_err))
            {
                run_ok = false;
                run_err = "churn call failed: " + eval_err;
            }
        });
    sim.set_inter_tick_hook(
        [&](std::uint64_t completed_tick)
        {
            if (!run_ok)
                return;
            cjs::GcWindowResult result;
            std::string window_err;
            if (!engine->gcWindow(window_options, result, window_err))
            {
                run_ok = false;
                run_err = "gc window failed: " + window_err;
                return;
            }
            if (result.collected)
                ++collected_windows;
            channel.drain(*engine, completed_tick);
        });

    const session::StepResult stepped = sim.step(ticks);
    if (!run_ok)
        return Envelope::failure(cjs::kGcWindowFailedCode, run_err);

    cjs::GcHeapStats heap;
    if (!engine->gcHeapStats(heap, err))
        return Envelope::failure(cjs::kGcWindowFailedCode, "heap stats failed: " + err);

    // --- report the channel (mirroring the session envelope shapes) ----------------------------
    const profile::GcPauseAggregates& agg = channel.aggregates();
    Json data = Json::object();
    data.set("channel", Json(std::string("sim.js.gc_pause")));
    data.set("simTick", Json(static_cast<std::uint64_t>(stepped.sim_tick)));
    data.set("tickCount", Json(ticks));
    data.set("tickHz", Json(static_cast<std::uint64_t>(sim.tick_hz())));
    data.set("budgetMs", Json(budget_ms));
    data.set("churnPerSystem", Json(churn));
    data.set("forcedWindows", Json(window_options.force_collect));
    data.set("collectedWindows", Json(collected_windows));

    Json aggregates = Json::object();
    aggregates.set("pauseCount", Json(agg.pause_count));
    aggregates.set("inWindowCount", Json(agg.in_window_count));
    aggregates.set("totalPauseMs", Json(agg.total_pause_ms));
    aggregates.set("maxPauseMs", Json(agg.max_pause_ms));
    aggregates.set("maxMidTickPauseMs", Json(agg.max_mid_tick_pause_ms));
    aggregates.set("dropped", Json(agg.dropped));
    data.set("aggregates", std::move(aggregates));

    Json samples = Json::array();
    std::size_t emitted = 0;
    for (const profile::GcPauseSample& s : channel.samples())
    {
        if (emitted++ == kMaxEnvelopeSamples)
            break;
        Json entry = Json::object();
        entry.set("tick", Json(s.tick));
        entry.set("durationMs", Json(s.duration_ms));
        entry.set("kind", Json(static_cast<std::uint64_t>(s.kind)));
        entry.set("inWindow", Json(s.in_window));
        samples.push_back(std::move(entry));
    }
    data.set("samples", std::move(samples));
    data.set("samplesTruncated", Json(channel.samples().size() > kMaxEnvelopeSamples));

    Json heap_json = Json::object();
    heap_json.set("usedBytes", Json(heap.used_bytes));
    heap_json.set("totalBytes", Json(heap.total_bytes));
    heap_json.set("externalBytes", Json(heap.external_bytes));
    data.set("heap", std::move(heap_json));

    data.set("withinBudget", Json(channel.within_budget(budget_ms)));
    return Envelope::success(std::move(data));
}

// --- `context profile session` — the a15 unified profiling surface (L-47 / R-OBS-002/004) --------
// Step a headless session N ticks, emitting the L-47 profiler channels as ONE JSON snapshot: the
// scheduler's per-system CPU spans (native lane, always present), the JS churn lane (script — only
// when the in-process VM is built), and the GC-pause channel FOLDED into the same surface
// (R-SIM-008 — the M6 X1 channel, reused not reimplemented). Unlike `profile gc`, this verb does NOT
// refuse on a stub-JS build: spans + counters are pure C++ and answer headless everywhere; the `gc`
// block honestly reports `available:false` when no VM is present. `--trace-out <path>` additionally
// writes a Chrome-trace file importable into Tracy/Perfetto (L-47 deep-capture export).

// Serialize the flattened snapshot to the envelope's data object.
Json snapshot_to_json(const profile::ProfileSnapshot& snap, bool spans_truncated)
{
    Json data = Json::object();
    data.set("channel", Json(std::string("profile.session")));
    data.set("tickCount", Json(snap.tick_count));
    data.set("tickHz", Json(snap.tick_hz));
    data.set("systemCount", Json(snap.system_count));
    data.set("totalCpuMs", Json(snap.total_cpu_ms));

    Json spans = Json::array();
    for (const profile::SystemStat& s : snap.systems)
    {
        Json entry = Json::object();
        entry.set("system", Json(s.name));
        entry.set("lane", Json(std::string(profile::lane_name(s.lane))));
        entry.set("callCount", Json(s.call_count));
        entry.set("totalMs", Json(s.total_ms));
        entry.set("maxMs", Json(s.max_ms));
        spans.push_back(std::move(entry));
    }
    data.set("spans", std::move(spans));
    data.set("spansTruncated", Json(spans_truncated));

    Json lanes = Json::array();
    for (const profile::LaneCounters& l : snap.lanes)
    {
        Json entry = Json::object();
        entry.set("lane", Json(std::string(profile::lane_name(l.lane))));
        entry.set("spanCount", Json(l.span_count));
        entry.set("totalMs", Json(l.total_ms));
        lanes.push_back(std::move(entry));
    }
    data.set("lanes", std::move(lanes));

    Json gc = Json::object();
    gc.set("available", Json(snap.gc.available));
    if (!snap.gc.available)
        gc.set("reason", Json(snap.gc.reason));
    gc.set("pauseCount", Json(snap.gc.pause_count));
    gc.set("inWindowCount", Json(snap.gc.in_window_count));
    gc.set("totalPauseMs", Json(snap.gc.total_pause_ms));
    gc.set("maxPauseMs", Json(snap.gc.max_pause_ms));
    gc.set("maxMidTickPauseMs", Json(snap.gc.max_mid_tick_pause_ms));
    gc.set("dropped", Json(snap.gc.dropped));
    gc.set("budgetMs", Json(snap.gc.budget_ms));
    gc.set("withinBudget", Json(snap.gc.within_budget));
    data.set("gc", std::move(gc));
    return data;
}

Envelope profile_session(const std::map<std::string, std::string>& flags)
{
    // --- resolve workload parameters ------------------------------------------------------------
    std::uint64_t ticks = 60;
    if (const std::string* t = flag(flags, "ticks"))
    {
        const std::optional<std::uint64_t> parsed = parse_u64(*t);
        if (!parsed.has_value() || *parsed == 0)
            return Envelope::failure("usage.invalid", "--ticks must be a positive integer");
        ticks = *parsed;
    }
    std::uint64_t churn = 2000;
    if (const std::string* c = flag(flags, "churn"))
    {
        const std::optional<std::uint64_t> parsed = parse_u64(*c);
        if (!parsed.has_value())
            return Envelope::failure("usage.invalid", "--churn must be an unsigned integer");
        churn = *parsed;
    }
    const std::string* trace_out = flag(flags, "trace-out");

    session::SessionConfig config; // the demo scenario at the 60 Hz fixed timestep
    session::Session sim(config);

    // --- per-system CPU spans (always available — pure C++, no VM needed) ------------------------
    profile::SpanChannel spans;
    for (const std::string& name : sim.system_names())
        spans.register_system(name, profile::Lane::Native);
    sim.set_system_span_sink(
        [&](std::uint64_t tick, std::size_t index, const std::string&, double duration_ms)
        { spans.record(tick, static_cast<std::uint32_t>(index), duration_ms); });

    // --- optional JS lane + GC channel (only when the in-process VM is built) --------------------
    profile::GcPauseChannel gc_channel;
    profile::GcSummary gc_summary;
    // The R-SIM-008 budget: a quarter of the tick gap (~4.17 ms at 60 Hz) — same default as `gc`.
    const double budget_ms = 0.25 * (1000.0 / static_cast<double>(sim.tick_hz()));
    gc_summary.budget_ms = budget_ms;

    std::string boot_err;
    std::unique_ptr<cjs::JsEngine> engine = cjs::createV8Engine(boot_err);
    std::string run_err;
    bool run_ok = true;

    // The observer + inter-tick-hook lambdas set up below are stored on `sim` and invoked later,
    // from `sim.step(ticks)`, AFTER the `if (engine != nullptr)` block below has exited — so every
    // local a lambda captures BY REFERENCE must live at THIS (function) scope, never nested inside
    // that block, or the captured reference dangles once the block's scope ends (an ASan-caught
    // stack-use-after-scope: churn_fn first, then churn_ms_this_tick/js_system/window_options hit the
    // SAME bug on the next CI round). Declared here unconditionally and populated only when
    // engine != nullptr below; harmless-unused on a stub (non-V8) build.
    std::uint32_t js_system = 0;
    cjs::FunctionHandle churn_fn = cjs::kInvalidFunction;
    cjs::GcWindowOptions window_options;
    double churn_ms_this_tick = 0.0;

    if (engine != nullptr)
    {
        // A Script-lane system representing the per-tick TS gameplay churn (R-OBS-004: the script
        // lane is legible next to the native systems). Registered AFTER the native systems, so the
        // native span sink's session indices stay aligned.
        js_system = spans.register_system("js.gameplay", profile::Lane::Script);

        char code[192];
        std::snprintf(code, sizeof(code),
                      "function __profile_churn() { var a = []; for (var i = 0; i < %llu; i++) { "
                      "a.push({x: i}); } return a.length; }",
                      static_cast<unsigned long long>(churn));
        if (!engine->eval(code, nullptr, boot_err))
            return Envelope::failure(cjs::kGcWindowFailedCode, "workload eval failed: " + boot_err);
        churn_fn = engine->getFunction("__profile_churn");
        if (churn_fn == cjs::kInvalidFunction)
            return Envelope::failure(cjs::kGcWindowFailedCode, "workload function did not resolve");

        window_options.budget_ms = budget_ms;
        window_options.force_collect = true; // collect every window (the sustained-profile default)

        // Discard boot/eval-provoked GC before tick 0 (mirrors `profile gc`).
        gc_channel.drain(*engine, 0);
        gc_channel.clear();

        sim.set_system_observer(
            [&](std::uint64_t, std::size_t, const std::string&, const kernel::World&)
            {
                if (!run_ok)
                    return;
                const auto t0 = std::chrono::steady_clock::now();
                std::string eval_err;
                if (!engine->callFunction(churn_fn, nullptr, 0, nullptr, eval_err))
                {
                    run_ok = false;
                    run_err = "churn call failed: " + eval_err;
                    return;
                }
                const auto t1 = std::chrono::steady_clock::now();
                churn_ms_this_tick +=
                    std::chrono::duration<double, std::milli>(t1 - t0).count();
            });
        sim.set_inter_tick_hook(
            [&](std::uint64_t completed_tick)
            {
                if (!run_ok)
                    return;
                cjs::GcWindowResult result;
                std::string window_err;
                if (!engine->gcWindow(window_options, result, window_err))
                {
                    run_ok = false;
                    run_err = "gc window failed: " + window_err;
                    return;
                }
                gc_channel.drain(*engine, completed_tick);
                // One aggregated Script-lane span per tick (the whole tick's JS churn).
                spans.record(completed_tick, js_system, churn_ms_this_tick);
                churn_ms_this_tick = 0.0;
            });
    }

    const session::StepResult stepped = sim.step(ticks);
    if (!run_ok)
        return Envelope::failure(cjs::kGcWindowFailedCode, run_err);

    // --- assemble the GC block ------------------------------------------------------------------
    if (engine != nullptr)
    {
        const profile::GcPauseAggregates& agg = gc_channel.aggregates();
        gc_summary.available = true;
        gc_summary.pause_count = agg.pause_count;
        gc_summary.in_window_count = agg.in_window_count;
        gc_summary.total_pause_ms = agg.total_pause_ms;
        gc_summary.max_pause_ms = agg.max_pause_ms;
        gc_summary.max_mid_tick_pause_ms = agg.max_mid_tick_pause_ms;
        gc_summary.dropped = agg.dropped;
        gc_summary.within_budget = gc_channel.within_budget(budget_ms);
    }
    else
    {
        gc_summary.available = false;
        gc_summary.reason =
            "the in-process JS VM is not built into this binary (" + boot_err +
            ") — per-system spans + counters are reported; GC-pause attribution needs the VM";
    }

    profile::ProfileSnapshot snap = profile::build_snapshot(spans, ticks, sim.tick_hz());
    snap.gc = gc_summary;

    Json data = snapshot_to_json(snap, spans.overflowed());
    data.set("simTick", Json(static_cast<std::uint64_t>(stepped.sim_tick)));

    // --- optional Tracy/Perfetto trace export ---------------------------------------------------
    if (trace_out != nullptr)
    {
        std::ofstream file(*trace_out, std::ios::binary | std::ios::trunc);
        if (!file)
            return Envelope::failure("internal.error",
                                     "could not open trace output path: " + *trace_out);
        const std::uint64_t events = profile::write_chrome_trace(
            spans, engine != nullptr ? &gc_channel : nullptr, sim.tick_hz(), file);
        file.flush();
        if (!file)
            return Envelope::failure("internal.error",
                                     "could not write trace output: " + *trace_out);
        Json trace = Json::object();
        trace.set("written", Json(true));
        trace.set("file", Json(*trace_out));
        trace.set("format", Json(std::string("chrome-trace-event")));
        trace.set("events", Json(events));
        data.set("trace", std::move(trace));
    }

    return Envelope::success(std::move(data));
}
} // namespace

Envelope run_profile(const std::string& verb, const std::map<std::string, std::string>& /*bound*/,
                     const std::map<std::string, std::string>& flags)
{
    if (verb == "gc")
        return profile_gc(flags);
    if (verb == "session")
        return profile_session(flags);
    return Envelope::failure("usage.unknown_verb", "unknown profile verb: '" + verb + "'");
}

} // namespace context::cli
