// build-test_adapter (M8 task a06) — the pure export-adapter plan + launcher + manifest generators.
// Coverage (R-QA-013 happy + edge): the two real Linux flavors (desktop render-present / server render-
// absent) plan a correct tarball layout; unsupported targets/flavors plan the honest stub; the launcher
// boots relative to its own dir + forwards args; the manifest is deterministic machine-readable JSON.

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

    // --- unsupported target / flavor: the honest stub (R-BUILD-007) ----------------------------------
    {
        const build::AdapterPlan win = build::plan_adapter("windows", "desktop", "game.pack");
        CHECK(!win.supported);
        CHECK(win.layout.empty());
        CHECK(build::render_launcher(win).empty());
        CHECK(build::render_manifest(win, 1, 2, 3).empty());

        const build::AdapterPlan bad_flavor = build::plan_adapter("linux", "console", "game.pack");
        CHECK(!bad_flavor.supported);
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
