// Cross-process e2e (M1 issue #34, R-QA-013): spawns REAL processes and proves the whole wire.
//   Test 1 — a `context daemon` process + a SEPARATE `context attach` process: the attach process
//            connects over the IPC wire via the capability handshake, edits a file (which lands on
//            REAL disk under the daemon) and queries the derived World (read-your-writes), then asks
//            the daemon to shut down cleanly. Asserts on-disk effects + the attach result envelope.
//   Test 2 — a 2nd `context daemon` on the SAME project detects the single-instance lock and exits
//            with the attach signal (R-BRIDGE-001), while the first stays live.
//
// CONTEXT_BINARY is the built `context` executable path (a compile-time define). The daemon children
// are reaped (shutdown, else killed) so the test never hangs / leaks a process.

#include "context/editor/contract/json.h"

#include "cli_test.h"
#include "process_util.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>

namespace fs = std::filesystem;
using context::editor::contract::Json;

#ifndef CONTEXT_BINARY
#error "CONTEXT_BINARY (path to the built context executable) must be defined by the build."
#endif

namespace
{
fs::path make_temp_project(const char* tag)
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir = fs::temp_directory_path() / ("ctx-e2e-" + std::string(tag) + "-" + std::to_string(stamp));
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

std::string read_file(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return std::string();
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Poll until the daemon publishes its discovery hint (proving it booted + bound the transport).
bool wait_for_instance(const fs::path& project, int timeout_ms)
{
    const fs::path instance = project / ".editor" / "instance.json";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;)
    {
        std::error_code ec;
        if (fs::exists(instance, ec) && !read_file(instance).empty())
            return true;
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}
} // namespace

int main()
{
    const std::string bin = CONTEXT_BINARY;

    // ============================================================================================
    // Test 1 — cross-process attach: daemon process + separate CLI process over the wire.
    // ============================================================================================
    {
        const fs::path project = make_temp_project("drive");

        ctest_proc::Process daemon =
            ctest_proc::spawn(bin, {"daemon", "--project", project.string()});
        CHECK(ctest_proc::valid(daemon));
        CHECK(wait_for_instance(project, 15000));

        // Seed an OUT-OF-BAND authored file the daemon has never been told about — the attach
        // client's `--reconcile` must fold it in over the wire (R-FILE-002; the path the R-FILE-011
        // N-daemons benchmark scenario drives against generated corpora).
        {
            std::error_code seed_ec;
            fs::create_directories(project / "proj", seed_ec);
            std::ofstream seeded(project / "proj" / "seeded.scene", std::ios::binary);
            seeded << "seeded: 1";
        }

        // A SEPARATE process attaches over the wire, reconciles the seeded file, edits + queries,
        // writes its envelope to a file, and asks the daemon to shut down. Reading the file AFTER
        // the child exits is race-free.
        const fs::path result = project / "attach-result.json";
        ctest_proc::Process attach = ctest_proc::spawn(
            bin, {"attach", "--project", project.string(), "--out", result.string(), "--reconcile",
                  "--shutdown"});
        CHECK(ctest_proc::valid(attach));

        int attach_code = -1;
        const bool attach_done = ctest_proc::wait_for(attach, 25000, attach_code);
        if (!attach_done)
            ctest_proc::kill(attach);
        ctest_proc::release(attach);
        CHECK(attach_done);
        CHECK(attach_code == 0); // the full cross-process drive succeeded (edit reflected + query hit)

        // The edit landed on REAL disk, written by the daemon process on the attach process's behalf.
        CHECK(fs::exists(project / "proj" / "a.scene"));
        CHECK(read_file(project / "proj" / "a.scene") == "entity: 1");

        // The attach process's own result envelope confirms the cross-process semantics.
        const std::string rj = read_file(result);
        CHECK(!rj.empty());
        if (!rj.empty())
        {
            const Json env = Json::parse(rj);
            CHECK(env.at("ok").as_bool());
            const Json& data = env.at("data");
            CHECK(data.at("attached").as_bool());
            CHECK(data.at("hashesMatch").as_bool());
            // --reconcile folded the seeded out-of-band file in over the wire BEFORE the edit.
            CHECK(data.at("reconcile").at("changes").as_int() == 1);
            CHECK(data.at("reconcile").at("worldEntities").as_int() == 1);
            CHECK(data.at("edit").at("reflected").as_bool());
            CHECK(data.at("edit").at("worldEntities").as_int() == 2); // seeded + the edit's a.scene
            CHECK(data.at("query").at("present").as_bool());
            CHECK(data.at("shutdownAck").as_bool());
        }

        // The daemon received the shutdown and exits cleanly (0), no kill needed.
        int daemon_code = -1;
        const bool daemon_done = ctest_proc::wait_for(daemon, 15000, daemon_code);
        if (!daemon_done)
            ctest_proc::kill(daemon);
        ctest_proc::release(daemon);
        CHECK(daemon_done);
        if (daemon_done)
            CHECK(daemon_code == 0);

        std::error_code ec;
        fs::remove_all(project, ec);
    }

    // ============================================================================================
    // Test 2 — 2nd daemon launch detects the single-instance lock -> attach signal (R-BRIDGE-001).
    // ============================================================================================
    {
        const fs::path project = make_temp_project("lock");

        ctest_proc::Process d1 = ctest_proc::spawn(bin, {"daemon", "--project", project.string()});
        CHECK(ctest_proc::valid(d1));
        CHECK(wait_for_instance(project, 15000));

        // A second daemon PROCESS on the same project must NOT boot — the lock try-acquire fails and
        // that failure IS the attach signal (a distinct, machine-legible exit code).
        const fs::path signal_out = project / "d2.json";
        ctest_proc::Process d2 = ctest_proc::spawn(
            bin, {"daemon", "--project", project.string(), "--out", signal_out.string()});
        CHECK(ctest_proc::valid(d2));

        int d2_code = -1;
        const bool d2_done = ctest_proc::wait_for(d2, 15000, d2_code);
        if (!d2_done)
            ctest_proc::kill(d2);
        ctest_proc::release(d2);
        CHECK(d2_done);
        CHECK(d2_code == 3); // kDaemonAttachSignalExit

        const std::string sj = read_file(signal_out);
        CHECK(!sj.empty());
        if (!sj.empty())
        {
            const Json env = Json::parse(sj);
            CHECK(env.at("data").at("attachSignal").as_bool());
        }

        // Tear down the first daemon via a separate attach --shutdown; kill as a backstop.
        ctest_proc::Process stopper =
            ctest_proc::spawn(bin, {"attach", "--project", project.string(), "--shutdown"});
        int stop_code = -1;
        if (!ctest_proc::wait_for(stopper, 15000, stop_code))
            ctest_proc::kill(stopper);
        ctest_proc::release(stopper);

        int d1_code = -1;
        if (!ctest_proc::wait_for(d1, 15000, d1_code))
            ctest_proc::kill(d1);
        ctest_proc::release(d1);

        std::error_code ec;
        fs::remove_all(project, ec);
    }

    CLI_TEST_MAIN_END();
}
