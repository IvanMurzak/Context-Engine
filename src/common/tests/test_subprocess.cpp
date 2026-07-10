// Unit tests for the shared subprocess runner (issue #146). The quoting/metacharacter policy is
// exercised for BOTH shell shapes (Cmd + Posix) on EVERY OS leg via the explicit Shell parameter — so
// the Windows reject list is verified on Linux/macOS and the POSIX reject list on Windows, closing the
// platform-#if blind spot a #ifdef-only policy would leave. Also covers wrap_command (the cmd.exe
// outer-quote workaround, pure), scratch-path uniqueness, ScratchFile RAII, and the file read/write
// helpers. run_command's live std::system launch + exit-code decode is exercised by the caller ctests
// (ts / merge-git-driver / shadercc round-trip) on each real host, so it is not re-launched here.

#include "context/common/subprocess.h"

#include "common_test.h"

#include <filesystem>
#include <string>

using context::common::subprocess::has_shell_metacharacters;
using context::common::subprocess::make_scratch_path;
using context::common::subprocess::MetacharacterError;
using context::common::subprocess::quote_argument;
using context::common::subprocess::read_file;
using context::common::subprocess::ScratchFile;
using context::common::subprocess::Shell;
using context::common::subprocess::wrap_command;
using context::common::subprocess::write_file;

