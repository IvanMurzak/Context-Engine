// The shipped RuntimeKernel host binary entrypoint (M8 task a06). The FIRST standalone runtime main()
// in the engine — it boots a packed v1 artifact headlessly and prints the R-BUILD-009 boot/state
// signal to stdout as one JSON line. Two flavors build from this same source: the desktop flavor links
// context_render (CONTEXT_HOST_RENDER on), the server/headless flavor omits it (see host.h / CMakeLists).
//
// Usage: context-runtime --pack <file> [--ticks N] [--seed S] [--scenario NAME]
//   --pack <file>   REQUIRED: the v1 pack to boot (produced by `context build`).
//   --ticks N       fixed ticks to step the shipped RuntimeKernel (default 0 — a boot-only smoke).
//   --seed S        the deterministic session seed (default 0).
//   --scenario NAME the session scenario tenant (default "demo").
// Exit code: 0 = booted + stepped; 1 = a usage error or a malformed/undecodable pack (fail-closed).

#include "context/runtime/host/host.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace
{

// Parse "--flag value" pairs; a bare "--flag" with no following token is treated as an empty value.
// Deliberately tiny — the host binary takes only the four documented flags, no verb grammar.
[[nodiscard]] std::string flag_value(int argc, char** argv, std::string_view name)
{
    for (int i = 1; i + 1 < argc; ++i)
        if (name == argv[i])
            return argv[i + 1];
    return {};
}

[[nodiscard]] bool read_file(const std::string& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    const std::string pack_path = flag_value(argc, argv, "--pack");
    if (pack_path.empty())
    {
        std::fprintf(stderr,
                     "usage: context-runtime --pack <file> [--ticks N] [--seed S] [--scenario NAME]\n");
        return 1;
    }

    context::runtime::host::HostConfig config;
    if (!read_file(pack_path, config.pack_bytes))
    {
        std::fprintf(stderr, "context-runtime: cannot read pack file: %s\n", pack_path.c_str());
        return 1;
    }

    const std::string ticks = flag_value(argc, argv, "--ticks");
    const std::string seed = flag_value(argc, argv, "--seed");
    const std::string scenario = flag_value(argc, argv, "--scenario");
    // Parse a non-negative u64 flag. std::stoull SILENTLY WRAPS a leading '-' (it negates modulo 2^64
    // rather than throwing), so rejecting only on the exception would let `--ticks -5` become a ~1.8e19
    // tick count and hang the boot — reject a leading '-' explicitly so the "non-negative" contract the
    // error below states actually holds for the SHIPPED binary (and the launch.sh that forwards "$@").
    const auto parse_u64 = [](const std::string& text, std::uint64_t& out) -> bool {
        if (text.empty())
            return true;
        if (text[0] == '-')
            return false;
        try
        {
            out = static_cast<std::uint64_t>(std::stoull(text));
        }
        catch (...)
        {
            return false;
        }
        return true;
    };
    if (!parse_u64(ticks, config.ticks) || !parse_u64(seed, config.seed))
    {
        std::fprintf(stderr, "context-runtime: --ticks / --seed must be non-negative integers\n");
        return 1;
    }
    if (!scenario.empty())
        config.scenario = scenario;

    const context::runtime::host::HostResult result = context::runtime::host::run_host(config);
    const std::string signal = context::runtime::host::host_signal_json(result);
    std::fwrite(signal.data(), 1, signal.size(), stdout);
    std::fputc('\n', stdout);
    return result.ok ? 0 : 1;
}
