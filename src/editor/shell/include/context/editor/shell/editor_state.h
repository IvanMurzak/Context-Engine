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
//
// A third property since M9 e09d:
//
//   * DISPOSABLE BY CONTRACT, LOUDLY (07 §6). This file is session state, so a document that will
//     not load must never block the boot — but it must never be reset SILENTLY either. It is renamed
//     ASIDE to `.editor/editor-state.corrupt[-N].json`, defaults are loaded, and `load()` hands the
//     caller a report to announce. The daemon does exactly this for its half of the split
//     (`editorkernel::restore_session_state` -> `.editor/session.corrupt[-N].json`, announced on the
//     `diagnostics` topic + stderr under `editor.session_state_invalid`); this is the Shell's half,
//     and the two files now recover the same way because they are the same KIND of file.

#pragma once

#include "context/editor/client/arbitration.h" // the D15/C-F23 editor presence marker (e14b)
#include "context/editor/contract/json.h"
#include "context/render/rhi.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace context::editor::shell
{

// The schema version this build writes and understands (M9 e10d). It is written as the document's
// `version` tag by `to_json` and CHECKED by `from_json`: a document whose `version` is PRESENT and
// does not equal this is a FUTURE (or otherwise foreign) build's state, which `from_json` refuses to
// reinterpret — it degrades to a null state and reports a diagnostic instead (honest degradation, T1).
// A document with NO `version` at all is a pre-versioning / partial write and is still read
// tolerantly, exactly as before: the guard fires only on a version that is present AND wrong, never
// on its absence, so a hand-truncated or legacy document keeps degrading to defaults rather than
// being rejected wholesale.
//
// ⚠ AN ADDITIVE OPTIONAL MEMBER DOES NOT BUMP THIS. The guard is symmetric — it refuses a document
// whose version differs in EITHER direction — so bumping for a member that older builds simply
// ignore and newer builds default would make every already-written document on every user's disk
// unreadable, losing exactly the layout this file exists to preserve. `presence` (e14b) and `undo`
// (e09c) were both added at version 1 for this reason. Bump only for a change that would be
// MISREAD under the old field meanings.
inline constexpr int kEditorStateSchemaVersion = 1;

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
    // The M9 e09c SESSION UNDO JOURNAL, as an opaque blob — the third thing 03 §1 names as the
    // Shell's to own in this file, and the one that had no home until e09c (`undo_journal.h`'s
    // to_json/load_json were called by no host at all).
    //
    // WHY A STRING AND NOT A NESTED OBJECT. The journal's own DOM is `serializer::JsonValue` (the
    // engine's authored-value type), not `contract::Json`, and the two disagree on number identity:
    // contract::Json holds every number as a `double`, so a u64 field value that survives a canonical
    // round-trip today would come back rounded through a nested-object conversion. The journal
    // therefore rides here as its CANONICAL serialization (R-FILE-001, `serialize_canonical` — the
    // engine's one value identity), and the Shell round-trips those bytes without interpreting them,
    // exactly as it does `layout` / `panels`. That is also why this is not a second serializer: the
    // ONE journal serializer stays `UndoJournal::to_json`, and this member is its transport.
    //
    // Absent / empty string == no journal, which is the honest state for a fresh project.
    contract::Json undo;
    // The D15/C-F23 editor presence marker (e14b): set while THIS editor process holds the project,
    // cleared on clean exit. An opener (`context edit .`) reads it to decide focus-vs-spawn. Because the
    // Shell is this file's SINGLE WRITER (C-F3), the marker is written ONLY through this store — an
    // opener never writes it, it only reads the serialized document.
    std::optional<client::PresenceMarker> presence;

    [[nodiscard]] contract::Json to_json() const;
    // Parse, tolerating a partially-written or older document: every field is optional and a bad
    // one falls back to its default rather than failing the load. A session file that will not load
    // is a user losing their layout, so it degrades instead of refusing.
    //
    // THE SCHEMA GUARD (M9 e10d, T1). A document whose `version` is PRESENT and does NOT equal
    // `kEditorStateSchemaVersion` is a foreign (typically FUTURE) build's state. It is NEITHER
    // crashed on NOR silently reinterpreted: this returns a DEFAULT (null) `EditorState` and, when
    // `schema_diagnostic` is non-null, writes a human-readable reason into it (found-vs-supported).
    // An ABSENT `version` is not a mismatch — it degrades tolerantly as described above. A caller
    // that wants to tell "null because a future build wrote this" from "null because fresh/malformed"
    // passes a `schema_diagnostic` and checks whether it came back non-empty.
    [[nodiscard]] static EditorState from_json(const contract::Json& json,
                                               std::string* schema_diagnostic = nullptr);
};

