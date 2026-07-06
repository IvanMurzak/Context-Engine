// esbuild-backed TypeScript toolchain (issue #83 / L-61 / R-LANG-002/004). Shells out to the
// SHA-pinned esbuild prebuilt (a build-TIME native binary staged by tools/fetch_esbuild.py) to
// transpile + bundle authored .ts into a JS module the runtime/js V8 host evaluates. esbuild is
// invoked as a SUBPROCESS (never linked), so this driver builds + runs on every toolchain —
// including the local Strawberry-GCC Windows dev gate — and its transpile ctest is a LOCAL gate
// (unlike runtime/js, whose V8 link is CI-only).
//
// Subprocess model: esbuild reads the input .ts and writes the emitted JS to stdout and any
// diagnostics to stderr. We redirect both to temp files via std::system and read them back —
// the same std::system pattern src/editor/merge/tests/test_git_driver.cpp uses, including the
// Windows cmd.exe outer-quote fix (cmd /c strips ONE outer quote pair, so a command with several
// quoted segments is wrapped in an extra outer pair on _WIN32).

#include "context/runtime/ts/ts_toolchain.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#if defined(_WIN32)
#include <process.h> // _getpid
#else
#include <sys/wait.h> // WIFEXITED / WEXITSTATUS
#include <unistd.h>   // getpid
#endif

namespace context::runtime::ts
{
namespace
{

namespace fs = std::filesystem;

// A unique temp-file stem per call, so concurrent transpiles (ctest -j) never collide.
std::string uniqueStem()
{
    static std::atomic<unsigned long long> counter{0};
    const unsigned long long n = counter.fetch_add(1, std::memory_order_relaxed);
    std::ostringstream os;
    os << "ctx-ts-" << static_cast<unsigned long long>(
#if defined(_WIN32)
              _getpid()
#else
              ::getpid()
#endif
              )
       << "-" << n;
    return os.str();
}

// Read a whole file into a string ("" if absent/unreadable — a missing esbuild stdout on a
// failed run is expected, not an error to re-report).
std::string slurp(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in)
    {
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

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

// Wrap a token in double quotes for the shell (paths may contain spaces).
std::string q(const std::string& s) { return "\"" + s + "\""; }

// Run `command` (already fully quoted), returning the process exit code. Applies the Windows
// cmd.exe extra-outer-quote-pair fix so a multi-quoted command survives `cmd /c`.
int runShell(const std::string& command)
{
#if defined(_WIN32)
    const std::string wrapped = "\"" + command + "\"";
#else
    const std::string wrapped = command;
#endif
    const int rc = std::system(wrapped.c_str());
    // std::system encodes the child status; on POSIX extract the exit code, on Windows it is the
    // code directly. A non-zero result is all this driver needs (esbuild returns 1 on failure).
#if defined(_WIN32)
    return rc;
#else
    if (rc == -1)
    {
        return -1;
    }
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : (rc == 0 ? 0 : 1);
#endif
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
        const fs::path tmp = fs::temp_directory_path();
        const std::string stem = uniqueStem();
        const fs::path out = tmp / (stem + ".ver");
        const std::string cmd = q(binaryPath_) + " --version >" + q(out.string());
        const int rc = runShell(cmd);
        std::string v = rstrip(slurp(out));
        std::error_code ec;
        fs::remove(out, ec);
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

        const fs::path tmp = fs::temp_directory_path();
        const std::string stem = uniqueStem();
        const fs::path outFile = tmp / (stem + ".js");
        const fs::path errFile = tmp / (stem + ".err");

        std::string cmd = q(binaryPath_) + " " + q(tsFilePath);
        if (opts.bundle)
        {
            cmd += " --bundle";
        }
        cmd += std::string(" --format=") + formatFlag(opts.format);
        cmd += " --log-level=warning";
        cmd += " >" + q(outFile.string()) + " 2>" + q(errFile.string());

        const int rc = runShell(cmd);
        const std::string js = slurp(outFile);
        const std::string diag = rstrip(slurp(errFile));
        fs::remove(outFile, ec);
        fs::remove(errFile, ec);

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
