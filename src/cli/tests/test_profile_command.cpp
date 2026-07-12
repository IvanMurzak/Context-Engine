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

#include <map>
#include <string>

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

    if (!cjs::v8BackendAvailable())
    {
        // --- the stub build refuses fail-closed with the minted catalog code --------------------
        const Envelope e = run({"profile", "gc", "--ticks", "2"});
        CHECK(!e.ok());
        CHECK(err_code(e) == cjs::kGcUnavailableCode); // sim.gc.unavailable
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

    CLI_TEST_MAIN_END();
}
