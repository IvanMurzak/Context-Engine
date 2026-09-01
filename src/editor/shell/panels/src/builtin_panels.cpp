// The panel composition root (see builtin_panels.h for the D10 layering rationale).

#include "context/editor/shell/panels/builtin_panels.h"

#include "context/editor/client/client.h" // the pump's live daemon reads (complete type HERE only)
#include "context/editor/gui/panels/problems/problems_panel.h"
#include "context/editor/gui/playbar/playbar_model.h"
#include "context/editor/gui/uitree/builtin.h"
#include "context/editor/serializer/canonical.h" // the ONE value identity the observe seam reports (R-FILE-001)
#include "context/editor/shell/editor_state.h"   // EditorState / EditorStateStore (e09c)
#include "context/editor/shell/panels/files_feed.h"    // M9 e1: FilesFeed complete type (D10 read half)
#include "context/editor/shell/panels/inspector_feed.h"
#include "context/editor/shell/panels/problems_feed.h" // ProblemsFeed complete type (see builtin_panels.h)
#include "context/editor/shell/panels/scenetree_feed.h"
#include "context/editor/shell/panels/session_feed.h" // SessionFeed complete type (e08b)
#include "context/editor/shell/panels/undo_feed.h"    // UndoFeed complete type (e09c)
#include "context/editor/shell/panels/wire_override_gateway.h" // complete type (e09b-2)
#include "context/editor/shell/write_notice.h" // WriteNoticeRelay complete type (e09b-3)

#include <cstdio>
#include <string_view>
#include <utility>

