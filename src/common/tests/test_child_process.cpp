// context_common ChildProcess: the long-running-child spawn primitive with captured stdout (M9 e14a).
//
// Fully portable + deterministic on all three OS legs by SELF-RE-EXEC: main() dispatches a set of
// child modes (print a marker line to stdout, emit several lines in one write, exit with a chosen
// code, or stay silent), and the test body spawns THIS SAME executable in each mode and drives the
// primitive against it. No external binary, no shell, no timing race beyond generous waits.

#include "context/common/child_process.h"

#include "common_test.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace proc = context::common::process;
namespace fs = std::filesystem;

namespace
{

void emit(const char* text)
{
    std::fputs(text, stdout);
    std::fflush(stdout);
}

// The child modes. Each is deterministic; the sleeps give the parent a window to observe running().
int run_child_mode(const std::string& mode)
{
    if (mode == "ready-then-live")
    {
        emit("CHILD-READY token=abc123\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(3000)); // stay alive for running()/terminate()
        return 0;
    }
    if (mode == "line-then-exit")
    {
        emit("ONE-LINE done\n");
        return 7; // a distinctive exit code the parent asserts
    }
    if (mode == "multi-line")
    {
        emit("L1\nL2\nL3\n"); // three lines in ONE write — the parent drains them one at a time
        return 0;
    }
    if (mode == "silent")
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(400)); // no output — read_line must time out
        return 0;
    }
    return 99; // unknown mode
}

proc::SpawnOptions mode_options(const fs::path& self, const char* mode)
{
    proc::SpawnOptions options;
    options.executable = self;
    options.args = {"--child", mode};
    return options;
}

// --- the primitive under test ---------------------------------------------------------------------

void test_spawn_captures_a_ready_line_and_tracks_liveness(const fs::path& self)
{
    std::string error;
    proc::ChildProcess child = proc::ChildProcess::spawn(mode_options(self, "ready-then-live"), error);
    CHECK(child.valid());
    CHECK(error.empty());
    CHECK(child.pid() != 0);

    // The ready line arrives over stdio — never argv/env (the DoD token channel, proven here).
    std::string line;
    CHECK(child.read_line(line, 5000));
    CHECK(line == "CHILD-READY token=abc123");
    // Still running while it sleeps.
    CHECK(child.running());

    child.terminate(); // force-kill + reap
    CHECK(!child.running());
    CHECK(!child.valid());
    child.terminate(); // idempotent — a second terminate is a no-op
}

void test_wait_reports_the_exit_code_then_read_line_sees_eof(const fs::path& self)
{
    std::string error;
    proc::ChildProcess child = proc::ChildProcess::spawn(mode_options(self, "line-then-exit"), error);
    CHECK(child.valid());

    std::string line;
    CHECK(child.read_line(line, 5000));
    CHECK(line == "ONE-LINE done");

    int code = -1;
    CHECK(child.wait(5000, code));
    CHECK(code == 7);
    // After the child exited and closed stdout, a further read reports end-of-stream, not a hang.
    std::string more;
    CHECK(!child.read_line(more, 200));
}

void test_multiple_lines_from_one_write_drain_in_order(const fs::path& self)
{
    std::string error;
    proc::ChildProcess child = proc::ChildProcess::spawn(mode_options(self, "multi-line"), error);
    CHECK(child.valid());

    std::string a, b, c;
    CHECK(child.read_line(a, 5000));
    CHECK(child.read_line(b, 5000));
    CHECK(child.read_line(c, 5000));
    CHECK(a == "L1");
    CHECK(b == "L2");
    CHECK(c == "L3");
    int code = -1;
    CHECK(child.wait(5000, code));
    CHECK(code == 0);
}

void test_read_line_times_out_when_no_line_arrives(const fs::path& self)
{
    std::string error;
    proc::ChildProcess child = proc::ChildProcess::spawn(mode_options(self, "silent"), error);
    CHECK(child.valid());

    std::string line;
    const auto start = std::chrono::steady_clock::now();
    CHECK(!child.read_line(line, 100)); // no output for 400ms → this 100ms read must time out, not hang
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
            .count();
    CHECK(elapsed < 3000); // it returned promptly rather than blocking on the whole child lifetime

    int code = -1;
    CHECK(child.wait(5000, code));
    CHECK(code == 0);
}

void test_detach_relinquishes_ownership_safely(const fs::path& self)
{
    std::string error;
    proc::ChildProcess child = proc::ChildProcess::spawn(mode_options(self, "ready-then-live"), error);
    CHECK(child.valid());
    std::string line;
    CHECK(child.read_line(line, 5000));

    child.detach(); // leave the child running (the exit-policy "other clients attached" path)
    CHECK(!child.valid());
    // A terminate() after detach is a safe no-op (we no longer own it).
    child.terminate();
    CHECK(!child.valid());
    // The detached child (still sleeping) reparents + exits on its own; nothing here waits on it.
}

void test_spawn_of_a_missing_executable_fails_cleanly(const fs::path& self)
{
    // Cross-platform failure shape: Windows CreateProcessW refuses (invalid + error); POSIX fork
    // succeeds but the child's execv fails and it _exit(127)s — so accept EITHER a failed spawn OR a
    // valid handle whose child exits non-zero. Both are "the tool did not run".
    fs::path missing = self.parent_path() / "no-such-context-child-binary-xyz";
    proc::SpawnOptions options;
    options.executable = missing;
    options.args = {"--child", "ready-then-live"};

    std::string error;
    proc::ChildProcess child = proc::ChildProcess::spawn(options, error);
    if (!child.valid())
    {
        CHECK(!error.empty());
        return;
    }
    int code = -1;
    CHECK(child.wait(5000, code));
    CHECK(code != 0);
}

} // namespace

int main(int argc, char** argv)
{
    // Self-re-exec child dispatch, FIRST — a child launch must return immediately and never run the
    // test body (which would fork-bomb).
    if (argc >= 3 && std::string(argv[1]) == "--child")
        return run_child_mode(argv[2]);

    fs::path self;
    try
    {
        self = fs::absolute(fs::path(argv[0]));
    }
    catch (const std::exception&)
    {
        self = fs::path(argv[0]);
    }

    test_spawn_captures_a_ready_line_and_tracks_liveness(self);
    test_wait_reports_the_exit_code_then_read_line_sees_eof(self);
    test_multiple_lines_from_one_write_drain_in_order(self);
    test_read_line_times_out_when_no_line_arrives(self);
    test_detach_relinquishes_ownership_safely(self);
    test_spawn_of_a_missing_executable_fails_cleanly(self);
    COMMON_TEST_MAIN_END();
}
