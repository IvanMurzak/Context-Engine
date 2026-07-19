// Cross-process R-CLI-017 e2e (R-QA-013): the large-result handle round-trip over REAL processes.
//   * a REAL `context daemon` is spawned with a lowered --large-result-threshold, so a routine
//     `describe` over the wire comes back as a `largeResult` HANDLE envelope (never an oversized
//     frame) — the oversized-response path;
//   * a SEPARATE `context fetch <handle>` CLI process retrieves the payload over the same channel
//     (resource.read chunks) and its printed envelope carries the EXACT original describe result;
//   * a range fetch (`context fetch <handle> <offset>:<length>`) returns the requested byte window;
//   * a bogus handle fails with resource.unknown_handle (exit-code table respected).
// The daemon child is reaped (wire shutdown, else killed) so the test never hangs / leaks.

#include "context/editor/client/client.h"
#include "context/editor/bridge/resource_store.h" // hex codec
#include "context/editor/client/instance.h" // discover_instance (the daemon-publish race)
#include "context/editor/contract/json.h"

#include "cli_test.h"
#include "process_util.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;
using context::editor::client::AttachOptions;
using context::editor::client::Client;
using context::editor::contract::Json;

#ifndef CONTEXT_BINARY
#error "CONTEXT_BINARY (path to the built context executable) must be defined by the build."
#endif

