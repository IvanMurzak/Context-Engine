// The a19 GUI<->CLI in-context override-editing PARITY gate (R-HUX-006 MUST core / R-CLI-006 / L-35 /
// L-20; ctest `viewport-override-parity`): the SAME override edits driven through (a) the viewport
// override-editing GUI model's L-20 gesture lifecycle over the REAL disk-backed override gateway and
// (b) the REAL `context set` CLI verb (in-process cli::run over the shipped verb grammar), on two
// identical staged projects, must produce BYTE-IDENTICAL authored files — "the GUI is sugar over the
// same composed write path, never a parallel one" (proven, not assumed). Covers all three R-CLI-006
// write targets: the OUTERMOST override (a new one + updating an existing one, on the committed
// platformer-2d sample), the --edit-template retarget, and the --at-instance mid-level retarget (on a
// staged three-level composition). Then the DoD's provenance half: the model's provenance display is
// byte-identical to the flatten's canonical provenance JSON — the SAME emitter `context query` serves.

#include "context/cli/app.h"
#include "context/editor/compose/flatten.h"
#include "context/editor/compose/project_resolver.h"
#include "context/editor/gui/viewport/project_override_gateway.h"
#include "context/editor/gui/viewport/viewport_edit_model.h"
#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_tree.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using context::cli::run;
using context::editor::contract::Envelope;
namespace compose = context::editor::compose;
namespace vp = context::editor::gui::viewport;
namespace serializer = context::editor::serializer;
using serializer::JsonValue;

namespace
{

int g_failures = 0;

void fail(const char* file, int line, const char* expr)
{
    std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", expr, file, line);
    ++g_failures;
}

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
            fail(__FILE__, __LINE__, #cond);                                                       \
    } while (false)

[[nodiscard]] std::string read_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::string();
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void write_file(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

[[nodiscard]] std::string canonical(const JsonValue& v)
{
    std::string s;
    if (!serializer::serialize_canonical(v, s))
        s.clear();
    return s;
}

[[nodiscard]] long long stamp()
{
    return static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count() &
                                  0xffffffffLL);
}

[[nodiscard]] fs::path stage_platformer(const char* tag)
{
    const fs::path dir =
        fs::temp_directory_path() / ("ctx-vpedit-" + std::string(tag) + "-" + std::to_string(stamp()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::copy(fs::path(CONTEXT_SAMPLES_DIR) / "platformer-2d", dir,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    CHECK(!ec);
    return dir;
}

void remove_quiet(const fs::path& p)
{
    std::error_code ec;
    fs::remove_all(p, ec);
}

// Stage the three-level composition fixture (root -> mid -> child{entity}) into a fresh dir.
[[nodiscard]] fs::path stage_three_level(const char* tag)
{
    const fs::path dir =
        fs::temp_directory_path() / ("ctx-vp3-" + std::string(tag) + "-" + std::to_string(stamp()));
    remove_quiet(dir);
    write_file(dir / "root.scene.json", R"({
  "$schema": "ctx:scene",
  "version": 1,
  "entities": [],
  "instances": [{"id": "aaaa0000aaaa0001", "scene": "mid.scene.json"}]
})");
    write_file(dir / "mid.scene.json", R"({
  "$schema": "ctx:scene",
  "version": 1,
  "entities": [],
  "instances": [{"id": "bbbb0000bbbb0001", "scene": "child.scene.json"}]
})");
    write_file(dir / "child.scene.json", R"({
  "$schema": "ctx:scene",
  "version": 1,
  "entities": [
    {"id": "cccc0000cccc0001", "name": "Widget",
     "components": {"transform": {"position": [1, 2, 3]}}}
  ]
})");
    return dir;
}

// Drive ONE viewport override edit through the GUI model over the real disk gateway on `stage`.
// Returns the value literal the gesture produced (fed verbatim to the CLI so the two paths splice the
// exact same value — parity then proves the SHARED plan_write path, not a lucky value match).
[[nodiscard]] std::string gui_edit(const fs::path& stage, const std::string& root_scene,
                                   const std::string& identity, vp::EditTarget target,
                                   const std::vector<std::string>& at_instance,
                                   const JsonValue& new_value)
{
    compose::ProjectSceneResolver resolver(stage.string());
    vp::ProjectOverrideWriteGateway gateway(stage.string());
    vp::ViewportEditModel model;
    CHECK(model.open(resolver, root_scene));
    CHECK(model.select(identity));
    model.set_edit_target(target);
    model.set_at_instance(at_instance);
    CHECK(model.begin_gesture(gateway));
    model.set_pending_value(new_value);
    const std::string literal = canonical(model.pending_value());
    const context::editor::gui::panels::inspector::CommitResult r = model.commit_gesture(gateway);
    CHECK(r.ok()); // applied or rebased (single writer -> applied)
    return literal;
}

// The composed entity with `id_path` in a fresh flatten of `root_scene` under `stage`.
[[nodiscard]] const compose::ComposedEntity* find_entity(const compose::ComposedScene& scene,
                                                         const std::vector<std::string>& id_path)
{
    for (const compose::ComposedEntity& e : scene.entities)
        if (e.id_path == id_path)
            return &e;
    return nullptr;
}

} // namespace

