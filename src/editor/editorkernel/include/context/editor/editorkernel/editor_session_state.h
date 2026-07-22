// The DAEMON-side editor session state (M9 e08a, design 05 §4 / D7 tier 1): the semantic human
// state — SELECTION, CAMERAS, PLAY STATE — promoted out of the GUI panels' private members and into
// the daemon, so every client (a second window, the CLI, a scripted agent) sees and can drive what
// the human sees over the ONE contract surface.
//
// In-memory per L-20 (this is NOT authored data — no file kind, no derivation, no schema), with one
// convenience projection onto disk: `.editor/session.json`, written by the DAEMON on clean shutdown
// and restored on the next boot (03 §1 makes the daemon the SINGLE writer of that file; the Shell
// owns `config.json` / the layout, never this). A corrupt file is renamed aside and defaults are
// loaded LOUDLY (07 §6) — recovery is never silent and never blocking.
//
// PLAY-STATE semantics mirror the M5-F5 playbar state machine EXACTLY (L-51 edit/play provenance:
// `edit` is authored truth with no live session; `playing`/`paused` run a live session whose
// mutations are discarded on stop) and reuse its reserved `play.*` error codes, so e08b can rewire
// gui::playbar::PlaybarModel onto this state with no semantic translation. The ONE deliberate
// refinement is at the reply boundary: the playbar signals a benign no-op as `ok=false` with NO
// error code, which is not expressible in an R-CLI-008 envelope (a failure MUST carry a catalog
// code) — so a benign no-op here is `ok=true, changed=false` and a real refusal is
// `ok=false, error_code=play.not_running`. The mapping is lossless: the playbar's `ok` is this
// struct's `changed`.
//
// Play state is deliberately NOT persisted: a restarted daemon holds no live session, so restoring
// `playing` would be a lie about L-51 provenance. Boot is always `edit`.

#pragma once

