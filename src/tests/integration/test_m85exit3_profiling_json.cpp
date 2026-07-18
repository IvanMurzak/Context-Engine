// M8.5 exit criterion 3 — `m85-exit-3-profiling-json` (ROADMAP §1-M8.5 exit / a23-m85-exit; L-47 /
// R-OBS-002/004, a15): "profiling data is JSON-queryable on a live session." Driven through the REAL
// shipped `context` binary's a15 `context profile session` verb across a TRUE process boundary — NOT a
// mock, NOT the in-process cli::run. The milestone-closing proof that an operator can point the shipped
// CLI at a live headless session and get the L-47 profiler channels back as ONE queryable JSON snapshot:
// per-system CPU spans (native lane, always present — pure C++ on every toolchain), per-lane rollups,
// and the R-SIM-008 GC-pause channel folded in (available:true on the V8 CI legs, an honest
// available:false on a stub build — never a refusal). `--trace-out` additionally writes an importable
// Chrome-trace file (L-47's "deep capture via export to world-class tools", Tracy/Perfetto).
//
// The binary is spawned via the shared hardened std::system runner (context_common/subprocess.h; the
// Windows cmd.exe outer-quote fix + POSIX exit-code decode), its stdout captured to a scratch file and
// parsed as the R-CLI-008 envelope (the m7-exit-2 / m1-exit e2e pattern). Runs in the blocking "M8.5
// exit gate" build-job step on all three OS legs.

#include "m85_exit_test.h"

#include "context/common/subprocess.h"
#include "context/editor/contract/json.h"

#include <filesystem>
#include <string>
#include <vector>

#ifndef CONTEXT_BINARY
#error "CONTEXT_BINARY (path to the built context executable) must be defined by the build."
#endif

namespace subprocess = context::common::subprocess;
using context::editor::contract::Json;
using context::tests::m85::report;

namespace
{

const std::string kBin = CONTEXT_BINARY;

struct CliResult
{
    int exit_code = -1;
    Json envelope;
};

// One cross-process `context profile …` invocation: spawn the REAL binary via the shared std::system
// runner, capture STDOUT ONLY to a scratch file, and return the exit code + the parsed envelope. `argv`
// is the argument list AFTER the binary. The R-CLI-008 envelope (success AND error) is a stdout-only
// contract (app.cpp writes it via fwrite(..., stdout)); stderr carries no envelope, so it is deliberately
// NOT redirected — under the sanitize (ASan+UBSan) leg the spawned `context` binary links the
// uninstrumented rusty_v8 prebuilt whose duplicate C++-runtime typeinfo makes UBSan's vptr sub-check
// false-positive on valid libstdc++ streams; merging that stderr noise into the capture would break
// Json::parse at byte 0 (docs/sanitizer-v8-false-positives.md).
CliResult drive(const std::vector<std::string>& argv)
{
    const std::filesystem::path out = subprocess::make_scratch_path("m85-exit-3", ".json");
    std::string command = subprocess::quote_argument(kBin);
    for (const std::string& a : argv)
        command += " " + subprocess::quote_argument(a);
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
    // --- profile session: the shipped binary returns the L-47 channels as ONE queryable JSON snapshot ---
    {
        const CliResult r = drive({"profile", "session", "--ticks", "5"});
        CHECK(r.exit_code == 0);
        CHECK(env_ok(r.envelope));
        if (env_ok(r.envelope))
        {
            const Json& d = r.envelope.at("data");
            // The snapshot is JSON-queryable: named channel, the live session's tick metadata, and the
            // per-system CPU spans (pure C++ — present on EVERY toolchain, so this gate is green even on a
            // stub-JS build).
            CHECK(d.at("channel").as_string() == "profile.session");
            CHECK(d.at("tickCount").as_number() == 5.0);
            CHECK(d.at("simTick").as_number() == 5.0); // the session really stepped 5 ticks
            CHECK(d.at("tickHz").as_number() == 60.0); // the R-SIM-002 fixed timestep
            CHECK(d.at("systemCount").as_number() >= 3.0);
            CHECK(d.at("totalCpuMs").is_number());
            // Spans: one per scheduler system, each queryable down to its lane + call/timing counters.
            CHECK(d.at("spans").is_array());
            CHECK(d.at("spans").size() >= 3);
            if (d.at("spans").size() >= 1)
            {
                CHECK(d.at("spans").at(0).at("system").is_string());
                CHECK(d.at("spans").at(0).at("lane").as_string() == "native");
                CHECK(d.at("spans").at(0).at("callCount").as_number() == 5.0); // once per tick
                CHECK(d.at("spans").at(0).at("maxMs").is_number());
            }
            // Per-lane rollups + the folded-in R-SIM-008 GC-pause channel. gc.available is
            // backend-dependent (true on the V8 CI legs, honestly false on a stub build) — assert the
            // block is present + queryable, never a specific availability (which would break one host).
            CHECK(d.at("lanes").is_array());
            CHECK(d.at("lanes").size() >= 1);
            CHECK(d.at("lanes").at(0).at("lane").as_string() == "native");
            CHECK(d.at("gc").is_object());
            CHECK(d.at("gc").at("available").is_bool());
            CHECK(d.at("gc").at("budgetMs").is_number());
            CHECK(d.at("spansTruncated").is_bool());
        }
    }

    // --- fail-closed on a bad workload parameter (the real verb, not a mock that always says yes) --------
    {
        const CliResult r = drive({"profile", "session", "--ticks", "0"});
        CHECK(r.exit_code != 0);
        CHECK(!env_ok(r.envelope));
        if (r.envelope.is_object() && r.envelope.contains("error"))
            CHECK(r.envelope.at("error").at("code").as_string() == "usage.invalid");
    }

    // --- profile session --trace-out: the L-47 deep-capture export writes an importable Chrome trace -----
    {
        const std::filesystem::path trace = subprocess::make_scratch_path("m85-exit-3-trace", ".json");
        std::error_code ec;
        std::filesystem::remove(trace, ec);
        const CliResult r = drive({"profile", "session", "--ticks", "3", "--trace-out", trace.string()});
        CHECK(r.exit_code == 0);
        CHECK(env_ok(r.envelope));
        if (env_ok(r.envelope))
        {
            const Json& t = r.envelope.at("data").at("trace");
            CHECK(t.at("written").as_bool());
            CHECK(t.at("format").as_string() == "chrome-trace-event");
            CHECK(t.at("events").as_number() >= 1.0);
        }
        // The exported artifact is a real Chrome trace-event file on disk (importable into Tracy/Perfetto).
        const std::string body = subprocess::read_file(trace);
        CHECK(!body.empty());
        CHECK(body.find("\"traceEvents\":[") != std::string::npos);
        std::filesystem::remove(trace, ec);
    }

    return report("m85-exit-3-profiling-json",
                  "the REAL context binary answers `profile session` with a JSON-queryable L-47 "
                  "snapshot (spans + lanes + gc) + a Chrome-trace export on a live headless session");
}
