// `context install` CLI-level tests (R-SEC-005, issue #100): the end-to-end envelope over a REAL
// temp project + an offline --source cache — happy install, --dry-run plan, a tampered-integrity
// failure with the right exit class, the native-tier consent gate, and the missing-file guards.
// Fixtures (a ustar artifact + its real SRI) are built with the pkg module directly.

#include "context/cli/app.h"
#include "context/editor/contract/envelope.h"
#include "context/editor/pkg/base64.h"
#include "context/editor/pkg/codes.h"
#include "context/editor/pkg/sha512.h"
#include "context/editor/pkg/tar.h"
#include "cli_test.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using context::cli::run;
using context::editor::contract::Envelope;
namespace pkg = context::editor::pkg;
namespace fs = std::filesystem;

namespace
{
std::string err_code(const Envelope& e)
{
    return e.error().has_value() ? e.error()->code : std::string();
}

std::string sri512(std::string_view bytes)
{
    const pkg::Sha512Digest d = pkg::sha512(bytes);
    return "sha512-" +
           pkg::base64_encode(std::string(reinterpret_cast<const char*>(d.data()), d.size()));
}

void write_file(const fs::path& p, const std::string& content)
{
    fs::create_directories(p.parent_path());
    std::ofstream os(p, std::ios::binary | std::ios::trunc);
    os.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::string package_tar(const std::string& pj, const std::string& index)
{
    std::vector<pkg::TarEntry> entries = {
        {"package/", "", true},
        {"package/package.json", pj, false},
        {"package/index.js", index, false},
    };
    return pkg::tar_write(entries).value();
}
} // namespace

int main()
{
    const fs::path base = fs::temp_directory_path() / "ctx-install-cli-test";
    std::error_code ec;
    fs::remove_all(base, ec);

    const fs::path project = base / "project";
    const fs::path cache = base / "cache";
    fs::create_directories(project);
    fs::create_directories(cache);

    const std::string pj = R"({"name":"left-pad","version":"1.3.0","main":"index.js"})";
    const std::string index = "module.exports = function(){};\n";
    const std::string tar = package_tar(pj, index);
    // The offline cache filename convention: <sanitized-name>-<version>.tar
    write_file(cache / "left-pad-1.3.0.tar", tar);

    write_file(project / "package.json", R"({"dependencies":{"left-pad":"1.3.0"}})");
    const std::string good_lock = R"({"lockfileVersion":3,"packages":{
        "":{"name":"app"},
        "node_modules/left-pad":{"version":"1.3.0","resolved":"https://r/left-pad-1.3.0.tgz",
          "integrity":")" + sri512(tar) + R"("}}})";
    write_file(project / "package-lock.json", good_lock);

    // --- --dry-run: validates the plan (tiers), fetches nothing --------------------------------
    {
        const Envelope e = run({"install", "--project", project.string(), "--dry-run"});
        CHECK(e.ok());
        CHECK(e.data().at("wouldApply").as_bool() == false);
        CHECK(e.data().at("packages").size() == 1);
        CHECK(e.data().at("packages").at(std::size_t{0}).at("tier").as_string() == "sandbox");
        CHECK(!fs::exists(project / "node_modules")); // nothing installed
    }

    // --- happy install over the offline cache ---------------------------------------------------
    {
        const Envelope e =
            run({"install", "--project", project.string(), "--source", cache.string()});
        CHECK(e.ok());
        CHECK(e.data().at("count").as_int() == 1);
        CHECK(e.data().at("ignoredScripts").as_bool() == true);
        CHECK(fs::exists(project / "node_modules" / "left-pad" / "index.js"));
    }

    // --- missing --source (and not --dry-run) is a usage error ----------------------------------
    {
        const Envelope e = run({"install", "--project", project.string()});
        CHECK(!e.ok());
        CHECK(err_code(e) == "usage.missing_argument");
    }

    // --- a tampered lockfile integrity is rejected with the validation exit class ---------------
    {
        const fs::path proj2 = base / "project-tamper";
        fs::create_directories(proj2);
        write_file(proj2 / "package.json", R"({"dependencies":{"left-pad":"1.3.0"}})");
        const std::string bad_lock = R"({"lockfileVersion":3,"packages":{
            "":{"name":"app"},
            "node_modules/left-pad":{"version":"1.3.0","resolved":"https://r/x.tgz",
              "integrity":")" + sri512(tar + "tampered") + R"("}}})";
        write_file(proj2 / "package-lock.json", bad_lock);

        const Envelope e =
            run({"install", "--project", proj2.string(), "--source", cache.string()});
        CHECK(!e.ok());
        CHECK(err_code(e) == std::string(pkg::kInstallIntegrityMismatchCode));
        CHECK(e.exit_code() == 5); // validation class
        CHECK(!fs::exists(proj2 / "node_modules" / "left-pad"));
    }

    // --- a scripts-requiring package is refused fail-closed at the consent gate -----------------
    {
        const fs::path proj3 = base / "project-native";
        fs::create_directories(proj3);
        write_file(proj3 / "package.json", R"({"dependencies":{"native-lib":"1.0.0"}})");
        const std::string native_pj =
            R"({"name":"native-lib","version":"1.0.0","scripts":{"install":"node-gyp rebuild"}})";
        const std::string native_tar = package_tar(native_pj, "module.exports=1;\n");
        write_file(cache / "native-lib-1.0.0.tar", native_tar);
        const std::string native_lock = R"({"lockfileVersion":3,"packages":{
            "":{"name":"app"},
            "node_modules/native-lib":{"version":"1.0.0","resolved":"https://r/x.tgz",
              "integrity":")" + sri512(native_tar) + R"(","hasInstallScript":true}}})";
        write_file(proj3 / "package-lock.json", native_lock);

        const Envelope e =
            run({"install", "--project", proj3.string(), "--source", cache.string()});
        CHECK(!e.ok());
        CHECK(err_code(e) == std::string(pkg::kInstallScriptsRequiredCode));
        CHECK(e.exit_code() == 6); // permission class
        CHECK(!fs::exists(proj3 / "node_modules" / "native-lib"));
    }

    // --- missing package.json / package-lock.json guards ----------------------------------------
    {
        const fs::path empty = base / "empty-project";
        fs::create_directories(empty);
        const Envelope e = run({"install", "--project", empty.string(), "--dry-run"});
        CHECK(!e.ok());
        CHECK(err_code(e) == "file.not_found");
    }

    fs::remove_all(base, ec);
    CLI_TEST_MAIN_END();
}
