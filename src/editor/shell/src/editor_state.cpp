// `.editor/editor-state.json` — see editor_state.h for the single-writer split, the debounce, and
// why the atomic write is implemented here rather than pulled from filesync.

#include "context/editor/shell/editor_state.h"

#include "json_number_read.h" // the shared range-guarded numeric read (float-cast-overflow UB guard)

#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

namespace context::editor::shell
{
namespace
{

using contract::Json;

// Drawn ONCE per process. Used to make the atomic write's staging file process-unique — see the
// call site for the cross-process corruption window a fixed `.tmp` name leaves open. random_device
// rather than a pid: this file is portable core, and a pid would need the one platform #ifdef the
// module deliberately confines to win32_window.cpp.
[[nodiscard]] const std::string& staging_token()
{
    static const std::string token = [] {
        std::random_device source;
        std::ostringstream out;
        out << std::hex << source() << source();
        return out.str();
    }();
    return token;
}

// Both readers route through detail::number_in_range (json_number_read.h): the range check runs on
// the DOUBLE before any integral cast, because `as_int()` on an out-of-int64-range double (a
// hand-edited `1e300` placement — this file is on-disk, corruptible input) is UB the blocking
// `sanitize` leg reports as float-cast-overflow. This unifies the previously per-site (and here
// previously MISSING) guard with editor_state_bridge.cpp's read_pixel (M9 e05d3 inherited fix).

[[nodiscard]] std::int32_t read_i32(const Json& obj, const char* key, std::int32_t fallback)
{
    const std::optional<double> raw =
        detail::number_in_range(obj, key, -2147483648.0, 2147483647.0);
    return raw.has_value() ? static_cast<std::int32_t>(*raw) : fallback;
}

// A negative extent in a hand-edited or corrupted document would wrap to an enormous unsigned one
// and be handed to the swapchain; the defaulted value is the honest read.
[[nodiscard]] std::uint32_t read_u32(const Json& obj, const char* key, std::uint32_t fallback)
{
    const std::optional<double> raw = detail::number_in_range(obj, key, 0.0, 4294967295.0);
    return raw.has_value() ? static_cast<std::uint32_t>(*raw) : fallback;
}

[[nodiscard]] WindowPlacement placement_from_json(const Json& obj)
{
    WindowPlacement placement;
    if (!obj.is_object())
    {
        return placement;
    }
    if (obj.at("monitor").is_string())
    {
        placement.monitor = obj.at("monitor").as_string();
    }
    placement.x = read_i32(obj, "x", placement.x);
    placement.y = read_i32(obj, "y", placement.y);
    placement.width = read_u32(obj, "width", placement.width);
    placement.height = read_u32(obj, "height", placement.height);
    placement.maximized = obj.at("maximized").as_bool();
    return placement;
}

[[nodiscard]] Json placement_to_json(const WindowPlacement& placement)
{
    Json obj = Json::object();
    obj.set("monitor", Json(placement.monitor));
    obj.set("x", Json(static_cast<std::int64_t>(placement.x)));
    obj.set("y", Json(static_cast<std::int64_t>(placement.y)));
    obj.set("width", Json(static_cast<std::int64_t>(placement.width)));
    obj.set("height", Json(static_cast<std::int64_t>(placement.height)));
    obj.set("maximized", Json(placement.maximized));
    return obj;
}

// Stage into a sibling temp file, then rename over the target. The rename is the atomic step: a
// crash before it leaves the previous complete document, a crash after it leaves the new one.
// `std::filesystem::rename` replaces an existing destination on both POSIX and Windows.
[[nodiscard]] bool atomic_write_text(const std::filesystem::path& target, const std::string& text,
                                     std::string& error)
{
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec)
    {
        error = "could not create " + target.parent_path().string() + ": " + ec.message();
        return false;
    }

