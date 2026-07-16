// determinism-packed-wedge (M8 task a07, R-BUILD-009 / R-QA-005 / L-54) — the L-54 state-hash gate
// re-run against the PACKED WEDGE BUILD, not the editor-embedded kernel.
//
// R-BUILD-009's core claim is that "editor-embedded play proves nothing about the packed binary": the
// only honest place to read the deterministic sim state is by stepping the SHIPPED RuntimeKernel inside
// the artifact. So this gate:
//   1. builds a real v1 pack (the wedge artifact), writes it to a scratch file;
//   2. runs the EDITOR-EMBEDDED RuntimeKernel in-process — a Session on the fixed (seed, scenario)
//      stepped N fixed ticks — and folds its post-step hierarchical state-hash root (H_editor);
//   3. LAUNCHES the shipped server/headless host binary as a real child process
//      (`context-runtime-server --pack <p> --seed S --ticks N`), parses its machine-readable
//      boot/state signal, and reads the SHIPPED kernel's post-step sim state hash (H_shipped);
//   4. asserts H_shipped == H_editor — the packed wedge build computes the SAME deterministic sim
//      state as the editor-embedded kernel (the R-BUILD-009 "shipped kernel is the honest source");
//   5. asserts H_editor == a committed cross-platform GOLDEN — the integer-only sim state + fixed-endian
//      hashing make the value PORTABLE, so any per-platform divergence on the Linux-x64 / Win-x64 /
//      macOS-ARM64 determinism matrix reds that leg (exactly the L-54 guarantee).
//
// The SERVER/headless flavor is the wedge server build (R-NET-001 server-authoritative sim); render is
// presentation strictly OFF the sim path (m7-exit-4 already pins that UI-present vs absent never moves
// the hash), so the desktop flavor's sim determinism is identical by construction and needs no separate
// launch here. Registered as `determinism-packed-wedge` — it joins the `determinism-*` family the
// blocking CI "Determinism gate" step (-R "^determinism-") runs on all three build-matrix legs, AND is
// added to the strict-FP `deterministic` job's hand-maintained --target list in ci.yml (with
// context_runtime_server) so the "Not Run = RED" tripwire holds. The shipped binary path is threaded in
// as a compile-time define ($<TARGET_FILE:context_runtime_server>); the launch uses the hardened
// context_common subprocess runner (its cmd.exe outer-quote workaround makes it robust on Windows too).
//
// Updating the golden: it changes only when the demo scenario / systems / hash change ON PURPOSE.
// Re-derive by running this gate — it prints the observed value — then paste it below.

#include "context/common/subprocess.h"

#include "context/editor/compose/content_unit.h"
#include "context/editor/compose/flatten.h"
#include "context/editor/compose/scene_model.h"
#include "context/editor/pack/pack_writer.h"
#include "context/editor/serializer/json_parse.h"

#include "context/runtime/session/session.h"
#include "context/runtime/session/state_hash.h"

#include "host_test.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace compose = context::editor::compose;
namespace pack = context::editor::pack;
namespace serializer = context::editor::serializer;
namespace session = context::runtime::session;
namespace sp = context::common::subprocess;

