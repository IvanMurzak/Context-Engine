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

// --- selection SUBJECTS (c1 / D1 / D2) -----------------------------------------------------------
//
// Selection is TYPED: what the human has selected is always "these ids OF THIS KIND". Selections of
// different subjects COEXIST — selecting a file does not clear the entity selection (the Unreal
// model, D1) — and `selection-focus` (D3) is the arbiter of which live selection the human is
// actually working on.
//
// These three kinds are CONTRACT-OWNED. The vocabulary is deliberately OPEN: a package declares
// `<pkg>.<kind>` in its manifest's `selection.subjects[]` (c2's surface), which is why the state
// stores a subject as a plain string rather than an enum. What this header owns is the DEFAULT
// (`entity`, which is what makes the whole change additive under protocolMajor 1) and the
// contract-owned set the wire accepts until c2 lands its declaration surface.
inline constexpr const char* kSelectionSubjectEntity = "entity";
inline constexpr const char* kSelectionSubjectFile = "file";
inline constexpr const char* kSelectionSubjectAsset = "asset";

// Is `subject` one of the three contract-owned kinds? The wire REFUSES anything else rather than
// coercing it to `entity` — the same reasoning `parse_selection_mode` carries: a silent fallback
// would mutate a different selection than the caller asked for, and unlike a bad mode token that
// mistake is invisible (the caller's own subject stays untouched while another one moves).
//
// ⚠ The PERSISTED file deliberately does NOT apply this check (see `apply_json`): a session file can
// legitimately outlive the package that declared its subject, and refusing the document would
// quarantine the cameras too.
[[nodiscard]] bool is_contract_selection_subject(const std::string& subject);

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

// The outcome of ONE typed `editor select`. `changed` is the no-op verdict the caller publishes
// `selection-changed` on; `focus_changed` is the separate D3 verdict for `selection-focus`. They are
// reported apart because they are two facts on the wire, and a caller that conflated them would
// either publish a focus fact nobody moved or swallow one that moved.
struct SelectionOutcome
{
    bool changed = false;       // the subject's selection actually differs now
    bool focus_changed = false; // ...and the focus moved to this subject (D3)
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
    // --- selection, TYPED per subject (c1 / D1) --------------------------------------------------
    // One subject's ids (L-35 id-path keys for `entity`, the same strings the panels already use;
    // project-relative paths for `file`, and so on — the SUBJECT names the vocabulary). A subject
    // with nothing selected answers an empty vector, whether or not it was ever selected.
    [[nodiscard]] const std::vector<std::string>&
    selection(const std::string& subject = kSelectionSubjectEntity) const;
    // Every LIVE selection, keyed by subject. Empty selections are pruned rather than kept as empty
    // entries, so "is this subject in the map" and "does this subject have a selection" are the same
    // question — which is what lets the persisted `selections` array and the `selection-get` reply
    // share one encoder without either inventing a rule the other does not apply.
    [[nodiscard]] const std::map<std::string, std::vector<std::string>>& selections() const noexcept
    {
        return selections_;
    }

    // Apply `ids` to `subject` under `mode`, then apply the D3 focus rule. Selections of different
    // subjects are INDEPENDENT: this never touches another subject's ids.
    //
    // THE FOCUS RULE, in one place: a change that leaves `subject` with a NON-EMPTY selection focuses
    // it. A change that leaves it EMPTY does not move the focus — there is nothing there to work on,
    // and handing the focus to some other subject would be a claim nobody made. A no-op (`changed ==
    // false`) moves nothing at all, which is what keeps "re-selecting the same ids publishes nothing"
    // true for BOTH facts.
    SelectionOutcome apply_selection(const std::string& subject,
                                     const std::vector<std::string>& ids, SelectionMode mode);
    // The pre-c1 spelling, acting on the default `entity` subject. ⚠ It reports only the `changed`
    // half and DISCARDS `focus_changed`, which is the conflation `SelectionOutcome` above says a
    // caller must not make — so it is kept for the tests that pin pre-c1 selection semantics, and a
    // caller that publishes wire facts wants the typed overload. The daemon (kernel_server.cpp) uses
    // the typed one for exactly that reason.
    bool apply_selection(const std::vector<std::string>& ids, SelectionMode mode);

