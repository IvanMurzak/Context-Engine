// tsgo-backed TypeScript semantic typechecker (issue #85 / R-LANG-002/004 / R-CLI-008). Shells out to
// the SHA-pinned tsgo prebuilt (microsoft/typescript-go — a build-TIME native binary staged by
// tools/fetch_tsc.py) to run a `--noEmit` semantic typecheck over authored .ts, surfacing each type
// error as a ts.type_error diagnostic. tsgo is invoked as a SUBPROCESS (never linked), so — like the
// esbuild transpile driver and UNLIKE runtime/js's V8 link — this builds + runs on every toolchain
// including the local Strawberry-GCC Windows dev gate, and its ctest is a LOCAL gate.
//
// esbuild (ts_toolchain.h) transpiles by STRIPPING types without checking them; tsgo is the tsc-class
// tool that actually type-analyzes, closing the agent author->typecheck->fix loop.
//
// Subprocess model + the std::system quoting/redirect helpers mirror esbuild_toolchain.cpp (and
// src/editor/merge/tests/test_git_driver.cpp), including the Windows cmd.exe outer-quote fix (cmd /c
// strips ONE outer quote pair). The small subprocess helpers are duplicated from esbuild_toolchain.cpp
// on purpose for now — consolidating the repo's several std::system runner copies is a tracked P3
// backlog item (PR #134 finding); a premature shared header would couple the transpile + typecheck TUs.

#include "context/runtime/ts/ts_typecheck.h"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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

// A unique temp-file stem per call, so concurrent typechecks (ctest -j) never collide.
std::string uniqueStem()
{
    static std::atomic<unsigned long long> counter{0};
    const unsigned long long n = counter.fetch_add(1, std::memory_order_relaxed);
    std::ostringstream os;
    os << "ctx-tsc-" << static_cast<unsigned long long>(
#if defined(_WIN32)
              _getpid()
#else
              ::getpid()
#endif
              )
       << "-" << n;
    return os.str();
}

// Read a whole file into a string ("" if absent/unreadable).
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

// Trim trailing whitespace/newlines.
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

// Wrap a token in double quotes for the shell (paths may contain spaces). SECURITY: this does NOT
// escape a double-quote (or other shell metacharacter) already inside `s`, so every caller MUST pass
// a CONTROLLED value — today the CMake-staged tsgo binary path (binaryPath_) and a caller-owned .ts
// path — never an untrusted / agent-authored string. A hardened shared escaper is the tracked P3
// subprocess-runner consolidation (see the file header + PR #134 finding); until it lands, the
// trusted-callers precondition is what keeps this injection-safe.
std::string q(const std::string& s) { return "\"" + s + "\""; }

