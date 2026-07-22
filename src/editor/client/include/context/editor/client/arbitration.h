// Per-project single-instance arbitration for the editor app (M9 e14b, design 07 §4 / 05 — D15/C-F23):
// a second open of the SAME project FOCUSES the running editor instead of duplicating it; a DIFFERENT
// project spawns a new one. Two cooperating artifacts, split by writer so neither races the other:
//
//   * the PRESENCE MARKER lives inside `<project>/.editor/editor-state.json`. The Shell is that file's
//     SINGLE WRITER (C-F3), so this header only defines the marker SHAPE and a READ-ONLY parse for an
//     opener — it never writes editor-state.json. The Shell embeds the marker via its own state store.
//   * the FOCUS-REQUEST handshake rides a SEPARATE file, `<project>/.editor/focus-request`. An opener
//     WRITES a request there (writing it does NOT touch editor-state.json, so C-F3 is intact) and waits
//     for the running editor to CONSUME it. "Consumed within the timeout" IS the liveness proof: a
//     crashed editor never consumes it, so a stale presence marker resolves to spawn with no separate
//     process-liveness probe. The editor watches the same file from its owner loop and raises its window.
//
// Everything here is boundary-clean (D10): it links only the published contract JSON + std, so it sits
// beside instance.h on the exported context_client surface and both the `context edit .` opener (in the
// CLI) and the Shell reuse ONE implementation of the marker + the handshake.

#pragma once

#include "context/editor/contract/json.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace context::editor::client
{

// The running editor's presence marker, embedded as the `presence` object of editor-state.json. `pid`
// is informational (diagnostics + a future liveness refinement); `boot_nonce` is a random per-boot id
// that distinguishes a fresh editor incarnation from a reused OS pid.
struct PresenceMarker
{
    std::int64_t pid = 0;
    std::string boot_nonce;

    [[nodiscard]] contract::Json to_json() const;
    // nullopt when `obj` is not an object or carries no non-empty `bootNonce` (the one field that makes
    // a marker meaningful) — a torn/partial document therefore reads as "no editor present".
    [[nodiscard]] static std::optional<PresenceMarker> from_json(const contract::Json& obj);
};

// Read the presence marker out of an editor-state.json DOCUMENT text. READ-ONLY — never writes the file
// (C-F3). nullopt when the text is absent/empty/malformed or carries no `presence` marker.
[[nodiscard]] std::optional<PresenceMarker>
parse_presence_from_editor_state(const std::string& editor_state_text);

// The current process id (informational, for a fresh marker or a focus request). Platform-confined to
// the .cpp so this header names no OS type.
[[nodiscard]] std::int64_t current_process_id() noexcept;

// A fresh random per-boot nonce (lowercase hex). Drawn from random_device, like the editor-state
// staging token — portable core, no pid #ifdef in a header.
[[nodiscard]] std::string make_boot_nonce();

// The focus-request handshake file for `project` — `<project>/.editor/focus-request`. A SEPARATE file
// from editor-state.json, so an opener writing it does not violate the Shell's single-writer ownership.
[[nodiscard]] std::filesystem::path focus_request_path(const std::filesystem::path& project);

// One focus request: a unique `nonce` the opener waits to see disappear, plus the requester's pid.
struct FocusRequest
{
    std::string nonce;
    std::int64_t requester_pid = 0;

    [[nodiscard]] std::string encode() const;
    // nullopt when `text` is not a request object with a non-empty `nonce`.
    [[nodiscard]] static std::optional<FocusRequest> decode(const std::string& text);
};

// The arbitration verdict for an OPEN.
enum class OpenAction
{
    focus_existing, // a live editor is present and acknowledged the focus request — do not duplicate
    spawn_new,      // no editor present, or a stale marker that never acknowledged — launch one
};

// The pure decision (unit-tested with no filesystem): focus_existing IFF a presence marker was present
// AND the focus request it left was acknowledged (consumed by a live editor); else spawn_new.
[[nodiscard]] OpenAction decide_open_action(bool marker_present, bool focus_acknowledged) noexcept;

// The full opener-side arbitration for `project`, the coordinator `context edit .` / the file-assoc
// handler run: parse the presence marker; if present, write a focus request and poll up to
// `focus_timeout_ms` for the running editor to consume it. Never throws.
struct OpenArbitration
{
    OpenAction action = OpenAction::spawn_new;
    bool marker_present = false;
    bool focus_acknowledged = false;
    std::int64_t existing_pid = 0; // the marker's pid, when one was present
};
[[nodiscard]] OpenArbitration arbitrate_open(const std::filesystem::path& project,
                                             int focus_timeout_ms);

// The editor side (driven from the Shell's owner loop): poll the focus-request file and, when a NEW
// request appears, CONSUME it (delete the file) so the opener's poll unblocks. Returns true exactly on
// the poll that consumed a request — the caller raises its window then. Idempotent; cross-platform
// (a file poll, no OS IPC primitive), so the whole handshake is a deterministic ctest.
class FocusRequestWatcher
{
public:
    explicit FocusRequestWatcher(std::filesystem::path project);

    // true exactly when this call consumed a pending focus request (the caller should activate its
    // window); false when nothing was pending or the request was a torn/partial write (retried next
    // poll). A request whose nonce matches the last one consumed is ignored (the writer re-observing
    // its own not-yet-deleted file cannot double-fire).
    [[nodiscard]] bool poll();

    [[nodiscard]] int served() const noexcept { return served_; }
    [[nodiscard]] const std::string& last_nonce() const noexcept { return last_nonce_; }

private:
    std::filesystem::path path_;
    std::string last_nonce_;
    int served_ = 0;
};

} // namespace context::editor::client
