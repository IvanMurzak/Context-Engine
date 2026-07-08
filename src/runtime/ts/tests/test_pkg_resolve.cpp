// R-SEC-005 end-to-end (issue #100): an engine-installed pure-JS package RESOLVES + IMPORTS in the
// TS host. The install path (src/editor/pkg/) extracts a fixture package into a temp project's
// node_modules WITHOUT running scripts; the TS toolchain (esbuild) then bundles an authored .ts
// entry that imports it, proving esbuild's node module-resolution finds + inlines the installed
// package. A LOCAL gate (esbuild is a subprocess; no V8 link).

#include "context/editor/pkg/base64.h"
#include "context/editor/pkg/npm_install.h"
#include "context/editor/pkg/sha512.h"
#include "context/editor/pkg/tar.h"
#include "context/runtime/ts/ts_toolchain.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace itest
{
int g_failures = 0;
inline void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}
} // namespace itest

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            itest::fail(__FILE__, __LINE__, #cond);                                                \
    } while (false)

#ifndef CONTEXT_ESBUILD_PATH
#error "CONTEXT_ESBUILD_PATH must be defined by CMake (the staged esbuild binary path)"
#endif

namespace pkg = context::editor::pkg;
namespace cts = context::runtime::ts;
namespace fs = std::filesystem;

namespace
{
class MapSource final : public pkg::PackageSource
{
public:
    void put(const std::string& key, std::string bytes) { artifacts_[key] = std::move(bytes); }
    std::optional<std::string> fetch(const pkg::ResolvedPackage& p) override
    {
        const auto it = artifacts_.find(p.name + "@" + p.version);
        return it == artifacts_.end() ? std::nullopt : std::optional<std::string>(it->second);
    }

private:
    std::map<std::string, std::string> artifacts_;
};

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
} // namespace

int main()
{
    const std::string kEsbuild = CONTEXT_ESBUILD_PATH;

    // Build a pure-JS (CommonJS) fixture package `greeter@1.0.0`.
    const std::string pj = R"({"name":"greeter","version":"1.0.0","main":"index.js"})";
    const std::string index = "module.exports.greet = function greet(){ return 'hi from greeter'; };\n";
    std::vector<pkg::TarEntry> entries = {
        {"package/", "", true},
        {"package/package.json", pj, false},
        {"package/index.js", index, false},
    };
    const std::string tar = pkg::tar_write(entries).value();

    const std::string manifest = R"({"dependencies":{"greeter":"1.0.0"}})";
    const std::string lock = R"({"lockfileVersion":3,"packages":{
        "":{"name":"app"},
        "node_modules/greeter":{"version":"1.0.0","resolved":"https://r/x.tgz","integrity":")" +
                             sri512(tar) + R"("}}})";

    MapSource source;
    source.put("greeter@1.0.0", tar);

    const fs::path project = fs::temp_directory_path() / "ctx-install-ts-resolve";
    std::error_code ec;
    fs::remove_all(project, ec);
    fs::create_directories(project);

    // Install (no scripts) into the temp project.
    const pkg::InstallOutcome out =
        pkg::execute_install(pkg::plan_install(manifest, lock), source, project.string(), {});
    CHECK(out.status == pkg::InstallStatus::Ok);
    CHECK(fs::exists(project / "node_modules" / "greeter" / "index.js"));

    // An authored .ts entry that imports the installed package.
    const fs::path entry = project / "entry.ts";
    write_file(entry, "import greeter from 'greeter';\nexport const msg: string = greeter.greet();\n");

    // Bundle it: esbuild's node resolution must find node_modules/greeter and inline its code.
    std::string err;
    std::unique_ptr<cts::TsToolchain> tc = cts::createEsbuildToolchain(kEsbuild, err);
    CHECK(tc != nullptr);
    if (tc == nullptr)
    {
        std::fprintf(stderr, "esbuild unavailable: %s\n", err.c_str());
        return itest::g_failures == 0 ? 0 : 1;
    }

    cts::TranspileOptions opts;
    opts.bundle = true;
    opts.format = cts::ModuleFormat::Esm;
    const cts::TranspileResult result = tc->transpile(entry.string(), opts);
    CHECK(result.ok);
    // The installed package's code was resolved + inlined into the bundle (it imports + resolves).
    CHECK(result.js.find("hi from greeter") != std::string::npos);

    fs::remove_all(project, ec);
    return itest::g_failures == 0 ? 0 : 1;
}