    // The staging name carries a PROCESS-UNIQUE token. A fixed `<target>.tmp` is a shared name, so
    // two context_editor processes opened on one project root truncate the SAME staging file and one
    // can rename the other's half-written bytes over the target — turning the atomic write into a
    // corruption window. The single-writer invariant this file documents holds per process; it does
    // not make a fixed staging name safe.
    std::filesystem::path temp = target;
    temp += ".tmp." + staging_token();
    {
        // std::ios::binary so the bytes on disk are exactly what was serialized: without it the
        // Windows CRT translates every '\n' into "\r\n", and the file a POSIX box wrote and the
        // file a Windows box wrote would differ byte-for-byte for identical state.
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            error = "could not open " + temp.string() + " for writing";
            return false;
        }
        out << text;
        out.flush();
        if (!out)
        {
            error = "could not write " + temp.string();
            // Do NOT leave the partial temp behind to be renamed by a later attempt.
            std::error_code remove_ec;
            std::filesystem::remove(temp, remove_ec);
            return false;
        }
    }

    std::filesystem::rename(temp, target, ec);
    if (ec)
    {
        error = "could not replace " + target.string() + ": " + ec.message();
        std::error_code remove_ec;
        std::filesystem::remove(temp, remove_ec);
        return false;
    }
    error.clear();
    return true;
}

} // namespace

bool WindowPlacement::operator==(const WindowPlacement& other) const
{
    return monitor == other.monitor && x == other.x && y == other.y && width == other.width &&
           height == other.height && maximized == other.maximized;
}

Json EditorState::to_json() const
{
    Json doc = Json::object();
    // A version tag from the first byte written: the alternative is inferring the shape from which
    // keys happen to be present, which stops working the moment the format actually changes. This
    // is the value `from_json` guards against (M9 e10d): a document carrying a DIFFERENT version is
    // a foreign build's state and is refused rather than reinterpreted.
    doc.set("version", Json(kEditorStateSchemaVersion));
    Json array = Json::array();
    for (const WindowPlacement& placement : windows)
    {
        array.push_back(placement_to_json(placement));
    }
    doc.set("windows", array);
    doc.set("layout", layout.is_null() ? Json::object() : layout);
    doc.set("panels", panels.is_null() ? Json::object() : panels);
    // The e14b presence marker: emitted ONLY while an editor holds the project. Its ABSENCE from the
    // document is the honest "no editor present" an opener reads, so a cleared marker drops the key
    // entirely rather than writing an empty object.
    if (presence.has_value())
    {
        doc.set("presence", presence->to_json());
    }
    return doc;
}

EditorState EditorState::from_json(const Json& json, std::string* schema_diagnostic)
{
    if (schema_diagnostic != nullptr)
    {
        schema_diagnostic->clear();
    }
    EditorState state;
    if (!json.is_object())
    {
        return state;
    }
    // THE SCHEMA GUARD (M9 e10d, T1). Read the `version` tag FIRST, before any field. A version that
    // is PRESENT and does not equal this build's is a foreign — typically FUTURE — document. Honest
    // degradation forbids both crashing on it AND silently reinterpreting it under this build's
    // field meanings, so a mismatch returns the DEFAULT (null) state plus a diagnostic and reads no
    // further. An ABSENT version is a pre-versioning / partial document and still degrades tolerantly
    // below (it is not a mismatch — the guard fires only on a version that is present and wrong).
    const Json& version = json.at("version");
    if (version.is_number())
    {
        const int found = static_cast<int>(version.as_int());
        if (found != kEditorStateSchemaVersion)
        {
            if (schema_diagnostic != nullptr)
            {
                *schema_diagnostic = "editor-state.json schema version " + std::to_string(found) +
                                     " does not match this build's version " +
                                     std::to_string(kEditorStateSchemaVersion) +
                                     "; refusing to reinterpret it (state reset to empty)";
            }
            return EditorState{};
        }
    }
    const Json& windows = json.at("windows");
    if (windows.is_array())
    {
        for (std::size_t i = 0; i < windows.size(); ++i)
        {
            state.windows.push_back(placement_from_json(windows.at(i)));
        }
    }
    state.layout = json.at("layout");
    state.panels = json.at("panels");
    state.presence = client::PresenceMarker::from_json(json.at("presence"));
    return state;
}

std::filesystem::path editor_state_path(const std::filesystem::path& project_root)
{
    return project_root / ".editor" / "editor-state.json";
}

