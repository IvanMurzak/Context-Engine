// `context profile gc` implementation (see profile_command.h).

#include "context/cli/profile_command.h"

#include "context/runtime/js/gc_errors.h"
#include "context/runtime/js/js_host.h"
#include "context/runtime/profile/gc_channel.h"
#include "context/runtime/session/session.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
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

// Strict base-10 unsigned parse (the session_command.cpp contract: reject a leading '-' so the
// strtoull wraparound never smuggles a negative in; reject leading-zero octal and 0x hex).
std::optional<std::uint64_t> parse_u64(const std::string& s)
{
    const std::size_t first = s.find_first_not_of(" \t");
    if (first != std::string::npos && s[first] == '-')
        return std::nullopt;
    try
    {
        std::size_t pos = 0;
        const unsigned long long v = std::stoull(s, &pos, 10);
        if (pos != s.size())
            return std::nullopt;
        return static_cast<std::uint64_t>(v);
    }
    catch (...)
    {
        return std::nullopt;
    }
}

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
        const std::optional<std::uint64_t> parsed = parse_u64(*tb);
        if (!parsed.has_value())
            return Envelope::failure("usage.invalid",
                                     "--trigger-bytes must be an unsigned integer");
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
} // namespace

Envelope run_profile(const std::string& verb, const std::map<std::string, std::string>& /*bound*/,
                     const std::map<std::string, std::string>& flags)
{
    if (verb == "gc")
        return profile_gc(flags);
    return Envelope::failure("usage.unknown_verb", "unknown profile verb: '" + verb + "'");
}

} // namespace context::cli
