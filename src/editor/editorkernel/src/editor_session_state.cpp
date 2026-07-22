// Daemon editor session-state implementation (see editor_session_state.h).

#include "context/editor/editorkernel/editor_session_state.h"

#include <algorithm>
#include <exception>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace context::editor::editorkernel
{

namespace fs = std::filesystem;
using contract::Json;

namespace
{
// The reserved play.* codes, mirrored from gui::playbar (playbar_model.h owns the canonical
// constants; src/editor/contract/src/error_catalog.cpp registers the same strings). Repeated here as
// values rather than linked, exactly like the playbar's own copy: the editorkernel must not take a
// dependency on a GUI library to answer a play verb.
constexpr const char* kPlayNotRunningCode = "play.not_running";

// The persisted document version. Bumped only for a shape change the loader cannot absorb
// additively; a document from a FUTURE version is treated as corrupt (quarantined, defaults loaded)
// rather than half-applied.
constexpr std::int64_t kSessionFileVersion = 1;

// Deep equality by canonical rendering. contract::Json preserves insertion order and dumps
// deterministically, so this is a total, allocation-cheap comparison for the small camera payloads —
// and it needs no operator== on Json (which the contract type deliberately does not expose).
bool json_equal(const Json& a, const Json& b)
{
    return a.dump(0) == b.dump(0);
}
} // namespace

std::optional<SelectionMode> parse_selection_mode(const std::string& token)
{
    if (token == "replace")
        return SelectionMode::replace;
    if (token == "add")
        return SelectionMode::add;
    if (token == "toggle")
        return SelectionMode::toggle;
    if (token == "remove")
        return SelectionMode::remove;
    return std::nullopt;
}

const char* selection_mode_token(SelectionMode mode)
{
    switch (mode)
    {
    case SelectionMode::replace:
        return "replace";
    case SelectionMode::add:
        return "add";
    case SelectionMode::toggle:
        return "toggle";
    case SelectionMode::remove:
        return "remove";
    }
    return "replace";
}

const char* play_state_token(EditorPlayState state)
{
    switch (state)
    {
    case EditorPlayState::edit:
        return "edit";
    case EditorPlayState::playing:
        return "playing";
    case EditorPlayState::paused:
        return "paused";
    }
    return "edit";
}

// --- selection -----------------------------------------------------------------------------------

bool EditorSessionState::apply_selection(const std::vector<std::string>& ids, SelectionMode mode)
{
    std::vector<std::string> next = selection_;

    const auto contains = [&next](const std::string& id) {
        return std::find(next.begin(), next.end(), id) != next.end();
    };
    const auto erase_id = [&next](const std::string& id) {
        next.erase(std::remove(next.begin(), next.end(), id), next.end());
    };

    switch (mode)
    {
    case SelectionMode::replace:
        next.clear();
        for (const std::string& id : ids)
            if (!contains(id))
                next.push_back(id); // de-duplicate: a selection is a SET, order = first mention
        break;
    case SelectionMode::add:
        for (const std::string& id : ids)
            if (!contains(id))
                next.push_back(id);
        break;
    case SelectionMode::toggle:
        for (const std::string& id : ids)
        {
            if (contains(id))
                erase_id(id);
            else
                next.push_back(id);
        }
        break;
    case SelectionMode::remove:
        for (const std::string& id : ids)
            erase_id(id);
        break;
    }

    if (next == selection_)
        return false;
    selection_ = std::move(next);
    return true;
}

// --- cameras -------------------------------------------------------------------------------------

bool EditorSessionState::set_camera(const std::string& viewport_id, Json transform, Json projection)
{
    const auto it = cameras_.find(viewport_id);
    if (it != cameras_.end() && json_equal(it->second.transform, transform) &&
        json_equal(it->second.projection, projection))
        return false;

    CameraState next;
    next.transform = std::move(transform);
    next.projection = std::move(projection);
    cameras_[viewport_id] = std::move(next);
    return true;
}

// --- play control (L-51) -------------------------------------------------------------------------

PlayOutcome EditorSessionState::play()
{
    PlayOutcome out;
    if (play_ == EditorPlayState::playing)
    {
        out.state = play_;
        out.sim_tick = sim_tick_;
        return out; // already running — benign no-op
    }
    // edit -> playing begins a live session over the edit-state view; paused -> playing RESUMES the
    // same one, so the tick counter is retained across a resume and reset only by stop().
    play_ = EditorPlayState::playing;
    out.changed = true;
    out.state = play_;
    out.sim_tick = sim_tick_;
    return out;
}

PlayOutcome EditorSessionState::pause()
{
    PlayOutcome out;
    out.state = play_;
    out.sim_tick = sim_tick_;
    if (play_ == EditorPlayState::edit)
    {
        out.ok = false;
        out.error_code = kPlayNotRunningCode; // nothing to pause (L-51 edit state)
        return out;
    }
    if (play_ == EditorPlayState::paused)
        return out; // already paused — benign no-op
    play_ = EditorPlayState::paused;
    out.changed = true;
    out.state = play_;
    return out;
}

PlayOutcome EditorSessionState::stop()
{
    PlayOutcome out;
    out.state = play_;
    out.sim_tick = sim_tick_;
    if (play_ == EditorPlayState::edit)
        return out; // idempotent — nothing to stop
    // L-51: the runtime session's state is DISCARDED, never written back to authored files.
    play_ = EditorPlayState::edit;
    sim_tick_ = 0;
    out.changed = true;
    out.state = play_;
    out.sim_tick = sim_tick_;
    return out;
}

PlayOutcome EditorSessionState::step(std::uint64_t ticks)
{
    PlayOutcome out;
    out.state = play_;
    out.sim_tick = sim_tick_;
    if (play_ == EditorPlayState::edit)
    {
        out.ok = false;
        out.error_code = kPlayNotRunningCode; // no live session to advance
        return out;
    }
    if (ticks == 0)
        return out; // a zero-tick step advances nothing — benign no-op, no event
    sim_tick_ += ticks;
    out.changed = true;
    out.sim_tick = sim_tick_;
    return out; // stepping leaves playing/paused alone (you may step from either)
}

// --- the persisted projection --------------------------------------------------------------------

Json selection_ids_json(const EditorSessionState& state)
{
    Json ids = Json::array();
    for (const std::string& id : state.selection())
        ids.push_back(Json(id));
    return ids;
}

Json cameras_json(const EditorSessionState& state)
{
    Json cameras = Json::array();
    for (const auto& [viewport_id, cam] : state.cameras()) // std::map => stable, sorted order
    {
        Json entry = Json::object();
        entry.set("viewportId", Json(viewport_id));
        entry.set("transform", cam.transform);
        entry.set("projection", cam.projection);
        cameras.push_back(std::move(entry));
    }
    return cameras;
}

Json EditorSessionState::to_json() const
{
    Json selection = Json::object();
    selection.set("ids", selection_ids_json(*this));

    Json doc = Json::object();
    doc.set("version", Json(static_cast<std::int64_t>(kSessionFileVersion)));
    doc.set("selection", std::move(selection));
    doc.set("cameras", cameras_json(*this));
    return doc;
}

bool EditorSessionState::apply_json(const Json& doc)
{
    if (!doc.is_object())
        return false;
    // A version from the FUTURE cannot be half-applied honestly — refuse it as corrupt so the caller
    // quarantines it and boots on defaults instead of silently dropping members it cannot read.
    if (doc.contains("version"))
    {
        if (!doc.at("version").is_number() || doc.at("version").as_int() > kSessionFileVersion ||
            doc.at("version").as_int() < 1)
            return false;
    }

    std::vector<std::string> selection;
    if (doc.contains("selection"))
    {
        const Json& sel = doc.at("selection");
        if (!sel.is_object())
            return false;
        if (sel.contains("ids"))
        {
            const Json& ids = sel.at("ids");
            if (!ids.is_array())
                return false;
            for (std::size_t i = 0; i < ids.size(); ++i)
            {
                if (!ids.at(i).is_string())
                    return false;
                const std::string& id = ids.at(i).as_string();
                if (std::find(selection.begin(), selection.end(), id) == selection.end())
                    selection.push_back(id);
            }
        }
    }

    std::map<std::string, CameraState> cameras;
    if (doc.contains("cameras"))
    {
        const Json& arr = doc.at("cameras");
        if (!arr.is_array())
            return false;
        for (std::size_t i = 0; i < arr.size(); ++i)
        {
            const Json& entry = arr.at(i);
            if (!entry.is_object() || !entry.contains("viewportId") ||
                !entry.at("viewportId").is_string())
                return false;
            CameraState cam;
            if (entry.contains("transform"))
                cam.transform = entry.at("transform");
            if (entry.contains("projection"))
                cam.projection = entry.at("projection");
            cameras[entry.at("viewportId").as_string()] = std::move(cam);
        }
    }

    selection_ = std::move(selection);
    cameras_ = std::move(cameras);
    // Play state is never persisted (L-51: a restarted daemon holds no live session), so a restore
    // leaves the boot default in place rather than reviving a stale `playing`.
    return true;
}

// --- persistence ---------------------------------------------------------------------------------

fs::path session_state_path(const fs::path& project_root)
{
    return project_root / ".editor" / "session.json";
}

namespace
{
// Pick a free quarantine name next to the corrupt file: session.corrupt.json, then -1, -2, … The
// bounded search keeps a pathological directory from spinning; the last candidate is overwritten.
fs::path quarantine_path_for(const fs::path& session_file)
{
    const fs::path dir = session_file.parent_path();
    for (int n = 0; n < 64; ++n)
    {
        const std::string name =
            n == 0 ? "session.corrupt.json" : "session.corrupt-" + std::to_string(n) + ".json";
        std::error_code ec;
        if (!fs::exists(dir / name, ec))
            return dir / name;
    }
    return dir / "session.corrupt.json";
}

std::string read_text(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return std::string();
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
} // namespace

SessionRestoreReport restore_session_state(const fs::path& project_root, EditorSessionState& state)
{
    SessionRestoreReport report;
    const fs::path path = session_state_path(project_root);
    report.path = path.string();

    std::error_code ec;
    if (!fs::exists(path, ec))
        return report; // fresh — a first boot on this project, not an error

    const std::string text = read_text(path);
    std::string detail;
    bool ok = false;
    if (text.empty())
    {
        detail = "the session file is empty or unreadable";
    }
    else
    {
        try
        {
            const Json doc = Json::parse(text);
            ok = state.apply_json(doc);
            if (!ok)
                detail = "the session file parsed but its shape is not a readable session document";
        }
        catch (const std::exception& e)
        {
            detail = std::string("the session file is not well-formed JSON: ") + e.what();
        }
    }

    if (ok)
    {
        report.outcome = SessionRestoreOutcome::restored;
        return report;
    }

    // Corrupt (07 §6): move it aside so the next clean shutdown can write a good one, load defaults,
    // and hand the caller a report to announce LOUDLY. Recovery never blocks the boot — a daemon
    // that refused to start over a convenience file would be strictly worse than one that forgets a
    // selection.
    const fs::path quarantine = quarantine_path_for(path);
    std::error_code rename_ec;
    fs::rename(path, quarantine, rename_ec);
    report.outcome = SessionRestoreOutcome::recovered;
    report.detail = detail;
    if (rename_ec)
    {
        // Could not move it aside — say so instead of claiming a quarantine that did not happen.
        // The file stays put; the next restore will quarantine it again (still non-blocking).
        report.detail += "; the corrupt file could NOT be renamed aside (" + rename_ec.message() +
                         ") and remains at " + report.path;
    }
    else
    {
        report.quarantined_path = quarantine.string();
    }
    return report;
}

bool persist_session_state(const fs::path& project_root, const EditorSessionState& state,
                           std::string& error)
{
    const fs::path path = session_state_path(project_root);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec)
    {
        error = "could not create the control directory " + path.parent_path().string() + ": " +
                ec.message();
        return false;
    }

    const std::string text = state.to_json().dump(2) + "\n";

    // Write-then-rename: a crash mid-write leaves the PREVIOUS good file intact instead of a torn
    // one. (A torn file is survivable — restore quarantines it — but not producing one is better.)
    const fs::path tmp = path.parent_path() / "session.json.tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f)
        {
            error = "could not open " + tmp.string() + " for writing";
            return false;
        }
        f << text;
        if (!f)
        {
            error = "could not write " + tmp.string();
            return false;
        }
    }
    std::error_code rename_ec;
    fs::rename(tmp, path, rename_ec);
    if (!rename_ec)
        return true;

    // Rename refused (a filesystem/AV interposition): fall back to a direct truncate write so the
    // state is still persisted, and do not leave the temp file behind either way.
    fs::remove(tmp, ec);
    std::ofstream direct(path, std::ios::binary | std::ios::trunc);
    if (!direct)
    {
        error = "could not write " + path.string() + " (rename fallback: " + rename_ec.message() + ")";
        return false;
    }
    direct << text;
    if (!direct)
    {
        error = "could not write " + path.string();
        return false;
    }
    return true;
}

} // namespace context::editor::editorkernel
