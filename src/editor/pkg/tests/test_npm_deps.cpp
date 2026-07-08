// The heavy R-QA-013 install suite — the R-SEC-005 security invariants, each proven, not configured:
//   1. --ignore-scripts: a package with a postinstall installs but its script NEVER runs.
//   2. a tampered lockfile integrity hash is rejected fail-closed (nothing extracted).
//   3. an unpinned / underspecified version is rejected.
//   4. an incomplete lockfile (missing integrity / entry) is rejected fail-closed.
//   5. a scripts-requiring package is classified native-tier + gated (fail-closed consent gate).
//   6. happy path: a pinned, integrity-valid, no-scripts package extracts + resolves on disk.

#include "context/editor/pkg/base64.h"
#include "context/editor/pkg/codes.h"
#include "context/editor/pkg/npm_install.h"
#include "context/editor/pkg/sha512.h"
#include "context/editor/pkg/tar.h"
#include "pkg_test.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace context::editor::pkg;
namespace fs = std::filesystem;

namespace
{

std::string sri512(std::string_view bytes)
{
    const Sha512Digest d = sha512(bytes);
    return "sha512-" +
           base64_encode(std::string(reinterpret_cast<const char*>(d.data()), d.size()));
}

// Build a package ustar: package/package.json (with the given contents) + package/index.js.
std::string make_package_tar(const std::string& package_json, const std::string& index_js)
{
    std::vector<TarEntry> entries = {
        {"package/", "", true},
        {"package/package.json", package_json, false},
        {"package/index.js", index_js, false},
    };
    return tar_write(entries).value();
}

// An in-memory PackageSource keyed by "<name>@<version>".
class MapSource final : public PackageSource
{
public:
    void put(const std::string& name, const std::string& version, std::string bytes)
    {
        artifacts_[name + "@" + version] = std::move(bytes);
    }
    std::optional<std::string> fetch(const ResolvedPackage& pkg) override
    {
        const auto it = artifacts_.find(pkg.name + "@" + pkg.version);
        if (it == artifacts_.end())
            return std::nullopt;
        return it->second;
    }

private:
    std::map<std::string, std::string> artifacts_;
};

std::string read_file(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return s;
}

fs::path make_temp_dir(const std::string& tag)
{
    // One deterministic per-scenario dir under the OS temp root; cleared first so a prior run leaves
    // no residue that could mask a "nothing was extracted" assertion.
    const fs::path base = fs::temp_directory_path() / ("ctx-install-test-" + tag);
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    return base;
}

} // namespace

