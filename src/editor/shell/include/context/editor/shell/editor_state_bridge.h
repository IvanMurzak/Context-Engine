// The editor-state + region-map bridge surface (M9 e05d2, design 03 §1 / §6 / 04 §2).
//
// WHAT THIS IS. e05d1 gave editor-core a live panel layer; e05d2 makes its ARRANGEMENT durable and
// makes the Shell aware of it. Two seams live here, both over the SAME privileged e05c bridge the
// panel surface uses, and both keeping editor-core an ordinary wire client (D18):
//
//   1. LAYOUT PERSISTENCE. editor-core owns the Dockview arrangement (browser-side `toJSON`) and the
//      per-panel D6 state blobs. It PUBLISHES them over the bridge; the Shell records them into
//      `.editor/editor-state.json` through the EditorStateStore — of which the Shell is the SINGLE
//      WRITER (C-F3, design 03 §1). editor-core never opens, writes, or locks that file. That is not
//      a style preference: it is the ownership split that keeps the editor an ordinary client. A
//      direct write from editor-core would be a defect even if it worked, and it is structurally
//      impossible here — this bridge has no path to the file, only to the store's opaque-blob
//      setters. On boot editor-core reads the persisted blob back through `editor.state.get` and
//      rebuilds the arrangement itself; the Shell round-trips the blob without interpreting it.
//
//   2. REGION MAPS (03 §6). editor-core publishes the window's viewport / native-interaction rects on
//      EVERY layout change — dock, split, tab, float, resize, panel add/remove. The Shell replaces
//      its per-window RegionMap wholesale (input.h states why a region map is never patched), and the
//      input pump arbitrates the next pointer against it. Today the editor has no viewport panels
//      (those are e11), so the published set is typically EMPTY — but the PATH is the deliverable:
//      e11 fills the region list and inherits a wired, tested channel rather than building it. An
//      empty publish is not a no-op either — it CLEARS a rect a removed viewport left behind.
//
// CEF-FREE, like ipc_bridge.h / panel_host.h and for the same reason: CEF is a CI-only dependency
// path, so logic living inside the message-router handler would be exercised by nothing the local
// dev gate runs. Every inbound message is UNTRUSTED renderer input; each method body below is TOTAL
// over arbitrary params — a malformed publish degrades to a refusal or a skipped element, never a
// crash — and the T1 suite (tests/test_editor_state_bridge.cpp) drives the SAME code the renderer
// reaches, on all three default `build` legs.
//
// LIFETIME / COLLABORATOR WIRING. The store lives in WindowManager and the RegionMap lives per-window
// in an InputArbiter, both of which the composition root creates around this bridge. So the
// collaborators are BOUND after construction (`bind_store` / `bind_regions`) rather than taken by the
// constructor, and a method invoked before its collaborator is bound answers `editor.not_ready`
// rather than dereferencing null. In practice the renderer boots only after everything is wired, so
// `not_ready` is a guard, not a normal path.

#pragma once

