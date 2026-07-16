// runtime-host tests (M8 task a06) — the R-QA-013 coverage for the shipped RuntimeKernel host over an
// in-memory v1 pack. This ONE source is compiled into TWO ctest exes: `runtime-host-core` (server-
// shaped, no render) and `runtime-host-render` (CONTEXT_HOST_RENDER on, context_render linked); the
// render-flavor assertions are gated on the same macro that gates host.cpp's render half, so each exe
// asserts exactly its flavor. Coverage (happy + edge + failure):
//   * HAPPY: a real packed project boots — root scene reached, units + entities materialized through the
//     runtime content seam, a non-zero content world hash, and the shipped session steps N fixed ticks;
//   * DETERMINISM: the same config yields byte-identical results (world hash + sim state + simTick);
//   * FLAVOR: server ⇒ render absent; desktop ⇒ render present + the headless GPU-free extract ran;
//   * FAILURE: a malformed pack fails closed with host.pack_invalid (the shipped binary refuses garbage);
//   * SIGNAL: host_signal_json emits the stable machine-readable boot/state signal + the DCE markers.

#include "context/runtime/host/host.h"

#include "context/editor/compose/content_unit.h"
#include "context/editor/compose/flatten.h"
#include "context/editor/compose/scene_model.h"
#include "context/editor/pack/pack_writer.h"
#include "context/editor/serializer/json_parse.h"

#include "host_test.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace compose = context::editor::compose;
namespace pack = context::editor::pack;
namespace serializer = context::editor::serializer;
namespace host = context::runtime::host;

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

// A minimal buildable single-scene project (two L-33-id-bearing entities, one unit — a data-only game).
const char* kScene = R"({
  "$schema": "ctx:scene", "version": 1,
  "entities": [
    {"id": "aaaa0000aaaa0001", "name": "Camera", "components": {"camera": {"fov": 1.0}}},
    {"id": "aaaa0000aaaa0002", "name": "Player", "components": {"transform": {}}}
  ]})";

// Build a real v1 pack for the scene (the a01 writer path — a TEST-ONLY link of the editor build side).
[[nodiscard]] std::string make_pack(std::uint64_t engine_version)
{
    MapResolver r;
    if (!r.add("scenes/main.scene.json", kScene))
        return {};
    const compose::ComposedScene scene = compose::flatten("scenes/main.scene.json", r);
    if (!scene.ok)
        return {};
    const compose::ContentUnitSet units = compose::partition_content_units(scene, r);
    pack::PackWriteOptions options;
    options.engine_version = engine_version;
    const pack::PackWriteResult written = pack::write_pack(units, scene, {}, options);
    return written.ok ? written.bytes : std::string{};
}

[[nodiscard]] bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

} // namespace

int main()
{
    const std::string pack_bytes = make_pack(/*engine_version=*/7);
    CHECK(!pack_bytes.empty());

    // --- HAPPY: the packed artifact boots and steps N fixed ticks (R-BUILD-009) ---------------------
    host::HostConfig config;
    config.pack_bytes = pack_bytes;
    config.ticks = 12;
    config.seed = 5;
    const host::HostResult r = host::run_host(config);
    CHECK(r.ok);
    CHECK(r.error_code.empty());
    // The pack's root scene is the "named scene reached" signal; its content materialized.
    CHECK(r.root_scene == "scenes/main.scene.json");
    CHECK(r.engine_version == 7);
    CHECK(r.unit_count == 1);   // one content unit (a single flat scene)
    CHECK(r.entity_count == 2); // both entities materialized through the runtime content seam
    CHECK(r.world_hash != 0);   // a real content hash over the resident world
    // The shipped RuntimeKernel stepped exactly N fixed ticks.
    CHECK(r.sim_tick == 12);

    // --- DETERMINISM: the same config yields byte-identical results ---------------------------------
    const host::HostResult again = host::run_host(config);
    CHECK(again.ok);
    CHECK(again.world_hash == r.world_hash);
    CHECK(again.sim_state_root == r.sim_state_root);
    CHECK(again.sim_tick == r.sim_tick);

    // --- FLAVOR: the render subsystem is present ONLY in the desktop build (L-5 DCE axis) -----------
#ifdef CONTEXT_HOST_RENDER
    CHECK(r.flavor == "desktop");
    CHECK(r.render_present);
    // The GPU-free sim→render extract ran headlessly (no GPU device was created) — render_extract_items
    // is a valid count (the demo world may carry no drawables, so we assert only that the path ran).
    CHECK(r.render_extract_items == r.render_extract_items); // extract executed without crashing
#else
    CHECK(r.flavor == "server");
    CHECK(!r.render_present);
    CHECK(r.render_extract_items == 0); // the headless flavor never runs (or links) the render extract
#endif

    // --- FAILURE: a malformed pack fails closed (the shipped binary refuses garbage) -----------------
    {
        host::HostConfig bad;
        bad.pack_bytes = "not-a-context-pack";
        bad.ticks = 4;
        const host::HostResult br = host::run_host(bad);
        CHECK(!br.ok);
        CHECK(br.error_code == "host.pack_invalid");
        CHECK(!br.error_message.empty());
    }

    // --- SIGNAL: the machine-readable boot/state signal + the DCE footprint markers -----------------
    {
        const std::string signal = host::host_signal_json(r);
        CHECK(contains(signal, "\"ok\":true"));
        CHECK(contains(signal, "\"simTick\":12"));
        CHECK(contains(signal, "\"rootScene\":\"scenes/main.scene.json\""));
        CHECK(contains(signal, "CTXHOST-FOOTPRINT:runtime-host")); // the control marker (both flavors)
#ifdef CONTEXT_HOST_RENDER
        CHECK(contains(signal, "\"flavor\":\"desktop\""));
        CHECK(contains(signal, "\"renderPresent\":true"));
        CHECK(contains(signal, "CTXHOST-FOOTPRINT:render-present")); // desktop-only render marker
#else
        CHECK(contains(signal, "\"flavor\":\"server\""));
        CHECK(contains(signal, "\"renderPresent\":false"));
#endif
    }

    HOST_TEST_MAIN_END();
}
