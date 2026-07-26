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

// The message that EXPLAINS a non-ok replay: the first edit that did not land, not the first edit
// full stop. A multi-edit checkpoint whose SECOND field collided has an `applied` result at
// `edits.front()` carrying an empty message, so reporting `front()` would degrade the diagnostic to
// "undo dropped: " — the one thing a loud refusal must never be. (The Inspector commits one field
// per gesture today, so this is latent; it is one line to keep it from becoming a bug when a
// batched gesture lands.)
[[nodiscard]] const char* first_message(const undo::ReplayResult& result)
{
    for (const auto& edit : result.edits)
    {
        if (!edit.ok() && !edit.message.empty())
        {
            return edit.message.c_str();
        }
    }
    return "";
}
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
    if (!journal.load_json(parsed.root))
    {
        // Well-formed JSON that is not a journal document (a wrong-shaped or hand-edited blob).
        // `load_json` has already left the journal EMPTY; reporting it is what makes the "a corrupt
        // one leaves the journal EMPTY and reports why" contract in builtin_panels.h true on THIS
        // branch too, not just on the unparseable one above.
        std::fprintf(stderr, "context_editor: the persisted undo journal was not a journal "
                             "document; the undo history was reset\n");
        return false;
    }
    return true;
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

void UndoFeed::bind_notice_sink(NoticeSink sink)
{
    notice_sink_ = std::move(sink);
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
    return run_replay(/*redo=*/false);
}

undo::ReplayResult UndoFeed::replay_redo()
{
    return run_replay(/*redo=*/true);
}

undo::ReplayResult UndoFeed::run_replay(bool redo)
{
    // Sampled HERE rather than by each caller: "the depths must be read before the replay" is then
    // an invariant of this function instead of a contract two call sites have to remember.
    const std::size_t undo_before = journal_.undo_depth();
    const std::size_t redo_before = journal_.redo_depth();
    const char* const verb = redo ? "redo" : "undo";
    undo::ReplayResult result = redo ? journal_.redo() : journal_.undo();
    if (result.status == Status::none)
    {
        return result; // nothing to undo/redo, or no gateway — the stacks did not move
    }
    ++replays_run_;
    if (result.status == Status::dropped)
    {
        ++replay_drops_;
        // R-HUX-001: the field moved under us, so the revert was REFUSED rather than clobbering a
        // co-writer, and the checkpoint is consumed (it can never be replayed now). The stderr line
        // is the LAST-RESORT channel (a headless run, a build with no renderer); the human-visible
        // one is the notice sink below (M9 e09b-3).
        std::fprintf(stderr, "context_editor: %s dropped: %s\n", verb, first_message(result));
    }
    else if (result.status == Status::error)
    {
        ++replay_refusals_;
        // NOT a concurrency event and NOT a loss: the write path refused (no daemon, an unreadable
        // field), so nothing was written and the journal KEPT the step for the human to retry once
        // the project is reachable again (undo_journal.h § undo). Reported for the same reason a
        // drop is — a refusal the human never hears about looks exactly like an undo that worked.
        std::fprintf(stderr, "context_editor: %s refused: %s\n", verb, first_message(result));
    }
    // M9 e09b-3 — THE LOUD SURFACE. Both non-landing outcomes go out, and only those: an
    // applied/rebased replay DID what the human asked, so it is not a notice. Placed after the two
    // branches so the sink sees the same classification the counters recorded.
    if (!result.ok() && notice_sink_)
    {
        ++notices_sent_;
        notice_sink_(verb, result);
    }
    // DIRTY IFF THE STACKS ACTUALLY MOVED — read off the journal rather than inferred from the
    // status, so this stays correct however the journal's keep-vs-consume policy evolves. A refusal
    // that kept its checkpoint changed nothing, so it must not dirty the persisted blob (which would
    // rewrite `.editor/editor-state.json` byte-identically) and must not bump the panel's revision
    // (which would force a re-render of an unchanged depth).
    if (journal_.undo_depth() != undo_before || journal_.redo_depth() != redo_before)
    {
        replay_landed_ = replay_landed_ || result.ok();
        dirty_ = true;
        host_.touch(panel_id_);
    }
    return result;
}

bool UndoFeed::take_replay_landed() noexcept
{
    const bool landed = replay_landed_;
    replay_landed_ = false;
    return landed;
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
        // `ok()`, NOT `status != none`: `dispatched` is the ONLY bit that reaches the human here.
        // The renderer's session action maps it straight to the palette's success/failure (boot.ts
        // § sessionActions), so reporting `true` for a LOUDLY DROPPED or REFUSED replay would tell
        // the human their undo worked while the file was left exactly as it was — the silent
        // failure R-HUX-001 exists to forbid. The panel's revision is bumped by `host_.touch`
        // independently, so declining here costs no re-render. e09b-3 gives the refusal its own
        // chrome; until then "it did not happen" is the honest bit to send.
        if (command_id == undo::UndoJournal::kUndoCommand)
        {
            return replay_undo().ok();
        }
        if (command_id == undo::UndoJournal::kRedoCommand)
        {
            return replay_redo().ok();
        }
        return false;
    };
    return provider;
}

} // namespace context::editor::shell::panels