namespace
{
fs::path make_temp_project(const char* tag)
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir =
        fs::temp_directory_path() / ("ctx-fetch-e2e-" + std::string(tag) + "-" + std::to_string(stamp));
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

// Parse defensively: a failed child leaves an empty/garbage --out file, and an uncaught parse throw
// would skip the daemon reap below (leaking the child past the test). nullopt -> CHECK + skip.
std::optional<Json> safe_parse(const std::string& text)
{
    try
    {
        return Json::parse(text);
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}
} // namespace

int main()
{
    const std::string bin = CONTEXT_BINARY;
    const fs::path project = make_temp_project("drive");
    const fs::path fetch_out = project / "fetch-result.json";
    const fs::path range_out = project / "range-result.json";
    const fs::path bogus_out = project / "bogus-result.json";

    // --- spawn the daemon with a LOWERED spool threshold (the R-CLI-017 operational knob) --------
    ctest_proc::Process daemon = ctest_proc::spawn(
        bin, {"daemon", "--project", project.string(), "--large-result-threshold", "600"});
    CHECK(ctest_proc::valid(daemon));
    // The SDK owns the publish race (retry until the document is readable) — riding it here keeps
    // this test on the same discovery path the product uses instead of a private re-implementation.
    CHECK(context::editor::client::discover_instance(project, 15000).has_value());

    // --- wire client: describe MUST come back as a largeResult handle, not inline ----------------
    std::string handle_uri;
    std::uint64_t total_bytes = 0;
    {
        AttachOptions attach_options; // scope defaults to "read"
        std::string err;
        const std::unique_ptr<Client> client_ptr = Client::connect_to_project(project, 5000, err);
        CHECK(client_ptr != nullptr);
        // No early return: the daemon child below must still be reaped even if the connect failed.
        std::optional<Json> described;
        if (client_ptr)
        {
            CHECK(client_ptr->attach(attach_options, err));
            described = client_ptr->call("describe", Json::object(), err);
        }
        CHECK(described.has_value());
        if (described.has_value())
        {
            CHECK(described->at("ok").as_bool());
            CHECK(!described->at("data").contains("contract")); // NOT inline — spooled
            const Json& lr = described->at("data").at("largeResult");
            handle_uri = lr.at("handle").as_string();
            total_bytes = static_cast<std::uint64_t>(lr.at("sizeBytes").as_int());
            CHECK(!handle_uri.empty());
            CHECK(total_bytes > 600);
        }
    }

    // --- a SEPARATE `context fetch` process retrieves + reassembles the original result ----------
    {
        ctest_proc::Process fetch = ctest_proc::spawn(bin, {"fetch", handle_uri, "--project",
                                                            project.string(), "--out",
                                                            fetch_out.string()});
        CHECK(ctest_proc::valid(fetch));
        int code = -1;
        CHECK(ctest_proc::wait_for(fetch, 20000, code));
        CHECK(code == 0);

        const std::optional<Json> env = safe_parse(read_file(fetch_out));
        CHECK(env.has_value());
        if (env.has_value())
        {
            CHECK(env->at("ok").as_bool());
            // data = the ORIGINAL (spooled) describe result envelope, reassembled then re-parsed.
            const Json& original = env->at("data");
            CHECK(original.at("ok").as_bool());
            CHECK(original.at("data").at("contract").contains("protocol"));
            CHECK(original.at("data").at("contract").at("largeResult").at("rpcMethod").as_string() ==
                  "resource.read");
        }
    }

    // --- a range fetch returns exactly the requested byte window ---------------------------------
    {
        ctest_proc::Process fetch =
            ctest_proc::spawn(bin, {"fetch", handle_uri, "0:64", "--project", project.string(),
                                    "--out", range_out.string()});
        CHECK(ctest_proc::valid(fetch));
        int code = -1;
        CHECK(ctest_proc::wait_for(fetch, 20000, code));
        CHECK(code == 0);

        const std::optional<Json> env = safe_parse(read_file(range_out));
        CHECK(env.has_value());
        if (env.has_value())
        {
            CHECK(env->at("ok").as_bool());
            const Json& data = env->at("data");
            CHECK(static_cast<std::uint64_t>(data.at("totalBytes").as_int()) == total_bytes);
            CHECK(data.at("offsetBytes").as_int() == 0);
            CHECK(data.at("lengthBytes").as_int() == 64);
            const auto bytes =
                context::editor::bridge::hex_decode(data.at("chunkHex").as_string());
            CHECK(bytes.has_value());
            // The spooled payload is the describe result envelope — its first bytes are its head.
            CHECK(bytes.has_value() && bytes->rfind("{\"ok\":true", 0) == 0);
        }
    }

    // --- failure path: a bogus (well-formed but unknown) handle → resource.unknown_handle. The
    // --- doctored ?bytes= is deliberately HUGE: the client must clamp its up-front reservation
    // --- (untrusted input) and fail with a clean envelope, never abort — a crash here would leave
    // --- an empty/garbage --out file and a non-envelope exit.
    {
        ctest_proc::Process fetch = ctest_proc::spawn(
            bin, {"fetch", "context-res://v0/no-such-instance/0?bytes=18446744073709551615",
                  "--project", project.string(), "--out", bogus_out.string()});
        CHECK(ctest_proc::valid(fetch));
        int code = -1;
        CHECK(ctest_proc::wait_for(fetch, 20000, code));
        CHECK(code != 0); // the exit-code table classes the failure (not-found class)

        const std::optional<Json> env = safe_parse(read_file(bogus_out));
        CHECK(env.has_value());
        if (env.has_value())
        {
            CHECK(!env->at("ok").as_bool());
            CHECK(env->at("error").at("code").as_string() == "resource.unknown_handle");
        }
    }

    // --- reap: ask the daemon to stop over the wire; kill only if it ignores us ------------------
    {
        AttachOptions attach_options;
        attach_options.scope = "session";
        std::string err;
        if (const std::unique_ptr<Client> client = Client::connect_to_project(project, 2000, err))
        {
            if (client->attach(attach_options, err))
                (void)client->call("shutdown", Json::object(), err);
        }
        int code = -1;
        if (!ctest_proc::wait_for(daemon, 10000, code))
        {
            ctest_proc::kill(daemon);
            (void)ctest_proc::wait_for(daemon, 5000, code);
        }
    }

    std::error_code ec;
    fs::remove_all(project, ec);
    CLI_TEST_MAIN_END();
}