#include "context/editor/contract/json.h"
#include "context/editor/shell/editor_state.h"
#include "context/editor/shell/input.h"
#include "context/editor/shell/ipc_bridge.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace context::editor::shell
{

// --------------------------------------------------------------------------- the wire vocabulary
//
// Grep-stable and MIRRORED by the TS side (src/editor/webui/core/src/editorstate.ts). The
// `webui-panel-contract` gate re-reads these values out of the BUILT bundle and compares them to the
// C++ constants here — the same cross-language discipline panel_host.h's `kPanel*Method` uses, so a
// rename on either side reds a ctest instead of silently unbinding persistence at runtime (a layout
// that never saves, with no build error anywhere).

// Restore path: editor-core asks for the persisted `{layout, panels}` blob on boot.
inline constexpr const char* kEditorStateGetMethod = "editor.state.get";
// Persistence path: editor-core publishes the current `{layout, panels}` blob for the Shell to store.
inline constexpr const char* kEditorStatePublishMethod = "editor.state.publish";
// Region-map path: editor-core publishes the window's viewport / native rects (03 §6).
inline constexpr const char* kEditorRegionsPublishMethod = "editor.regions.publish";

// The RegionKind wire tokens (03 §6). A CLOSED vocabulary — the two native consumers the Shell knows
// — so a third token invented in the renderer is REFUSED at parse rather than routed to a consumer
// that cannot mean anything by it. Mirrored in TS and cross-checked like the method names above.
inline constexpr const char* kRegionKindViewport = "viewport";
inline constexpr const char* kRegionKindNative = "native";

// Refusal codes these methods answer with. LOCAL codes (not R-CLI-008 catalog codes), the same
// rationale panel_host.h states for its `panel.*` codes: they classify a HOST-side wiring or caller
// error, not a daemon-contract failure, so minting catalog codes for them would pollute the
// published surface.
inline constexpr const char* kErrEditorBadParams = "editor.bad_params";
inline constexpr const char* kErrEditorNotReady = "editor.not_ready";

// The token a RegionKind travels as, and the inverse. nullopt for a token outside the closed set
// (including the empty string) — a caller distinguishes "unknown kind" from a valid one rather than
// silently defaulting hostile input into `viewport`.
[[nodiscard]] const char* region_kind_token(RegionKind kind);
[[nodiscard]] std::optional<RegionKind> parse_region_kind(std::string_view token);

// Parse ONE region-array element (`{id, kind, rect:{x,y,width,height}}`) into a ShellRegion. Returns
// false — and leaves `out` untouched — when the element is not an object, carries no string `id`, has
// an unrecognised `kind`, or has no `rect` object. Rect components are read as NON-NEGATIVE physical
// client pixels (a negative in a hostile or corrupted payload clamps to 0, mirroring editor_state's
// read_u32) because a region rect is an area of the client area, never left/above it.
[[nodiscard]] bool parse_shell_region(const contract::Json& element, ShellRegion& out);

// ---------------------------------------------------------------------------------- the bridge

class EditorStateBridge
{
public:
    // The clock the persistence path stamps its store writes with. Passed IN (never read here) so the
    // debounce the store applies is deterministically testable, the same discipline EditorStateStore
    // itself follows.
    using Clock = std::function<std::uint64_t()>;
    // Where a published region map goes: the composition root routes it to the target window's
    // InputArbiter. Erased through a std::function so this class never names WindowManager /
    // EditorWindow — it stays a pure parse+route surface with no window ownership.
    using RegionSink = std::function<void(std::vector<ShellRegion>)>;

    EditorStateBridge() = default;

    // Non-copyable and non-movable, for the same reason BridgeRouter / PanelHost are: `install` binds
    // handlers that capture `this`, and a router outlives nothing that could be relocated out from
    // under them.
    EditorStateBridge(const EditorStateBridge&) = delete;
    EditorStateBridge& operator=(const EditorStateBridge&) = delete;
    EditorStateBridge(EditorStateBridge&&) = delete;
    EditorStateBridge& operator=(EditorStateBridge&&) = delete;

    // Wire the editor-state store the persistence + restore paths read and write. The store is owned
    // by WindowManager and outlives this bridge; a null store leaves both paths answering
    // `editor.not_ready`.
    void bind_store(EditorStateStore* store, Clock clock);
    // Wire where published regions go. An unbound sink leaves `editor.regions.publish` answering
    // `editor.not_ready`.
    void bind_regions(RegionSink sink);

    // --- the method bodies, exposed for direct testing ------------------------------------------
    // Each is total over arbitrary input and is what the corresponding handler calls, so the T1 suite
    // exercises the SAME code the renderer reaches rather than a lookalike.

    // The persisted `{layout, panels}` blob (the restore path). Empty objects when nothing is bound
    // or nothing was persisted — a fresh project restores nothing rather than erroring.
    [[nodiscard]] contract::Json snapshot() const;

    // Record a published `{layout, panels}` blob into the store (the persistence path). At least one
    // of `layout` / `panels` must be present, else `error_code` is set to kErrEditorBadParams and
    // nothing is recorded. The store's own debounce + atomic write turn a stream of these into one
    // crash-safe file per quiet period.
    [[nodiscard]] bool publish_state(const contract::Json& params, std::string& error_code);

    // Parse a published region array and forward it to the sink (the region-map path). `regions` must
    // be an array (an EMPTY one is valid — it clears the map); malformed ELEMENTS are skipped rather
    // than failing the whole publish, so one bad rect from a hostile renderer cannot deny the rest.
    // `out`, when non-null, receives the parsed regions for inspection.
    [[nodiscard]] bool publish_regions(const contract::Json& params, std::string& error_code,
                                       std::vector<ShellRegion>* out = nullptr);

    // Bind every `editor.*` method on `router`. False when ANY binding was refused (a name collision
    // with something already registered), which is a wiring bug the caller must not ignore.
    [[nodiscard]] bool install(BridgeRouter& router);

    // --- what it saw (the live-smoke assertion surface) -------------------------------------------
    // `editor-cef-smoke-shell` can assert these are non-zero after the real renderer boots — the
    // end-to-end proof that the LIVE layout runtime actually published state / regions and read the
    // persisted blob back, a claim no local test can make since the local gate cannot link CEF.
    [[nodiscard]] std::size_t state_reads() const { return state_reads_; }
    [[nodiscard]] std::size_t states_published() const { return states_published_; }
    [[nodiscard]] std::size_t regions_published() const { return regions_published_; }

private:
    EditorStateStore* store_ = nullptr;
    Clock clock_;
    RegionSink region_sink_;
    std::size_t state_reads_ = 0;
    std::size_t states_published_ = 0;
    std::size_t regions_published_ = 0;
};

} // namespace context::editor::shell