// Run `command` (already fully quoted), returning the process exit code. Applies the Windows cmd.exe
// extra-outer-quote-pair fix so a multi-quoted command survives `cmd /c`.
int runShell(const std::string& command)
{
#if defined(_WIN32)
    const std::string wrapped = "\"" + command + "\"";
#else
    const std::string wrapped = command;
#endif
    const int rc = std::system(wrapped.c_str());
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

// Parse a decimal integer (all digits, non-empty). No exceptions, warning-clean under -Werror.
bool parseInt(const std::string& s, int& out)
{
    if (s.empty())
    {
        return false;
    }
    int v = 0;
    for (const char c : s)
    {
        if (c < '0' || c > '9')
        {
            return false;
        }
        v = v * 10 + (c - '0');
    }
    out = v;
    return true;
}

// Parse ONE tsc-format diagnostic line: `path(line,col): error TSxxxx: message`. Returns false for a
// non-diagnostic line (blank, a summary, or a continuation). tsgo emits exactly this shape under
// `--pretty false`, one line per error. On success fills file/line/col and the message (which keeps
// the leading `TSxxxx:` so the caller sees the tsc code alongside the text).
bool parseDiagnosticLine(const std::string& line, TsDiagnostic& out)
{
    const std::string anchor = "): error ";
    const std::size_t ap = line.find(anchor);
    if (ap == std::string::npos)
    {
        return false;
    }
    const std::size_t open = line.rfind('(', ap);
    if (open == std::string::npos || open == 0)
    {
        return false; // no file part before the (line,col) group
    }
    const std::string pos = line.substr(open + 1, ap - (open + 1)); // "line,col"
    const std::size_t comma = pos.find(',');
    if (comma == std::string::npos)
    {
        return false;
    }
    int lineNo = 0;
    int colNo = 0;
    if (!parseInt(pos.substr(0, comma), lineNo) || !parseInt(pos.substr(comma + 1), colNo))
    {
        return false;
    }
    out.code = std::string(kTsTypeErrorCode);
    out.file = line.substr(0, open);
    out.line = lineNo;
    out.column = colNo;
    out.message = line.substr(ap + anchor.size()); // "TSxxxx: message"
    return true;
}

class TsgoTypechecker final : public TsTypechecker
{
public:
    explicit TsgoTypechecker(std::string binaryPath) : binaryPath_(std::move(binaryPath)) {}

    std::string_view name() const override { return "tsgo"; }

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
            err = "tsgo --version failed (rc=" + std::to_string(rc) + ")";
            return {};
        }
        // tsgo prints "Version <x>"; strip the leading label so callers get a bare version string
        // (nicer as the derivation cache-key component). Fall back to the raw text if the shape differs.
        const std::string prefix = "Version ";
        if (v.rfind(prefix, 0) == 0)
        {
            v = v.substr(prefix.size());
        }
        return v;
    }

    TypecheckResult check(const std::string& tsFilePath) override
    {
        TypecheckResult result;
        std::error_code ec;
        if (!fs::exists(tsFilePath, ec) || !fs::is_regular_file(tsFilePath, ec))
        {
            result.diagnostics.push_back({std::string(kTsTypeErrorCode),
                                          "input TypeScript file does not exist: " + tsFilePath,
                                          tsFilePath, 0, 0});
            return result;
        }

        const fs::path tmp = fs::temp_directory_path();
        const std::string stem = uniqueStem();
        const fs::path outFile = tmp / (stem + ".out");
        const fs::path errFile = tmp / (stem + ".err");

        // `--noEmit` = typecheck only (no JS written; the transpile is ts_toolchain.h's job).
        // `--pretty false` = stable, ANSI-free, one-line-per-diagnostic output for parsing.
        std::string cmd = q(binaryPath_) + " --noEmit --pretty false " + q(tsFilePath);
        cmd += " >" + q(outFile.string()) + " 2>" + q(errFile.string());
        const int rc = runShell(cmd);

        const std::string out = slurp(outFile);
        const std::string errText = slurp(errFile);
        fs::remove(outFile, ec);
        fs::remove(errFile, ec);

        if (rc == 0)
        {
            result.ok = true; // 0 diagnostics — the source is type-valid
            return result;
        }

        // Non-zero exit: parse every `path(line,col): error TSxxxx: message` line from stdout+stderr.
        parseDiagnostics(out, result.diagnostics);
        parseDiagnostics(errText, result.diagnostics);

        if (result.diagnostics.empty())
        {
            // A non-zero exit with no parseable diagnostic (e.g. a tool misconfiguration or panic):
            // surface it as ONE ts.type_error rather than silently dropping it, carrying the raw tool
            // output so the failure is never lost.
            std::string raw = rstrip(out);
            if (raw.empty())
            {
                raw = rstrip(errText);
            }
            result.diagnostics.push_back(
                {std::string(kTsTypeErrorCode),
                 raw.empty() ? "tsgo exited " + std::to_string(rc) + " with no diagnostic" : raw,
                 tsFilePath, 0, 0});
        }
        return result;
    }

private:
    static void parseDiagnostics(const std::string& text, std::vector<TsDiagnostic>& into)
    {
        std::istringstream stream(text);
        std::string line;
        while (std::getline(stream, line))
        {
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back(); // strip a trailing CR on Windows output
            }
            TsDiagnostic diag;
            if (parseDiagnosticLine(line, diag))
            {
                into.push_back(std::move(diag));
            }
        }
    }

    std::string binaryPath_;
};

} // namespace

bool tscToolchainAvailable(const std::string& tscBinaryPath)
{
    std::error_code ec;
    return fs::exists(tscBinaryPath, ec) && fs::is_regular_file(tscBinaryPath, ec);
}

std::unique_ptr<TsTypechecker> createTsgoTypechecker(std::string tscBinaryPath, std::string& err)
{
    if (!tscToolchainAvailable(tscBinaryPath))
    {
        err = "tsgo binary not found or not a regular file: " + tscBinaryPath;
        return nullptr;
    }
    return std::make_unique<TsgoTypechecker>(std::move(tscBinaryPath));
}

} // namespace context::runtime::ts
