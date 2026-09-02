// T1 for the LIVE files feed (M9 e1/e2, D10): the kernel-side builder+wire -> Shell-side parser
// ROUND-TRIP (the two halves that must not drift), the envelope tolerance, the settle->refetch
// cadence, the node-id -> identity mapping, the selection dispatch through the provider, and
// (e2) the WRITE FAN-OUT: a landed operation becomes an undo checkpoint AND arms a refetch, a
// refused one becomes a loud notice and arms NOTHING. Mirrors test_scenetree_feed.cpp — see that
// file's header for the fuller rationale.

#include "context/editor/shell/panels/files_feed.h"

#include "context/editor/assetdb/asset_database.h"
#include "context/editor/filesync/file_store.h"
#include "context/editor/gui/contract/extension.h"
#include "context/editor/gui/panels/builders/files_builder.h"
#include "context/editor/gui/panels/builders/wire.h"
#include "context/editor/shell/panel_host.h"

#include "panels_test.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace shell = context::editor::shell;
namespace panels = context::editor::shell::panels;
namespace files = context::editor::gui::panels::files;
namespace builders = context::editor::gui::panels::builders;
namespace assetdb = context::editor::assetdb;
namespace filesync = context::editor::filesync;
namespace gc = context::editor::gui::contract;
namespace undo = context::editor::gui::session::undo;
using Json = context::editor::contract::Json;

namespace
{

constexpr const char* kPanelId = "builtin.files";

// M9 e1: the daemon side of the selection seam, mirroring RecordingSelectionGateway in
// test_scenetree_feed.cpp. RECORDS the write and answers with the selection the daemon would then
// hold; it never touches the panel.
class RecordingSelectionGateway final : public files::SelectionGateway
{
public:
    std::vector<std::vector<std::string>> requests;

