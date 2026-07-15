// M7 exit criterion 2 — `m7-exit-2-cli-drive` (design 2026-07-13-m7-runtime-ui / a12-m7-exit;
// R-UI-006 / R-CLI-008/009, a5): drive the authored platformer-2d HUD through the REAL shipped `context`
// binary's `ui.*` verbs across a TRUE process boundary — NOT a mock, NOT the in-process cli::run. The
// milestone-closing proof that the a5 headless drive/assert surface (ui dump / query / send / assert)
// works end to end on the shipped executable: it loads the authored ctx:ui-hud document, lays it out,
// resolves the read-only data bindings, runs the UI->state action path, and fails closed on a wrong
// assertion — the whole R-UI-006 loop with no GPU and no renderer.
//
// The binary is spawned via the shared hardened std::system runner (context_common/subprocess.h; the
// Windows cmd.exe outer-quote fix + POSIX exit-code decode), its stdout captured to a scratch file and
// parsed as the R-CLI-008 envelope. Exit codes ALSO gate: a wrong `ui assert` returns the catalog's
// non-zero exit code, so "the binary really evaluated the HUD" is proven two ways (envelope + rc).
// Runs in the blocking "M7 exit gate" build-job step on all three OS legs.

#include "m7_exit_test.h"

#include "context/common/subprocess.h"
#include "context/editor/contract/json.h"

#include <string>
#include <vector>

#ifndef CONTEXT_BINARY
#error "CONTEXT_BINARY (path to the built context executable) must be defined by the build."
#endif
#ifndef CONTEXT_SAMPLES_DIR
#error "CONTEXT_SAMPLES_DIR (path to the samples/ corpus root) must be defined by the build."
#endif

namespace subprocess = context::common::subprocess;
using context::editor::contract::Json;
using context::tests::m7::report;

namespace
{

const std::string kBin = CONTEXT_BINARY;
const std::string kHud = std::string(CONTEXT_SAMPLES_DIR) + "/platformer-2d/ui/hud.ui-hud.json";

// One cross-process `context ui …` invocation: spawn the REAL binary via the shared std::system runner,
// capture STDOUT ONLY to a scratch file, and return the exit code + the parsed envelope. `argv` is the
// argument list AFTER the binary (e.g. {"ui","dump","<hud>"}). The R-CLI-008 envelope (success AND
// error) is a stdout-only contract (app.cpp writes it via fwrite(..., stdout)); stderr carries no
// envelope, so it is deliberately NOT redirected into the scratch file.
struct CliResult
{
    int exit_code = -1;
    Json envelope;
};

CliResult drive(const std::vector<std::string>& argv)
{
    const std::filesystem::path out = subprocess::make_scratch_path("m7-exit-2", ".json");
    std::string command = subprocess::quote_argument(kBin);
    for (const std::string& a : argv)
        command += " " + subprocess::quote_argument(a);
    // Redirect STDOUT ONLY — never `2>&1`. Under the sanitize (ASan+UBSan) leg the spawned `context`
    // binary links the uninstrumented rusty_v8 prebuilt, whose duplicate C++-runtime typeinfo makes
    // UBSan's vptr sub-check false-positive on valid libstdc++ streams; those `runtime error:` lines
    // are recovered (benign) but still print to stderr (docs/sanitizer-v8-false-positives.md). Merging
    // stderr into the capture would prepend that noise ahead of the `{`, breaking Json::parse at byte 0.
    command += " > " + subprocess::quote_argument(out.string());

    CliResult r;
    r.exit_code = subprocess::run_command(command);
    const std::string text = subprocess::read_file(out);
    std::error_code ec;
    std::filesystem::remove(out, ec);
    if (!text.empty())
        r.envelope = Json::parse(text);
    return r;
}

[[nodiscard]] bool env_ok(const Json& env)
{
    return env.is_object() && env.contains("ok") && env.at("ok").as_bool();
}

} // namespace

int main()
{
    // --- ui dump: the shipped binary loads + lays out the authored HUD, reports the tree + state -----
    {
        const CliResult r = drive({"ui", "dump", kHud});
        CHECK(r.exit_code == 0);
        CHECK(env_ok(r.envelope));
        if (env_ok(r.envelope))
        {
            const Json& data = r.envelope.at("data");
            CHECK(data.at("nodeCount").as_number() == 5.0);
            CHECK(data.at("state").at("score").as_number() == 0.0);
            CHECK(data.at("state").at("health").as_number() == 100.0);
        }
    }

    // --- ui query: a single data-bound node read across the process boundary -------------------------
    {
        const CliResult r = drive({"ui", "query", kHud, "health-bar"});
        CHECK(r.exit_code == 0);
        CHECK(env_ok(r.envelope));
        if (env_ok(r.envelope))
            CHECK(r.envelope.at("data").at("boundValue").as_number() == 100.0);
    }

    // --- a missing node fails closed (the real verb, not a mock that always says yes) -----------------
    {
        const CliResult r = drive({"ui", "query", kHud, "no-such-node"});
        CHECK(r.exit_code != 0);
        CHECK(!env_ok(r.envelope));
        if (r.envelope.is_object() && r.envelope.contains("error"))
            CHECK(r.envelope.at("error").at("code").as_string() == "ui.node_not_found");
    }

    // --- ui send click: the UI->state action path runs on the shipped binary (collect: +10 / -5) -----
    {
        const CliResult r = drive({"ui", "send", kHud, "click", "--target", "collect-button"});
        CHECK(r.exit_code == 0);
        CHECK(env_ok(r.envelope));
        if (env_ok(r.envelope))
        {
            CHECK(r.envelope.at("data").at("state").at("score").as_number() == 10.0);
            CHECK(r.envelope.at("data").at("state").at("health").as_number() == 95.0);
        }
    }

    // --- ui assert: the binary REALLY evaluates the HUD — a correct value passes (rc 0), a wrong one
    //     fails closed (non-zero exit code from the catalog) + a role assertion passes -----------------
    {
        const CliResult ok = drive({"ui", "assert", kHud, "health-bar", "--value", "100"});
        CHECK(ok.exit_code == 0);
        CHECK(env_ok(ok.envelope));

        const CliResult wrong = drive({"ui", "assert", kHud, "health-bar", "--value", "999"});
        CHECK(wrong.exit_code != 0); // fail-closed: real evaluation, not a mock
        CHECK(!env_ok(wrong.envelope));
        if (wrong.envelope.is_object() && wrong.envelope.contains("error"))
            CHECK(wrong.envelope.at("error").at("code").as_string() == "ui.assertion_failed");

        const CliResult role = drive({"ui", "assert", kHud, "collect-button", "--role", "button"});
        CHECK(role.exit_code == 0);
        CHECK(env_ok(role.envelope));
    }

    return report("m7-exit-2-cli-drive",
                  "the REAL context binary drives the authored HUD via ui dump/query/send/assert");
}
