// esbuild-backed TypeScript toolchain (issue #83 / L-61 / R-LANG-002/004). Shells out to the
// SHA-pinned esbuild prebuilt (a build-TIME native binary staged by tools/fetch_esbuild.py) to
// transpile + bundle authored .ts into a JS module the runtime/js V8 host evaluates. esbuild is
// invoked as a SUBPROCESS (never linked), so this driver builds + runs on every toolchain —
// including the local Strawberry-GCC Windows dev gate — and its transpile ctest is a LOCAL gate
// (unlike runtime/js, whose V8 link is CI-only).
//
// Subprocess model: esbuild reads the input .ts and writes the emitted JS to stdout and any
// diagnostics to stderr. We redirect both to scratch files and read them back via the shared
// std::system runner (context/common/subprocess.h, issue #146) — one hardened quoting/metacharacter
// policy + scratch-file RAII + the Windows cmd.exe outer-quote fix, replacing this driver's former
// private copy. Argument quoting is now fail-closed: a path bearing a shell metacharacter is refused
// and surfaced as a structured transpile diagnostic rather than run through an ambiguous command line.

#include "context/runtime/ts/ts_toolchain.h"

#include "context/common/subprocess.h"

#include <filesystem>
#include <string>

namespace context::runtime::ts
{
namespace
{

namespace fs = std::filesystem;
namespace subprocess = context::common::subprocess;

// Trim trailing whitespace/newlines (esbuild --version prints a trailing newline).
std::string rstrip(std::string s)
{
    while (!s.empty())
    {
        const char c = s.back();
        if (c != '\n' && c != '\r' && c != ' ' && c != '\t')
        {
            break;
        }
        s.pop_back();
    }
    return s;
}

const char* formatFlag(ModuleFormat f)
{
    switch (f)
    {
    case ModuleFormat::Iife:
        return "iife";
    case ModuleFormat::Esm:
        return "esm";
    }
    return "esm";
}

class EsbuildToolchain final : public TsToolchain
{
public:
    explicit EsbuildToolchain(std::string binaryPath) : binaryPath_(std::move(binaryPath)) {}

    std::string_view name() const override { return "esbuild"; }

    std::string version(std::string& err) const override
    {
        const subprocess::ScratchFile out(subprocess::make_scratch_path("ctx-ts", ".ver"));
        std::string cmd;
        try
        {
            cmd = subprocess::quote_argument(binaryPath_) + " --version >" +
                  subprocess::quote_argument(out.path().string());
        }
        catch (const subprocess::MetacharacterError& e)
        {
            err = e.what();
            return {};
        }
        const int rc = subprocess::run_command(cmd);
        std::string v = rstrip(subprocess::read_file(out.path()));
        if (rc != 0 || v.empty())
        {
            err = "esbuild --version failed (rc=" + std::to_string(rc) + ")";
            return {};
        }
        return v;
    }

    TranspileResult transpile(const std::string& tsFilePath, const TranspileOptions& opts) override
    {
        TranspileResult result;
        std::error_code ec;
        if (!fs::exists(tsFilePath, ec) || !fs::is_regular_file(tsFilePath, ec))
        {
            result.diagnostics.push_back(
                {std::string(kTsTranspileFailedCode),
                 "input TypeScript file does not exist: " + tsFilePath, tsFilePath});
            return result;
        }

        const subprocess::ScratchFile out(subprocess::make_scratch_path("ctx-ts", ".js"));
        // esbuild's external map path = <outfile>.map; RAII-remove it too (a no-op when no map is
        // requested and none is written).
        const subprocess::ScratchFile mapScratch(fs::path(out.path().string() + ".map"));
        const subprocess::ScratchFile err(subprocess::make_scratch_path("ctx-ts", ".err"));

        std::string cmd;
        try
        {
            cmd = subprocess::quote_argument(binaryPath_) + " " + subprocess::quote_argument(tsFilePath);
            if (opts.bundle)
            {
                cmd += " --bundle";
            }
            cmd += std::string(" --format=") + formatFlag(opts.format);
            cmd += " --log-level=warning";
            if (opts.sourcemap)
            {
                // External sourcemap needs an --outfile (esbuild writes <outfile> + <outfile>.map and
                // appends the trailing `//# sourceMappingURL=` comment to the JS). We read both files
                // back; stderr still carries diagnostics.
                cmd += " --sourcemap --outfile=" + subprocess::quote_argument(out.path().string());
                cmd += " 2>" + subprocess::quote_argument(err.path().string());
            }
            else
            {
                // No map: esbuild writes the JS module to stdout, which we redirect to the out scratch.
                cmd += " >" + subprocess::quote_argument(out.path().string()) + " 2>" +
                       subprocess::quote_argument(err.path().string());
            }
        }
        catch (const subprocess::MetacharacterError& e)
        {
            // A path carrying a shell metacharacter is refused fail-closed (reconciled policy); surface
            // it as ONE structured envelope rather than running an ambiguous command line.
            const std::string_view code =
                opts.bundle ? kTsBundleFailedCode : kTsTranspileFailedCode;
            result.diagnostics.push_back({std::string(code), e.what(), tsFilePath});
            return result;
        }

        const int rc = subprocess::run_command(cmd);
        const std::string js = subprocess::read_file(out.path());
        const std::string mapJson = opts.sourcemap ? subprocess::read_file(mapScratch.path())
                                                   : std::string{};
        const std::string diag = rstrip(subprocess::read_file(err.path()));

        if (rc != 0)
        {
            // A non-zero esbuild exit = a transpile/bundle failure. Surface it as ONE structured
            // envelope carrying the stable catalog code; `--bundle` failures (unresolved imports)
            // get the bundle-class code, plain transpile failures the transpile-class code.
            const std::string_view code =
                opts.bundle ? kTsBundleFailedCode : kTsTranspileFailedCode;
            result.diagnostics.push_back(
                {std::string(code),
                 diag.empty() ? "esbuild exited " + std::to_string(rc) + " with no diagnostic"
                              : diag,
                 tsFilePath});
            return result;
        }

        result.ok = true;
        result.js = js;
        result.sourceMap = mapJson;
        return result;
    }

private:
    std::string binaryPath_;
};

} // namespace

bool esbuildToolchainAvailable(const std::string& esbuildBinaryPath)
{
    std::error_code ec;
    return fs::exists(esbuildBinaryPath, ec) && fs::is_regular_file(esbuildBinaryPath, ec);
}

std::unique_ptr<TsToolchain> createEsbuildToolchain(std::string esbuildBinaryPath, std::string& err)
{
    if (!esbuildToolchainAvailable(esbuildBinaryPath))
    {
        err = "esbuild binary not found or not a regular file: " + esbuildBinaryPath;
        return nullptr;
    }
    return std::make_unique<EsbuildToolchain>(std::move(esbuildBinaryPath));
}

} // namespace context::runtime::ts