    std::optional<std::vector<std::string>>
    request_selection(const std::vector<std::string>& ids) override
    {
        requests.push_back(ids);
        return ids;
    }
};

void put_asset(filesync::FileStore& fs, std::string_view path, std::string_view guid,
               std::string_view kind)
{
    fs.write(path, "content");
    assetdb::AssetMeta meta;
    meta.guid = std::string(guid);
    meta.kind = std::string(kind);
    fs.write(assetdb::meta_path_for(path), assetdb::serialize_meta(meta));
}

[[nodiscard]] std::vector<gc::Contribution> roster_with_files()
{
    gc::Contribution c;
    c.id = kPanelId;
    c.kind = gc::ContributionKind::panel;
    c.title = "Files";
    c.content.type = gc::ContentType::uitree;
    c.state.schema_version = 1;
    return {c};
}

[[nodiscard]] bool models_equal(const files::FileNode& a, const files::FileNode& b)
{
    if (a.identity != b.identity || a.display_name != b.display_name || a.kind != b.kind ||
        a.guid != b.guid || a.asset_kind != b.asset_kind || a.children.size() != b.children.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < a.children.size(); ++i)
    {
        if (!models_equal(a.children[i], b.children[i]))
        {
            return false;
        }
    }
    return true;
}

// --- cases ----------------------------------------------------------------------------------------

void wire_round_trips_the_built_model()
{
    filesync::MemoryFileStore fs;
    put_asset(fs, "proj/textures/wall.tex.json", "00000000000000000000000000000aaa", "ctx:texture");
    fs.write("proj/README.md", "hello");

    assetdb::SequenceGuidGenerator guids;
    assetdb::AssetDatabase db(guids);
    CHECK(db.scan(fs, "proj").assets_indexed == 1);

    const files::FilesModel built = builders::build_files_model(fs.list("proj"), db);
    CHECK(built.ok);
    CHECK(built.file_count == 2);

    const Json wire = builders::files_to_wire(built);
    const std::optional<files::FilesModel> parsed = panels::parse_files(wire);
    CHECK(parsed.has_value());
    CHECK(parsed->ok == built.ok);
    CHECK(parsed->file_count == built.file_count);
    CHECK(parsed->roots.size() == built.roots.size());
    for (std::size_t i = 0; i < built.roots.size(); ++i)
    {
        CHECK(models_equal(parsed->roots[i], built.roots[i]));
    }

    // The specific shape this fixture pins: a directory wrapping a meta'd asset, alongside a plain
    // top-level file with no meta — both must survive the wire.
    const files::FileNode* texdir = files::find_node(*parsed, "proj/textures");
    CHECK(texdir != nullptr && texdir->kind == files::FileNodeKind::directory);
    const files::FileNode* tex = files::find_node(*parsed, "proj/textures/wall.tex.json");
    CHECK(tex != nullptr);
    CHECK(tex->guid == "00000000000000000000000000000aaa");
    CHECK(tex->asset_kind == "ctx:texture");
    const files::FileNode* readme = files::find_node(*parsed, "proj/README.md");
    CHECK(readme != nullptr);
    CHECK(readme->guid.empty()); // no meta sidecar yet — unknown, not a defect
}

void unrecognized_payloads_say_nothing()
{
    CHECK(!panels::parse_files(Json(std::string("nope"))).has_value());
    CHECK(!panels::parse_files(Json::object()).has_value()); // no roots container
    // Unparseable NODES inside a recognized container are skipped, never fatal.
    Json wire = Json::object();
    Json roots = Json::array();
    roots.push_back(Json(std::string("junk")));
    Json good = Json::object();
    good.set("identity", Json(std::string("README.md")));
    roots.push_back(std::move(good));
    wire.set("roots", std::move(roots));
    const std::optional<files::FilesModel> parsed = panels::parse_files(wire);
    CHECK(parsed.has_value());
    CHECK(parsed->roots.size() == 1u);
    CHECK(parsed->roots[0].identity == "README.md");
    CHECK(parsed->roots[0].kind == files::FileNodeKind::file); // the safe default (not "directory")
}

void apply_result_tolerates_the_envelope_and_touches_the_host()
{
    shell::PanelHost host(roster_with_files());
    panels::FilesFeed feed(host, kPanelId);
    CHECK(host.provide(kPanelId, feed.make_provider()));
    const std::uint64_t before = host.revision(kPanelId);

    // The full envelope shape the pump hands over: {ok, data: {files: {...}}}.
    Json tree = Json::object();
    Json roots = Json::array();
    Json node = Json::object();
    node.set("identity", Json(std::string("README.md")));
    node.set("displayName", Json(std::string("README.md")));
    node.set("kind", Json(std::string("file")));
    roots.push_back(std::move(node));
    tree.set("roots", std::move(roots));
    tree.set("fileCount", Json(std::uint64_t{1}));
    Json data = Json::object();
    data.set("files", std::move(tree));
    Json envelope = Json::object();
    envelope.set("ok", Json(true));
    envelope.set("data", std::move(data));

    CHECK(feed.apply_result(envelope));
    CHECK(feed.results_applied() == 1u);
    CHECK(host.revision(kPanelId) > before); // the renderer will see a fresh render
    CHECK(feed.panel().model().roots.size() == 1u);

    // A reply that says nothing leaves the adopted model alone.
    CHECK(!feed.apply_result(Json::object()));
    CHECK(feed.panel().model().roots.size() == 1u);
}

void settle_marks_a_refetch_due()
{
    shell::PanelHost host(roster_with_files());
    panels::FilesFeed feed(host, kPanelId);
    CHECK(host.provide(kPanelId, feed.make_provider()));

    CHECK(feed.fetch_due()); // born due: the first pump performs the initial hydration
    feed.mark_fetched();
    CHECK(!feed.fetch_due());

    Json payload = Json::object();
    payload.set("event", Json(std::string("derivation.settled")));
    CHECK(feed.apply_event("derivation", payload, 7));
    CHECK(feed.fetch_due()); // the settle scheduled the re-read (unlike the tree, no status to advance)

    // Unknown topics / other derivation events are ignored.
    CHECK(!feed.apply_event("files", payload, 8));
    Json other = Json::object();
    other.set("event", Json(std::string("derivation.started")));
    CHECK(!feed.apply_event("derivation", other, 8));
}

void selection_dispatches_through_the_provider()
{
    CHECK(panels::files_row_identity("files.item.a/b") == std::optional<std::string>("a/b"));
    CHECK(!panels::files_row_identity("files.item.").has_value());
    CHECK(!panels::files_row_identity("scenetree.item.a").has_value());

    RecordingSelectionGateway gateway;
    shell::PanelHost host(roster_with_files());
    panels::FilesFeed feed(host, kPanelId, &gateway);
    CHECK(host.provide(kPanelId, feed.make_provider()));

    files::FilesModel model;
    model.file_count = 1;
    files::FileNode node;
    node.identity = "README.md";
    node.display_name = "README.md";
    node.kind = files::FileNodeKind::file;
    model.roots.push_back(std::move(node));
    feed.panel().set_model(std::move(model));

    bool dispatched = false;
    std::string error;
    Json params = Json::object();
    params.set("nodeId", Json(std::string("files.item.README.md")));
    CHECK(host.invoke(kPanelId, files::kSelectCommand, params, dispatched, error));
    CHECK(dispatched);
    CHECK(gateway.requests.size() == 1);
    CHECK(gateway.requests[0] == std::vector<std::string>{"README.md"});
    CHECK(feed.panel().selection().identity == "README.md");

    // An unknown row / a foreign node id is DECLINED, not an error — and never reaches the daemon.
    Json bad = Json::object();
    bad.set("nodeId", Json(std::string("files.item.ghost")));
    CHECK(host.invoke(kPanelId, files::kSelectCommand, bad, dispatched, error));
    CHECK(!dispatched);
    CHECK(gateway.requests.size() == 1);
}

// --- M9 e2: the write fan-out --------------------------------------------------------------------

// The file write double, at the same fidelity as the daemon (records, answers, never reaches in).
class RecordingFileWrites final : public files::FileWriteGateway
{
public:
    std::vector<std::string> calls;
    bool refuse = false;
    std::string token = "00000000000000000000000000000aaa";