// The path this store owns, relative to a project root: `<project>/.editor/editor-state.json`.
[[nodiscard]] std::filesystem::path editor_state_path(const std::filesystem::path& project_root);

// Where a corrupt document is moved aside to (M9 e09d, 07 §6): `editor-state.corrupt.json`, then
// `-1`, `-2`, … next to the file it replaced. Exposed so a test — and a human reading a bug report —
// can name the quarantine without re-deriving the convention. `n == 0` is the unsuffixed name.
[[nodiscard]] std::filesystem::path editor_state_quarantine_path(
    const std::filesystem::path& project_root, int n = 0);

// The LOUD corrupt-recovery diagnostic code for `.editor/editor-state.json` (07 §6, M9 e09d). Owned
// HERE as a string constant — the promote-a-local-string pattern of
// editorkernel::kEditorSessionStateInvalidCode / bridge::kAttachDeniedCode — and registered with the
// SAME string by src/editor/contract/src/error_catalog.cpp (append-only tail), asserted by the
// catalog test. Deliberately its OWN code, NOT the daemon's `editor.session_state_invalid`: the two
// files have two different owners, and a reader who cannot tell WHICH session file was reset from
// the diagnostic has lost the only thing the ownership split was for.
inline constexpr const char* kEditorStateInvalidCode = "editor.editor_state_invalid";

// The ceiling on a document `load()` will read into memory. Named here rather than buried in the
// .cpp to match every sibling reader in this module (`kMaxUserConfigBytes`, `kMaxKeybindingsBytes`,
// `kMaxThemeBytes`, `kMaxReleaseBodyBytes`), and generous on purpose: since e09c this file carries
// the session undo journal, so it is the one editor-state document with a real growth path. The cap
// exists to stop an unbounded slurp throwing `bad_alloc` out of a CONSTRUCTOR on the boot path, not
// to police size.
inline constexpr std::uintmax_t kMaxEditorStateBytes = 64u * 1024u * 1024u;

// How a `load()` went. `recovered` is the LOUD path (07 §6): the file existed but was
// unreadable / malformed / structurally wrong / written by a foreign build, so it was renamed aside
// and the defaults were loaded.
enum class EditorStateRestoreOutcome
{
    fresh,     // no file on disk — a first boot on this project (not an error)
    restored,  // the document parsed and was adopted
    recovered, // the document was unusable: quarantined, defaults loaded (report it LOUDLY)
};

