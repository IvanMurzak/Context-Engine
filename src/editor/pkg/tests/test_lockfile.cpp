// package.json + package-lock.json parsing + the pin/completeness gate: exact-pin acceptance,
// unpinned rejection, transitive integrity enforcement, hasInstallScript classification, and
// fail-closed handling of an incomplete / malformed lockfile.

#include "context/editor/pkg/lockfile.h"
#include "context/editor/pkg/codes.h"
#include "pkg_test.h"

#include <string>

using namespace context::editor::pkg;

int main()
{
    // --- is_exact_version -----------------------------------------------------------------------
    CHECK(is_exact_version("1.2.3"));
    CHECK(is_exact_version("0.0.1"));
    CHECK(is_exact_version("1.2.3-rc.1"));
    CHECK(is_exact_version("1.0.0-alpha.1"));
    CHECK(is_exact_version("1.0.0+build.5"));
    CHECK(is_exact_version("1.0.0-beta+exp.sha.5114f85"));
    CHECK(!is_exact_version("^1.0.0"));
    CHECK(!is_exact_version("~1.2.3"));
    CHECK(!is_exact_version(">=1.0.0"));
    CHECK(!is_exact_version("1.x"));
    CHECK(!is_exact_version("1.2"));
    CHECK(!is_exact_version("*"));
    CHECK(!is_exact_version("latest"));
    CHECK(!is_exact_version("1.0.0 || 2.0.0"));
    CHECK(!is_exact_version("npm:foo@1.0.0"));
    CHECK(!is_exact_version("git+https://github.com/x/y.git"));
    CHECK(!is_exact_version("01.2.3")); // leading zero
    CHECK(!is_exact_version(""));

    // --- a fully-pinned, integrity-bearing, transitive lock parses cleanly ----------------------
    const std::string manifest = R"({
        "name": "app", "version": "1.0.0",
        "dependencies": { "left-pad": "1.3.0" },
        "devDependencies": { "tape": "5.7.5" }
    })";
    const std::string lock = R"({
        "name": "app", "lockfileVersion": 3,
        "packages": {
            "": { "name": "app", "version": "1.0.0" },
            "node_modules/left-pad": {
                "version": "1.3.0", "resolved": "https://r/left-pad-1.3.0.tgz",
                "integrity": "sha512-AAAA"
            },
            "node_modules/tape": {
                "version": "5.7.5", "resolved": "https://r/tape-5.7.5.tgz",
                "integrity": "sha512-BBBB", "dev": true
            },
            "node_modules/deep-transitive": {
                "version": "2.1.0", "resolved": "https://r/deep-2.1.0.tgz",
                "integrity": "sha512-CCCC"
            }
        }
    })";
    {
        const LockfileParse p = parse_lockfile(manifest, lock);
        CHECK(p.status == LockParseStatus::Ok);
        CHECK(p.error_code.empty());
        CHECK(p.packages.size() == 3); // left-pad, tape, deep-transitive (transitive incl.)
        bool saw_leftpad = false, saw_tape = false, saw_deep = false;
        for (const ResolvedPackage& pkg : p.packages)
        {
            if (pkg.name == "left-pad")
            {
                saw_leftpad = true;
                CHECK(pkg.version == "1.3.0");
                CHECK(pkg.integrity == "sha512-AAAA");
                CHECK(!pkg.has_install_script);
                CHECK(!pkg.dev);
            }
            if (pkg.name == "tape")
            {
                saw_tape = true;
                CHECK(pkg.dev); // devDependency
            }
            if (pkg.name == "deep-transitive")
                saw_deep = true;
        }
        CHECK(saw_leftpad && saw_tape && saw_deep);
    }

    // --- an unpinned root spec is rejected (version-pin-violation) -------------------------------
    {
        const std::string bad_manifest = R"({ "dependencies": { "left-pad": "^1.3.0" } })";
        const LockfileParse p = parse_lockfile(bad_manifest, lock);
        CHECK(p.status == LockParseStatus::VersionUnpinned);
        CHECK(p.error_code == std::string(kInstallVersionUnpinnedCode));
        CHECK(p.offending == "left-pad@^1.3.0");
    }

    // --- a transitive entry missing integrity is rejected fail-closed ---------------------------
    {
        const std::string no_integrity_lock = R"({
            "lockfileVersion": 3,
            "packages": {
                "": { "name": "app" },
                "node_modules/left-pad": {
                    "version": "1.3.0", "resolved": "https://r/left-pad-1.3.0.tgz",
                    "integrity": "sha512-AAAA"
                },
                "node_modules/left-pad/node_modules/dep": {
                    "version": "9.9.9", "resolved": "https://r/dep-9.9.9.tgz"
                }
            }
        })";
        const std::string min_manifest = R"({ "dependencies": { "left-pad": "1.3.0" } })";
        const LockfileParse p = parse_lockfile(min_manifest, no_integrity_lock);
        CHECK(p.status == LockParseStatus::Incomplete);
        CHECK(p.error_code == std::string(kInstallLockfileIncompleteCode));
    }

    // --- a declared dependency absent from the lock is rejected ---------------------------------
    {
        const std::string missing_lock = R"({ "lockfileVersion": 3, "packages": {
            "": { "name": "app" } } })";
        const std::string min_manifest = R"({ "dependencies": { "left-pad": "1.3.0" } })";
        const LockfileParse p = parse_lockfile(min_manifest, missing_lock);
        CHECK(p.status == LockParseStatus::Incomplete);
        CHECK(p.error_code == std::string(kInstallLockfileIncompleteCode));
    }

    // --- hasInstallScript is read from the lock entry (native-tier classification signal) --------
    {
        const std::string scripts_lock = R"({ "lockfileVersion": 3, "packages": {
            "": { "name": "app" },
            "node_modules/native-dep": {
                "version": "1.0.0", "resolved": "https://r/native-1.0.0.tgz",
                "integrity": "sha512-DDDD", "hasInstallScript": true
            } } })";
        const std::string min_manifest = R"({ "dependencies": { "native-dep": "1.0.0" } })";
        const LockfileParse p = parse_lockfile(min_manifest, scripts_lock);
        CHECK(p.status == LockParseStatus::Ok);
        CHECK(p.packages.size() == 1);
        CHECK(p.packages[0].has_install_script);
    }

    // --- a traversing install path is rejected fail-closed (directory-traversal guard, R-SEC-008) --
    // The `packages` key becomes the extraction destination; a ".." segment would escape project_root
    // at extract time, so it must be refused at parse — before it ever becomes an install_path.
    {
        const std::string min_manifest = R"({ "dependencies": { "left-pad": "1.3.0" } })";
        for (const char* evil_key :
             {"node_modules/../../../../tmp/evil", "node_modules/left-pad/../../../evil"})
        {
            const std::string traversal_lock =
                std::string("{ \"lockfileVersion\": 3, \"packages\": {"
                            " \"\": { \"name\": \"app\" }, \"") +
                evil_key +
                "\": { \"version\": \"1.3.0\", \"resolved\": \"https://r/x-1.3.0.tgz\","
                " \"integrity\": \"sha512-AAAA\" } } }";
            const LockfileParse p = parse_lockfile(min_manifest, traversal_lock);
            CHECK(p.status == LockParseStatus::Malformed);
            CHECK(p.error_code == std::string(kInstallLockfileIncompleteCode));
        }
    }

    // --- malformed JSON / non-v3 shape -> fail-closed -------------------------------------------
    CHECK(parse_lockfile("{ not json", lock).status == LockParseStatus::Malformed);
    CHECK(parse_lockfile(manifest, "{ \"lockfileVersion\": 1 }").status ==
          LockParseStatus::Malformed); // no `packages` map

    PKG_TEST_MAIN_END();
}
