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
//
// 1 -> 2 (c1): `selection: {ids}` became `selections: [{subject, ids}]` + `selectionFocus:
// {subject}`. That is exactly the shape change additive absorption CANNOT handle — a v1 file would
// pass the version check, hit a `contains("selections")` that finds nothing, and lose the human's
// selection in silence — so `apply_json` carries a real MIGRATION branch rather than relying on the
// tolerance the previous bump-free years relied on.
constexpr std::int64_t kSessionFileVersion = 2;

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

bool is_contract_selection_subject(const std::string& subject)
{
    return subject == kSelectionSubjectEntity || subject == kSelectionSubjectFile ||
           subject == kSelectionSubjectAsset;
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

const std::vector<std::string>& EditorSessionState::selection(const std::string& subject) const
{
    // A subject with nothing selected answers an empty vector rather than being absent: "what is
    // selected here" is always a well-formed question, and the empty answer is the honest one.
    static const std::vector<std::string> kNone;
    const auto it = selections_.find(subject);
    return it == selections_.end() ? kNone : it->second;
}

bool EditorSessionState::set_selection_focus(const std::string& subject)
{
    if (subject.empty() || subject == selection_focus_)
        return false;
    selection_focus_ = subject;
    return true;
}

bool EditorSessionState::apply_selection(const std::vector<std::string>& ids, SelectionMode mode)
{
    return apply_selection(kSelectionSubjectEntity, ids, mode).changed;
}

SelectionOutcome EditorSessionState::apply_selection(const std::string& subject,
                                                     const std::vector<std::string>& ids,
                                                     SelectionMode mode)
{
    const std::vector<std::string>& current = selection(subject);
    std::vector<std::string> next = current;

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

    SelectionOutcome out;
    if (next == current)
        return out; // a no-op: neither fact is published, per subject

    out.changed = true;
    const bool empty = next.empty();
    if (empty)
        selections_.erase(subject); // pruned, so `selections()` holds only LIVE selections
    else
        selections_[subject] = std::move(next);

    // D3: a change that leaves something selected focuses this subject; one that leaves it empty
    // moves nothing (see the header — there is nothing there to work on).
    if (!empty)
        out.focus_changed = set_selection_focus(subject);
    return out;
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

namespace
{
// The ONE ids-array encoder. Both spellings of the selection wire — the bare `ids` array and the
// `ids` member of a `{subject, ids}` entry — are the SAME array, so they are built here rather than
// by two loops that agree only by inspection.
Json ids_json(const std::vector<std::string>& ids)
{
    Json out = Json::array();
    for (const std::string& id : ids)
        out.push_back(Json(id));
    return out;
}

// The ONE `{subject, ids}` entry encoder, so the persisted array and the wire array cannot drift.
Json selection_entry_json(const std::string& subject, const std::vector<std::string>& ids)
{
    Json entry = Json::object();
    entry.set("subject", Json(subject));
    entry.set("ids", ids_json(ids));
    return entry;
}
} // namespace

Json selection_ids_json(const EditorSessionState& state, const std::string& subject)
{
    return ids_json(state.selection(subject));
}

Json selections_json(const EditorSessionState& state)
{
    Json out = Json::array();
    for (const auto& [subject, ids] : state.selections()) // std::map => stable, sorted order
        out.push_back(selection_entry_json(subject, ids));
    return out;
}

Json selections_json(const EditorSessionState& state, const std::string& subject)
{
    Json out = Json::array();
    const std::vector<std::string>& ids = state.selection(subject);
    if (!ids.empty()) // a FILTER of the unnarrowed array, so an unselected subject answers []
        out.push_back(selection_entry_json(subject, ids));
    return out;
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
    Json focus = Json::object();
    focus.set("subject", Json(selection_focus_));

    Json doc = Json::object();
    doc.set("version", Json(static_cast<std::int64_t>(kSessionFileVersion)));
    doc.set("selections", selections_json(*this));
    doc.set("selectionFocus", std::move(focus));
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

    // The ids of one selection entry: an array of strings, de-duplicated in first-mention order —
    // the same set semantics `apply_selection` enforces in memory, applied once for BOTH the v2
    // `selections[]` reader and the v1 migration below.
    const auto read_ids = [](const Json& array, std::vector<std::string>& out) {
        if (!array.is_array())
            return false;
        for (std::size_t i = 0; i < array.size(); ++i)
        {
            if (!array.at(i).is_string())
                return false;
            const std::string& id = array.at(i).as_string();
            if (std::find(out.begin(), out.end(), id) == out.end())
                out.push_back(id);
        }
        return true;
    };

    std::map<std::string, std::vector<std::string>> selections;
    if (doc.contains("selections"))
    {
        // v2: an ARRAY of objects carrying their key (L-33), never map-keyed.
        const Json& arr = doc.at("selections");
        if (!arr.is_array())
            return false;
        for (std::size_t i = 0; i < arr.size(); ++i)
        {
            const Json& entry = arr.at(i);
            if (!entry.is_object() || !entry.contains("subject") ||
                !entry.at("subject").is_string() || entry.at("subject").as_string().empty())
                return false;
            // A document saying two different things about ONE subject is not readable — refusing it
            // is the same discipline the rest of this loader applies to a wrong type. INSERTION
            // ITSELF is the record of what the document mentioned, which is why an empty entry is
            // inserted here rather than skipped: skipping it would let `[{file, []}, {file, [a]}]`
            // through, accepted because the first thing it said was "nothing". The empties are
            // pruned once, below.
            auto [slot, inserted] =
                selections.emplace(entry.at("subject").as_string(), std::vector<std::string>{});
            if (!inserted)
                return false;
            if (entry.contains("ids") && !read_ids(entry.at("ids"), slot->second))
                return false;
        }
        // The subject VOCABULARY is deliberately NOT validated here (see the header): a session file
        // can outlive the package that declared its subject, and refusing the document would
        // quarantine the cameras with it. The WIRE is where an undeclared subject is refused.
    }
    else if (doc.contains("selection"))
    {
        // v1 -> v2 MIGRATION (08 §3). `selection: {ids}` was the ENTITY selection and nothing else,
        // so it maps losslessly onto one `selections` entry. Without this branch a v1 file would be
        // accepted with its selection silently dropped — no quarantine, no diagnostic, which on a
        // user's persisted session is the worse of the two failures.
        const Json& sel = doc.at("selection");
        if (!sel.is_object())
            return false;
        std::vector<std::string> ids;
        if (sel.contains("ids") && !read_ids(sel.at("ids"), ids))
            return false;
        selections.emplace(kSelectionSubjectEntity, std::move(ids));
    }

    // Both readers above insert every subject their document MENTIONED, empty ones included, so that
    // insertion could serve as the duplicate-subject check. Narrow to the LIVE selections here, once,
    // giving the restored map the same invariant `apply_selection` maintains in memory: "is this
    // subject in the map" and "does this subject have a selection" are the same question.
    for (auto it = selections.begin(); it != selections.end();)
    {
        if (it->second.empty())
            it = selections.erase(it);
        else
            ++it;
    }

    // The D3 focus. Absent => the boot default (`entity`), which is also what a migrated v1 document
    // gets: v1 had exactly one selection, so `entity` is not a guess there.
    std::string focus = kSelectionSubjectEntity;
    if (doc.contains("selectionFocus"))
    {
        const Json& node = doc.at("selectionFocus");
        if (!node.is_object() || !node.contains("subject") || !node.at("subject").is_string() ||
            node.at("subject").as_string().empty())
            return false;
        focus = node.at("subject").as_string();
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

    selections_ = std::move(selections);
    selection_focus_ = std::move(focus);
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