// What `load()` reports, member-for-member the shape the daemon's `SessionRestoreReport` carries for
// `.editor/session.json` — deliberately, so the two halves of the split are announced identically.
struct EditorStateRestoreReport
{
    EditorStateRestoreOutcome outcome = EditorStateRestoreOutcome::fresh;
    std::string path; // the file the store looked at, as an ABSOLUTE native path (for the operator
                      // channel: stderr, where the reader may have no project context at all)
    // The SAME document as `path`, project-relative and slash-separated — what a Problems-panel row
    // must carry in its payload's `file` member. That member is the one the panel RENDERS and GROUPS
    // BY, and every other row in it is project-relative (merge_command.cpp, the panel's own tests),
    // so an absolute `C:\…\.editor\editor-state.json` group header would be the one odd entry in the
    // list. Derived lexically, so it costs no filesystem call and cannot throw.
    std::string project_relative_path;
    std::string quarantined_path; // where the unusable document was preserved (empty unless
                                  // `recovered`, and ALSO empty when NEITHER the rename nor the
                                  // copy fallback could preserve it — see `detail` and
                                  // `preservation_failed`)
    std::string detail;           // human-readable reason (empty unless `recovered`)
    // A named shorthand for `outcome == recovered && quarantined_path.empty()`, promoted to a member
    // so no caller has to re-derive the rule — because this is the rule that GATES `write()`. It
    // means: the document was unusable, nothing could be saved, and the bytes still on disk are
    // therefore the user's only copy. The store refuses to write while it holds.
    bool preservation_failed = false;
};

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
    // a fresh project should not have to guess from the contents. A SCHEMA MISMATCH (a future
    // build's `version`) also loads a defaulted document with `*loaded_existing == false`, but is
    // distinguished by a non-empty `schema_diagnostic()` (M9 e10d, T1): the layout is not lost
    // SILENTLY — the reason is reported and the state is null rather than reinterpreted.
    //
    // ⚠ NEVER THROWS AND NEVER BLOCKS (07 §6). The whole classify path — including the slurp, which
    // is SIZE-CAPPED first so a runaway document cannot throw `bad_alloc` out of a constructor and
    // take the boot with it — is exception-guarded.
    //
    // Since M9 e09d a document that cannot be adopted is also PRESERVED before the defaults are
    // taken — see `restore_report()`. That is why "reported, not silent" is more than a log line: a
    // user who lost their layout still HAS the bytes. And preservation is a PRECONDITION, not a
    // best effort: if neither the rename nor the copy could save them, the store refuses to write
    // at all rather than let the next flush destroy the only copy — see
    // `restore_report().preservation_failed`.
    const EditorState& load(bool* loaded_existing = nullptr);

    [[nodiscard]] const EditorState& state() const { return state_; }

    // Record a placement for window `index`, growing the vector as needed. A placement IDENTICAL to
    // the stored one does NOT dirty the store: a window that merely repainted would otherwise
    // schedule a write, and at 60 Hz that is a file write per second forever.
    void set_placement(std::size_t index, const WindowPlacement& placement, std::uint64_t now_us);

    // Record an editor-core blob. Same identical-value rule as set_placement.
    void set_layout(contract::Json layout, std::uint64_t now_us);
    void set_panels(contract::Json panels, std::uint64_t now_us);

    // Record the M9 e09c session undo journal blob (see EditorState::undo). Same identical-value
    // rule: a journal that has not moved does NOT dirty the store, so an owner loop may re-offer it
    // every frame for free.
    //
    // THIS IS THE ONE SHELL-SIDE SEAM THE JOURNAL REACHES THE FILE THROUGH (C-F3 / e09d). The journal
    // itself lives in `context_editor_panels` and owns no disk; every write of it funnels here, into
    // the store the Shell is the single writer of. There is deliberately no second call site and no
    // second path — which is what lets e09d assert the single-writer split structurally rather than
    // by inspection.
    void set_undo(contract::Json undo, std::uint64_t now_us);

    // Set / clear the D15/C-F23 editor presence marker (e14b). Same identical-value rule: setting the
    // marker already stored, or clearing an already-absent one, does NOT dirty the store. The Shell
    // sets it on boot and clears it on clean exit; a caller flushes immediately (flush_now) rather than
    // waiting out the debounce so a concurrent opener sees the presence without a 500 ms window.
    void set_presence(const client::PresenceMarker& marker, std::uint64_t now_us);
    void clear_presence(std::uint64_t now_us);

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
    // The reason the last `load()` degraded to a null state because the on-disk `version` did not
    // match this build (M9 e10d, T1). Empty when the load succeeded, was fresh, or was merely
    // malformed — a non-empty value is specifically the "a foreign/future build wrote this; refused
    // to reinterpret it" signal, so a caller (and the live restore report) can surface it distinctly
    // from an ordinary fresh boot.
    [[nodiscard]] const std::string& schema_diagnostic() const { return schema_diagnostic_; }
    // How the last `load()` went (M9 e09d, 07 §6). `recovered` is the one the caller must ANNOUNCE:
    // the user's layout and undo history were reset, and the reason plus the quarantine path are the
    // only things that make that survivable. Defaults to `fresh` before the first load.
    [[nodiscard]] const EditorStateRestoreReport& restore_report() const { return restore_report_; }
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    bool write();
    // Start the debounce clock on the FIRST change of a dirty run — a later change inside the same
    // run must not push the deadline out, or a stream of edits would defer the write forever.
    // Every setter routes through here so that rule lives in exactly one place.
    void mark_dirty(std::uint64_t now_us);

    // Why a preservation attempt ended. THREE outcomes, not two: "there was nothing there to save"
    // is emphatically NOT "we failed to save it", and collapsing them makes the store refuse to
    // write over a file that does not exist.
    enum class Preserved
    {
        yes,     // a copy now exists at `restore_report_.quarantined_path`
        nothing, // the document is not on disk at all — nothing to lose, nothing to announce
        failed,  // it IS there and nothing could be saved; those bytes are the only copy
    };

    // Put a copy of the UNUSABLE document somewhere safe, and report where (`note` is OUTPUT-only —
    // the caller appends it, so this never has to be pre-seeded).
    //
    // It tries the RENAME first (it preserves the bytes without needing to read them, so it works
    // even on the document that could not be OPENED) and falls back to a byte COPY, which covers the
    // one case a rename cannot: the source is locked against being moved or deleted — a reader's
    // open handle on Windows, a backup/AV agent — while the directory itself is perfectly writable.
    // That fallback is the whole reason this is a function: `load()` and `write()` both need it, the
    // second so a preservation that failed at boot is RETRIED rather than latched forever.
    Preserved preserve_unusable_document(std::string& note);

    std::filesystem::path project_root_;
    std::filesystem::path path_;
    std::uint64_t debounce_us_;
    EditorState state_;
    std::uint64_t dirty_since_us_ = 0;
    bool dirty_ = false;
    int write_count_ = 0;
    std::string last_error_;
    std::string schema_diagnostic_;
    EditorStateRestoreReport restore_report_;
};

} // namespace context::editor::shell
