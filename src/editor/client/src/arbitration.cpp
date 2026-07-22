// Per-project single-instance arbitration (see arbitration.h): the presence marker read, the
// focus-request handshake, and the pure open decision.

#include "context/editor/client/arbitration.h"

#include <chrono>
#include <fstream>
#include <random>
#include <sstream>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#include <process.h> // _getpid
#else
#include <unistd.h> // getpid
#endif

namespace fs = std::filesystem;

namespace context::editor::client
{

using contract::Json;

namespace
{

std::string read_file(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return std::string();
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Stage into a process-unique sibling temp file, then rename over the target — the same crash-safe
// atomic-replace the Shell's editor-state store uses, so a concurrent reader sees either the old file
// or the whole new one, never a half-written request. A fixed `.tmp` name would let two openers
// truncate the same staging file, so the name carries a random token.
[[nodiscard]] bool atomic_write_text(const fs::path& target, const std::string& text)
{
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    if (ec)
        return false;

    std::random_device source;
    std::ostringstream token;
    token << std::hex << source() << source();
    fs::path temp = target;
    temp += ".tmp." + token.str();
    {
        // std::ios::binary so the on-disk bytes are exactly what was serialized (no CRT '\n'->"\r\n"
        // translation), matching the editor-state writer and keeping POSIX/Windows byte-identical.
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out << text;
        out.flush();
        if (!out)
        {
            std::error_code rm;
            fs::remove(temp, rm);
            return false;
        }
    }
    fs::rename(temp, target, ec);
    if (ec)
    {
        std::error_code rm;
        fs::remove(temp, rm);
        return false;
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------- presence marker

Json PresenceMarker::to_json() const
{
    Json obj = Json::object();
    obj.set("pid", Json(pid));
    obj.set("bootNonce", Json(boot_nonce));
    return obj;
}

std::optional<PresenceMarker> PresenceMarker::from_json(const Json& obj)
{
    if (!obj.is_object())
        return std::nullopt;
    const Json& nonce = obj.at("bootNonce");
    if (!nonce.is_string() || nonce.as_string().empty())
        return std::nullopt; // no meaningful marker
    PresenceMarker marker;
    marker.boot_nonce = nonce.as_string();
    marker.pid = obj.at("pid").as_int(); // 0 on a missing/non-numeric field
    return marker;
}

std::optional<PresenceMarker> parse_presence_from_editor_state(const std::string& editor_state_text)
{
    if (editor_state_text.empty())
        return std::nullopt;
    Json doc;
    try
    {
        doc = Json::parse(editor_state_text);
    }
    catch (const std::exception&)
    {
        return std::nullopt; // a torn/malformed document reads as "no editor present"
    }
    if (!doc.is_object())
        return std::nullopt;
    return PresenceMarker::from_json(doc.at("presence"));
}

std::int64_t current_process_id() noexcept
{
#if defined(_WIN32)
    return static_cast<std::int64_t>(_getpid());
#else
    return static_cast<std::int64_t>(::getpid());
#endif
}

std::string make_boot_nonce()
{
    std::random_device source;
    std::ostringstream out;
    out << std::hex << source() << source();
    return out.str();
}

// ---------------------------------------------------------------------------- focus request

fs::path focus_request_path(const fs::path& project)
{
    return project / ".editor" / "focus-request";
}

std::string FocusRequest::encode() const
{
    Json obj = Json::object();
    obj.set("nonce", Json(nonce));
    obj.set("requesterPid", Json(requester_pid));
    return obj.dump();
}

std::optional<FocusRequest> FocusRequest::decode(const std::string& text)
{
    if (text.empty())
        return std::nullopt;
    Json doc;
    try
    {
        doc = Json::parse(text);
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
    if (!doc.is_object())
        return std::nullopt;
    const Json& nonce = doc.at("nonce");
    if (!nonce.is_string() || nonce.as_string().empty())
        return std::nullopt;
    FocusRequest request;
    request.nonce = nonce.as_string();
    request.requester_pid = doc.at("requesterPid").as_int();
    return request;
}

// ---------------------------------------------------------------------------- the decision

OpenAction decide_open_action(bool marker_present, bool focus_acknowledged) noexcept
{
    return (marker_present && focus_acknowledged) ? OpenAction::focus_existing
                                                  : OpenAction::spawn_new;
}

OpenArbitration arbitrate_open(const fs::path& project, int focus_timeout_ms)
{
    OpenArbitration result;
    const std::optional<PresenceMarker> marker =
        parse_presence_from_editor_state(read_file(project / ".editor" / "editor-state.json"));
    if (!marker.has_value())
    {
        result.action = OpenAction::spawn_new; // no editor present
        return result;
    }
    result.marker_present = true;
    result.existing_pid = marker->pid;

    // A marker is present — prove it belongs to a LIVE editor by the focus handshake: write a request
    // and wait for the editor's owner loop to consume it. A crashed editor never consumes it, so the
    // wait timing out is exactly "the marker is stale -> spawn".
    FocusRequest request;
    request.nonce = make_boot_nonce();
    request.requester_pid = current_process_id();
    const fs::path request_path = focus_request_path(project);

    bool acknowledged = false;
    if (atomic_write_text(request_path, request.encode()))
    {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(focus_timeout_ms);
        for (;;)
        {
            // Acknowledged when the request file is GONE (the editor consumed it) or a DIFFERENT request
            // replaced ours (another opener is being served concurrently — an editor is clearly live).
            const std::optional<FocusRequest> current = FocusRequest::decode(read_file(request_path));
            if (!current.has_value() || current->nonce != request.nonce)
            {
                acknowledged = true;
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        // On a timeout, retract OUR still-pending request so it does not later focus an editor that was
        // spawned in the meantime. Only remove it if it is still ours (a live editor may consume it
        // between the loop exit and here — that is fine, remove() is a no-op then).
        if (!acknowledged)
        {
            const std::optional<FocusRequest> current = FocusRequest::decode(read_file(request_path));
            if (current.has_value() && current->nonce == request.nonce)
            {
                std::error_code rm;
                fs::remove(request_path, rm);
            }
        }
    }

    result.focus_acknowledged = acknowledged;
    result.action = decide_open_action(result.marker_present, acknowledged);
    return result;
}

// ---------------------------------------------------------------------------- watcher (editor side)

FocusRequestWatcher::FocusRequestWatcher(fs::path project)
    : path_(focus_request_path(project))
{
}

bool FocusRequestWatcher::poll()
{
    const std::optional<FocusRequest> request = FocusRequest::decode(read_file(path_));
    if (!request.has_value())
        return false; // nothing pending, or a torn write (retried next poll)
    if (request->nonce == last_nonce_)
        return false; // already served this one (the delete below not yet observed)

    // Consume it: delete the file so the opener's poll unblocks. If the delete fails (a racing writer
    // held it), report NOT served this poll and retry — never double-count.
    std::error_code ec;
    fs::remove(path_, ec);
    if (ec)
        return false;
    last_nonce_ = request->nonce;
    ++served_;
    return true;
}

} // namespace context::editor::client
