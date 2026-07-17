// Environment-doctor core tests (a09, R-BUILD-008) — the R-QA-011 corpus for the PURE diagnosis spine.
// DoD-1: a healthy REFERENCE environment passes (the "doctor passes on the reference dev machine + CI
// legs" proof, deterministic + host-independent because the probe is INJECTED), and deliberately-broken
// environment fixtures produce the documented machine-readable findings + doctor.* codes:
//   * a missing required toolchain component      -> doctor.toolchain_missing, blocking, report.ok=false
//   * a strict-pin version mismatch (linux clang) -> doctor.toolchain_version_mismatch, blocking
//   * an advisory/documented version drift        -> version_mismatch, NON-blocking (a warning)
//   * a starved file-sync watch budget            -> doctor.filesync_budget_low, advisory (never blocks)
//   * an absent signing prerequisite              -> doctor.signing_prereq_absent, advisory
//   * the fetchable-vs-preinstalled split          -> per-component acquisition + can_fetch_now
// Plus the version-compare semantics + the component/signing enumeration (happy + failure, R-QA-013).

#include "context/editor/build/doctor.h"
#include "context/editor/build/toolchain_manifest.h"
#include "build_test.h"

#include <string>
#include <vector>

using namespace context::editor::build;

namespace
{
// A tool present at the given version.
ToolProbe tool(std::string name, std::string version)
{
    return ToolProbe{std::move(name), true, std::move(version)};
}

// Find a component finding for (target, component) in a report; nullptr when absent.
const ComponentFinding* find_component(const DoctorReport& r, const std::string& target,
                                       const std::string& component)
{
    for (const ComponentFinding& c : r.components)
        if (c.target == target && c.component == component)
            return &c;
    return nullptr;
}

const SigningFinding* find_signing(const DoctorReport& r, const std::string& target,
                                   const std::string& requirement)
{
    for (const SigningFinding& s : r.signing)
        if (s.target == target && s.requirement == requirement)
            return &s;
    return nullptr;
}

// A fully-satisfied Linux reference environment (clang 20.1, cmake, node; a generous watch budget).
EnvironmentProbe healthy_linux()
{
    EnvironmentProbe p;
    p.tools = {tool("clang", "20.1.2"), tool("cmake", "3.29.2"), tool("node", "20.11.0")};
    p.filesync.project_file_count = 1200;
    p.filesync.worktree_daemon_count = 2;
    p.filesync.watch_limit = 524288;
    return p;
}
} // namespace

