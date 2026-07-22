// The live daemon session feed (see session_feed.h for the two-directional design + the
// echo-suppression rationale).

#include "context/editor/shell/panels/session_feed.h"

#include "context/editor/client/client.h" // the wire writes (complete type HERE only)
#include "context/editor/gui/playbar/playbar_panel.h"
#include "wire_read.h" // read_string / read_bool

#include <cstdio>
#include <optional>
#include <utility>

namespace context::editor::shell::panels
{

namespace
{

// The `data` member of an R-CLI-008 success envelope, or the reply itself when it is already bare.
// The daemon always answers `{ok, data, generationAfter, warnings}` (dispatcher.cpp's
// envelope_to_response), but tolerating the bare shape keeps this readable against a hand-built reply
// in a test without the parser having to be told which it is.
[[nodiscard]] const contract::Json& envelope_data(const contract::Json& reply)
{
    const contract::Json& data = reply.at("data");
    return data.is_object() ? data : reply;
}

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

// A non-negative integer member as a u64; 0 when absent / not a number / negative.
[[nodiscard]] std::uint64_t read_u64(const contract::Json& object, const std::string& key)
{
    const contract::Json& value = object.at(key);
    if (!value.is_number())
    {
        return 0;
    }
    const std::int64_t raw = value.as_int();
    return raw > 0 ? static_cast<std::uint64_t>(raw) : 0;
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
    return read_ids(envelope_data(*reply));
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

PanelProvider SessionFeed::make_provider()
{
    PanelProvider provider;
    provider.build = [this] { return playbar::build_playbar_panel(playbar_); };
    provider.invoke = [this](const std::string& command_id, const contract::Json&)
    {
        // Each transport command is a WRITE to the daemon. `dispatched` reports whether the command
        // was RECOGNISED — a refused or no-op transition is an ordinary outcome the panel's own
        // status line surfaces, not a protocol fault (panel_host.h states the rule).
        if (command_id == playbar::kPlayCommand)
        {
            (void)playbar_.play();
        }
        else if (command_id == playbar::kPauseCommand)
        {
            (void)playbar_.pause();
        }
        else if (command_id == playbar::kStopCommand)
        {
            (void)playbar_.stop();
        }
        else if (command_id == playbar::kStepCommand)
        {
            (void)playbar_.step(1);
        }
        else
        {
            return false;
        }
        host_.touch(playbar_panel_id_);
        return true;
    };
    // No gesture, no state pair: a transport bar with nothing continuous and nothing to persist.
    return provider;
}

} // namespace context::editor::shell::panels
