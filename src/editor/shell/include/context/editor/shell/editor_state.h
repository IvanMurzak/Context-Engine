// `.editor/editor-state.json` — the editor app's session file, and the Shell is its SINGLE WRITER
// (design 03 §1, the L-20 ownership split refined by review C-F3).
//
// The split this implements, and why one writer per file is the whole point:
//
//   * the DAEMON is the single writer of `.editor/session.json` (daemon session state — selection,
//     camera, play state);
//   * the SHELL is the single writer of `.editor/editor-state.json` (dock layout, WINDOW PLACEMENT,
//     panel state blobs, the undo journal, the editor presence marker).
//
// Two processes writing one file is a torn write nobody detects until a user's layout comes back
// mangled. Splitting by file means neither ever has to coordinate with the other.
//
// Two properties beyond "serialize a struct":
//
//   * DEBOUNCED. A window drag emits a placement change per mouse-move. Writing each one would put
//     a synchronous file write in the input path; instead a change marks the store dirty and the
//     write happens once the quiet period has elapsed. The clock is passed IN, never read here, so
//     the debounce is deterministically testable rather than sleep-driven.
//   * CRASH-SAFE. The write stages into a sibling temp file and RENAMES it over the target, so a
//     crash mid-write leaves either the old complete file or the new one — never a half-written
//     document that fails to parse on the next boot and loses the user's layout.
//
// The atomic write is implemented here rather than reused from `filesync::atomic_write`
// deliberately: the Shell is an ORDINARY CLIENT (D10) and links the published client surface only.
// Pulling in context_filesync for twenty lines of rename would drag a kernel-internal module across
// the boundary the `editor-boundary` CI job exists to defend.

#pragma once

#include "context/editor/contract/json.h"
#include "context/render/rhi.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace context::editor::shell
{

// Where a window was, well enough to put it back: which monitor, the restored rect, and whether it
// was maximized. The RESTORED rect is stored even while maximized — restoring a maximized window
// with no restore rect leaves it stuck full-screen the first time the user un-maximizes it.
struct WindowPlacement
{
    // A stable monitor identity (the OS device name). Empty == "wherever the OS puts it", which is
    // also the correct fallback when the remembered monitor is no longer attached.
    std::string monitor;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t width = 1280;
    std::uint32_t height = 800;
    bool maximized = false;

    [[nodiscard]] render::Extent2D size() const { return render::Extent2D{width, height}; }
    [[nodiscard]] bool operator==(const WindowPlacement& other) const;
    [[nodiscard]] bool operator!=(const WindowPlacement& other) const { return !(*this == other); }
};

// The document. `windows` is ordered: index 0 is the window that hosts the app menu + welcome
// screen (D13). `layout` and `panels` are opaque blobs the editor-core owns — the Shell round-trips
// them without interpreting them, which is what lets editor-core evolve its layout format without a
// Shell change.
struct EditorState
{
    std::vector<WindowPlacement> windows;
    contract::Json layout;
    contract::Json panels;

    [[nodiscard]] contract::Json to_json() const;
    // Parse, tolerating a partially-written or older document: every field is optional and a bad
    // one falls back to its default rather than failing the load. A session file that will not load
    // is a user losing their layout, so it degrades instead of refusing.
    [[nodiscard]] static EditorState from_json(const contract::Json& json);
};

// The path this store owns, relative to a project root: `<project>/.editor/editor-state.json`.
[[nodiscard]] std::filesystem::path editor_state_path(const std::filesystem::path& project_root);

// The debounced, crash-safe writer. Times are microseconds on a monotonic clock the CALLER supplies.
class EditorStateStore
{
public:
    // The quiet period a change waits out before it is written. Long enough that a window drag is
    // one write, short enough that a crash right after the user stops moving loses nothing they
    // would notice.
    static constexpr std::uint64_t kDefaultDebounceUs = 500'000; // 500 ms

    explicit EditorStateStore(std::filesystem::path project_root,
                              std::uint64_t debounce_us = kDefaultDebounceUs);

    // Load the on-disk document, or a defaulted one when the file is absent/unreadable/malformed.
    // `loaded_existing` reports which happened — a caller that wants to know whether it is booting
    // a fresh project should not have to guess from the contents.
    const EditorState& load(bool* loaded_existing = nullptr);

    [[nodiscard]] const EditorState& state() const { return state_; }

    // Record a placement for window `index`, growing the vector as needed. A placement IDENTICAL to
    // the stored one does NOT dirty the store: a window that merely repainted would otherwise
    // schedule a write, and at 60 Hz that is a file write per second forever.
    void set_placement(std::size_t index, const WindowPlacement& placement, std::uint64_t now_us);

    // Record an editor-core blob. Same identical-value rule as set_placement.
    void set_layout(contract::Json layout, std::uint64_t now_us);
    void set_panels(contract::Json panels, std::uint64_t now_us);

    // Write if a change is dirty AND the quiet period has elapsed. Returns true when it wrote.
    bool flush_if_due(std::uint64_t now_us);
    // Write unconditionally if dirty, ignoring the debounce — the clean-shutdown path, where waiting
    // out a quiet period would just lose the last change.
    bool flush_now();

    [[nodiscard]] bool dirty() const { return dirty_; }
    [[nodiscard]] int write_count() const { return write_count_; }
    // The reason the last write failed (empty when it succeeded). A failed session write is
    // REPORTED, never fatal: the editor keeps running on a read-only or full disk.
    [[nodiscard]] const std::string& last_error() const { return last_error_; }
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    bool write();

    std::filesystem::path project_root_;
    std::filesystem::path path_;
    std::uint64_t debounce_us_;
    EditorState state_;
    std::uint64_t dirty_since_us_ = 0;
    bool dirty_ = false;
    int write_count_ = 0;
    std::string last_error_;
};

} // namespace context::editor::shell