#include "context/editor/contract/json.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace context::editor::editorkernel
{

// The LOUD corrupt-recovery diagnostic for `.editor/session.json` (07 §6). Owned HERE as a string
// constant — the promote-a-local-string pattern of bridge::kAttachDeniedCode /
// gui::playbar::kPlay*Code — and registered with the SAME string by
// src/editor/contract/src/error_catalog.cpp (append-only tail), asserted by the catalog test.
// Deliberately its own code, NOT the R-QA-005 `session.state_invalid` of the deterministic
// `session *` FILE-harness family: C-F4 keeps those two families distinct.
inline constexpr const char* kEditorSessionStateInvalidCode = "editor.session_state_invalid";

// How an `editor select` applies its ids against the current selection. The token strings are the
// wire contract (registry.cpp documents them on the verb's `mode` param).
enum class SelectionMode
{
    replace, // the ids BECOME the selection (the default)
    add,     // union
    toggle,  // present ids are removed, absent ids are added
    remove,  // set difference
};

// Parse a wire `mode` token; nullopt for an unknown token (the caller answers usage.invalid rather
// than silently falling back to `replace` — a silent fallback would mutate more than the caller asked).
[[nodiscard]] std::optional<SelectionMode> parse_selection_mode(const std::string& token);
[[nodiscard]] const char* selection_mode_token(SelectionMode mode);

// The L-51 edit/play provenance state. Token-for-token identical to
// gui::playbar::PlayState / state_token() — the indicator the playbar renders is fed from the
// `play-state` topic event carrying exactly these tokens (the e08a DoD's "L-51 indicator is fed").
enum class EditorPlayState
{
    edit,
    playing,
    paused,
};

[[nodiscard]] const char* play_state_token(EditorPlayState state);

// One viewport's camera. `transform` / `projection` are carried as OPAQUE JSON: the daemon is the
// custodian of the human's camera, not its interpreter — the renderer/viewport owns the meaning, and
// pinning a struct here would freeze a shape the render path is still free to evolve.
struct CameraState
{
    contract::Json transform;
    contract::Json projection;
};

// The outcome of a play-control transition (see the header note on the playbar mapping).
struct PlayOutcome
{
    bool ok = true;
    bool changed = false;   // false => a benign, idempotent no-op (no event is published)
    std::string error_code; // empty on ok; else a reserved play.* catalog code
    EditorPlayState state = EditorPlayState::edit;
    std::uint64_t sim_tick = 0;
};

// How a restore from `.editor/session.json` went. `recovered` is the LOUD path: the file existed but
// was unreadable/malformed/structurally wrong, so it was renamed aside and defaults were loaded.
enum class SessionRestoreOutcome
{
    fresh,     // no file on disk — a first boot (not an error)
    restored,  // the file parsed and applied
    recovered, // the file was corrupt: quarantined, defaults loaded (07 §6 — report it LOUDLY)
};

struct SessionRestoreReport
{
    SessionRestoreOutcome outcome = SessionRestoreOutcome::fresh;
    std::string path;             // the session file the daemon looked at
    std::string quarantined_path; // where a corrupt file was moved (empty unless `recovered`)
    std::string detail;           // human-readable reason (empty unless `recovered`)
};

// The daemon's editor session state. Total — every mutator reports by value whether anything
// actually changed, so the caller publishes an event ONLY on a real change (an unchanged re-select
// that still fanned out an event would be an echo generator, which is exactly what `origin` exists
// to prevent).
//
// NOT internally synchronized: the daemon serializes every dispatch through ONE mutex (L-50), so the
// serve loop is the single accessor. A second accessor would need its own lock.
class EditorSessionState
{
public:
    // --- selection (L-35 id-path keys, the same strings the panels already use) -----------------
    [[nodiscard]] const std::vector<std::string>& selection() const noexcept { return selection_; }
    // Apply `ids` under `mode`. Returns true when the resulting selection actually differs.
    bool apply_selection(const std::vector<std::string>& ids, SelectionMode mode);

    // --- cameras (per viewport) ----------------------------------------------------------------
    [[nodiscard]] const std::map<std::string, CameraState>& cameras() const noexcept
    {
        return cameras_;
    }
    // Set (or replace) one viewport's camera. Returns true when the stored camera actually differs.
    bool set_camera(const std::string& viewport_id, contract::Json transform,
                    contract::Json projection);

    // --- play control (L-51; mirrors gui::playbar::PlaybarModel) --------------------------------
    [[nodiscard]] EditorPlayState play_state() const noexcept { return play_; }
    [[nodiscard]] std::uint64_t sim_tick() const noexcept { return sim_tick_; }
    // edit|paused -> playing. Already playing is a benign no-op.
    PlayOutcome play();
    // playing -> paused. In `edit`: play.not_running. Already paused is a benign no-op.
    PlayOutcome pause();
    // playing|paused -> edit, discarding the runtime tick counter (L-51). Idempotent in `edit`.
    PlayOutcome stop();
    // Advance `ticks` fixed ticks (R-SIM-002). In `edit`: play.not_running. Stepping does NOT change
    // playing/paused (you may step from either), exactly like the playbar.
    PlayOutcome step(std::uint64_t ticks);

    // --- the persisted projection ---------------------------------------------------------------
    // The `.editor/session.json` document: {version, selection:{ids[]}, cameras:[{viewportId,…}]}.
    // Cameras are an ARRAY of objects carrying their key, never a map-keyed object — the same
    // encoding discipline the authored-data conventions mandate (L-33), so the file stays diffable
    // and stable-ordered. Play state is deliberately absent (see the header note).
    [[nodiscard]] contract::Json to_json() const;
    // Apply a persisted document. Returns false when the document is structurally wrong (wrong
    // types / not an object) — the caller then treats the file as CORRUPT. A document missing an
    // optional section is not an error (forward/backward tolerance on an additive file).
    bool apply_json(const contract::Json& doc);

private:
    std::vector<std::string> selection_;
    std::map<std::string, CameraState> cameras_;
    EditorPlayState play_ = EditorPlayState::edit;
    std::uint64_t sim_tick_ = 0;
};

// The encoding of the state's two projectable halves — ONE implementation each, deliberately shared
// by the `session` topic facts, the `editor.*-get` replies, and `.editor/session.json` (to_json()
// below). The wire and the file are documented to carry the SAME shape; a second copy of these six
// lines is exactly how that stops being true.
[[nodiscard]] contract::Json selection_ids_json(const EditorSessionState& state);
// Cameras are an ARRAY of objects carrying their key (`{viewportId, transform, projection}`), never
// a map-keyed object — the L-33 encoding discipline, so the file stays diffable and stable-ordered.
[[nodiscard]] contract::Json cameras_json(const EditorSessionState& state);

// The daemon-owned session file for a project root: `<project_root>/.editor/session.json`.
[[nodiscard]] std::filesystem::path session_state_path(const std::filesystem::path& project_root);

// Restore `state` from the project's session file. Never throws and never blocks: a missing file is
// `fresh`, a corrupt one is renamed aside (`<...>/.editor/session.corrupt[-N].json`) and `state` is
// left at its defaults. The report is what the caller announces LOUDLY (07 §6).
SessionRestoreReport restore_session_state(const std::filesystem::path& project_root,
                                           EditorSessionState& state);

// Persist `state` to the project's session file (the clean-shutdown write; daemon = single writer).
// Returns false and fills `error` when the directory or file could not be written.
bool persist_session_state(const std::filesystem::path& project_root,
                           const EditorSessionState& state, std::string& error);

} // namespace context::editor::editorkernel
