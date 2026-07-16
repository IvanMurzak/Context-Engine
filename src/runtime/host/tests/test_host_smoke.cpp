// runtime-host-smoke (M8 task a06, R-BUILD-009) — the cross-process SHIPPED-BINARY smoke: build a real
// v1 pack to a scratch file, then LAUNCH each shipped host binary (server + desktop) as a real child
// process with `--pack <file> --ticks N`, capture its stdout, and assert the boot/state signal (booted
// ok + stepped exactly N fixed ticks + a named scene reached). This is the honest "does the artifact
// actually run?" proof R-BUILD-009 names — exit 0 from BUILDING proves nothing about the packed binary;
// stepping the shipped RuntimeKernel is the only place to get the answer. The two binary paths are
// threaded in as compile-time defines ($<TARGET_FILE:...>); the launch uses the hardened context_common
// subprocess runner (its cmd.exe outer-quote workaround makes this robust on the Windows executor too).

#include "context/common/subprocess.h"

#include "context/editor/compose/content_unit.h"
#include "context/editor/compose/flatten.h"
#include "context/editor/compose/scene_model.h"
#include "context/editor/pack/pack_writer.h"
#include "context/editor/serializer/json_parse.h"

#include "host_test.h"

#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace compose = context::editor::compose;
namespace pack = context::editor::pack;
namespace serializer = context::editor::serializer;
namespace sp = context::common::subprocess;

namespace
{

class MapResolver final : public compose::SceneResolver
{
public:
    bool add(const std::string& path, const char* json)
    {
        serializer::ParseResult parsed = serializer::parse_json(json);
        if (!parsed.ok)
            return false;
        std::optional<compose::SceneDoc> doc = compose::build_scene_doc(path, parsed.root);
        if (!doc.has_value())
            return false;
        docs_[path] = std::move(*doc);
        return true;
    }
    [[nodiscard]] const compose::SceneDoc* resolve(std::string_view path) const override
    {
        auto it = docs_.find(std::string(path));
        return it == docs_.end() ? nullptr : &it->second;
    }

private:
    std::map<std::string, compose::SceneDoc, std::less<>> docs_;
};

const char* kScene = R"({
  "$schema": "ctx:scene", "version": 1,
  "entities": [
    {"id": "aaaa0000aaaa0001", "name": "Camera", "components": {"camera": {"fov": 1.0}}},
    {"id": "aaaa0000aaaa0002", "name": "Player", "components": {"transform": {}}}
  ]})";

[[nodiscard]] std::string make_pack()
{
    MapResolver r;
    if (!r.add("scenes/main.scene.json", kScene))
        return {};
    const compose::ComposedScene scene = compose::flatten("scenes/main.scene.json", r);
    if (!scene.ok)
        return {};
    const compose::ContentUnitSet units = compose::partition_content_units(scene, r);
    pack::PackWriteOptions options;
    options.engine_version = 9;
    const pack::PackWriteResult written = pack::write_pack(units, scene, {}, options);
    return written.ok ? written.bytes : std::string{};
}

[[nodiscard]] bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

// Launch a shipped host binary over `pack_path`, stepping `ticks`, and return its captured stdout.
// Redirects the child's stdout to a scratch file (the subprocess runner returns only the exit code).
[[nodiscard]] std::string run_host_binary(const char* binary, const std::string& pack_path,
                                          int ticks, int& exit_code)
{
    const std::filesystem::path out = sp::make_scratch_path("ctxhost-smoke", ".json");
    sp::ScratchFile out_guard(out);
    const std::string cmd = sp::quote_argument(binary) + " --pack " + sp::quote_argument(pack_path) +
                            " --ticks " + std::to_string(ticks) + " > " +
                            sp::quote_argument(out.string());
    exit_code = sp::run_command(cmd);
    return sp::read_file(out);
}

} // namespace

int main()
{
    const std::string pack_bytes = make_pack();
    CHECK(!pack_bytes.empty());

    const std::filesystem::path pack_path = sp::make_scratch_path("ctxhost-smoke", ".pack");
    sp::ScratchFile pack_guard(pack_path);
    CHECK(sp::write_file(pack_path, pack_bytes.data(), pack_bytes.size()));

    constexpr int kTicks = 8;

    // The two SHIPPED binaries must each boot the pack and step exactly N ticks, reporting their flavor.
    struct Flavor
    {
        const char* binary;
        const char* flavor_signal;
        const char* render_signal;
    };
    const Flavor flavors[] = {
        {CONTEXT_RUNTIME_SERVER_BIN, "\"flavor\":\"server\"", "\"renderPresent\":false"},
        {CONTEXT_RUNTIME_DESKTOP_BIN, "\"flavor\":\"desktop\"", "\"renderPresent\":true"},
    };

    for (const Flavor& f : flavors)
    {
        int exit_code = -1;
        const std::string stdout_text = run_host_binary(f.binary, pack_path.string(), kTicks, exit_code);
        CHECK(exit_code == 0);                                   // the artifact booted + stepped ok
        CHECK(contains(stdout_text, "\"ok\":true"));
        CHECK(contains(stdout_text, "\"simTick\":8"));           // stepped exactly N fixed ticks
        CHECK(contains(stdout_text, "\"rootScene\":\"scenes/main.scene.json\"")); // named scene reached
        CHECK(contains(stdout_text, f.flavor_signal));
        CHECK(contains(stdout_text, f.render_signal));
    }

    // A negative --ticks is rejected by the SHIPPED binary (std::stoull would otherwise wrap it to a
    // ~1.8e19 tick count and hang the boot); it exits non-zero, matching its "non-negative" contract.
    {
        int exit_code = 0;
        const std::string stdout_text =
            run_host_binary(CONTEXT_RUNTIME_SERVER_BIN, pack_path.string(), -5, exit_code);
        CHECK(exit_code != 0);            // fail-closed on a negative tick count, never a hang
        static_cast<void>(stdout_text);
    }

    HOST_TEST_MAIN_END();
}
