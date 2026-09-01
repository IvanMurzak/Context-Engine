// Files panel WRITE-half tests (M9 e2, D10): rename / move / delete as write REQUESTS through the
// FileWriteGateway seam, the LOCAL refusals the panel makes before asking, the loud refusal surface,
// the write-listener fan-out the Shell journals and narrates through, and the a11y rule that an
// authoring command is exposed only when it is actually reachable.
//
// ⚠ THE GATEWAY DOUBLE IS DELIBERATELY NO MORE CAPABLE THAN THE REAL WRITE PATH (the standing rule
// in test_files_panel.cpp): it answers what the daemon would answer and never reaches into the
// panel. A double that mutated the panel's model would let this suite pass over a panel that decides
// things itself — which is the exact property the write half must NOT have.
//
// EVERY REFUSAL ASSERTION IS PAIRED WITH THE APPLY IT PERFORMS on the same fixture. "It refused" is
// worthless on a fixture that could never have succeeded.

#include "context/editor/gui/panels/files/files_panel.h"

#include "context/editor/gui/panels/files/files_model.h"
#include "context/editor/gui/uitree/panel.h"

#include "files_test.h"

#include <string>
#include <vector>

using namespace context::editor::gui::panels::files;
namespace uitree = context::editor::gui::uitree;

namespace
{

[[nodiscard]] FilesModel standard_model()
{
    FilesModel model;
    model.ok = true;
    model.file_count = 2;

    FileNode dir;
    dir.identity = "art";
    dir.display_name = "art";
    dir.kind = FileNodeKind::directory;

    FileNode hero;
    hero.identity = "art/hero.png";
    hero.display_name = "hero.png";
    hero.kind = FileNodeKind::file;
    hero.asset_kind = "ctx:texture";
    dir.children.push_back(hero);

    FileNode readme;
    readme.identity = "README.md";
    readme.display_name = "README.md";
    readme.kind = FileNodeKind::file;

    model.roots.push_back(dir);
    model.roots.push_back(readme);
    return model;
}

// The recording write double. It answers exactly what the daemon would: an applied result carrying
// what the write path decided (the destination, the restore token), or a refusal carrying the
// daemon's own code + message.
class RecordingWrites final : public FileWriteGateway
{
public:
    struct Call
    {
        std::string verb;
        std::string a;
        std::string b;
    };

    std::vector<Call> calls;
    bool refuse = false;
    std::string refusal_code = "asset.delete_referenced";
    std::string refusal_message = "`scenes/main.json` references this asset at `/texture`";
    std::string token = "00000000000000000000000000000aaa";

    FileWriteResult move_file(const std::string& from, const std::string& to) override
    {
        calls.push_back({"move", from, to});
        if (refuse)
        {
            return refusal(from);
        }
        FileWriteResult out;
        out.status = FileWriteResult::Status::applied;
        out.path = from;
        out.other_path = to;
        return out;
    }

    FileWriteResult delete_file(const std::string& path) override
    {
        calls.push_back({"delete", path, ""});
        if (refuse)
        {
            return refusal(path);
        }
        FileWriteResult out;
        out.status = FileWriteResult::Status::applied;
        out.path = path;
        out.restore_token = token;
        return out;
    }

    FileWriteResult restore_file(const std::string& restore_token) override
    {
        calls.push_back({"restore", restore_token, ""});
        if (refuse)
        {
            return refusal("");
        }
        FileWriteResult out;
        out.status = FileWriteResult::Status::applied;
        out.path = "art/hero.png";
        out.restore_token = restore_token;
        return out;
    }

private:
    [[nodiscard]] FileWriteResult refusal(const std::string& path) const
    {
        FileWriteResult out;
        out.status = FileWriteResult::Status::refused;
        out.code = refusal_code;
        out.message = refusal_message;
        out.path = path;
        return out;
    }
};

// One recorded listener notification.
struct Heard
{
    FileWriteVerb verb = FileWriteVerb::move;
    FileWriteResult result;
};

[[nodiscard]] bool panel_exposes(const uitree::Panel& panel, const char* command)
{
    return panel.has_command(command);
}

// The rendered node with `node_id`, found by walking the tree (uitree exposes no lookup — every
// sibling suite reads the HTML, which is fine for a substring check but cannot tell "absent" from
// "present and empty", and the write-status node's ABSENCE before the first write is an assertion
// this suite makes).
[[nodiscard]] const uitree::UiNode* find_node(const uitree::UiNode& node, const char* node_id)
{
    if (node.id() == node_id)
    {
        return &node;
    }
    for (const uitree::UiNode& child : node.children())
    {
        if (const uitree::UiNode* hit = find_node(child, node_id))
        {
            return hit;
        }
    }
    return nullptr;
}

[[nodiscard]] std::string status_text(const uitree::Panel& panel, const char* node_id)
{
    const uitree::UiNode* node = find_node(panel.root(), node_id);
    return node == nullptr ? std::string() : node->text();
}

} // namespace

