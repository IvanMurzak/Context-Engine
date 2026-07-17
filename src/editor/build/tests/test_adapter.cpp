// build-test_adapter (M8 tasks a06 + a10) — the pure export-adapter plan + launcher + manifest
// generators. Coverage (R-QA-013 happy + edge): the Linux flavors (desktop render-present / server
// render-absent) AND the a10 Windows flavors (`.exe` binary, launch.cmd batch launcher, requires_signing)
// plan a correct tarball layout; unsupported targets/flavors plan the honest stub; the launcher boots
// relative to its own dir + forwards args (posix launch.sh / windows launch.cmd); the manifest is
// deterministic machine-readable JSON carrying requiresSigning.

#include "context/editor/build/adapter.h"

#include "build_test.h"

#include <string>

namespace build = context::editor::build;

namespace
{
[[nodiscard]] bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}
} // namespace

int main()
{
    // --- Linux DESKTOP: render subsystem present -----------------------------------------------------
    {
        const build::AdapterPlan p = build::plan_adapter("linux", "desktop", "game.pack");
        CHECK(p.supported);
        CHECK(p.target == "linux");
        CHECK(p.flavor == "desktop");
        CHECK(p.render_present);
        CHECK(p.runtime_binary == "context-runtime");
        CHECK(p.pack_name == "game.pack");
        CHECK(p.launcher_name == "launch.sh");
        CHECK(p.manifest_name == "context.build.json");
        // The documented tarball layout: bin/<runtime>, content/<pack>, launcher, manifest.
        CHECK(p.layout.size() == 4);
        CHECK(p.layout[0].archive_path == "bin/context-runtime");
        CHECK(p.layout[0].role == "runtime");
        CHECK(p.layout[1].archive_path == "content/game.pack");
        CHECK(p.layout[1].role == "pack");
        CHECK(p.layout[2].archive_path == "launch.sh");
        CHECK(p.layout[3].archive_path == "context.build.json");
    }

    // --- Linux SERVER/headless: render subsystem absent (L-5 DCE) ------------------------------------
    {
        const build::AdapterPlan p = build::plan_adapter("linux", "server", "");
        CHECK(p.supported);
        CHECK(p.flavor == "server");
        CHECK(!p.render_present);
        CHECK(p.runtime_binary == "context-runtime-server");
        CHECK(p.pack_name == "game.pack"); // default when empty
        CHECK(p.layout[0].archive_path == "bin/context-runtime-server");
    }

    // --- Windows DESKTOP (a10): .exe binary, launch.cmd, requires_signing, render present ------------
    {
        const build::AdapterPlan p = build::plan_adapter("windows", "desktop", "game.pack");
        CHECK(p.supported);
        CHECK(p.target == "windows");
        CHECK(p.flavor == "desktop");
        CHECK(p.render_present);
        CHECK(p.requires_signing); // windows ships an Authenticode-signed .exe (R-SEC-003)
        CHECK(p.runtime_binary == "context-runtime.exe");
        CHECK(p.launcher_kind == build::LauncherKind::WindowsCmd);
        CHECK(p.launcher_name == "launch.cmd");
        CHECK(p.layout.size() == 4);
        CHECK(p.layout[0].archive_path == "bin/context-runtime.exe"); // forward-slash archive path
        CHECK(p.layout[0].role == "runtime");
        CHECK(p.layout[2].archive_path == "launch.cmd");
        CHECK(p.layout[3].archive_path == "context.build.json");
    }

    // --- Windows SERVER/headless (a10): .exe server binary, render absent, still signing-required -----
    {
        const build::AdapterPlan p = build::plan_adapter("windows", "server", "");
        CHECK(p.supported);
        CHECK(p.flavor == "server");
        CHECK(!p.render_present);
        CHECK(p.requires_signing);
        CHECK(p.runtime_binary == "context-runtime-server.exe");
        CHECK(p.pack_name == "game.pack"); // default when empty
        CHECK(p.layout[0].archive_path == "bin/context-runtime-server.exe");
    }

    // --- Linux requires NO signing (a06) — the signing axis is Windows-only here ----------------------
    {
        const build::AdapterPlan p = build::plan_adapter("linux", "desktop", "game.pack");
        CHECK(!p.requires_signing);
        CHECK(p.launcher_kind == build::LauncherKind::PosixSh);
    }

    // --- unsupported target / flavor: the honest stub (R-BUILD-007) ----------------------------------
    {
        const build::AdapterPlan mac = build::plan_adapter("macos", "desktop", "game.pack");
        CHECK(!mac.supported);
        CHECK(mac.layout.empty());
        CHECK(build::render_launcher(mac).empty());
        CHECK(build::render_manifest(mac, 1, 2, 3).empty());

        const build::AdapterPlan web = build::plan_adapter("web", "desktop", "game.pack");
        CHECK(!web.supported);

        const build::AdapterPlan bad_flavor = build::plan_adapter("windows", "console", "game.pack");
        CHECK(!bad_flavor.supported);
    }

    // --- Windows launcher: a launch.cmd batch that boots relative to %~dp0 + forwards args ------------
    {
        const build::AdapterPlan p = build::plan_adapter("windows", "desktop", "game.pack");
        const std::string cmd = build::render_launcher(p);
        CHECK(contains(cmd, "@echo off"));
        CHECK(contains(cmd, "\"%~dp0bin\\context-runtime.exe\" --pack \"%~dp0content\\game.pack\" %*"));
    }

    // --- Windows manifest: requiresSigning is true; Linux manifest carries it false ------------------
    {
        const build::AdapterPlan win = build::plan_adapter("windows", "server", "game.pack");
        const std::string mw = build::render_manifest(win, 1, 2, 3);
        CHECK(contains(mw, "\"target\": \"windows\""));
        CHECK(contains(mw, "\"runtimeBinary\": \"bin/context-runtime-server.exe\""));
        CHECK(contains(mw, "\"launcher\": \"launch.cmd\""));
        CHECK(contains(mw, "\"requiresSigning\": true"));

        const build::AdapterPlan lin = build::plan_adapter("linux", "server", "game.pack");
        CHECK(contains(build::render_manifest(lin, 1, 2, 3), "\"requiresSigning\": false"));
    }

    CHECK(build::is_known_flavor("desktop"));
    CHECK(build::is_known_flavor("server"));
    CHECK(!build::is_known_flavor("console"));

    // --- launcher: boots relative to its own dir, forwards extra args, LF-only -----------------------
    {
        const build::AdapterPlan p = build::plan_adapter("linux", "server", "game.pack");
        const std::string sh = build::render_launcher(p);
        CHECK(contains(sh, "#!/bin/sh"));
        CHECK(contains(sh, "here=\"$(cd \"$(dirname \"$0\")\" && pwd)\""));
        CHECK(contains(sh, "exec \"$here/bin/context-runtime-server\" --pack \"$here/content/game.pack\" \"$@\""));
        CHECK(sh.find('\r') == std::string::npos); // LF-only (deterministic)
    }

    // --- manifest: deterministic machine-readable JSON; 64-bit values as decimal strings -------------
    {
        const build::AdapterPlan p = build::plan_adapter("linux", "desktop", "game.pack");
        const std::string m1 = build::render_manifest(p, /*engine*/ 7, /*generation*/ 42,
                                                       /*pack_hash*/ 9007199254740993ULL);
        CHECK(contains(m1, "\"schema\": \"ctx:build-artifact\""));
        CHECK(contains(m1, "\"target\": \"linux\""));
        CHECK(contains(m1, "\"flavor\": \"desktop\""));
        CHECK(contains(m1, "\"renderPresent\": true"));
        CHECK(contains(m1, "\"runtimeBinary\": \"bin/context-runtime\""));
        CHECK(contains(m1, "\"engineVersion\": \"7\""));
        // Above 2^53: decimal string preserves the exact value (a JSON double would not).
        CHECK(contains(m1, "\"packHash\": \"9007199254740993\""));
        CHECK(contains(m1, "\"deterministicModuloLink\": true"));
        // Deterministic: the same inputs render byte-identical JSON.
        const std::string m2 = build::render_manifest(p, 7, 42, 9007199254740993ULL);
        CHECK(m1 == m2);
    }

    BUILD_TEST_MAIN_END();
}