int main()
{
    // ============ 1. --ignore-scripts: a postinstall NEVER runs ============
    {
        const std::string pj =
            R"({"name":"with-postinstall","version":"1.0.0","main":"index.js",)"
            R"("scripts":{"postinstall":"node -e \"require('fs').writeFileSync('PWNED','x')\""}})";
        const std::string tar = make_package_tar(pj, "module.exports = 1;\n");

        // The LOCK does NOT flag hasInstallScript -> sandbox tier -> it installs; --ignore-scripts is
        // structural (the extractor executes nothing), so even a smuggled postinstall cannot run.
        const std::string manifest = R"({"dependencies":{"with-postinstall":"1.0.0"}})";
        const std::string lock = R"({"lockfileVersion":3,"packages":{
            "":{"name":"app"},
            "node_modules/with-postinstall":{"version":"1.0.0",
              "resolved":"https://r/x.tgz","integrity":")" +
                                 sri512(tar) + R"("}}})";

        MapSource source;
        source.put("with-postinstall", "1.0.0", tar);

        const InstallPlan plan = plan_install(manifest, lock);
        CHECK(plan.status == PlanStatus::Ok);
        CHECK(plan.packages.size() == 1);
        CHECK(plan.packages[0].tier == TrustTier::Sandbox); // not flagged -> sandbox

        const fs::path root = make_temp_dir("ignorescripts");
        const InstallOutcome out = execute_install(plan, source, root.string(), {});
        CHECK(out.status == InstallStatus::Ok);
        CHECK(out.installed.size() == 1);

        // The package extracted...
        CHECK(fs::exists(root / "node_modules" / "with-postinstall" / "index.js"));
        // ...the postinstall side-effect NEVER happened (no script was executed)...
        CHECK(!fs::exists(root / "PWNED"));
        CHECK(!fs::exists(root / "node_modules" / "with-postinstall" / "PWNED"));
        // ...and the script text was preserved verbatim (we did not run it, nor strip it).
        const std::string extracted_pj =
            read_file(root / "node_modules" / "with-postinstall" / "package.json");
        CHECK(extracted_pj.find("postinstall") != std::string::npos);

        std::error_code ec;
        fs::remove_all(root, ec);
    }

    // ============ 2. a tampered integrity hash is rejected fail-closed ============
    {
        const std::string pj = R"({"name":"lib","version":"2.0.0","main":"index.js"})";
        const std::string tar = make_package_tar(pj, "module.exports = 2;\n");
        // The lock integrity is the SRI of DIFFERENT bytes (a supply-chain tamper).
        const std::string wrong_sri = sri512(tar + "tampered");
        const std::string manifest = R"({"dependencies":{"lib":"2.0.0"}})";
        const std::string lock = R"({"lockfileVersion":3,"packages":{
            "":{"name":"app"},
            "node_modules/lib":{"version":"2.0.0","resolved":"https://r/x.tgz","integrity":")" +
                                 wrong_sri + R"("}}})";

        MapSource source;
        source.put("lib", "2.0.0", tar);

        const InstallPlan plan = plan_install(manifest, lock);
        CHECK(plan.status == PlanStatus::Ok); // the lock is well-formed; the mismatch shows at fetch

        const fs::path root = make_temp_dir("tamper");
        const InstallOutcome out = execute_install(plan, source, root.string(), {});
        CHECK(out.status == InstallStatus::IntegrityMismatch);
        CHECK(out.error_code == std::string(kInstallIntegrityMismatchCode));
        // Fail-closed: nothing was extracted.
        CHECK(!fs::exists(root / "node_modules" / "lib"));

        std::error_code ec;
        fs::remove_all(root, ec);
    }

    // ============ 3. an unpinned / underspecified version is rejected ============
    {
        const std::string manifest = R"({"dependencies":{"lib":"^2.0.0"}})";
        const std::string lock = R"({"lockfileVersion":3,"packages":{
            "":{"name":"app"},
            "node_modules/lib":{"version":"2.0.0","resolved":"https://r/x.tgz",
              "integrity":"sha512-AAAA"}}})";
        const InstallPlan plan = plan_install(manifest, lock);
        CHECK(plan.status == PlanStatus::Rejected);
        CHECK(plan.error_code == std::string(kInstallVersionUnpinnedCode));

        // execute_install propagates the rejection without touching disk.
        MapSource source;
        const fs::path root = make_temp_dir("unpinned");
        const InstallOutcome out = execute_install(plan, source, root.string(), {});
        CHECK(out.status == InstallStatus::Rejected);
        CHECK(out.error_code == std::string(kInstallVersionUnpinnedCode));
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    // ============ 4. an incomplete lockfile (missing integrity) is rejected ============
    {
        const std::string manifest = R"({"dependencies":{"lib":"2.0.0"}})";
        const std::string lock = R"({"lockfileVersion":3,"packages":{
            "":{"name":"app"},
            "node_modules/lib":{"version":"2.0.0","resolved":"https://r/x.tgz"}}})";
        const InstallPlan plan = plan_install(manifest, lock);
        CHECK(plan.status == PlanStatus::Rejected);
        CHECK(plan.error_code == std::string(kInstallLockfileIncompleteCode));
    }

    // ============ 5. a scripts-requiring package is native-tier + consent-gated (fail-closed) ====
    {
        const std::string pj =
            R"({"name":"native-lib","version":"3.0.0","main":"index.js",)"
            R"("scripts":{"install":"node-gyp rebuild"}})";
        const std::string tar = make_package_tar(pj, "module.exports = 3;\n");
        const std::string manifest = R"({"dependencies":{"native-lib":"3.0.0"}})";
        // The LOCK flags hasInstallScript -> native tier.
        const std::string lock = R"({"lockfileVersion":3,"packages":{
            "":{"name":"app"},
            "node_modules/native-lib":{"version":"3.0.0","resolved":"https://r/x.tgz",
              "integrity":")" + sri512(tar) +
                                 R"(","hasInstallScript":true}}})";

        MapSource source;
        source.put("native-lib", "3.0.0", tar);

        const InstallPlan plan = plan_install(manifest, lock);
        CHECK(plan.status == PlanStatus::Ok);
        CHECK(plan.packages.size() == 1);
        CHECK(plan.packages[0].tier == TrustTier::Native); // classified native-tier

        const fs::path root = make_temp_dir("native");
        const InstallOutcome out = execute_install(plan, source, root.string(), {});
        CHECK(out.status == InstallStatus::ConsentRequired);
        CHECK(out.error_code == std::string(kInstallScriptsRequiredCode));
        // Fail-closed: even though the integrity was valid, nothing was extracted.
        CHECK(!fs::exists(root / "node_modules" / "native-lib"));

        // The L-49 gate is the ONLY thing stopping it: WITH a native-consent grant it would proceed.
        InstallOptions granted;
        granted.allow_native = true;
        const InstallOutcome consented = execute_install(plan, source, root.string(), granted);
        CHECK(consented.status == InstallStatus::Ok);
        CHECK(fs::exists(root / "node_modules" / "native-lib" / "index.js"));

        std::error_code ec;
        fs::remove_all(root, ec);
    }

    // ============ 6. happy path: a pinned, integrity-valid, no-scripts package resolves on disk ==
    {
        const std::string pj = R"({"name":"pure-js","version":"1.5.0","main":"index.js"})";
        const std::string index = "exports.greet = function(){ return 'hi'; };\n";
        const std::string tar = make_package_tar(pj, index);
        const std::string manifest = R"({"dependencies":{"pure-js":"1.5.0"}})";
        const std::string lock = R"({"lockfileVersion":3,"packages":{
            "":{"name":"app"},
            "node_modules/pure-js":{"version":"1.5.0","resolved":"https://r/x.tgz",
              "integrity":")" + sri512(tar) +
                                 R"("}}})";

        MapSource source;
        source.put("pure-js", "1.5.0", tar);

        const fs::path root = make_temp_dir("happy");
        const InstallOutcome out = execute_install(plan_install(manifest, lock), source,
                                                   root.string(), {});
        CHECK(out.status == InstallStatus::Ok);
        CHECK(out.installed.size() == 1);
        CHECK(out.installed[0].name == "pure-js");
        CHECK(out.installed[0].version == "1.5.0");

        // The package resolves: package.json `main` -> index.js, both present with the right bytes.
        CHECK(fs::exists(root / "node_modules" / "pure-js" / "package.json"));
        const fs::path idx = root / "node_modules" / "pure-js" / "index.js";
        CHECK(fs::exists(idx));
        CHECK(read_file(idx) == index);

        std::error_code ec;
        fs::remove_all(root, ec);
    }

    // ===== 7. a multi-package plan is ATOMIC: a later failure rolls back an earlier extraction =====
    {
        const std::string good_pj = R"({"name":"good-lib","version":"1.0.0","main":"index.js"})";
        const std::string good_tar = make_package_tar(good_pj, "module.exports = 'good';\n");
        const std::string bad_pj = R"({"name":"bad-lib","version":"1.0.0","main":"index.js"})";
        const std::string bad_tar = make_package_tar(bad_pj, "module.exports = 'bad';\n");

        // good-lib's integrity is correct; bad-lib's lock integrity is the SRI of DIFFERENT bytes
        // (a tamper), so it fails AFTER good-lib has already extracted. good-lib is listed first so
        // the insertion-ordered `packages` map plans + extracts it before bad-lib.
        const std::string manifest =
            R"({"dependencies":{"good-lib":"1.0.0","bad-lib":"1.0.0"}})";
        const std::string lock = R"({"lockfileVersion":3,"packages":{
            "":{"name":"app"},
            "node_modules/good-lib":{"version":"1.0.0","resolved":"https://r/g.tgz","integrity":")" +
                                 sri512(good_tar) + R"("},
            "node_modules/bad-lib":{"version":"1.0.0","resolved":"https://r/b.tgz","integrity":")" +
                                 sri512(bad_tar + "tampered") + R"("}}})";

        MapSource source;
        source.put("good-lib", "1.0.0", good_tar);
        source.put("bad-lib", "1.0.0", bad_tar);

        const InstallPlan plan = plan_install(manifest, lock);
        CHECK(plan.status == PlanStatus::Ok);
        CHECK(plan.packages.size() == 2);
        CHECK(plan.packages[0].pkg.name == "good-lib"); // insertion order preserved
        CHECK(plan.packages[1].pkg.name == "bad-lib");

        const fs::path root = make_temp_dir("atomic");
        const InstallOutcome out = execute_install(plan, source, root.string(), {});
        CHECK(out.status == InstallStatus::IntegrityMismatch);
        CHECK(out.error_code == std::string(kInstallIntegrityMismatchCode));
        // Fail-closed + ATOMIC: good-lib had already extracted, but the bad-lib failure rolled it
        // back, so NOTHING from this call is left on disk (no half-populated node_modules).
        CHECK(!fs::exists(root / "node_modules" / "good-lib"));
        CHECK(!fs::exists(root / "node_modules" / "bad-lib"));

        std::error_code ec;
        fs::remove_all(root, ec);
    }

    PKG_TEST_MAIN_END();
}