int main()
{
    // ========================= rename / move: the request the panel sends ========================
    {
        RecordingWrites writes;
        FilesPanel panel;
        panel.set_write_gateway(&writes);
        panel.set_model(standard_model());

        CHECK(panel.can_write());
        // A rename is a BASENAME change resolved against the row's OWN directory — the panel does
        // that resolution, so a caller never has to reconstruct the parent path (and so a rename can
        // never accidentally relocate a file).
        CHECK(panel.rename("art/hero.png", "villain.png"));
        CHECK(writes.calls.size() == 1);
        CHECK(writes.calls[0].verb == "move");
        CHECK(writes.calls[0].a == "art/hero.png");
        CHECK(writes.calls[0].b == "art/villain.png");
        CHECK(panel.last_write().ok());

        // A top-level row has no directory prefix to preserve.
        CHECK(panel.rename("README.md", "READ.md"));
        CHECK(writes.calls[1].b == "READ.md");

        // An explicit move takes the destination verbatim.
        CHECK(panel.move("art/hero.png", "sprites/hero.png"));
        CHECK(writes.calls[2].b == "sprites/hero.png");
    }

    // ============ the LOCAL refusals: refused BEFORE the write path is even asked ================
    {
        RecordingWrites writes;
        FilesPanel panel;
        panel.set_write_gateway(&writes);
        panel.set_model(standard_model());

        // A name carrying a separator is a MOVE, and silently reinterpreting it would relocate the
        // human's file into a directory they never named.
        CHECK(!panel.rename("art/hero.png", "sub/villain.png"));
        CHECK(panel.last_write().code == std::string(FilesPanel::kInvalidRequestCode));
        CHECK(!panel.rename("art/hero.png", ""));
        CHECK(!panel.rename("art/hero.png", ".."));
        // Renaming to the name it already has.
        CHECK(!panel.rename("art/hero.png", "hero.png"));
        // A row this panel cannot even name, and a DIRECTORY row (there is nothing to move).
        CHECK(!panel.move("art/ghost.png", "art/x.png"));
        CHECK(!panel.remove("art"));
        CHECK(!panel.move("art/hero.png", ""));
        // NOT ONE of them reached the write path — which is what "refused before asking" means.
        CHECK(writes.calls.empty());

        // The producible sibling: the same panel, the same gateway, a well-formed request applies.
        CHECK(panel.rename("art/hero.png", "villain.png"));
        CHECK(writes.calls.size() == 1);
    }

    // ================== FAIL-CLOSED: no write path bound => refuse, never no-op ==================
    {
        FilesPanel panel; // no gateway
        panel.set_model(standard_model());
        CHECK(!panel.can_write());

        CHECK(!panel.remove("art/hero.png"));
        CHECK(panel.last_write().status == FileWriteResult::Status::refused);
        CHECK(panel.last_write().code == std::string(FilesPanel::kNoWritePathCode));
        // The message is what a human reads on a destructive action that did not happen.
        CHECK(!panel.last_write().message.empty());
        CHECK(!panel.rename("art/hero.png", "x.png"));
        CHECK(!panel.restore("tok"));

        // Sibling: bind a gateway and the SAME calls apply.
        RecordingWrites writes;
        panel.set_write_gateway(&writes);
        CHECK(panel.remove("art/hero.png"));
        CHECK(panel.restore("tok"));
    }

    // ===================== delete: the request, the token, the listener =========================
    {
        RecordingWrites writes;
        FilesPanel panel;
        panel.set_write_gateway(&writes);
        panel.set_model(standard_model());

        std::vector<Heard> heard;
        panel.add_write_listener([&heard](FileWriteVerb verb, const FileWriteResult& result)
                                 { heard.push_back({verb, result}); });

        CHECK(panel.remove("art/hero.png"));
        CHECK(writes.calls.size() == 1);
        CHECK(writes.calls[0].verb == "delete");
        CHECK(heard.size() == 1);
        CHECK(heard[0].verb == FileWriteVerb::remove);
        CHECK(heard[0].result.ok());
        // The restore token is what makes the delete reversible — it must reach the listener, since
        // that is the ONLY channel the session journal learns about it through.
        CHECK(heard[0].result.restore_token == "00000000000000000000000000000aaa");

        // A REFUSED write reaches the listener too. This is the load-bearing half: the Shell's loud
        // `editor.ui.write-notice` is published from exactly this callback, so a refusal that did not
        // fire it would be a silent failure on a destructive operation.
        writes.refuse = true;
        CHECK(!panel.remove("art/hero.png"));
        CHECK(heard.size() == 2);
        CHECK(!heard[1].result.ok());
        CHECK(heard[1].result.code == "asset.delete_referenced");
        CHECK(!heard[1].result.message.empty());

        // ...and so does a LOCAL refusal, which never reached the write path at all. A caller that
        // heard nothing here would have no way to tell "refused" from "the click did nothing".
        CHECK(!panel.remove("art/ghost.png"));
        CHECK(heard.size() == 3);
        CHECK(heard[2].result.code == std::string(FilesPanel::kInvalidRequestCode));
    }

    // ============================ the LOUD surface, rendered ====================================
    {
        RecordingWrites writes;
        writes.refuse = true;
        FilesPanel panel;
        panel.set_write_gateway(&writes);
        panel.set_model(standard_model());

        // Before any write there is NO write-status node at all — a panel that has authored nothing
        // renders exactly what it rendered before e2.
        CHECK(find_node(panel.build_panel().root(), "files.write-status") == nullptr);

        CHECK(!panel.remove("art/hero.png"));
        const std::string refused = status_text(panel.build_panel(), "files.write-status");
        // Loud means the human can SEE what happened, which file, and why — not a generic failure.
        CHECK(refused.find("REFUSED") != std::string::npos);
        CHECK(refused.find("art/hero.png") != std::string::npos);
        CHECK(refused.find("asset.delete_referenced") != std::string::npos);
        CHECK(refused.find("scenes/main.json") != std::string::npos);

        writes.refuse = false;
        CHECK(panel.remove("art/hero.png"));
        const std::string applied = status_text(panel.build_panel(), "files.write-status");
        CHECK(applied.find("applied") != std::string::npos);
        CHECK(applied.find("REFUSED") == std::string::npos);
    }

    // ===================== commands: exposed only when actually reachable =======================
    {
        FilesPanel panel;
        panel.set_model(standard_model());

        // No gateway, no selection: no authoring command. Offering one would be an affordance that
        // silently does nothing.
        {
            const uitree::Panel built = panel.build_panel();
            CHECK(panel_exposes(built, kSelectCommand));
            CHECK(!panel_exposes(built, kRenameCommand));
            CHECK(!panel_exposes(built, kDeleteCommand));
        }

        RecordingWrites writes;
        panel.set_write_gateway(&writes);
        // A gateway but no selected row: still nothing, because the commands have no subject.
        {
            const uitree::Panel built = panel.build_panel();
            CHECK(!panel_exposes(built, kRenameCommand));
            CHECK(!panel_exposes(built, kMoveCommand));
            CHECK(!panel_exposes(built, kDeleteCommand));
        }

        CHECK(panel.apply_selection({"art/hero.png"}));
        {
            const uitree::Panel built = panel.build_panel();
            CHECK(panel_exposes(built, kRenameCommand));
            CHECK(panel_exposes(built, kMoveCommand));
            CHECK(panel_exposes(built, kDeleteCommand));
            // Every exposed command is backed by a focusable widget, so audit_a11y never reports an
            // unreachable command — asserted, not assumed.
            CHECK(uitree::audit_a11y(built).empty());
            CHECK(find_node(built.root(), "files.button.delete") != nullptr);
        }

        // Detaching the gateway withdraws them again (the LIFETIME clear point is not cosmetic: a
        // panel still offering Delete after the daemon went away is offering a lie).
        panel.set_write_gateway(nullptr);
        {
            const uitree::Panel built = panel.build_panel();
            CHECK(!panel_exposes(built, kDeleteCommand));
            CHECK(uitree::audit_a11y(built).empty());
        }
    }

    // ======================= a11y stays clean with the loud refusal rendered ====================
    {
        RecordingWrites writes;
        writes.refuse = true;
        FilesPanel panel;
        panel.set_write_gateway(&writes);
        panel.set_model(standard_model());
        CHECK(panel.apply_selection({"art/hero.png"}));
        CHECK(!panel.remove("art/hero.png"));

        const uitree::Panel built = panel.build_panel();
        CHECK(uitree::audit_a11y(built).empty());
        // Deterministic: identical state re-renders byte-identically.
        CHECK(uitree::render_html(built) == uitree::render_html(panel.build_panel()));
    }

    FILES_TEST_MAIN_END();
}
