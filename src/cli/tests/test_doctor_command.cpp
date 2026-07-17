// `context doctor` CLI tests (a09, R-BUILD-008). Proves the CLI wiring + the R-CLI-008 envelope shape;
// the exhaustive diagnosis corpus (every finding + doctor.* code) lives in the pure-core test
// src/editor/build/tests/test_doctor.cpp. Here:
//   * the report-JSON shape is asserted DETERMINISTICALLY over an INJECTED fixture report (no host
//     dependence) — healthy (ok) and incomplete (code: doctor.environment_incomplete);
//   * an unknown --target is a doctor.unknown_target usage failure through the CLI grammar;
//   * a REAL `context doctor` invocation returns a well-formed success envelope on THIS host (a smoke —
//     it asserts the envelope/report SHAPE, never that the uncontrolled host is healthy);
//   * per-verb --help + the diagnostic-verb contract (exit 0 / assert data.ok, the `validate` idiom).

#include "context/cli/app.h"
#include "context/cli/doctor_command.h"
#include "context/editor/build/doctor.h"
#include "context/editor/build/toolchain_manifest.h"
#include "context/editor/contract/envelope.h"
#include "context/editor/contract/json.h"
#include "cli_test.h"

#include <map>
#include <string>
#include <vector>

using namespace context::cli;
using context::editor::contract::Envelope;
using context::editor::contract::Json;
namespace build = context::editor::build;

namespace
{
build::ToolProbe tool(std::string name, std::string version)
{
    return build::ToolProbe{std::move(name), true, std::move(version)};
}

const Json* find_component(const Json& components, const std::string& target,
                           const std::string& component)
{
    for (std::size_t i = 0; i < components.size(); ++i)
    {
        const Json& c = components.at(i);
        if (c.at("target").as_string() == target && c.at("component").as_string() == component)
            return &components.at(i);
    }
    return nullptr;
}
} // namespace