namespace context::editor::shell::panels
{

namespace
{

// The roster id of the M5-F0b placeholder panel. A LITERAL because `gui/uitree/builtin.h` declares
// no id constant (builtin_roster.h records the same exception for the same reason) — the a11y
// coverage ctest is what keeps the two spellings honest.
constexpr const char* kPlaceholderPanelId = "placeholder";

// One synchronous daemon read for the pump below: claim-first is the CALLER's job (see the header's
// failure posture). Returns the reply's `result` (the R-CLI-008 envelope) or nullopt, reporting the
// failure to stderr — once per occurrence, because a claimed fetch is not retried until the next
// trigger, so this cannot spam.
[[nodiscard]] std::optional<contract::Json> pump_read(client::Client& client,
                                                      const std::string& method,
                                                      contract::Json params, const char* what)
{
    std::string error;
    std::optional<contract::Json> reply = client.call(method, std::move(params), error);
    if (!reply.has_value())
    {
        std::fprintf(stderr,
                     "context_editor: the %s read (%s) failed (%s); it will retry on the next "
                     "change\n",
                     what, method.c_str(), error.c_str());
    }
    return reply;
}

} // namespace

// Out-of-line so the feed types are complete here (builtin_panels.h forward-declares them to keep
// the header's include chain RTTI/CEF-clean — see that header's comment).
BuiltinPanels::BuiltinPanels() = default;
BuiltinPanels::~BuiltinPanels() = default;
BuiltinPanels::BuiltinPanels(BuiltinPanels&&) noexcept = default;
BuiltinPanels& BuiltinPanels::operator=(BuiltinPanels&&) noexcept = default;

// Same seam, non-member form (see builtin_panels.h): the complete `ProblemsFeed` type is available
// here because this TU includes `problems_feed.h` above.
void apply_problems_snapshot(ProblemsFeed& feed, const contract::Json& snapshot,
                             std::uint64_t generation)
{
    feed.apply_snapshot(snapshot, generation);
}

bool apply_problems_event(ProblemsFeed& feed, const std::string& topic, const contract::Json& payload,
                          std::uint64_t generation)
{
    return feed.apply_event(topic, payload, generation);
}

bool report_local_problem(BuiltinPanels& panels, const std::string& code, const std::string& message,
                          const std::string& file)
{
    if (panels.problems == nullptr)
    {
        return false; // no Problems feed in this build — see the header on why that is not fatal
    }
    contract::Json payload = contract::Json::object();
    payload.set("code", contract::Json(code));
    payload.set("message", contract::Json(message));
    if (!file.empty())
    {
        // `file`, NOT `pointer` — see the header. The parser reads both, but only `file` is rendered
        // and navigable, so a path in `pointer` is a path nobody in the GUI can see.
        payload.set("file", contract::Json(file));
    }
    // Generation 0: this diagnostic is not derived from a world generation at all (it is a boot-time
    // fact about a file on disk), and 0 is the same stamp the Shell already uses for its own
    // snapshot/event dispatch when it has no envelope generation to quote.
    return apply_problems_event(*panels.problems, kDiagnosticsTopic, payload, 0);
}

bool apply_scenetree_event(SceneTreeFeed& feed, const std::string& topic,
                           const contract::Json& payload, std::uint64_t generation)
{
    return feed.apply_event(topic, payload, generation);
}

bool apply_files_event(FilesFeed& feed, const std::string& topic, const contract::Json& payload,
                       std::uint64_t generation)
{
    return feed.apply_event(topic, payload, generation);
}

bool apply_inspector_event(InspectorFeed& feed, const std::string& topic,
                           const contract::Json& payload)
{
    return feed.apply_event(topic, payload);
}

// --- the e09e-3 Inspector drive + observe seams ---------------------------------------------------
//
// The three re-declared spellings, pinned to their owners here for the SAME reason `kSessionTopic` is
// pinned below: this is the one TU that sees both sides, so a rename cannot silently desynchronise a
// `-fno-rtti` caller's DOM selector from the node ids `build_panel` actually emits.
static_assert(std::string_view(kInspectorPanelId) ==
                  std::string_view(inspector::InspectorPanel::kContributionId),
              "the -fno-rtti caller's panel id and the panel's own must be the same string");
static_assert(std::string_view(kInspectorEditCommand) ==
                  std::string_view(inspector::InspectorPanel::kEditCommand),
              "the -fno-rtti caller's edit command and the panel's own must be the same string");
static_assert(std::string_view(kInspectorWidgetNodePrefix) ==
                  std::string_view(kInspectorWidgetPrefix),
              "the -fno-rtti caller's widget node prefix and the feed's must be the same string");

bool request_inspector(BuiltinPanels& panels, const std::string& identity)
{
    return panels.inspector != nullptr && panels.inspector->request(identity);
}

namespace
{

// The CommitResult::Status vocabulary as tokens. Named here rather than inlined so a failure message
// can say WHICH way a commit resolved — the distinction between `dropped` (the L-30 guard held) and
// `none` (nothing was staged, i.e. a served refresh had already discarded the gesture) is exactly the
// one a fan-out assertion turns on, and a bare boolean cannot carry it.
[[nodiscard]] const char* commit_status_token(inspector::CommitResult::Status status) noexcept
{
    switch (status)
    {
    case inspector::CommitResult::Status::applied:
        return "applied";
    case inspector::CommitResult::Status::rebased:
        return "rebased";
    case inspector::CommitResult::Status::dropped:
        return "dropped";
    case inspector::CommitResult::Status::error:
        return "error";
    case inspector::CommitResult::Status::none:
        break;
    }
    return "none";
}

// One JsonValue -> its canonical serialization (R-FILE-001), newline-stripped. "" when it does not
// serialize, which for a value that came out of the model means only that the model holds nothing.
[[nodiscard]] std::string canonical_or_empty(const serializer::JsonValue& value)
{
    std::string out;
    if (!serializer::serialize_canonical(value, out))
    {
        return {};
    }
    if (!out.empty() && out.back() == '\n')
    {
        out.pop_back();
    }
    return out;
}

} // namespace

InspectorObservation observe_inspector(const BuiltinPanels& panels)
{
    InspectorObservation out;
    if (panels.inspector == nullptr)
    {
        return out; // `present` stays false — see the header on why that is not "an idle Inspector"
    }
    const InspectorFeed& feed = *panels.inspector;
    const inspector::InspectorPanel& panel = feed.panel();
    out.present = true;
    out.has_selection = panel.has_selection();
    out.has_staged_edit = panel.has_staged_edit();
    out.refresh_deferred = feed.refresh_deferred();
    // x10 (CE #452): the selection-driven half of the same deferral, plus the LOUD abandonment counter.
    // Carried here so the `-fno-rtti` view of the feed stays COMPLETE — the header claims this struct
    // is "every observable the feed and its panel expose", and a claim like that decays the moment a
    // new observable is left out of it.
    out.selection_deferred = feed.selection_deferred();
    out.deferred_selection =
        feed.deferred_selection().has_value() ? feed.deferred_selection()->identity : std::string();
    out.selections_deferred = feed.selections_deferred();
    out.abandons_observed = feed.abandons_observed();
    out.fetch_pending = feed.pending().has_value();
    out.base_raw_hash = panel.base_raw_hash();
    out.results_applied = feed.results_applied();
    out.events_applied = feed.events_applied();
    out.rereads_armed = feed.rereads_armed();
    out.commits_observed = feed.commits_observed();
    out.drops_observed = feed.drops_observed();
    out.writes_applied = panels.writes != nullptr ? panels.writes->writes_applied() : 0u;
    out.last_commit_status = commit_status_token(feed.last_commit().status);
    out.last_commit_code = feed.last_commit().code;
    if (panel.has_staged_edit())
    {
        out.staged_pointer = panel.staged_pointer();
        if (const serializer::JsonValue* staged = panel.staged_value())
        {
            out.staged_value = canonical_or_empty(*staged);
        }
    }
    return out;
}

std::string inspector_field_value(const BuiltinPanels& panels, const std::string& pointer,
                                  bool& overridden)
{
    overridden = false;
    if (panels.inspector == nullptr)
    {
        return {};
    }
    for (const inspector::InspectorField& field : panels.inspector->panel().model().fields)
    {
        if (field.pointer == pointer)
        {
            overridden = field.overridden;
            return canonical_or_empty(field.value);
        }
    }
    return {};
}

// The e08b seams. The topic string is declared in BOTH headers for the RTTI/CEF reason builtin_panels.h
// documents; this is the one TU that sees both, so it is where the two spellings are pinned together.
static_assert(std::string_view(kSessionTopic) == std::string_view(kSessionTopicName),
              "the subscriber's topic string and the feed's must be the same string");

bool apply_session_event(SessionFeed& feed, const std::string& topic, const contract::Json& payload)
{
    return feed.apply_event(topic, payload);
}

std::string session_play_state(const SessionFeed& feed)
{
    // `state_token()` is the ONE spelling of the L-51 vocabulary (docs/editor-session-state.md): the
    // playbar, the daemon's `play-state` fact, and now editor-core's `when` contexts all compare
    // against these same three bytes, which is what makes the relay translation-free.
    return playbar::state_token(feed.playbar_model().state());
}

std::size_t session_facts_applied(const SessionFeed& feed)
{
    return feed.facts_applied();
}

std::uint64_t session_sim_tick(const SessionFeed& feed)
{
    return feed.playbar_model().sim_tick();
}

std::uint64_t session_control_generation(const SessionFeed& feed)
{
    return feed.playbar_model().control_generation();
}

std::uint64_t session_state_generation(const SessionFeed& feed)
{
    // The generation `session.state` relays SUMS the applied-fact count and the model's control
    // generation: a locally driven transition (`session.control`, the dock panel) never bumps
    // `facts_applied` — the daemon's echo of our own write is dropped (session_feed.h) — so a
    // generation built from it alone would freeze every `session.state` poller in this process
    // across exactly the transitions the strip drives. Both counters are monotone, so the sum is
    // too.
    return static_cast<std::uint64_t>(session_facts_applied(feed)) +
           session_control_generation(feed);
}

SessionControlOutcome session_control(SessionFeed& feed, const std::string& verb)
{
    SessionControlOutcome outcome;
    const std::optional<playbar::PlayAction> action = feed.control(verb);
    if (!action.has_value())
    {
        // Unreachable through the bridge (install() validates the verb first), but a seam that
        // silently mapped an unknown verb to "no-op" would hide exactly the wiring bug the bridge's
        // own validation exists to surface.
        outcome.error_code = kSessionControlBadVerbCode;
        outcome.play_state = session_play_state(feed);
        outcome.sim_tick = session_sim_tick(feed);
        return outcome;
    }
    // PlayAction::ok is fed from the daemon's `changed` (playbar_model.h) — a refused command
    // carries a reserved play.* code, a benign no-op (and a daemon-less write) carries neither.
    // Relayed as-is so the browser side renders exactly what the dock panel would.
    outcome.changed = action->ok;
    outcome.error_code = action->error_code;
    outcome.play_state = playbar::state_token(action->state);
    outcome.sim_tick = session_sim_tick(feed);
    return outcome;
}

SessionSelectOutcome session_select(SessionFeed& feed, const std::vector<std::string>& ids)
{
    SessionSelectOutcome outcome;
    // The writer's reply `ids` is THE DAEMON'S post-write selection (session_feed.cpp: including on
    // a `changed:false` no-op, where it is exactly what is already selected) — relayed as-is, never
    // re-derived from the request, so the browser side renders daemon truth.
    std::optional<std::vector<std::string>> applied = feed.request_selection(ids);
    if (applied.has_value())
    {
        outcome.applied = true;
        outcome.ids = std::move(*applied);
    }
    return outcome;
}

void bind_session_client(SessionFeed& feed, client::Client* client)
{
    // The id comes from the client itself (0 when there is none), so the pointer and the identity
    // cannot disagree — see the header for why a separately-carried id is a stale-id hazard.
    feed.bind_client(client, client != nullptr ? client->client_id() : 0);
}

void bind_selection_focus(SessionFeed& session, InspectorFeed& inspector, SceneTreeFeed* scenetree)
{
    // Raw captures, safe by the SAME construction the Scene tree's own selection listener relies on:
    // in a real bag all three live and die together, and a focus fact can only arrive from
    // `apply_event`, which the owner loop stops calling before teardown (builtin_panels.h documents
    // the member order that keeps that a non-event). The listener holds no ownership either way.
    InspectorFeed* inspector_ptr = &inspector;
    session.add_focus_listener(
        [inspector_ptr, scenetree](const std::string& subject)
        {
            // A subject other than `entity` is one this panel cannot inspect, so it has no identity
            // to show — rendering the last entity would be a claim about a selection nobody is
            // looking at. `entity` re-points at whatever the Scene tree currently holds, which is
            // nothing when its provider was refused. Both roads meet at the same clear/request pair
            // the Scene tree's own selection listener uses, so the L-30 staged-gesture deferral
            // binds this caller identically (inspector_feed.h § request / § request_clear).
            const std::string identity = (subject == kSelectionSubjectEntity && scenetree != nullptr)
                                             ? scenetree->panel().selection().identity
                                             : std::string();
            if (identity.empty())
            {
                (void)inspector_ptr->request_clear();
            }
            else
            {
                (void)inspector_ptr->request(identity);
            }
        });
}

void bind_files_focus(SessionFeed& session, FilesFeed& files_feed)
{
    // Raw capture, safe by the SAME construction bind_selection_focus's does: in a real bag both
    // live (and die) together, and a focus fact can only arrive from `apply_event`, which the owner
    // loop stops calling before teardown.
    FilesFeed* files_ptr = &files_feed;
    session.add_focus_listener([files_ptr](const std::string& subject)
                               { files_ptr->panel().set_focused(subject == kSelectionSubjectFile); });
}

void bind_write_client(BuiltinPanels& panels, client::Client* client)
{
    // A bag whose gateway was never created (impossible from install_builtin_panels, possible from a
    // hand-built bag in a test) is not an error to re-point — there is simply nothing to point.
    if (panels.writes != nullptr)
    {
        panels.writes->bind_client(client);
    }
}

namespace
{

// The hue decision itself, in ONE place and as an exhaustive switch with NO `default:` —
// deliberately. This repo builds warnings-as-errors (`context_warnings`: `/W4 /WX`, else
// `-Wall -Wextra -Werror`), so an unhandled enumerator is `-Wswitch` / C4062 and a SIXTH
// `CommitResult::Status` becomes a build break right here. A ternary or a `default:` would instead
// map that new status to `refusal` silently, i.e. paint a data-integrity moment the wrong hue — the
// failure write_notice.h calls harder to notice than a missing notice, because the human is TOLD
// something, just not the truth.
//
// The KIND is read off the STATUS, never off the code. `dropped` is L-30's concurrent-writer verdict
// — nothing was lost and nothing was overwritten, the edit simply must be re-made against the value
// that is there now, which is 06 §2's `wait` ("awaiting-human"). Everything else that did not land is
// a write-PATH refusal (no daemon, an unreadable field, a compose refusal), which is `bad`. Keying on
// the status rather than on `code == "cas.mismatch"` keeps this correct if the catalog ever grows a
// second mismatch code.
[[nodiscard]] const char* notice_kind_for(inspector::CommitResult::Status status)
{
    switch (status)
    {
    case inspector::CommitResult::Status::dropped:
        // L-30: a concurrent writer touched THIS field. Nothing was lost -> `wait` (awaiting-human).
        return shell::kWriteNoticeKindDrop;
    case inspector::CommitResult::Status::error:
        // The write path refused the request -> `bad`.
        break;
    case inspector::CommitResult::Status::none:
    case inspector::CommitResult::Status::applied:
    case inspector::CommitResult::Status::rebased:
        // Not reachable through either sink — both feeds return before calling it unless the result
        // is `!ok()` (inspector_feed.cpp / undo_feed.cpp) — but answered rather than defaulted, so the
        // switch stays exhaustive and the compiler keeps guarding the enumerator set.
        break;
    }
    // The fail-safe answer, and the one every other unknown-input path in this feature also chooses
    // (`writeNoticeTone`, notifications.ts): over-stating severity costs a second look, while calling
    // an unreachable project "awaiting you" sends the human re-applying an edit that cannot land.
    return shell::kWriteNoticeKindRefusal;
}

// One refused commit -> the notice the human is shown (M9 e09b-3).
//
// NO USER-FACING SENTENCE IS COMPOSED HERE. The Shell puts FACTS on the wire (kind / action / code /
// message / pointer) and the renderer writes the prose (notifications.ts), because that is where the
// copy, the translation and the a11y naming live — a sentence assembled in C++ would be a second
// place user-facing text has to be reviewed.
[[nodiscard]] shell::WriteNotice notice_from_commit(const inspector::CommitResult& result,
                                                    const char* action)
{
    shell::WriteNotice notice;
    notice.kind = notice_kind_for(result.status);
    notice.action = action;
    notice.code = result.code;
    notice.message = result.message;
    notice.pointer = result.pointer;
    return notice;
}

// The same, for a replay. `ReplayResult` carries the whole checkpoint's per-field outcomes, so the
// notice is built from the edit that EXPLAINS the refusal — not `edits.front()`, which on a
// multi-edit checkpoint whose SECOND field collided is an `applied` result carrying an empty code and
// message, and would hand the human a notice with nothing in it. The STATUS still comes from the
// replay as a whole: that is the journal's verdict about the checkpoint, not about one field.
//
// THE SELECTION MATCHES `first_message` (undo_feed.cpp) ON PURPOSE — prefer a non-ok edit that has a
// message, and only fall back to the first non-ok one when none does. The two are the SAME refusal
// reported on two channels (that stderr line and this toast), so a predicate that drifted between
// them would let one name one field and the other name a different one, which reads as two failures
// where there was one. The fallback is what keeps a code-and-pointer-but-no-message edit from
// producing an empty notice.
[[nodiscard]] shell::WriteNotice notice_from_replay(const char* verb,
                                                    const undo::ReplayResult& result)
{
    shell::WriteNotice notice;
    notice.kind = notice_kind_for(result.status);
    notice.action = verb;
    const inspector::CommitResult* explaining = nullptr;
    for (const inspector::CommitResult& edit : result.edits)
    {
        if (!edit.ok() && (explaining == nullptr || explaining->message.empty()))
        {
            explaining = &edit;
            if (!edit.message.empty())
            {
                break;
            }
        }
    }
    if (explaining != nullptr)
    {
        notice.code = explaining->code;
        notice.message = explaining->message;
        notice.pointer = explaining->pointer;
    }
    return notice;
}

// One ABANDONED gesture -> the notice the human is shown (M9 x10, CE #452).
//
// THE THIRD KIND, ASSIGNED UNCONDITIONALLY, and that is the whole point of the function. There is no
// status to switch on — an abandonment IS the outcome — so unlike `notice_kind_for` above there is
// nothing to get wrong at runtime; what there was to get wrong is the CATEGORY, and it is fixed here:
// an abandoned gesture is neither a co-writer DROP (no CAS, no collision, possibly no second writer at
// all) nor a write-path REFUSAL (nothing was ever sent). Hueing it as either would hand the human a
// confidently wrong explanation of how their typing disappeared — see write_notice.h § the three kinds.
//
// The `action` is `edit`, matching the gesture sink above: from the human's side the thing they were
// doing is the same, and only the outcome differs.
[[nodiscard]] shell::WriteNotice notice_from_abandon(const AbandonedGesture& abandoned)
{
    shell::WriteNotice notice;
    notice.kind = shell::kWriteNoticeKindAbandoned;
    notice.action = "edit";
    notice.code = abandoned.code;
    notice.message = abandoned.message;
    notice.pointer = abandoned.pointer;
    return notice;
}

} // namespace

void bind_write_notice_relay(BuiltinPanels& panels, WriteNoticeRelay& relay)
{
    // Raw-pointer capture, not a reference capture, for the reason the header states: the sinks
    // outlive this call and the relay is guaranteed (by the composition root's declaration order) to
    // outlive the bag.
    WriteNoticeRelay* const target = &relay;
    if (panels.inspector != nullptr)
    {
        panels.inspector->bind_notice_sink(
            [target](const inspector::CommitResult& result)
            { (void)target->publish(notice_from_commit(result, "edit")); });
        // M9 x10 (CE #452) — the ABANDONED-gesture arm of the same relay. Bound HERE, alongside the
        // refused-write sinks, because the human does not care WHICH mechanism lost their edit; they
        // care that the editor said so. Reusing the one relay is also what keeps the transport,
        // the topic and the broadcast-to-every-window behaviour identical for all three kinds instead
        // of growing a second, subtly different notice path.
        panels.inspector->bind_abandon_sink([target](const AbandonedGesture& abandoned)
                                           { (void)target->publish(notice_from_abandon(abandoned)); });
    }
    if (panels.undo != nullptr)
    {
        panels.undo->bind_notice_sink([target](const char* verb, const undo::ReplayResult& result)
                                      { (void)target->publish(notice_from_replay(verb, result)); });
    }
}

bool restore_undo_state(BuiltinPanels& panels, const EditorState& state)
{
    // A bag with no undo host (a hand-built one in a test) has nothing to restore INTO — not an
    // error, just nothing to do.
    if (panels.undo == nullptr)
    {
        return false;
    }
    const bool loaded = panels.undo->load_blob(state.undo);
    // "A STEP CAME BACK", not merely "the blob parsed". A well-formed but EMPTY journal document
    // loads perfectly and restores nothing, which for a caller (and for the human reading the boot
    // log) is indistinguishable from a fresh project — so it answers false, exactly as an absent or
    // corrupt blob does.
    return loaded && (panels.undo->journal().can_undo() || panels.undo->journal().can_redo());
}

bool publish_undo_state(BuiltinPanels& panels, EditorStateStore& store, std::uint64_t now_us)
{
    if (panels.undo == nullptr || !panels.undo->dirty())
    {
        return false; // an idle journal is not re-serialized at all (see the header)
    }
    store.set_undo(panels.undo->to_blob(), now_us);
    panels.undo->mark_clean();
    return true;
}

void pump_panel_feeds(BuiltinPanels& panels, client::Client& client, const std::string& scene_path)
{
    // Files (M9 e1): fetch when due — NO scene_path gate, unlike the tree/inspector below, since
    // `editor.files` lists the WHOLE project rather than one scene. The claim precedes the call
    // (header: a failure waits for the next settle rather than hammering).
    if (panels.files != nullptr && panels.files->fetch_due())
    {
        panels.files->mark_fetched();
        if (const std::optional<contract::Json> reply =
                pump_read(client, "editor.files", contract::Json::object(), "files"))
        {
            (void)panels.files->apply_result(*reply);
        }
    }

    // Scene tree: fetch when due AND a scene is named. The claim precedes the call (header: a
    // failure waits for the next settle rather than hammering).
    if (panels.scenetree != nullptr && !scene_path.empty() && panels.scenetree->fetch_due())
    {
        panels.scenetree->mark_fetched();
        contract::Json params = contract::Json::object();
        params.set("path", contract::Json(scene_path));
        if (const std::optional<contract::Json> reply =
                pump_read(client, "editor.scene-tree", std::move(params), "scene-tree"))
        {
            (void)panels.scenetree->apply_result(*reply);
        }
    }

    // e09c READ-YOUR-REPLAYS (undo_feed.h § take_replay_landed for why the flag exists at all).
    // Armed BEFORE the fetch below so the SAME frame re-reads, and PULLED here rather than pushed
    // from the feed so the undo host holds no pointer back to the Inspector — which is declared
    // after it and therefore destroyed first.
    if (panels.undo != nullptr && panels.inspector != nullptr && panels.undo->take_replay_landed())
    {
        (void)panels.inspector->request_refresh();
    }

    // Inspector: fetch the pending selection. `scene_path` addresses the same root scene the tree
    // was built from — a selection can only have come from it.
    if (panels.inspector != nullptr && !scene_path.empty() &&
        panels.inspector->pending().has_value())
    {
        const std::string identity = *panels.inspector->pending();
        panels.inspector->mark_fetched();
        contract::Json params = contract::Json::object();
        params.set("path", contract::Json(scene_path));
        params.set("idPath", contract::Json(identity));
        if (const std::optional<contract::Json> reply =
                pump_read(client, "editor.inspect", std::move(params), "inspector"))
        {
            (void)panels.inspector->apply_result(*reply);
        }
    }
}

const std::vector<std::string>& hostable_panel_ids()
{
    // Built once. Kept in lockstep with the bindings below by `test_builtin_panels.cpp`, which
    // asserts every id here is hosted after install and that nothing else is — so this list cannot
    // drift into a claim the bindings do not honor.
    static const std::vector<std::string> ids = {
        kPlaceholderPanelId,
        gui::panels::problems::ProblemsPanel::kContributionId,
        gui::panels::scenetree::SceneTreePanel::kContributionId,
        gui::panels::inspector::InspectorPanel::kContributionId,
        // M9 e1, D10 read half: hostable from day one — its kernel-typed builder was born in
        // gui/panels/builders/ (the e05d3 split's precedent applied up front), so this panel never
        // had a "listed but unhosted" phase the way scene-tree/inspector did pre-e05d3.
        gui::panels::files::FilesPanel::kContributionId,
        // `builtin.playbar` (hostable e08b..e09) is RETIRED — editor-window-chrome e1 (D2): the d1
        // titlebar strip is the Play Bar's only home, driving the SAME SessionFeed/PlaybarModel
        // transport over `session.control`, so the dock surface came off the roster and out of this
        // list together with its a11y and help anchors.
        // e09c: the session history surface. Rostered since e05b but UNHOSTED until now: with no
        // provider there was no way to reach `session.undo` / `session.redo` from the renderer at
        // all, so the journal could be neither driven nor observed (and, having no host, nothing
        // called its to_json/load_json either). Hosting it is what turns the R-CLI-001
        // keyboard/CLI path into a real one.
        undo::UndoJournal::kContributionId,
    };
    return ids;
}

BuiltinPanels install_builtin_panels(PanelHost& host)
{
    BuiltinPanels out;

    // --- the placeholder (M5-F0b, context_gui_uitree) -------------------------------------------
    // A pure function of nothing: a static, a11y-conformant tree. It has no model to feed, no
    // commands to dispatch beyond its own, no gestures and no state — so its provider is `build`
    // ALONE, and `panel.list` reports `gestures:false, persists:false` for it. That minimal shape is
    // useful on its own terms: it proves the host and the hydration runtime work for a panel that
    // supplies nothing optional, which is the floor every future panel clears.
    {
        PanelProvider provider;
        provider.build = [] { return gui::uitree::make_placeholder_panel(); };
        if (host.provide(kPlaceholderPanelId, std::move(provider)))
        {
            ++out.bound;
        }
    }

    // --- Problems (M5-F4, context_gui_panel_problems) -------------------------------------------
    // The panel the e05d1 DoD proves the LIVE read path on. The feed owns the model and is driven
    // from the daemon's `diagnostics` topic by the caller (see problems_feed.h); binding the
    // provider here is what publishes it through `panel.*`.
    {
        auto feed =
            std::make_unique<ProblemsFeed>(host, gui::panels::problems::ProblemsPanel::kContributionId);
        if (host.provide(gui::panels::problems::ProblemsPanel::kContributionId,
                         feed->make_provider()))
        {
            ++out.bound;
            out.problems = std::move(feed);
        }
        // On a refused binding the feed is DROPPED rather than kept: a feed nothing routes to would
        // pump daemon events into a model no renderer can ever see.
    }

    // --- the daemon session feed (M9 e08b; the docked playbar RETIRED by editor-window-chrome e1) -
    // Constructed BEFORE the Scene tree, because it IS the SelectionGateway that panel writes
    // through: the panel takes the gateway at construction, and a gateway must outlive its panel.
    // Since e1 the feed hosts NO panel — the d1 titlebar strip (session_bridge's `session.*` verbs)
    // is the Play Bar's only home — so the feed is pure transport now: the `session` topic
    // subscriber and the play/selection write seams. Created UNCONDITIONALLY (there is no binding
    // left to refuse), and counted in `bound` by nothing: `bound` counts hosted PROVIDERS, and the
    // feed no longer is one. The retired panel id it still carries only addresses `host.touch`,
    // which panel_host.h documents as a no-op for an id the roster no longer knows.
    out.session = std::make_unique<SessionFeed>(host, gui::playbar::PlaybarModel::kContributionId);

    // --- Scene tree + Inspector (M9 e05d3, the D10-split pair) ----------------------------------
    // Hostable because their kernel-typed builders moved daemon-side (gui/panels/builders/): the
    // panel libraries on THIS closure are boundary-clean, and the models arrive as data over
    // `editor.scene-tree` / `editor.inspect` (see the feeds). The Scene tree's selection is what
    // drives the Inspector's fetch (R-HUX-011) — wired HERE, the one place holding both feeds.
    {
        auto scenetree = std::make_unique<SceneTreeFeed>(
            host, gui::panels::scenetree::SceneTreePanel::kContributionId, out.session.get());
        auto inspector = std::make_unique<InspectorFeed>(
            host, gui::panels::inspector::InspectorPanel::kContributionId);

        // e09b-2: the Inspector's WRITE path. Created here and bound BEFORE `make_provider()`,
        // because that is what decides the manifest's `gestures` capability — the gesture verb
        // `commit` IS the write, so a provider bound without a gateway would advertise a gesture that
        // could only be swallowed (inspector_feed.h). The gateway starts with NO client: it is
        // re-pointed at the daemon link every frame through `bind_write_client`, exactly like the
        // session feed's, so a reconnect (a new Client, a new connection) needs no re-binding here
        // and a lost daemon makes every commit refuse honestly instead of calling a freed pointer.
        out.writes = std::make_unique<WireOverrideWriteGateway>();
        inspector->bind_gateway(out.writes.get());

        const bool tree_bound =
            host.provide(gui::panels::scenetree::SceneTreePanel::kContributionId,
                         scenetree->make_provider());
        const bool inspector_bound =
            host.provide(gui::panels::inspector::InspectorPanel::kContributionId,
                         inspector->make_provider());

        if (tree_bound)
        {
            ++out.bound;
        }
        if (inspector_bound)
        {
            ++out.bound;
        }

        // The selection loop, wired only when BOTH ends exist. The raw capture is safe by
        // construction: both feeds live (and die) together in this bag, and no selection fires
        // during destruction (builtin_panels.h documents the member order that keeps it so).
        if (tree_bound && inspector_bound)
        {
            InspectorFeed* inspector_ptr = inspector.get();
            scenetree->panel().add_selection_listener(
                [inspector_ptr](const gui::panels::scenetree::SceneSelection& selection)
                {
                    if (selection.identity.empty())
                    {
                        // ⚠ A false return is the M9 x10 DEFERRAL, not a failure (CE #452): an EMPTY
                        // id list on a FOREIGN `session` `selection-changed` used to run
                        // `panel_.clear()` straight through, discarding the human's in-flight edit AND
                        // the owed settle deferral. The feed now withholds the clear behind a staged
                        // gesture and performs it when the gesture ends (inspector_feed.h §
                        // request_clear).
                        (void)inspector_ptr->request_clear();
                    }
                    else
                    {
                        // A false return is the L-30 deferral, not a failure: this listener fires both
                        // when a settle-driven tree refetch RE-RESOLVES the selected row's identity
                        // hash (re-reading the entity a gesture is staged on would silently re-base
                        // it) and — since x10 — when the selection MOVED to another entity, which used
                        // to discard the gesture outright (inspector_feed.h § request).
                        (void)inspector_ptr->request(selection.identity);
                    }
                });
        }

        if (tree_bound)
        {
            out.scenetree = std::move(scenetree);
        }
        if (inspector_bound)
        {
            out.inspector = std::move(inspector);
        }
        // A refused binding drops its feed (the ProblemsFeed rule); the selection listener was only
        // wired when both bound, so a dangling capture cannot survive this scope.

        // e08b: point the session feed at the panel whose rendered selection it drives. Done AFTER
        // the move, so the pointer is into the feed that will actually live in the bag. (The feed
        // itself is unconditional since e1 — only the Scene tree's binding can have been refused.)
        if (out.scenetree != nullptr)
        {
            out.session->bind_scene_tree(&out.scenetree->panel(),
                                         gui::panels::scenetree::SceneTreePanel::kContributionId);
        }

        // c1/D3: the Inspector renders the FOCUSED subject. Wired AFTER the moves for the same
        // reason `bind_scene_tree` is — the pointers must be into the feeds that will actually live
        // in the bag. The Scene tree is optional here (its provider can be refused); the Inspector
        // is not, because there is nothing to re-point without it.
        if (out.inspector != nullptr)
        {
            bind_selection_focus(*out.session, *out.inspector, out.scenetree.get());
        }
    }

    // --- Files (M9 e1, D10 read half) ------------------------------------------------------------
    // Hostable from day one — its kernel-typed builder (assetdb-backed) was born daemon-side in
    // gui/panels/builders/, so unlike scene-tree/inspector it never had a "listed but unhosted"
    // phase. Its selection writes through `session`'s `files::SelectionGateway` adapter — the SAME
    // `editor.select` verb the Scene tree writes through, subject "file" instead of the default.
    {
        auto files_feed = std::make_unique<FilesFeed>(
            host, gui::panels::files::FilesPanel::kContributionId, &out.session->file_selection_gateway());
        if (host.provide(gui::panels::files::FilesPanel::kContributionId, files_feed->make_provider()))
        {
            ++out.bound;
            out.files = std::move(files_feed);
        }
        // A refused binding DROPS the feed (the ProblemsFeed rule): a feed nothing routes to would
        // pump daemon reads into a model no renderer can ever see.

        // c1/D3: the Files panel renders whether IT currently has focus (bind_files_focus, unlike
        // the Inspector's re-pointing, only flips a boolean the panel already owns — see that
        // seam's own comment). Wired AFTER the move for the same reason bind_scene_tree is.
        if (out.files != nullptr)
        {
            bind_files_focus(*out.session, *out.files);
        }
    }

    // --- the session undo journal (M9 e09c, design 03 §1 / 05 §7-§8, R-HUX-001) ------------------
    // Constructed AFTER the Inspector because it needs the gateway the block above created, and
    // bound to it: replay therefore goes out over the SAME `WireOverrideWriteGateway` a live gesture
    // commits through — the ONE L-30 engine over the ONE write path, never a second one. An
    // undo is not a privileged write; it can hit `cas.mismatch` exactly like a live gesture.
    {
        auto undo_feed =
            std::make_unique<UndoFeed>(host, std::string(undo::UndoJournal::kContributionId));
        undo_feed->bind_gateway(out.writes.get());
        if (host.provide(undo::UndoJournal::kContributionId, undo_feed->make_provider()))
        {
            ++out.bound;
            out.undo = std::move(undo_feed);
        }
        // A refused binding DROPS the host (the ProblemsFeed rule) — and with it the Inspector's
        // checkpoint sink below, so commits are simply not journaled rather than accumulating in a
        // journal no surface can ever replay.

        // The recording loop, wired only when BOTH ends exist. The raw capture is safe by
        // construction: both live (and die) together in this bag, and `undo` is declared BEFORE
        // `inspector` (builtin_panels.h documents the sandwich), so the sink's target outlives the
        // feed holding it.
        if (out.undo != nullptr && out.inspector != nullptr)
        {
            UndoFeed* undo_ptr = out.undo.get();
            out.inspector->bind_checkpoint_sink([undo_ptr](undo::FieldEdit edit)
                                                { undo_ptr->record(std::move(edit)); });
        }
    }

    return out;
}

} // namespace context::editor::shell::panels