int main()
{
    constexpr const char* kLevel = "scenes/level-1.scene.json";
    constexpr const char* kLevelRel = "scenes/level-1.scene.json";
    constexpr const char* kRoomInst = "c3c3c3c3c3c3c301";
    constexpr const char* kFloor = "a1a1a1a1a1a1a101"; // room Floor — NO existing override
    constexpr const char* kTorch = "a1a1a1a1a1a1a102"; // room Torch — HAS an outermost override
    const std::string position = vp::kPositionPointer;

    // --- Part A1: OUTERMOST, a NEW override (move the instanced Floor) ----------------------------
    {
        const fs::path gui = stage_platformer("a1-gui");
        const fs::path cli = stage_platformer("a1-cli");
        const std::string identity = std::string(kRoomInst) + "/" + kFloor;
        const std::string idpath = std::string(kRoomInst) + "/" + kFloor;

        // GUI: a move gizmo gesture translating the composed [0,0,0] to [2,0,0].
        JsonValue moved = serializer::canonicalize("[2, 0, 0]").root;
        const std::string literal = gui_edit(gui, kLevel, identity, vp::EditTarget::outermost, {}, moved);

        // CLI: `context set` with the EXACT value literal the gesture produced.
        const Envelope set = run({"set", kLevel, literal, "--pointer", position, "--id-path", idpath,
                                  "--project", cli.string()});
        if (!set.ok())
            std::fprintf(stderr, "context set failed: %s\n", set.dump(0).c_str());
        CHECK(set.ok());

        const std::string gui_bytes = read_file(gui / kLevelRel);
        const std::string cli_bytes = read_file(cli / kLevelRel);
        CHECK(!gui_bytes.empty());
        if (gui_bytes != cli_bytes)
            std::fprintf(stderr, "A1 parity break (gui %zu, cli %zu)\n", gui_bytes.size(),
                         cli_bytes.size());
        CHECK(gui_bytes == cli_bytes);
        remove_quiet(gui);
        remove_quiet(cli);
    }

    // --- Part A2: OUTERMOST, UPDATING the existing override (move the instanced Torch) ------------
    {
        const fs::path gui = stage_platformer("a2-gui");
        const fs::path cli = stage_platformer("a2-cli");
        const std::string identity = std::string(kRoomInst) + "/" + kTorch;
        const std::string idpath = std::string(kRoomInst) + "/" + kTorch;

        // The composed Torch position is the existing override [5,0,2]; translate to [6,0,2].
        JsonValue moved = serializer::canonicalize("[6, 0, 2]").root;
        const std::string literal = gui_edit(gui, kLevel, identity, vp::EditTarget::outermost, {}, moved);
        const Envelope set = run({"set", kLevel, literal, "--pointer", position, "--id-path", idpath,
                                  "--project", cli.string()});
        CHECK(set.ok());
        CHECK(read_file(gui / kLevelRel) == read_file(cli / kLevelRel));
        CHECK(!read_file(gui / kLevelRel).empty());

        // Provenance parity (R-CLI-006, DoD-2): after the override write, the model's provenance
        // display for the moved field is BYTE-IDENTICAL to a fresh flatten's canonical provenance JSON
        // (the SAME emitter `context query` serializes provenance through), and shows the winning
        // override at instancing level 0.
        compose::ProjectSceneResolver post(gui.string());
        vp::ViewportEditModel model;
        CHECK(model.open(post, kLevel));
        CHECK(model.select(identity));
        const compose::ComposedScene fresh = compose::flatten(kLevel, post);
        const compose::ComposedEntity* torch = find_entity(fresh, {kRoomInst, kTorch});
        CHECK(torch != nullptr);
        const std::string expected = compose::provenance_json(compose::provenance_for(*torch, position));
        CHECK(model.provenance_json(position) == expected);
        CHECK(model.overridden_at(position));
        const std::vector<compose::ProvenanceEntry> chain = model.provenance(position);
        CHECK(!chain.empty());
        CHECK(chain.front().source == compose::ProvenanceEntry::Source::override_value);
        CHECK(chain.front().level == 0);

        remove_quiet(gui);
        remove_quiet(cli);
    }

    // --- Part B: the RETARGET affordances on a three-level composition ----------------------------
    // edit-template writes the DEFINING template (child.scene.json); at-instance writes a MID-LEVEL
    // instancing scene (mid.scene.json). Both byte-identical to the corresponding `context set` flags.
    {
        const std::string identity = "aaaa0000aaaa0001/bbbb0000bbbb0001/cccc0000cccc0001";
        const std::string idpath = "aaaa0000aaaa0001/bbbb0000bbbb0001/cccc0000cccc0001";

        // --edit-template: the write lands in child.scene.json (the entity's authored value).
        {
            const fs::path gui = stage_three_level("tmpl-gui");
            const fs::path cli = stage_three_level("tmpl-cli");
            JsonValue moved = serializer::canonicalize("[7, 8, 9]").root;
            const std::string literal =
                gui_edit(gui, "root.scene.json", identity, vp::EditTarget::edit_template, {}, moved);
            const Envelope set = run({"set", "root.scene.json", literal, "--pointer", position,
                                      "--id-path", idpath, "--edit-template", "--project", cli.string()});
            if (!set.ok())
                std::fprintf(stderr, "edit-template set failed: %s\n", set.dump(0).c_str());
            CHECK(set.ok());
            CHECK(read_file(gui / "child.scene.json") == read_file(cli / "child.scene.json"));
            CHECK(!read_file(gui / "child.scene.json").empty());
            remove_quiet(gui);
            remove_quiet(cli);
        }

        // --at-instance <instA>: the write lands as an override in mid.scene.json (the mid-level scene).
        {
            const fs::path gui = stage_three_level("at-gui");
            const fs::path cli = stage_three_level("at-cli");
            JsonValue moved = serializer::canonicalize("[4, 5, 6]").root;
            const std::string literal = gui_edit(gui, "root.scene.json", identity,
                                                 vp::EditTarget::at_instance, {"aaaa0000aaaa0001"}, moved);
            const Envelope set =
                run({"set", "root.scene.json", literal, "--pointer", position, "--id-path", idpath,
                     "--at-instance", "aaaa0000aaaa0001", "--project", cli.string()});
            if (!set.ok())
                std::fprintf(stderr, "at-instance set failed: %s\n", set.dump(0).c_str());
            CHECK(set.ok());
            CHECK(read_file(gui / "mid.scene.json") == read_file(cli / "mid.scene.json"));
            CHECK(!read_file(gui / "mid.scene.json").empty());
            remove_quiet(gui);
            remove_quiet(cli);
        }
    }

    return g_failures == 0 ? 0 : 1;
}
