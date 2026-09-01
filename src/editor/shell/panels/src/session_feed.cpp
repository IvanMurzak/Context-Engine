// The live daemon session feed (see session_feed.h for the two-directional design + the
// echo-suppression rationale).

#include "context/editor/shell/panels/session_feed.h"

#include "context/editor/client/client.h" // the wire writes (complete type HERE only)
#include "context/editor/gui/playbar/playbar_panel.h"
#include "context/editor/shell/session_bridge.h" // the session.control verb vocabulary (d1)
#include "wire_read.h" // read_string / read_bool / read_u64 / envelope_data

#include <cstdio>
#include <optional>
#include <utility>

namespace context::editor::shell::panels
{

namespace
{

// The daemon's `state` token -> the L-51 PlayState. e08a publishes exactly the tokens
// `gui::playbar::state_token()` renders, so this is a lookup, not a translation layer.
//
// `nullopt` for anything else — an absent/malformed member, or a token from a NEWER daemon. It must
// NOT fall back to `edit`: `edit` is a POSITIVE L-51 claim ("authored truth, no live session"), so
// asserting it on a token we simply do not understand would be a confident lie about provenance, and
// the daemon publishes `play-state` only on a CHANGE — nothing would ever come along to correct it.
// Leaving the last known state alone is the honest reading, and is byte-for-byte the rule the TS
// side applies (`toPlayState` in src/editor/webui/core/src/when.ts).
[[nodiscard]] std::optional<playbar::PlayState> parse_play_state(const std::string& token)
{
    if (token == playbar::state_token(playbar::PlayState::edit))
    {
        return playbar::PlayState::edit;
    }
    if (token == playbar::state_token(playbar::PlayState::playing))
    {
        return playbar::PlayState::playing;
    }
    if (token == playbar::state_token(playbar::PlayState::paused))
    {
        return playbar::PlayState::paused;
    }
    return std::nullopt;
}

// The `ids` array of a `selection-changed` fact (or an `editor.selection-get` read). Non-string
// entries are SKIPPED rather than fatal — the ProblemsFeed tolerance discipline.
[[nodiscard]] std::vector<std::string> read_ids(const contract::Json& object)
{
    std::vector<std::string> ids;
    const contract::Json& array = object.at("ids");
    if (!array.is_array())
    {
        return ids;
    }
    ids.reserve(array.size());
    for (std::size_t i = 0; i < array.size(); ++i)
    {
        const contract::Json& entry = array.at(i);
        if (entry.is_string())
        {
            ids.push_back(entry.as_string());
        }
    }
    return ids;
}

// The `subject` of a selection fact. ABSENT reads as `entity` — the documented default of the wire
// parameter, so an older daemon or a hand-written client means what it always meant. A non-string
// member is NOT coerced: it is answered as an empty subject, which matches nothing and is therefore
// dropped, rather than being read as the one subject this Shell does apply.
[[nodiscard]] std::string read_subject(const contract::Json& payload)
{
    if (!payload.contains("subject"))
    {
        return kSelectionSubjectEntity;
    }
    const contract::Json& raw = payload.at("subject");
    return raw.is_string() ? raw.as_string() : std::string();
}

} // namespace

SessionFeed::SessionFeed(PanelHost& host, std::string playbar_panel_id)
    : host_(host), playbar_panel_id_(std::move(playbar_panel_id)), playbar_(this)
{
}

void SessionFeed::bind_client(client::Client* client, std::uint64_t client_id) noexcept
{
    client_ = client;
    client_id_ = client_id;
}

void SessionFeed::bind_scene_tree(scenetree::SceneTreePanel* panel, std::string panel_id)
{
    scene_tree_ = panel;
    scene_tree_panel_id_ = std::move(panel_id);
}

void SessionFeed::add_focus_listener(FocusListener listener)
{
    if (listener)
    {
        focus_listeners_.push_back(std::move(listener));
    }
}

bool SessionFeed::apply_event(const std::string& topic, const contract::Json& payload)
{
    if (topic != kSessionTopicName)
    {
        return false;
    }

    // ECHO SUPPRESSION — the whole contract, in one comparison. `origin` is the client id of whoever
    // caused the change; a fact matching OUR id is the echo of a write this Shell just made, and the
    // panels have already been told the outcome by the reply. Applying it again is the double-apply
    // (and, mid-gesture, the flicker) this rule exists to make impossible.
    //
    // client_id_ == 0 means NOT ATTACHED, and 0 is also the daemon's own origin — so a 0/0 match must
    // not be read as "our echo". Guarding on client_id_ != 0 keeps an unattached Shell a plain
    // subscriber rather than one that silently swallows every daemon-originated fact.
    const std::uint64_t origin = read_u64(payload, "origin");
    if (client_id_ != 0 && origin == client_id_)
    {
        ++echoes_dropped_;
        return false;
    }

    const std::string event = read_string(payload, "event");
    if (event == kSelectionChangedEvent)
    {
        // THE SUBJECT FILTER (session_feed.h § THE SUBJECT FILTER). The Scene tree renders ENTITIES;
        // feeding it a `file` selection would hand `apply_selection` project paths to resolve as L-35
        // entity id-paths, and the result — a tree with nothing selected — is indistinguishable from
        // a correct empty result. Counted, not silently skipped, so the drop is observable.
        if (read_subject(payload) != kSelectionSubjectEntity)
        {
            ++foreign_subject_facts_;
            return false;
        }
        if (scene_tree_ == nullptr)
        {
            return false;
        }
        if (!scene_tree_->apply_selection(read_ids(payload)))
        {
            return false;
        }
        ++facts_applied_;
        host_.touch(scene_tree_panel_id_);
        return true;
    }

    if (event == kSelectionFocusEvent)
    {
        // c1/D3. The focus is DAEMON truth about which selection the human is working on, so it is
        // adopted whatever the subject — including a subject this build cannot render, which is
        // precisely the case a consumer needs to be told about (the Inspector stops showing an entity
        // nobody is working on). An empty/absent subject is not a focus claim and moves nothing.
        //
        // ⚠ ABSENT IS READ DIFFERENTLY HERE THAN ON `selection-changed`, deliberately. There,
        // absence is the wire parameter's documented default (`entity`) because an OLDER daemon
        // published the fact without the member. `selection-focus` is NEW in c1 — no daemon ever
        // published it without a `subject` — so an absent member is a MALFORMED fact, and reading it
        // as `entity` would move the focus off whatever the human is really working on.
        const std::string subject =
            payload.contains("subject") ? read_subject(payload) : std::string();
        if (subject.empty() || subject == selection_focus_)
        {
            return false;
        }
        selection_focus_ = subject;
        ++facts_applied_;
        for (const FocusListener& listener : focus_listeners_)
        {
            listener(selection_focus_);
        }
        return true;
    }

    if (event == kPlayStateEvent)
    {
        // A token this build cannot name leaves the rendered state EXACTLY where it was (see
        // parse_play_state) — an unreadable fact is not a fact about `edit`.
        const std::optional<playbar::PlayState> state =
            parse_play_state(read_string(payload, "state"));
        if (!state.has_value())
        {
            return false;
        }
        if (!playbar_.apply_play_state(*state, read_u64(payload, "simTick")))
        {
            return false;
        }
        ++facts_applied_;
        host_.touch(playbar_panel_id_);
        return true;
    }

    // kCameraChangedEvent (and any future fact) is recognised and ignored: the camera UI is e11.
    return false;
}

std::optional<std::vector<std::string>>
SessionFeed::request_selection(const std::vector<std::string>& ids)
{
    if (client_ == nullptr)
    {
        return std::nullopt; // no daemon: the panel changes nothing, and says so
    }
    ++writes_issued_;

    contract::Json params = contract::Json::object();
    contract::Json wire = contract::Json::array();
    for (const std::string& id : ids)
    {
        wire.push_back(contract::Json(id));
    }
    params.set("ids", std::move(wire));
    // `mode` is deliberately omitted: the daemon defaults to `replace`, which is the only mode a
    // single-select panel can express. Sending it explicitly would pin a default that is the
    // daemon's to choose.

    std::string error;
    const std::optional<contract::Json> reply =
        client_->call("editor.select", std::move(params), error);
    if (!reply.has_value())
    {
        std::fprintf(stderr, "context_editor: `editor.select` was refused (%s: %s)\n",
                     client_->last_error_code().c_str(), error.c_str());
        return std::nullopt;
    }
    // The reply's `ids` is THE DAEMON'S post-write selection — including on a `changed:false` no-op,
    // where it is exactly what is already selected. It is the panel's only path to seeing its own
    // selection, because the `selection-changed` fact this write publishes carries OUR origin and is
    // dropped by apply_event below (session_feed.h's echo-suppression note).
    const contract::Json& data = envelope_data(*reply);

    // ⚠ c1/D3 — THE FOCUS MIRROR, and it rides the SAME echo-suppression note. The daemon focuses
    // the subject a NON-EMPTY change leaves behind, and the `selection-focus` fact it publishes for
    // this write carries OUR origin, so `apply_event` drops it. Left unmirrored, `selection_focus_`
    // keeps naming whatever a foreign client focused last — and then SWALLOWS the next genuine move
    // back to that subject (`subject == selection_focus_` returns false), so the Inspector goes on
    // rendering an entity nobody is working on. That is the exact failure D3 exists to prevent.
    //
    // `entity` is not a guess: this writer sends no `subject`, so the daemon applied the default.
    // A future writer that names one must mirror THAT subject here.
    //
    // Listeners are deliberately NOT notified — this window's own write already re-points the
    // Inspector through the Scene tree's selection listener, and firing them would re-enter the
    // L-30 seams a second time for one gesture.
    std::vector<std::string> applied = read_ids(data);
    if (read_bool(data, "changed") && !applied.empty())
    {
        selection_focus_ = kSelectionSubjectEntity;
    }
    return applied;
}

playbar::PlayCommandResult SessionFeed::drive_play(const char* method, contract::Json params)
{
    playbar::PlayCommandResult out;
    if (client_ == nullptr)
    {
        // No daemon. Not a refusal with a catalog code — there was nothing to refuse it.
        out.ok = false;
        out.state = playbar_.state();
        return out;
    }
    ++writes_issued_;

    std::string error;
    const std::optional<contract::Json> reply = client_->call(method, std::move(params), error);
    if (!reply.has_value())
    {
        // The daemon's own code, VERBATIM (each maps to a different exit class); `internal.error` for
        // a transport fault, which is what failure_code's fallback rule says a wire failure means.
        out.ok = false;
        out.error_code = client_->failure_code("internal.error");
        out.state = playbar_.state();
        return out;
    }

    const contract::Json& data = envelope_data(*reply);
    out.ok = true;
    out.changed = read_bool(data, "changed");
    // Same rule as the fact path: a token this build cannot name keeps the last known state rather
    // than asserting `edit`. `PlayCommandResult::state` defaults to `edit`, so it must be filled
    // explicitly — leaving it defaulted IS the fabricated "no live session" claim.
    out.state = parse_play_state(read_string(data, "state")).value_or(playbar_.state());
    out.sim_tick = read_u64(data, "simTick");
    return out;
}

playbar::PlayCommandResult SessionFeed::play()
{
    return drive_play("editor.play", contract::Json::object());
}

playbar::PlayCommandResult SessionFeed::pause()
{
    return drive_play("editor.pause", contract::Json::object());
}

playbar::PlayCommandResult SessionFeed::stop()
{
    return drive_play("editor.stop", contract::Json::object());
}

playbar::PlayCommandResult SessionFeed::step(std::uint64_t ticks)
{
    contract::Json params = contract::Json::object();
    // A number, not a string: the daemon accepts both (the CLI projection sends a string), and a
    // hand-written RPC caller has no reason to pay the string round trip.
    params.set("ticks", contract::Json(ticks));
    return drive_play("editor.step", std::move(params));
}

std::optional<playbar::PlayAction> SessionFeed::control(const std::string& verb)
{
    // The ONE verb -> model dispatch: make_provider()'s command path below funnels through here
    // too, so the strip's press and the dock panel's press are the same write — indistinguishable
    // from the daemon's point of view (same wire method, same origin, same echo suppression).
    playbar::PlayAction action;
    if (verb == kSessionControlVerbPlay)
    {
        action = playbar_.play();
    }
    else if (verb == kSessionControlVerbPause)
    {
        action = playbar_.pause();
    }
    else if (verb == kSessionControlVerbStop)
    {
        action = playbar_.stop();
    }
    else if (verb == kSessionControlVerbStep)
    {
        action = playbar_.step(1);
    }
    else
    {
        return std::nullopt;
    }
    host_.touch(playbar_panel_id_);
    return action;
}

PanelProvider SessionFeed::make_provider()
{
    PanelProvider provider;
    provider.build = [this] { return playbar::build_playbar_panel(playbar_); };
    provider.invoke = [this](const std::string& command_id, const contract::Json&)
    {
        // Each transport command is a WRITE to the daemon. `dispatched` reports whether the command
        // was RECOGNISED — a refused or no-op transition is an ordinary outcome the panel's own
        // status line surfaces, not a protocol fault (panel_host.h states the rule).
        //
        // Only the command-id -> verb MAP lives here: the write itself funnels through `control()`
        // above, so the dock panel's press and the strip's press are one code path, not two
        // dispatch ladders that can drift.
        const char* verb = command_id == playbar::kPlayCommand    ? kSessionControlVerbPlay
                           : command_id == playbar::kPauseCommand ? kSessionControlVerbPause
                           : command_id == playbar::kStopCommand  ? kSessionControlVerbStop
                           : command_id == playbar::kStepCommand  ? kSessionControlVerbStep
                                                                  : nullptr;
        return verb != nullptr && control(verb).has_value();
    };
    // No gesture, no state pair: a transport bar with nothing continuous and nothing to persist.
    return provider;
}

} // namespace context::editor::shell::panels