    // --- selection focus (D3) -------------------------------------------------------------------
    // WHICH live selection the human is actually working on. A tier-1 fact deliberately: deciding it
    // from tier-2 panel focus would make the answer invisible to the CLI, to agents, and to a second
    // window — which is exactly the question tier 1 exists to answer. Boot default: `entity`.
    [[nodiscard]] const std::string& selection_focus() const noexcept { return selection_focus_; }
    // Move the focus explicitly. Returns true when it actually moved; an empty subject is refused
    // (there is no such thing as focusing nothing — clearing a selection leaves the focus alone).
    bool set_selection_focus(const std::string& subject);

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
    // The `.editor/session.json` document — VERSION 2 since c1:
    //   {version: 2,
    //    selections: [{subject, ids[]}, …],
    //    selectionFocus: {subject},
    //    cameras: [{viewportId, …}]}
    // `selections` and `cameras` are both ARRAYS of objects carrying their key, never map-keyed —
    // the same encoding discipline the authored-data conventions mandate (L-33), so the file stays
    // diffable and stable-ordered. Play state is deliberately absent (see the header note).
    [[nodiscard]] contract::Json to_json() const;
    // Apply a persisted document. Returns false when the document is structurally wrong (wrong
    // types / not an object) — the caller then treats the file as CORRUPT. A document missing an
    // optional section is not an error (forward/backward tolerance on an additive file).
    //
    // THE v1 -> v2 MIGRATION LIVES HERE, and it is what stops a SILENT LOSS rather than a
    // quarantine: a v1 document (`selection: {ids}`) passes the version check untouched and every
    // member is read under a `contains` guard, so without this branch the loader would look for
    // `selections`, find nothing, and drop the human's selection with no diagnostic at all. The
    // branch maps `selection: {ids}` -> `selections: [{subject: "entity", ids}]`, losslessly.
    // Everything else is unchanged: a FUTURE version, a non-number version, and a malformed document
    // keep the quarantine-plus-defaults-plus-loud path exactly as it was.
    bool apply_json(const contract::Json& doc);

private:
    std::map<std::string, std::vector<std::string>> selections_;
    std::string selection_focus_ = kSelectionSubjectEntity;
    std::map<std::string, CameraState> cameras_;
    EditorPlayState play_ = EditorPlayState::edit;
    std::uint64_t sim_tick_ = 0;
};

// The encoding of the state's two projectable halves — ONE implementation each, deliberately shared
// by the `session` topic facts, the `editor.*-get` replies, and `.editor/session.json` (to_json()
// below). The wire and the file are documented to carry the SAME shape; a second copy of these six
// lines is exactly how that stops being true.
[[nodiscard]] contract::Json selection_ids_json(const EditorSessionState& state,
                                                const std::string& subject =
                                                    kSelectionSubjectEntity);
// The TYPED view (c1 / D1 REVISED): an ARRAY OF OBJECTS CARRYING THEIR KEY — `[{subject, ids}, …]` —
// never a map-keyed object, the same convention the camera array already follows. Only LIVE (non-
// empty) selections appear; subjects are stable-ordered (std::map). This is ADDITIVE to
// `selection_ids_json` above rather than a replacement: `editor.selection-get` answers BOTH, because
// a reader of today's `ids` (attach_command.cpp's observer among them) reads a removed member as
// ABSENT rather than as an error, so replacing it would have broken every such reader silently.
[[nodiscard]] contract::Json selections_json(const EditorSessionState& state);
// The same view NARROWED to one subject — a filter of the array above, never a different shape, so
// `--subject` changes what the reply reports and never how it is encoded.
[[nodiscard]] contract::Json selections_json(const EditorSessionState& state,
                                             const std::string& subject);
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
