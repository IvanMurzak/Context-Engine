// `context profile gc` CLI backend tests (R-QA-013, M6 X1 / issue #188). The verb needs the
// in-process JS VM, so the test branches on which backend THIS build carries (the runtime split
// the js module established): on the V8 CI legs it asserts the full GC-pause channel envelope; on
// the local stub gate it asserts the fail-closed sim.gc.unavailable refusal. Flag validation and
// the unknown-verb path are backend-independent and assert on every toolchain.

#include "context/cli/app.h"
#include "context/cli/profile_command.h"
#include "context/runtime/js/gc_errors.h"
#include "context/runtime/js/js_host.h"
#include "cli_test.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <system_error>

using context::cli::run;
using context::cli::run_profile;
using Envelope = context::editor::contract::Envelope;
using Json = context::editor::contract::Json;
namespace cjs = context::runtime::js;

namespace
{
std::string err_code(const Envelope& e)
{
    return e.error().has_value() ? e.error()->code : std::string();
}
} // namespace

int main()
{
    // --- unknown profile verb (backend-independent) ---------------------------------------------
    {
        const Envelope e = run_profile("hud", {}, {});
        CHECK(!e.ok());
        CHECK(err_code(e) == "usage.unknown_verb");
    }

    // --- flag validation fails fast, before the VM boots (backend-independent) ------------------
    {
        const Envelope e = run_profile("gc", {}, {{"ticks", "0"}});
        CHECK(!e.ok());
        CHECK(err_code(e) == "usage.invalid");
    }
    {
        const Envelope e = run_profile("gc", {}, {{"ticks", "-3"}});
        CHECK(!e.ok());
        CHECK(err_code(e) == "usage.invalid");
    }
    {
        const Envelope e = run_profile("gc", {}, {{"budget-ms", "0"}});
        CHECK(!e.ok());
        CHECK(err_code(e) == cjs::kGcInvalidBudgetCode); // sim.gc.invalid_budget
    }
    {
        const Envelope e = run_profile("gc", {}, {{"budget-ms", "nan"}});
        CHECK(!e.ok());
        CHECK(err_code(e) == cjs::kGcInvalidBudgetCode);
    }
    {
        const Envelope e = run_profile("gc", {}, {{"trigger-bytes", "x"}});
        CHECK(!e.ok());
        CHECK(err_code(e) == "usage.invalid");
    }
    {
        // 0 would disable BOTH the force policy and the growth trigger (a run that never
        // collects); it is rejected like --ticks 0, not silently accepted.
        const Envelope e = run_profile("gc", {}, {{"trigger-bytes", "0"}});
        CHECK(!e.ok());
        CHECK(err_code(e) == "usage.invalid");
    }

    // --- `profile session`: spans + counters answer headless on EVERY toolchain (a15) -----------
    // Unlike `profile gc`, this verb does NOT need the JS VM: the per-system CPU spans + counters
    // are pure C++, so it SUCCEEDS on the stub build too (the gc block reports availability).
    {
        const Envelope e = run({"profile", "session", "--ticks", "4"});
        CHECK(e.ok());
        const Json& d = e.data();
        CHECK(d.at("channel").as_string() == "profile.session");
        CHECK(d.at("tickCount").as_number() == 4.0);
        CHECK(d.at("tickHz").as_number() == 60.0);
        CHECK(d.at("simTick").as_number() == 4.0);
        CHECK(d.at("systemCount").as_number() >= 3.0); // input/control/motion (+ js on VM builds)
        CHECK(d.at("totalCpuMs").is_number());
        CHECK(d.at("spans").is_array());
        CHECK(d.at("spans").size() >= 3);
        CHECK(d.at("spans").at(0).at("system").is_string());
        CHECK(d.at("spans").at(0).at("lane").as_string() == "native");
        CHECK(d.at("spans").at(0).at("callCount").as_number() == 4.0); // once per tick
        CHECK(d.at("spans").at(0).at("maxMs").is_number());
        CHECK(d.at("lanes").is_array());
        CHECK(d.at("lanes").size() >= 1);
        CHECK(d.at("lanes").at(0).at("lane").as_string() == "native");
        CHECK(d.at("gc").is_object());
        CHECK(d.at("gc").at("available").is_bool()); // value asserted per backend below
        CHECK(d.at("spansTruncated").is_bool());
    }
    {
        // flag validation fails fast (backend-independent).
        const Envelope e = run_profile("session", {}, {{"ticks", "0"}});
        CHECK(!e.ok());
        CHECK(err_code(e) == "usage.invalid");
    }

    // --- `profile session --trace-out`: writes an importable Chrome trace (Tracy/Perfetto export) -
    {
        namespace fs = std::filesystem;
        const fs::path trace = fs::temp_directory_path() / "context_a15_profile_trace.json";
        std::error_code ec;
        fs::remove(trace, ec);
        const Envelope e =
            run({"profile", "session", "--ticks", "3", "--trace-out", trace.string()});
        CHECK(e.ok());
        CHECK(e.data().at("trace").at("written").as_bool());
        CHECK(e.data().at("trace").at("format").as_string() == "chrome-trace-event");
        CHECK(e.data().at("trace").at("events").as_number() >= 1.0);
        std::ifstream in(trace, std::ios::binary);
        CHECK(in.good());
        const std::string body((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        in.close();
        CHECK(body.find("\"traceEvents\":[") != std::string::npos);
        CHECK(body.find("\"ph\":\"X\"") != std::string::npos);
        fs::remove(trace, ec);
    }

    if (!cjs::v8BackendAvailable())
    {
        // --- the stub build refuses `profile gc` fail-closed with the minted catalog code -------
        const Envelope e = run({"profile", "gc", "--ticks", "2"});
        CHECK(!e.ok());
        CHECK(err_code(e) == cjs::kGcUnavailableCode); // sim.gc.unavailable

        // --- but `profile session` still answers spans/counters; gc reports unavailable ---------
        const Envelope s = run({"profile", "session", "--ticks", "2"});
        CHECK(s.ok());
        CHECK(!s.data().at("gc").at("available").as_bool());
        CHECK(s.data().at("gc").at("reason").is_string());
        CLI_TEST_MAIN_END();
    }

    // --- the V8 legs: the full channel envelope through the registry dispatch path --------------
    {
        const Envelope e = run({"profile", "gc", "--ticks", "5", "--churn", "3000"});
        CHECK(e.ok());
        const Json& d = e.data();
        CHECK(d.at("channel").as_string() == "sim.js.gc_pause");
        CHECK(d.at("simTick").as_number() == 5.0);
        CHECK(d.at("tickCount").as_number() == 5.0);
        CHECK(d.at("tickHz").as_number() == 60.0);
        CHECK(d.at("budgetMs").as_number() > 0.0);
        CHECK(d.at("forcedWindows").as_bool()); // no --trigger-bytes => collect every window
        CHECK(d.at("collectedWindows").as_number() == 5.0);
        CHECK(d.at("aggregates").is_object());
        CHECK(d.at("aggregates").at("pauseCount").as_number() >= 1.0);   // forced windows collect
        CHECK(d.at("aggregates").at("inWindowCount").as_number() >= 1.0); // ...attributed in-window
        CHECK(d.at("aggregates").at("dropped").as_number() == 0.0);
        CHECK(d.at("samples").is_array());
        CHECK(d.at("samples").size() >= 1);
        CHECK(d.at("samples").at(0).at("durationMs").is_number());
        CHECK(d.at("samples").at(0).at("inWindow").is_bool());
        CHECK(d.at("heap").at("usedBytes").as_number() > 0.0);
        CHECK(d.at("heap").at("totalBytes").as_number() > 0.0);
        CHECK(d.at("withinBudget").is_bool()); // a timing verdict — present, value not asserted
    }

    // --- the growth-trigger policy plumbs through (a huge trigger never collects) ---------------
    {
        const Envelope e =
            run({"profile", "gc", "--ticks", "3", "--trigger-bytes", "1099511627776"});
        CHECK(e.ok());
        CHECK(!e.data().at("forcedWindows").as_bool());
        CHECK(e.data().at("collectedWindows").as_number() == 0.0);
    }

    // --- `profile session` on the V8 legs: the GC channel is folded in + the script lane appears -
    {
        const Envelope e = run({"profile", "session", "--ticks", "5", "--churn", "3000"});
        CHECK(e.ok());
        const Json& d = e.data();
        CHECK(d.at("gc").at("available").as_bool());
        CHECK(d.at("gc").at("pauseCount").as_number() >= 1.0);   // forced windows collect
        CHECK(d.at("gc").at("inWindowCount").as_number() >= 1.0); // attributed in-window
        CHECK(d.at("gc").at("budgetMs").as_number() > 0.0);
        CHECK(d.at("gc").at("withinBudget").is_bool());
        CHECK(d.at("gc").at("dropped").as_number() == 0.0);
        CHECK(d.at("systemCount").as_number() >= 4.0); // input/control/motion + js.gameplay
        bool has_script_lane = false;
        for (std::size_t i = 0; i < d.at("lanes").size(); ++i)
            if (d.at("lanes").at(i).at("lane").as_string() == "script")
                has_script_lane = true;
        CHECK(has_script_lane); // the js.gameplay churn lane
    }

    CLI_TEST_MAIN_END();
}