int main()
{
    const std::vector<build::ToolchainEntry>& manifest = build::toolchain_manifest();

    // --- DETERMINISTIC report JSON: a HEALTHY injected environment ---------------------------------
    {
        build::EnvironmentProbe p;
        p.tools = {tool("clang", "20.1.2"), tool("cmake", "3.29.2"), tool("node", "20.11.0")};
        p.filesync.project_file_count = 1000;
        p.filesync.worktree_daemon_count = 1;
        p.filesync.watch_limit = 524288;
        const build::DoctorReport r = build::diagnose({"linux"}, manifest, p);
        const Json data = doctor_report_json(r, /*fetch_offered=*/false);

        CHECK(data.at("ok").as_bool());
        CHECK(!data.contains("code")); // no top-level code when healthy
        CHECK(data.at("targets").is_array());
        CHECK(data.at("targets").size() == 1);
        CHECK(data.at("components").is_array());
        CHECK(data.at("components").size() == 3);
        const Json* clang = find_component(data.at("components"), "linux", "clang");
        CHECK(clang != nullptr);
        CHECK(clang->at("status").as_string() == "ok");
        CHECK(clang->at("acquisition").as_string() == "fetchable");
        CHECK(clang->at("fetchable").as_bool());
        CHECK(clang->at("canFetchNow").as_bool());
        CHECK(clang->at("foundVersion").as_string() == "20.1.2");
        CHECK(clang->at("requiredVersion").as_string() == "20.1");
        CHECK(!clang->at("remediationPointer").as_string().empty());
        const Json& budget = data.at("fileSyncBudget");
        CHECK(budget.at("status").as_string() == "ok");
        CHECK(budget.at("requiredWatches").as_int() == 1000);
        CHECK(data.at("signing").is_array());
        CHECK(data.at("warnings").is_array());
        CHECK(!data.contains("fetch")); // no --fetch offered
    }

    // --- DETERMINISTIC report JSON: an INCOMPLETE environment carries the branchable code ----------
    {
        build::EnvironmentProbe p;
        // clang absent (blocking) + a degraded watch budget + windows signing enumerated.
        p.tools = {tool("cmake", "3.29.2"), tool("node", "20.11.0")};
        const build::DoctorReport r = build::diagnose({"linux"}, manifest, p);
        const Json data = doctor_report_json(r, /*fetch_offered=*/true);

        CHECK(!data.at("ok").as_bool());
        CHECK(data.at("code").as_string() == std::string(build::kDoctorEnvironmentIncompleteCode));
        CHECK(!data.at("summary").as_string().empty());
        const Json* clang = find_component(data.at("components"), "linux", "clang");
        CHECK(clang != nullptr);
        CHECK(clang->at("status").as_string() == "missing");
        CHECK(clang->at("blocking").as_bool());
        CHECK(clang->at("code").as_string() == std::string(build::kDoctorToolchainMissingCode));

        // --fetch: the offer enumerates the fetchable-and-not-ok components machine-readably.
        CHECK(data.contains("fetch"));
        CHECK(data.at("fetch").at("offered").as_bool());
        CHECK(data.at("fetch").at("components").is_array());
        CHECK(data.at("fetch").at("components").size() == 1); // clang is fetchable + missing
        CHECK(data.at("fetch").at("components").at(0).at("component").as_string() == "clang");
    }

    // --- signing findings are enumerated per target; presence-only, never a secret value ----------
    {
        build::EnvironmentProbe p;
        p.tools = {tool("msvc", "19.44.0"), tool("cmake", "3.29.2"), tool("node", "20.11.0")};
        const build::DoctorReport r = build::diagnose({"windows"}, manifest, p);
        const Json data = doctor_report_json(r, false);
        CHECK(data.at("signing").size() == 1);
        const Json& s = data.at("signing").at(0);
        CHECK(s.at("target").as_string() == "windows");
        CHECK(s.at("requirement").as_string() == "authenticode");
        CHECK(s.at("status").as_string() == "unknown"); // no presence probe supplied -> unknown
        // The finding carries a requirement + remediation POINTER, never a credential value.
        CHECK(!s.at("remediationPointer").as_string().empty());
    }

    // --- unknown --target is a doctor.unknown_target usage failure (a malformed command) ----------
    {
        const Envelope e = run_doctor({{"target", "playstation"}});
        CHECK(!e.ok());
        CHECK(e.error().has_value());
        CHECK(e.error()->code == std::string(build::kDoctorUnknownTargetCode));
    }
    {
        // via the CLI grammar
        const Envelope e = run({"doctor", "--target", "nintendo"});
        CHECK(!e.ok());
        CHECK(e.error()->code == std::string(build::kDoctorUnknownTargetCode));
    }
    {
        // a comma-separated list with one bad member is rejected as a whole
        const Envelope e = run_doctor({{"target", "linux,playstation"}});
        CHECK(!e.ok());
        CHECK(e.error()->code == std::string(build::kDoctorUnknownTargetCode));
    }

    // --- REAL smoke: a completed diagnosis is a well-formed SUCCESS envelope (shape, not health) ---
    // The diagnostic-verb contract (the `validate` idiom): `context doctor` exits 0 for a completed
    // diagnosis regardless of whether the host is healthy — assert data.ok is a boolean + the report is
    // well-formed, NEVER that this uncontrolled host satisfies the toolchain (that would be flaky).
    {
        const Envelope e = run_doctor({{"target", "linux"}});
        CHECK(e.ok()); // a completed diagnosis is always a success envelope (exit 0)
        const Json& data = e.data();
        CHECK(data.contains("ok"));
        CHECK(data.at("ok").is_bool());
        CHECK(data.at("targets").is_array());
        CHECK(data.at("targets").size() == 1);
        CHECK(data.at("targets").at(0).as_string() == "linux");
        CHECK(data.at("components").is_array());
        CHECK(data.at("components").size() == 3); // clang + cmake + node
        CHECK(data.at("fileSyncBudget").is_object());
        CHECK(data.at("fileSyncBudget").at("status").is_string());
    }

    // --- `all` expands to the v1 target set; every requested target's components are validated -----
    {
        const Envelope e = run_doctor({{"target", "all"}});
        CHECK(e.ok());
        const Json& data = e.data();
        CHECK(data.at("targets").size() == 4); // windows, linux, macos, web
        CHECK(data.at("components").size() == 12); // 3 per target
    }

    // --- default target (no --target) resolves to the host-native target --------------------------
    {
        const Envelope e = run_doctor({});
        CHECK(e.ok());
        CHECK(e.data().at("targets").size() == 1); // exactly the host-native target
    }

    // --- per-verb --help emits this verb's contract entry (R-CLI-013) + registry parity ------------
    {
        const Envelope help = run({"doctor", "--help"});
        CHECK(help.ok());
        CHECK(help.data().at("verb").as_string() == "doctor");
        CHECK(help.data().at("rpcMethod").as_string() == "doctor");
        CHECK(help.data().at("mcpTool").as_string() == "context_doctor");
        CHECK(help.data().at("command").as_string() == "context doctor");
        CHECK(help.data().at("stability").as_string() == "stable");
        // the --target + --fetch flags are declared beyond the core set
        const Json& flags = help.data().at("flags");
        bool saw_target = false;
        bool saw_fetch = false;
        for (std::size_t i = 0; i < flags.size(); ++i)
        {
            const std::string name = flags.at(i).at("name").as_string();
            saw_target = saw_target || name == "target";
            saw_fetch = saw_fetch || name == "fetch";
        }
        CHECK(saw_target);
        CHECK(saw_fetch);
    }

    CLI_TEST_MAIN_END();
}