namespace
{

// Does quote_argument(arg, shell) throw MetacharacterError?
bool quote_rejects(const std::string& arg, Shell shell)
{
    try
    {
        (void)quote_argument(arg, shell);
        return false;
    }
    catch (const MetacharacterError&)
    {
        return true;
    }
}

// The reconciled STRICTEST policy: Posix rejects " $ ` \ ; Cmd rejects " and % only.
void test_metacharacter_policy_posix()
{
    // Every POSIX metacharacter is rejected, one at a time inside an otherwise-clean path.
    CHECK(has_shell_metacharacters("a\"b", Shell::Posix));  // double-quote ends the token
    CHECK(has_shell_metacharacters("a$b", Shell::Posix));   // $ expands inside "..."
    CHECK(has_shell_metacharacters("a`b", Shell::Posix));   // backtick command substitution
    CHECK(has_shell_metacharacters("a\\b", Shell::Posix));  // backslash is special inside "..."
    // A percent is NOT a POSIX metacharacter (it is only special to cmd.exe).
    CHECK(!has_shell_metacharacters("a%b", Shell::Posix));
    // Clean tokens (including spaces — quoting handles those) are accepted.
    CHECK(!has_shell_metacharacters("/usr/local/bin/tint", Shell::Posix));
    CHECK(!has_shell_metacharacters("/tmp/dir with spaces/x.wgsl", Shell::Posix));
}

void test_metacharacter_policy_cmd()
{
    // cmd.exe rejects a bare double-quote and %VAR% expansion.
    CHECK(has_shell_metacharacters("a\"b", Shell::Cmd));
    CHECK(has_shell_metacharacters("a%PATH%b", Shell::Cmd));
    // Backslash is the Windows path separator (legitimate), and $/backtick are inert to cmd.exe.
    CHECK(!has_shell_metacharacters("C:\\Tools\\tint.exe", Shell::Cmd));
    CHECK(!has_shell_metacharacters("a$b", Shell::Cmd));
    CHECK(!has_shell_metacharacters("a`b", Shell::Cmd));
    // Clean tokens (including spaces) are accepted.
    CHECK(!has_shell_metacharacters("C:\\Program Files\\ctx\\tint.exe", Shell::Cmd));
}

void test_quote_argument_shapes()
{
    // Clean args quote to "arg" under either shell.
    CHECK(quote_argument("plain", Shell::Posix) == "\"plain\"");
    CHECK(quote_argument("with space", Shell::Cmd) == "\"with space\"");
    CHECK(quote_argument("C:\\Tools\\tint.exe", Shell::Cmd) == "\"C:\\Tools\\tint.exe\"");

    // Fail-closed: the metacharacter path throws, per shell.
    CHECK(quote_rejects("a$b", Shell::Posix));   // $ rejected on POSIX
    CHECK(quote_rejects("a\\b", Shell::Posix));  // \ rejected on POSIX
    CHECK(quote_rejects("a%b", Shell::Cmd));     // % rejected on cmd.exe
    CHECK(quote_rejects("a\"b", Shell::Cmd));    // " rejected on cmd.exe
    CHECK(quote_rejects("a\"b", Shell::Posix));  // " rejected on POSIX

    // The SAME arg's verdict differs by shell (proving the policy is per-shell, not global): a `\`
    // path is fine for cmd.exe but rejected for POSIX; a `%` is fine for POSIX but rejected for cmd.
    CHECK(!quote_rejects("C:\\x", Shell::Cmd));
    CHECK(quote_rejects("C:\\x", Shell::Posix));
    CHECK(!quote_rejects("100%done", Shell::Posix));
    CHECK(quote_rejects("100%done", Shell::Cmd));
}

void test_wrap_command()
{
    // cmd.exe: the whole line is wrapped in ONE extra outer pair (so `cmd /c` strips it, not the
    // inner quotes). POSIX: unchanged.
    CHECK(wrap_command("\"exe\" \"path\"", Shell::Cmd) == "\"\"exe\" \"path\"\"");
    CHECK(wrap_command("\"exe\" \"path\"", Shell::Posix) == "\"exe\" \"path\"");
    CHECK(wrap_command("", Shell::Cmd) == "\"\"");
    CHECK(wrap_command("", Shell::Posix).empty());
}

// Strip trailing directory separators, comparing in generic ('/') form — temp_directory_path()
// carries a trailing separator on Windows that operator/ + parent_path() drops, so a raw path==
// comparison is spuriously unequal.
std::string dir_key(const std::filesystem::path& p)
{
    std::string s = p.generic_string();
    while (!s.empty() && s.back() == '/')
    {
        s.pop_back();
    }
    return s;
}

void test_make_scratch_path()
{
    const std::filesystem::path a = make_scratch_path("ctx-test", ".tmp");
    const std::filesystem::path b = make_scratch_path("ctx-test", ".tmp");
    CHECK(a != b);                                                       // the counter advances
    CHECK(dir_key(a.parent_path()) == dir_key(std::filesystem::temp_directory_path())); // under temp
    const std::string fn = a.filename().string();
    CHECK(fn.rfind("ctx-test-", 0) == 0);                               // prefix honoured
    CHECK(fn.size() >= 4 && fn.substr(fn.size() - 4) == ".tmp");        // suffix honoured
}

void test_scratch_file_raii()
{
    std::filesystem::path p;
    {
        const ScratchFile sf(make_scratch_path("ctx-raii", ".dat"));
        p = sf.path();
        const std::string payload = "hello";
        CHECK(write_file(p, payload.data(), payload.size()));
        std::error_code ec;
        CHECK(std::filesystem::exists(p, ec)); // present while the ScratchFile is alive
    }
    std::error_code ec;
    CHECK(!std::filesystem::exists(p, ec)); // removed on scope exit
}

void test_read_write_file()
{
    const ScratchFile sf(make_scratch_path("ctx-io", ".bin"));
    // Built with an embedded NUL + a high byte so the round-trip proves binary safety (a plain string
    // literal would truncate at the NUL).
    std::string payload = "line1\nline2";
    payload.push_back('\0');
    payload += "binary";
    payload.push_back(static_cast<char>(0xff));
    CHECK(write_file(sf.path(), payload.data(), payload.size()));
    CHECK(read_file(sf.path()) == payload);

    // Reading an absent file yields "" (a missing tool stdout on a failed run is expected).
    const std::filesystem::path absent = make_scratch_path("ctx-absent", ".none");
    CHECK(read_file(absent).empty());

    // An empty write still creates the (empty) file.
    const ScratchFile empty(make_scratch_path("ctx-empty", ".bin"));
    CHECK(write_file(empty.path(), nullptr, 0));
    std::error_code ec;
    CHECK(std::filesystem::exists(empty.path(), ec));
    CHECK(read_file(empty.path()).empty());
}

} // namespace

int main()
{
    test_metacharacter_policy_posix();
    test_metacharacter_policy_cmd();
    test_quote_argument_shapes();
    test_wrap_command();
    test_make_scratch_path();
    test_scratch_file_raii();
    test_read_write_file();
    COMMON_TEST_MAIN_END();
}
