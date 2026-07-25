// The session undo host implementation (see undo_feed.h for the three halves it closes and why the
// persistence blob is a canonical string).

#include "context/editor/shell/panels/undo_feed.h"

#include "context/editor/serializer/canonical.h"
#include "context/editor/serializer/json_parse.h"

#include <cstdio>
#include <string>
#include <utility>

namespace context::editor::shell::panels
{

namespace
{
namespace serializer = context::editor::serializer;
using Status = undo::ReplayResult::Status;
} // namespace

// --------------------------------------------------------------------------- the persistence blob

contract::Json undo_journal_to_blob(const undo::UndoJournal& journal)
{
    std::string text;
    if (!serializer::serialize_canonical(journal.to_json(), text))
    {
        // Unreachable for a journal this build produced (every member it writes is canonical-clean),
        // but a silent empty blob would look exactly like "no history" and quietly lose the user's
        // undo stack — so it is reported, and the caller stores the honest empty state.
        std::fprintf(stderr, "context_editor: the undo journal could not be serialized; this "
                             "session's undo history will not persist\n");
        return contract::Json(std::string{});
    }
    return contract::Json(std::move(text));
}

bool undo_journal_from_blob(const contract::Json& blob, undo::UndoJournal& journal)
{
    if (!blob.is_string() || blob.as_string().empty())
    {
        // Not a failure worth reporting: a fresh project has no journal, which is the overwhelmingly
        // common case. Still clear the stacks so the caller cannot be left holding stale history.
        (void)journal.load_json(serializer::JsonValue{});
        return false;
    }
    serializer::ParseResult parsed = serializer::parse_json(blob.as_string());
    if (!parsed.ok)
    {
        (void)journal.load_json(serializer::JsonValue{}); // total: leaves the journal EMPTY
        // `ok == false` guarantees at least one FATAL diagnostic (json_parse.h), but the read is
        // guarded anyway — this path already handles a corrupt file, and indexing an empty vector to
        // report it would be a worse bug than the one being reported.
        std::fprintf(stderr, "context_editor: the persisted undo journal did not parse (%s); the "
                             "undo history was reset\n",
                     parsed.diagnostics.empty() ? "malformed"
                                                : parsed.diagnostics.front().message.c_str());
        return false;
    }
    return journal.load_json(parsed.root);
}

// ------------------------------------------------------------------------------------- the feed

UndoFeed::UndoFeed(PanelHost& host, std::string panel_id)
    : host_(host), panel_id_(std::move(panel_id))
{
}

void UndoFeed::bind_gateway(const inspector::OverrideWriteGateway* gateway)
{
    journal_.set_gateway(gateway);
    gateway_bound_ = gateway != nullptr;
}

void UndoFeed::record(undo::FieldEdit edit)
{
    // `capture` auto-checkpoints a lone edit as its own gesture (L-20) — the Inspector's shape, one
    // field per gesture. Recording a NEW gesture invalidates the redo stack inside the journal, so
    // the blob genuinely moved either way.
    journal_.capture(std::move(edit));
    ++checkpoints_recorded_;
    dirty_ = true;
    host_.touch(panel_id_);
}

undo::ReplayResult UndoFeed::replay_undo()
{
    undo::ReplayResult result = journal_.undo();
    if (result.status == Status::none)
    {
        return result; // nothing to undo / no gateway — the stacks did not move
    }
    ++replays_run_;
    if (result.status == Status::dropped)
    {
        ++replay_drops_;
        // R-HUX-001: the field moved under us, so the revert was REFUSED rather than clobbering a
        // co-writer. e09b-3 owns the human-visible chrome; reporting it here is what keeps the drop
        // from being silent in the meantime (the same posture inspector_feed.cpp takes).
        std::fprintf(stderr, "context_editor: undo dropped — %s\n",
                     result.edits.empty() ? "" : result.edits.front().message.c_str());
    }
    dirty_ = true;
    host_.touch(panel_id_);
    return result;
}

undo::ReplayResult UndoFeed::replay_redo()
{
    undo::ReplayResult result = journal_.redo();
    if (result.status == Status::none)
    {
        return result;
    }
    ++replays_run_;
    if (result.status == Status::dropped)
    {
        ++replay_drops_;
        std::fprintf(stderr, "context_editor: redo dropped — %s\n",
                     result.edits.empty() ? "" : result.edits.front().message.c_str());
    }
    dirty_ = true;
    host_.touch(panel_id_);
    return result;
}

contract::Json UndoFeed::to_blob() const
{
    return undo_journal_to_blob(journal_);
}

bool UndoFeed::load_blob(const contract::Json& blob)
{
    const bool loaded = undo_journal_from_blob(blob, journal_);
    // CLEAN either way: the feed now matches what is on disk, and re-publishing a blob we just read
    // back would dirty the store on every boot for no change.
    dirty_ = false;
    host_.touch(panel_id_);
    return loaded;
}

PanelProvider UndoFeed::make_provider()
{
    PanelProvider provider;
    provider.build = [this] { return journal_.build_panel(); };
    provider.invoke = [this](const std::string& command_id, const contract::Json&)
    {
        // The journal exposes a command ONLY when it is reachable (undo_journal.cpp's build_panel),
        // so a dispatch arriving for an unavailable action is a stale mounted DOM — it answers
        // `Status::none` below and is reported as not dispatched, which is the honest outcome.
        if (command_id == undo::UndoJournal::kUndoCommand)
        {
            return replay_undo().status != Status::none;
        }
        if (command_id == undo::UndoJournal::kRedoCommand)
        {
            return replay_redo().status != Status::none;
        }
        return false;
    };
    return provider;
}

} // namespace context::editor::shell::panels