    files::FileWriteResult move_file(const std::string& from, const std::string& to) override
    {
        calls.push_back("move");
        return answer(from, to, "");
    }
    files::FileWriteResult delete_file(const std::string& path) override
    {
        calls.push_back("delete");
        return answer(path, "", token);
    }
    files::FileWriteResult restore_file(const std::string& restore_token) override
    {
        calls.push_back("restore");
        return answer("art/hero.png", "", restore_token);
    }

private:
    [[nodiscard]] files::FileWriteResult answer(const std::string& path, const std::string& other,
                                                const std::string& tok) const
    {
        files::FileWriteResult out;
        if (refuse)
        {
            out.status = files::FileWriteResult::Status::refused;
            out.code = "asset.delete_referenced";
            out.message = "`scenes/main.json` references this asset at `/texture`";
            out.path = path;
            return out;
        }
        out.status = files::FileWriteResult::Status::applied;
        out.path = path;
        out.other_path = other;
        out.restore_token = tok;
        return out;
    }
};

// A two-row reply in the envelope shape the pump hands over — the fixture the write cases act on.
[[nodiscard]] Json two_row_reply()
{
    Json tree = Json::object();
    Json roots = Json::array();
    Json readme = Json::object();
    readme.set("identity", Json(std::string("README.md")));
    readme.set("displayName", Json(std::string("README.md")));
    readme.set("kind", Json(std::string("file")));
    roots.push_back(std::move(readme));
    Json wall = Json::object();
    wall.set("identity", Json(std::string("textures/wall.tex.json")));
    wall.set("displayName", Json(std::string("wall.tex.json")));
    wall.set("kind", Json(std::string("file")));
    roots.push_back(std::move(wall));
    tree.set("roots", std::move(roots));
    tree.set("fileCount", Json(std::uint64_t{2}));
    Json data = Json::object();
    data.set("files", std::move(tree));
    Json envelope = Json::object();
    envelope.set("ok", Json(true));
    envelope.set("data", std::move(data));
    return envelope;
}

void a_landed_write_journals_a_step_and_arms_a_refetch()
{
    shell::PanelHost host(roster_with_files());
    panels::FilesFeed feed(host, kPanelId);
    CHECK(feed.apply_result(two_row_reply()));
    feed.mark_fetched();
    CHECK(!feed.fetch_due());

    RecordingFileWrites writes;
    feed.bind_write_gateway(&writes);
    CHECK(feed.has_write_gateway());

    std::vector<undo::FileEdit> journaled;
    std::vector<std::string> labels;
    feed.bind_checkpoint_sink([&](undo::FileEdit edit, std::string label)
                              {
                                  journaled.push_back(std::move(edit));
                                  labels.push_back(std::move(label));
                              });
    std::vector<std::string> notices;
    feed.bind_notice_sink([&](const char* verb, const files::FileWriteResult&)
                          { notices.emplace_back(verb); });

    CHECK(feed.panel().remove("README.md"));
    CHECK(feed.writes_landed() == 1);
    CHECK(feed.writes_refused() == 0);
    // The undo step, minted from what the write path ANSWERED.
    CHECK(journaled.size() == 1);
    CHECK(journaled[0].op == undo::FileEdit::Op::remove);
    CHECK(journaled[0].from == "README.md");
    CHECK(journaled[0].restore_token == writes.token);
    // NAMED for Session History — a file step has no field name to identify it by.
    CHECK(labels[0] == "delete README.md");
    // No notice: nothing was refused.
    CHECK(notices.empty());
    // READ-YOUR-WRITES: the write went out over the gateway, so the tree this panel is rendering is
    // stale by exactly the row the human acted on.
    CHECK(feed.fetch_due());
}

void a_refused_write_is_loud_and_arms_nothing()
{
    shell::PanelHost host(roster_with_files());
    panels::FilesFeed feed(host, kPanelId);
    CHECK(feed.apply_result(two_row_reply()));
    feed.mark_fetched();

    RecordingFileWrites writes;
    writes.refuse = true;
    feed.bind_write_gateway(&writes);

    std::size_t journaled = 0;
    feed.bind_checkpoint_sink([&](undo::FileEdit, std::string) { ++journaled; });
    std::vector<std::string> notice_verbs;
    std::vector<files::FileWriteResult> notice_results;
    feed.bind_notice_sink([&](const char* verb, const files::FileWriteResult& result)
                          {
                              notice_verbs.emplace_back(verb);
                              notice_results.push_back(result);
                          });

    CHECK(!feed.panel().remove("README.md"));
    CHECK(feed.writes_refused() == 1);
    CHECK(feed.writes_landed() == 0);
    // LOUD: the refusal reaches the notice sink, carrying the daemon's own code and message. This is
    // the assertion that makes "a silent failure on a destructive operation" impossible by test.
    CHECK(notice_verbs.size() == 1);
    CHECK(notice_verbs[0] == "delete");
    CHECK(notice_results[0].code == "asset.delete_referenced");
    CHECK(!notice_results[0].message.empty());
    // NOTHING was journaled (there is no step to undo) and NO refetch was armed (nothing changed).
    CHECK(journaled == 0);
    CHECK(!feed.fetch_due());

    // The producible sibling: the SAME feed, the SAME call, lands once the write path says yes.
    writes.refuse = false;
    CHECK(feed.panel().remove("README.md"));
    CHECK(feed.writes_landed() == 1);
    CHECK(feed.fetch_due());
}

void an_irreversible_delete_is_not_journaled()
{
    // An applied delete with NO restore token is not undoable. Recording it would offer the human an
    // undo guaranteed to refuse; not recording it costs one history entry. The second is the honest
    // failure — asserted here so the choice cannot silently flip.
    shell::PanelHost host(roster_with_files());
    panels::FilesFeed feed(host, kPanelId);
    CHECK(feed.apply_result(two_row_reply()));
    feed.mark_fetched();

    RecordingFileWrites writes;
    writes.token.clear();
    feed.bind_write_gateway(&writes);
    std::size_t journaled = 0;
    feed.bind_checkpoint_sink([&](undo::FileEdit, std::string) { ++journaled; });

    CHECK(feed.panel().remove("README.md"));
    CHECK(feed.writes_landed() == 1);
    CHECK(journaled == 0);
    CHECK(feed.fetch_due()); // the row IS gone, so the panel must still re-read

    // Sibling: a delete that DOES come back with a token is journaled, so the check above is about
    // the token and not about deletes in general.
    writes.token = "00000000000000000000000000000aaa";
    CHECK(feed.panel().remove("textures/wall.tex.json"));
    CHECK(journaled == 1);
}

void a_restore_is_never_journaled_as_a_new_step()
{
    // The only caller that issues a restore is the journal's own undo replay; recording its inverse
    // as a NEW step would make Ctrl+Z followed by Ctrl+Z undo itself forever.
    shell::PanelHost host;
    panels::FilesFeed feed(host, kPanelId);
    RecordingFileWrites writes;
    feed.bind_write_gateway(&writes);
    std::size_t journaled = 0;
    feed.bind_checkpoint_sink([&](undo::FileEdit, std::string) { ++journaled; });

    feed.mark_fetched();
    CHECK(feed.panel().restore("00000000000000000000000000000aaa"));
    CHECK(feed.writes_landed() == 1);
    CHECK(journaled == 0);
    // ...but it DID happen, so the panel must still re-read: a restore brings back a row that is by
    // definition absent from the tree currently rendered. "Not journaled" and "not observed" are
    // different things, and conflating them would leave the restored file invisible until the next
    // unrelated settle.
    CHECK(feed.fetch_due());
}

void the_authoring_commands_dispatch_through_the_provider()
{
    shell::PanelHost host(roster_with_files());
    panels::FilesFeed feed(host, kPanelId);
    CHECK(host.provide(kPanelId, feed.make_provider()));
    CHECK(feed.apply_result(two_row_reply()));

    RecordingFileWrites writes;
    feed.bind_write_gateway(&writes);
    CHECK(feed.panel().apply_selection({"README.md"}));

    bool dispatched = false;
    std::string error;
    Json rename = Json::object();
    rename.set("name", Json(std::string("READ.md")));
    CHECK(host.invoke(kPanelId, files::kRenameCommand, rename, dispatched, error));
    CHECK(dispatched);
    CHECK(writes.calls == std::vector<std::string>{"move"});

    Json none = Json::object();
    CHECK(host.invoke(kPanelId, files::kDeleteCommand, none, dispatched, error));
    CHECK(dispatched);
    CHECK(writes.calls.size() == 2);
    CHECK(writes.calls[1] == "delete");

    // A rename with NO name is refused locally and never reaches the write path — the provider does
    // not invent one.
    Json empty = Json::object();
    CHECK(host.invoke(kPanelId, files::kRenameCommand, empty, dispatched, error));
    CHECK(!dispatched);
    CHECK(writes.calls.size() == 2);
}

} // namespace

int main()
{
    wire_round_trips_the_built_model();
    unrecognized_payloads_say_nothing();
    apply_result_tolerates_the_envelope_and_touches_the_host();
    settle_marks_a_refetch_due();
    selection_dispatches_through_the_provider();
    a_landed_write_journals_a_step_and_arms_a_refetch();
    a_refused_write_is_loud_and_arms_nothing();
    an_irreversible_delete_is_not_journaled();
    a_restore_is_never_journaled_as_a_new_step();
    the_authoring_commands_dispatch_through_the_provider();
    PANELS_TEST_MAIN_END();
}