namespace
{

// The fixed wedge run: a distinctive DECIMAL seed (the shipped binary parses --seed base-10, so a
// "0x.." literal would truncate at 'x' — decimal only) stepped a fixed tick count against the built-in
// "demo" wedge scenario (ARCHITECTURE §1.1). The sim is integer/fixed-point end to end, so the hash is
// portable across the determinism matrix.
constexpr std::uint64_t kSeed = 2718281;
constexpr int kTicks = 32;

// The cross-platform golden: the demo-scenario hierarchical state-hash root after kTicks fixed ticks
// from kSeed with no injected input (the exact run the shipped host performs). Portable (integer-only).
constexpr std::uint64_t kGoldenPackedWedgeRoot = 0xFAB83D3AAE445DA8ULL;

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

// Build a real v1 pack for the wedge scene (the a01 writer path — a TEST-ONLY link of the editor side).
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

// The editor-embedded reference: step the SHIPPED session code in-process on the fixed wedge run and
// fold the post-step hierarchical state-hash root. Identical code to the one baked into the host binary
// (context_session), so a divergence between this and the child is a real packed-vs-editor mismatch.
[[nodiscard]] std::uint64_t editor_embedded_hash()
{
    session::SessionConfig config;
    config.seed = kSeed;
    config.scenario = "demo";
    session::Session sim(config);
    const session::StepResult stepped = sim.step(static_cast<std::uint64_t>(kTicks));
    return stepped.state_hash.root;
}

// Extract the u64 value of a string-valued numeric field (`"<key>":"<decimal>"`) from the host's
// one-line JSON boot/state signal. The 64-bit hashes are emitted as DECIMAL STRINGS (host.cpp) because
// a JSON double loses precision above 2^53, so a plain numeric parse would corrupt them.
[[nodiscard]] bool extract_u64_string_field(const std::string& json, const std::string& key,
                                            std::uint64_t& out)
{
    const std::string needle = "\"" + key + "\":\"";
    const std::size_t at = json.find(needle);
    if (at == std::string::npos)
        return false;
    std::size_t i = at + needle.size();
    std::uint64_t value = 0;
    bool any = false;
    for (; i < json.size() && json[i] >= '0' && json[i] <= '9'; ++i)
    {
        value = value * 10 + static_cast<std::uint64_t>(json[i] - '0');
        any = true;
    }
    if (!any || i >= json.size() || json[i] != '"')
        return false;
    out = value;
    return true;
}

// Launch the shipped host binary over `pack_path` with the fixed wedge run and return its stdout. The
// child's stdout is redirected to a scratch file (the subprocess runner returns only the exit code).
[[nodiscard]] std::string run_shipped_wedge(const char* binary, const std::string& pack_path,
                                            int& exit_code)
{
    const std::filesystem::path out = sp::make_scratch_path("ctx-packed-det", ".json");
    sp::ScratchFile out_guard(out);
    const std::string cmd = sp::quote_argument(binary) + " --pack " + sp::quote_argument(pack_path) +
                            " --seed " + std::to_string(kSeed) + " --ticks " + std::to_string(kTicks) +
                            " > " + sp::quote_argument(out.string());
    exit_code = sp::run_command(cmd);
    return sp::read_file(out);
}

} // namespace

int main()
{
    // --- build the wedge artifact + compute the editor-embedded reference hash ----------------------
    const std::string pack_bytes = make_pack();
    CHECK(!pack_bytes.empty());
    const std::filesystem::path pack_path = sp::make_scratch_path("ctx-packed-det", ".pack");
    sp::ScratchFile pack_guard(pack_path);
    CHECK(sp::write_file(pack_path, pack_bytes.data(), pack_bytes.size()));

    const std::uint64_t h_editor = editor_embedded_hash();

    // --- run the SHIPPED server/headless wedge build and read its post-step sim state hash ----------
    int exit_code = -1;
    const std::string signal = run_shipped_wedge(CONTEXT_RUNTIME_SERVER_BIN, pack_path.string(), exit_code);
    std::uint64_t h_shipped = 0;
    const bool parsed = extract_u64_string_field(signal, "simStateHash", h_shipped);
    std::uint64_t shipped_tick = 0;
    const bool tick_parsed = [&] {
        const std::string needle = "\"simTick\":";
        const std::size_t at = signal.find(needle);
        if (at == std::string::npos)
            return false;
        std::size_t i = at + needle.size();
        bool any = false;
        for (; i < signal.size() && signal[i] >= '0' && signal[i] <= '9'; ++i)
        {
            shipped_tick = shipped_tick * 10 + static_cast<std::uint64_t>(signal[i] - '0');
            any = true;
        }
        return any;
    }();

    std::printf("[determinism-packed-wedge] seed=%llu ticks=%d editorHash=0x%016llX "
                "shippedHash=0x%016llX exit=%d\n",
                static_cast<unsigned long long>(kSeed), kTicks,
                static_cast<unsigned long long>(h_editor),
                static_cast<unsigned long long>(h_shipped), exit_code);

    // --- the shipped wedge build actually booted + stepped exactly N fixed ticks --------------------
    CHECK(exit_code == 0);
    CHECK(signal.find("\"ok\":true") != std::string::npos);
    CHECK(signal.find("\"flavor\":\"server\"") != std::string::npos);
    CHECK(parsed);
    CHECK(tick_parsed);
    CHECK(shipped_tick == static_cast<std::uint64_t>(kTicks));

    // --- R-BUILD-009: the PACKED wedge build's sim hash == the editor-embedded kernel's -------------
    CHECK(h_shipped == h_editor);

    // --- L-54 cross-platform golden (portable — integer-only sim): identical on every matrix leg ----
    CHECK(h_editor == kGoldenPackedWedgeRoot);

    HOST_TEST_MAIN_END();
}