int main()
{
    const std::vector<ToolchainEntry>& manifest = toolchain_manifest();

    // --- version_satisfies: numeric-component semantics (not string prefix) ------------------------
    {
        CHECK(version_satisfies("", "anything"));         // presence-only
        CHECK(version_satisfies("20.1", "20.1.2"));       // pin is a numeric prefix of found
        CHECK(version_satisfies("20.1", "20.1"));         // exact
        CHECK(!version_satisfies("20.1", "20.10.0"));     // 1 != 10 — NOT a string prefix match
        CHECK(!version_satisfies("20.1", "19.9.9"));      // wrong major
        CHECK(!version_satisfies("21.0", "20.1.2"));      // wrong major
        CHECK(version_satisfies("21.0", "21.0.3"));
        CHECK(!version_satisfies("20.1", ""));            // required but no version parsed -> mismatch
        CHECK(version_satisfies("20", "v20.11.0"));       // leading non-digit is skipped (node's "v")
    }

    // --- component enumeration + the fetchable-vs-preinstalled split (R-BUILD-008) ------------------
    {
        const std::vector<ComponentRequirement> linux = component_requirements("linux", manifest);
        CHECK(linux.size() == 3); // clang + cmake + node
        CHECK(linux[0].component == "clang");
        CHECK(linux[0].role == "compiler");
        CHECK(linux[0].acquisition == Acquisition::Fetchable); // mainline clang is engine-fetched
        CHECK(linux[0].required_version == "20.1");
        CHECK(linux[0].enforcement == "strict");

        const std::vector<ComponentRequirement> win = component_requirements("windows", manifest);
        CHECK(win.size() == 3);
        CHECK(win[0].component == "msvc");
        CHECK(win[0].acquisition == Acquisition::Preinstalled); // MSVC STL / Windows SDK is licensed
        CHECK(win[0].enforcement == "documented");

        const std::vector<ComponentRequirement> mac = component_requirements("macos", manifest);
        CHECK(mac[0].component == "apple-clang");
        CHECK(mac[0].acquisition == Acquisition::Preinstalled); // Xcode is non-fetchable
        CHECK(mac[0].enforcement == "advisory");

        const std::vector<ComponentRequirement> web = component_requirements("web", manifest);
        CHECK(web[0].component == "emscripten-clang");
        CHECK(web[0].acquisition == Acquisition::Fetchable); // emsdk is engine-fetched

        // cmake + node are preinstalled prerequisites on every target.
        for (const std::vector<ComponentRequirement>* reqs : {&linux, &win, &mac, &web})
        {
            CHECK((*reqs)[1].component == "cmake" && (*reqs)[1].role == "build-system");
            CHECK((*reqs)[2].component == "node" && (*reqs)[2].role == "js-toolchain");
        }

        // An unknown target enumerates nothing.
        CHECK(component_requirements("playstation", manifest).empty());
    }

    // --- signing requirements per target (R-BUILD-005) ---------------------------------------------
    {
        CHECK(signing_requirements("windows") == std::vector<std::string>{"authenticode"});
        CHECK(signing_requirements("macos") ==
              std::vector<std::string>{"developer-id-notarization"});
        CHECK(signing_requirements("linux").empty());
        CHECK(signing_requirements("web").empty());
    }

    // --- signing PRESENCE (a10): the injected SigningProbe drives configured / absent / unknown -------
    // The CLI (doctor_command.cpp) fills probe.signing from a real, presence-only host check (never a
    // secret value); here the injected probe exercises all three verdicts of the pure diagnose spine.
    {
        EnvironmentProbe base;
        base.tools = {tool("cmake", "3.29.2"), tool("node", "20.11.0")};

        // configured: the Windows Authenticode signing identity is present -> status "configured", no warn.
        {
            EnvironmentProbe p = base;
            p.signing = {SigningProbe{"windows", "authenticode", /*configured=*/true, /*known=*/true}};
            const DoctorReport r = diagnose({"windows"}, manifest, p);
            const SigningFinding* sf = find_signing(r, "windows", "authenticode");
            CHECK(sf != nullptr);
            CHECK(sf->status == "configured");
            CHECK(sf->code.empty());
            CHECK(!sf->blocking); // a ship-time prereq NEVER blocks the build
        }

        // absent: no signing identity -> status "absent" + doctor.signing_prereq_absent + a warning.
        {
            EnvironmentProbe p = base;
            p.signing = {SigningProbe{"windows", "authenticode", /*configured=*/false, /*known=*/true}};
            const DoctorReport r = diagnose({"windows"}, manifest, p);
            const SigningFinding* sf = find_signing(r, "windows", "authenticode");
            CHECK(sf != nullptr);
            CHECK(sf->status == "absent");
            CHECK(sf->code == std::string(kDoctorSigningPrereqAbsentCode));
            CHECK(!sf->blocking);
            CHECK(!r.warnings.empty()); // advisory warning, but never blocks report.ok on the signing axis
        }

        // unknown: the check could not run (no probe) -> status "unknown", no code, no warning.
        {
            EnvironmentProbe p = base; // no signing probe supplied
            const DoctorReport r = diagnose({"windows"}, manifest, p);
            const SigningFinding* sf = find_signing(r, "windows", "authenticode");
            CHECK(sf != nullptr);
            CHECK(sf->status == "unknown");
            CHECK(sf->code.empty());
        }
    }

    // --- DoD-1 HAPPY: a healthy reference Linux environment passes ---------------------------------
    {
        const DoctorReport r = diagnose({"linux"}, manifest, healthy_linux());
        CHECK(r.ok);
        CHECK(r.blocking_count() == 0);
        CHECK(r.warnings.empty());
        CHECK(r.components.size() == 3);
        const ComponentFinding* clang = find_component(r, "linux", "clang");
        CHECK(clang != nullptr);
        CHECK(clang->status == "ok");
        CHECK(clang->found_version == "20.1.2");
        CHECK(clang->fetchable);
        CHECK(clang->can_fetch_now);
        CHECK(clang->code.empty());
        const ComponentFinding* cmake = find_component(r, "linux", "cmake");
        CHECK(cmake != nullptr && cmake->status == "ok" && !cmake->fetchable);
        CHECK(r.filesync.status == "ok");
        CHECK(r.filesync.required_watches == 1200 * 2);
        CHECK(r.signing.empty()); // linux has no signing prereq
    }

    // --- BROKEN: a missing required component -> doctor.toolchain_missing, blocking ----------------
    {
        EnvironmentProbe p = healthy_linux();
        p.tools = {tool("cmake", "3.29.2"), tool("node", "20.11.0")}; // clang absent
        const DoctorReport r = diagnose({"linux"}, manifest, p);
        CHECK(!r.ok);
        CHECK(r.blocking_count() == 1);
        const ComponentFinding* clang = find_component(r, "linux", "clang");
        CHECK(clang != nullptr);
        CHECK(clang->status == "missing");
        CHECK(clang->blocking);
        CHECK(clang->code == std::string(kDoctorToolchainMissingCode));
        CHECK(clang->fetchable); // the remediation is an engine-fetch (a08-verified), not a preinstall
        CHECK(!clang->remediation.empty());
    }

    // --- BROKEN: a missing PREINSTALLED component's remediation is an install, not a fetch ---------
    {
        EnvironmentProbe p = healthy_linux();
        p.tools = {tool("clang", "20.1.2"), tool("node", "20.11.0")}; // cmake absent
        const DoctorReport r = diagnose({"linux"}, manifest, p);
        CHECK(!r.ok);
        const ComponentFinding* cmake = find_component(r, "linux", "cmake");
        CHECK(cmake != nullptr && cmake->status == "missing" && cmake->blocking);
        CHECK(!cmake->fetchable);
        CHECK(cmake->remediation.find("install") != std::string::npos);
    }

    // --- BROKEN: a STRICT-pin version mismatch (linux clang) is blocking ---------------------------
    {
        EnvironmentProbe p = healthy_linux();
        p.tools = {tool("clang", "19.1.0"), tool("cmake", "3.29.2"), tool("node", "20.11.0")};
        const DoctorReport r = diagnose({"linux"}, manifest, p);
        CHECK(!r.ok);
        const ComponentFinding* clang = find_component(r, "linux", "clang");
        CHECK(clang != nullptr);
        CHECK(clang->status == "version_mismatch");
        CHECK(clang->blocking); // linux clang enforcement is strict
        CHECK(clang->code == std::string(kDoctorToolchainVersionMismatchCode));
        CHECK(clang->found_version == "19.1.0");
        CHECK(clang->required_version == "20.1");
    }

    // --- BROKEN: an ADVISORY-pin version drift (macOS apple-clang) is NON-blocking (a warning) -----
    {
        EnvironmentProbe p;
        p.tools = {tool("apple-clang", "19.0.0"), tool("cmake", "3.29.2"), tool("node", "20.11.0")};
        p.signing = {SigningProbe{"macos", "developer-id-notarization", true, true}};
        const DoctorReport r = diagnose({"macos"}, manifest, p);
        CHECK(r.ok); // advisory drift does NOT block
        CHECK(r.blocking_count() == 0);
        const ComponentFinding* cc = find_component(r, "macos", "apple-clang");
        CHECK(cc != nullptr);
        CHECK(cc->status == "version_mismatch");
        CHECK(!cc->blocking); // advisory enforcement -> warning, not a blocker
        CHECK(cc->code == std::string(kDoctorToolchainVersionMismatchCode));
        CHECK(!r.warnings.empty()); // surfaced as an advisory warning
    }

    // --- ADVISORY: a starved file-sync watch budget -> doctor.filesync_budget_low, never blocking --
    {
        EnvironmentProbe p = healthy_linux();
        p.filesync.project_file_count = 100000; // 100k files (R-FILE-011 envelope)
        p.filesync.worktree_daemon_count = 8;   // the N-daemons-on-one-box scenario
        p.filesync.watch_limit = 8192;          // a default, un-raised inotify limit
        const DoctorReport r = diagnose({"linux"}, manifest, p);
        CHECK(r.ok); // advisory — the environment still builds (degraded change-detection only)
        CHECK(r.filesync.status == "degraded");
        CHECK(r.filesync.code == std::string(kDoctorFileSyncBudgetLowCode));
        CHECK(r.filesync.required_watches == 100000LL * 8);
        CHECK(!r.filesync.blocking);
        CHECK(!r.filesync.remediation.empty());
        bool warned = false;
        for (const std::string& w : r.warnings)
            warned = warned || w.find("file-sync budget") != std::string::npos;
        CHECK(warned);
    }

    // --- UNKNOWN: an unprobeable watch limit reports "unknown" (non-Linux host), never blocking ----
    {
        EnvironmentProbe p = healthy_linux();
        p.filesync.watch_limit = -1; // not probeable on this host
        const DoctorReport r = diagnose({"linux"}, manifest, p);
        CHECK(r.ok);
        CHECK(r.filesync.status == "unknown");
        CHECK(r.filesync.code.empty());
    }

    // --- SIGNING: absent prereq is advisory; configured passes; presence only (no secret value) ----
    {
        // Windows target, Authenticode NOT configured -> advisory absent finding, report still ok
        // (a healthy Windows toolchain otherwise).
        EnvironmentProbe p;
        p.tools = {tool("msvc", "19.44.0"), tool("cmake", "3.29.2"), tool("node", "20.11.0")};
        p.signing = {SigningProbe{"windows", "authenticode", /*configured=*/false, /*known=*/true}};
        const DoctorReport r = diagnose({"windows"}, manifest, p);
        CHECK(r.ok); // signing is a ship-time prereq, never a build blocker
        const SigningFinding* sf = find_signing(r, "windows", "authenticode");
        CHECK(sf != nullptr);
        CHECK(sf->status == "absent");
        CHECK(sf->code == std::string(kDoctorSigningPrereqAbsentCode));
        CHECK(!sf->blocking);

        // Configured -> "configured", no code.
        p.signing = {SigningProbe{"windows", "authenticode", /*configured=*/true, /*known=*/true}};
        const DoctorReport r2 = diagnose({"windows"}, manifest, p);
        const SigningFinding* sf2 = find_signing(r2, "windows", "authenticode");
        CHECK(sf2 != nullptr && sf2->status == "configured" && sf2->code.empty());

        // Unknown (the check could not run) -> "unknown", no code, no warning.
        p.signing.clear();
        const DoctorReport r3 = diagnose({"windows"}, manifest, p);
        const SigningFinding* sf3 = find_signing(r3, "windows", "authenticode");
        CHECK(sf3 != nullptr && sf3->status == "unknown" && sf3->code.empty());
    }

    // --- MULTI-TARGET: doctor validates every requested target's components ------------------------
    {
        EnvironmentProbe p;
        // A cross-target agent-pool host: linux clang + web emscripten + shared cmake/node.
        p.tools = {tool("clang", "20.1.5"), tool("emscripten-clang", "3.1.60"),
                   tool("cmake", "3.29.2"), tool("node", "20.11.0")};
        const DoctorReport r = diagnose({"linux", "web"}, manifest, p);
        CHECK(r.ok);
        CHECK(r.targets.size() == 2);
        CHECK(r.components.size() == 6); // 3 per target
        const ComponentFinding* em = find_component(r, "web", "emscripten-clang");
        CHECK(em != nullptr && em->status == "ok" && em->fetchable);
    }

    BUILD_TEST_MAIN_END();
}