EditorStateStore::EditorStateStore(std::filesystem::path project_root, std::uint64_t debounce_us)
    : project_root_(std::move(project_root)), path_(editor_state_path(project_root_)),
      debounce_us_(debounce_us)
{
}

const EditorState& EditorStateStore::load(bool* loaded_existing)
{
    if (loaded_existing != nullptr)
    {
        *loaded_existing = false;
    }
    schema_diagnostic_.clear();
    std::ifstream in(path_, std::ios::binary);
    if (!in)
    {
        state_ = EditorState{};
        return state_;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    try
    {
        state_ = EditorState::from_json(Json::parse(buffer.str()), &schema_diagnostic_);
        // A SCHEMA MISMATCH (M9 e10d, T1) is NOT a successful load: from_json returned the null
        // state and reported why. `loaded_existing` stays false so a caller does not restore the
        // (empty) layout as if it were the user's, and `schema_diagnostic()` carries the reason so
        // the loss is REPORTED, never silent. An ordinary versioned/legacy document (empty
        // diagnostic) loaded normally.
        if (schema_diagnostic_.empty() && loaded_existing != nullptr)
        {
            *loaded_existing = true;
        }
    }
    catch (const std::exception&)
    {
        // A malformed document degrades to defaults rather than refusing to boot — see the header.
        // `loaded_existing` stays false, which is how a caller distinguishes "fresh" from "salvaged".
        state_ = EditorState{};
    }
    return state_;
}

void EditorStateStore::mark_dirty(std::uint64_t now_us)
{
    if (!dirty_)
    {
        dirty_ = true;
        dirty_since_us_ = now_us;
    }
}

void EditorStateStore::set_placement(std::size_t index, const WindowPlacement& placement,
                                     std::uint64_t now_us)
{
    if (index < state_.windows.size() && state_.windows[index] == placement)
    {
        return; // identical: see the header on why this must not dirty the store
    }
    if (index >= state_.windows.size())
    {
        state_.windows.resize(index + 1);
    }
    state_.windows[index] = placement;
    mark_dirty(now_us);
}

void EditorStateStore::set_layout(Json layout, std::uint64_t now_us)
{
    if (state_.layout.dump() == layout.dump())
    {
        return;
    }
    state_.layout = std::move(layout);
    mark_dirty(now_us);
}

void EditorStateStore::set_panels(Json panels, std::uint64_t now_us)
{
    if (state_.panels.dump() == panels.dump())
    {
        return;
    }
    state_.panels = std::move(panels);
    mark_dirty(now_us);
}

void EditorStateStore::set_presence(const client::PresenceMarker& marker, std::uint64_t now_us)
{
    // Identical (same boot nonce + pid) => no dirty, so a per-frame re-assert is free.
    if (state_.presence.has_value() && state_.presence->boot_nonce == marker.boot_nonce &&
        state_.presence->pid == marker.pid)
    {
        return;
    }
    state_.presence = marker;
    mark_dirty(now_us);
}

void EditorStateStore::clear_presence(std::uint64_t now_us)
{
    if (!state_.presence.has_value())
    {
        return;
    }
    state_.presence.reset();
    mark_dirty(now_us);
}

bool EditorStateStore::flush_if_due(std::uint64_t now_us)
{
    if (!dirty_)
    {
        return false;
    }
    // Subtraction rather than `now < since + debounce` so a monotonic clock near its range end
    // cannot wrap the comparison into "never due".
    if (now_us < dirty_since_us_ || (now_us - dirty_since_us_) < debounce_us_)
    {
        return false;
    }
    return write();
}

bool EditorStateStore::flush_now()
{
    if (!dirty_)
    {
        return false;
    }
    return write();
}

bool EditorStateStore::write()
{
    const std::string text = state_.to_json().dump(2);
    if (!atomic_write_text(path_, text, last_error_))
    {
        // The store stays DIRTY on failure so the next flush retries: a transient full disk or a
        // locked file should not silently drop the user's layout for the rest of the session.
        return false;
    }
    dirty_ = false;
    ++write_count_;
    return true;
}

} // namespace context::editor::shell
