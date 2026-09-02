// The Scene viewport BINDING seam (M9 editor-UX e3, D7; taskflow 06 §1) — the producer half of the
// live viewport, and the only place the Shell's three viewport identifiers are mapped onto one
// another.
//
// WHAT THIS ADDS, AND WHAT IT DELIBERATELY DOES NOT. The composite half and the input half were
// already built and tested and this file rebuilds NEITHER:
//
//   * `WindowCompositor` already draws `ViewportLayer`s beneath the CEF layer through its
//     transparent holes (compositor.h §4 step 2), and `publish_viewports` was reached by nothing but
//     `test_compositor.cpp` until now;
//   * `InputArbiter` already routes a pointer inside a `RegionKind::viewport` rect to
//     `InputTarget::viewport` with a region-relative position (input.h §6.2);
//   * `render::ViewportTargetRegistry` (e11b) already owns per-viewport colour+depth targets with a
//     real create / resize-in-place / release lifecycle, and `render::render_viewport_view` (e11b)
//     already draws the D5 pass — grid plus proxy geometry — into one.
//
// The MISSING work, and therefore this file, is: take the window's published region map, keep one
// render target per live viewport in step with it, draw each viewport's own View into its target,
// and publish the resulting layer stack. Plus the CAMERA each of those views is, which the daemon
// persists for us.
//
// ------------------------------------------------------------------------- THREE IDS, ONE MAP
//
// Three different identifiers name "a viewport" in this codebase and NONE of them may be derived
// from another. This class owns the whole map, which is why it exists as a class at all:
//
//   1. the SHELL region id — a `std::string`, minted by editor-core as the panel INSTANCE id
//      (`builtin.viewport#2`, c3's `(panel_id, instance_id)` pair). It arrives over
//      `editor.regions.publish` and is what `ShellRegion::id` / `ViewportLayer::id` carry.
//   2. the RENDER slot — a `std::uint32_t` (`render::View::viewport_id`), session-local, never
//      persisted, and the key `ViewportTargetRegistry::acquire_for` takes. view.h's own warning
//      spells out that it is NOT the daemon's id.
//   3. the DAEMON viewport id — the `viewportId` string `editor.camera-set` / `editor.cameras-get`
//      key a `CameraState` by, persisted into `.editor/session.json`.
//
// (1) and (3) are the SAME STRING here, deliberately: the human's camera belongs to the panel copy
// they arranged, so a restart that restores `builtin.viewport#2` must restore the camera they left
// in it. (2) is minted here, densely, and recycled through the registry's generation-tagged handles.
//
// -------------------------------------------------------------- THE CAMERA PAYLOAD IS OURS
//
// `editor.camera-set` carries `transform` / `projection` as OPAQUE JSON — the daemon is the
// custodian of the human's camera, not its interpreter (editor_session_state.h states this). So the
// meaning of those two blobs is defined HERE, by the codec below, and nothing daemon-side may be
// taught to read them. That is also what makes this a no-new-contract task: the verbs already exist.
//
// --------------------------------------------------------------------- SCENE DATA, HONESTLY
//
// `publish` takes the `RenderSnapshot` to draw. There is still no scene-data wire path from the
// daemon to the Shell (the e11c verb was never built), so the live editor passes an EMPTY snapshot
// today and every viewport renders the D5 GRID and nothing else. That is the honest state, not a
// stub: the pass, the targets, the layers, the rects and the cameras are all real, and the day a
// scene read lands it changes one argument at one call site.
//
// ------------------------------------------------------------------------------ DPI (a2)
//
// There is NO DpiScale in this file, and that is the point. Region rects arrive ALREADY PHYSICAL —
// editor-core measures them through the single `devicePixelRatio` seam a2 established (chrome.ts's
// `physicalRegion`, now shared with viewport.ts) — and `ViewportLayer::content_rect`, the compositor
// surface and the OS pointer stream are physical too. Opening a second DPI source here to
// re-convert them is exactly the bug a2 fixed for the popup rect; the conversion is proved at
// dpr 1.5 / 2 / 3 in the webui tier, where it actually lives.
//
// GPU-BACKEND-FREE: it drives only rhi.h + the two e11b render entry points, so every branch runs
// against `rendertest::FakeDevice` on all three `build` legs with no adapter anywhere.

#pragma once

#include "context/editor/contract/json.h"
#include "context/editor/shell/compositor.h"
#include "context/editor/shell/input.h"
#include "context/render/render_world.h"
#include "context/render/rhi.h"
#include "context/render/view.h"
#include "context/render/viewport_target.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace context::editor::shell
{

// The reserved code the viewport reports when this build has no rendering adapter (R-HEAD-002:
// absence is REPORTED, never silently degraded). Byte-identical to
// `gui::viewport::kViewportAdapterAbsentCode` and to the contract error catalog's entry.
//
// Copied rather than shared because `context_editor_shell` does not link `context_gui_viewport`
// (that would widen the `-fno-rtti` CEF-smoke closure). The check that stops the two spellings
// drifting is the `static_assert` in `panels/viewport_feed.h` — the one header that sees BOTH —
// NOT `editor-shell-test_viewport_binding`, which only compares this constant to a string literal.
inline constexpr const char* kViewportAdapterAbsentCode = "viewport.adapter_absent";

// --------------------------------------------------------------- the camera payload codec (D7)
//
// The `transform` / `projection` halves of `editor.camera-set`, and their inverses. Opaque on the
// wire, defined here (see the header note). Both are TOTAL on the read side: a malformed or absent
// member leaves that field at the View's current value rather than failing the whole apply, because
// a camera blob written by an older build must not cost the human their viewport.

[[nodiscard]] contract::Json camera_transform_json(const render::View& view);
[[nodiscard]] contract::Json camera_projection_json(const render::View& view);

// Apply one half. Returns true when at least one recognized member was read.
bool apply_camera_transform(render::View& view, const contract::Json& transform);
bool apply_camera_projection(render::View& view, const contract::Json& projection);

// The full `editor.camera-set` params for one viewport: `{viewportId, transform, projection}`.
[[nodiscard]] contract::Json camera_set_params(const std::string& viewport_id,
                                               const render::View& view);

// ------------------------------------------------------------------------ the default framing
//
// The Scene camera a viewport starts at: pulled back and up, looking down the -Z axis the grid lies
// under, so a viewport whose camera the daemon has never heard of still frames something.
//
// EXPORTED because it has TWO consumers that must agree — this binding's first-sight camera
// (`camera()` on an unknown id) and the panel layer's `framed_scene_view` ("frame scene" returns
// here). They were two independent spellings of the same literals, under a comment asserting they
// were one; a change to either would have made "frame scene" jump somewhere a freshly opened
// viewport does not look. The direction is the linkable one: `context_editor_panels` links
// `context_editor_shell` PUBLIC, never the reverse.
[[nodiscard]] render::View default_scene_view();

// ------------------------------------------------------------------------------ publish stats

// What one `publish()` did. Every field is an observable a test asserts rather than a log line.
struct ViewportPublishStats
{
    // Viewport-kind regions seen in the map (whatever this build could do with them).
    std::size_t viewports = 0;
    // Layers actually published to the compositor. Equal to `viewports` except where a region was
    // degenerate (a zero-extent rect — a panel mid-resize), which is dropped rather than published.
    std::size_t layers = 0;
    // Targets a render pass actually drew into this call. 0 with no adapter.
    std::size_t rendered = 0;
    // No `render::IDevice`: the layers are published with NO content, so the compositor's clear
    // colour shows through the hole and the panel's own summary model is what the human reads.
    bool adapter_absent = false;
    // The published SET moved (a rect changed, a viewport appeared or went away), so the caller's
    // layout is stale. `publish()` has already told the compositor; this is for the caller's own
    // bookkeeping and for the tests.
    bool changed = false;
};

// --------------------------------------------------------------------------------- the binding

class ViewportBinding
{
public:
    ViewportBinding() = default;

    // Non-copyable: it owns device resources through the registry, and a copy would hand two
    // bindings the same viewport map (the position `ViewportTargetRegistry` takes, for the same
    // reason).
    ViewportBinding(const ViewportBinding&) = delete;
    ViewportBinding& operator=(const ViewportBinding&) = delete;

    // Adopt the window's device. `device` must outlive the binding (the window owns both, and
    // declares this one after the compositor for exactly that reason).
    //
    // Calling it AGAIN with a different device rebuilds the registry from scratch: a target is
    // device-bound, so a device rebuild (loss, a tear-out onto another window) cannot keep the old
    // textures. The camera map SURVIVES that — it is the human's, not the device's.
    void attach_device(render::IDevice& device);

    // Drop the device and every target with it — the CPU present path, or an unrecoverable device
    // loss. The next `publish()` reports `adapter_absent` and publishes contentless layers, which is
    // the honest degrade rather than an empty window.
    void detach_device();

    [[nodiscard]] bool adapter_available() const noexcept { return device_ != nullptr; }
    // `kViewportAdapterAbsentCode` while no device is attached, `""` otherwise. What the Shell
    // reports into the panel model so the human is TOLD why the viewport is a summary.
    [[nodiscard]] const char* degraded_code() const noexcept;

    // --- cameras (one per viewport id; D7's `editor.camera-set` payload) --------------------------

    // The camera of `viewport_id`, minting the default Scene view on first sight. The default is
    // deliberate and shared with `apply_cameras_result`: a viewport whose camera the daemon has
    // never heard of still frames something.
    [[nodiscard]] render::View& camera(const std::string& viewport_id);
    [[nodiscard]] const render::View* find_camera(const std::string& viewport_id) const;

    // Apply one daemon camera payload. Returns true when anything changed — which is also what
    // arms `take_dirty()`, so a camera arriving FROM the daemon is never echoed straight back to it
    // (the `origin` echo-suppression posture, applied at the one place that can).
    bool apply_camera(const std::string& viewport_id, const contract::Json& transform,
                      const contract::Json& projection);

    // Adopt an `editor.cameras-get` reply. Accepts the R-CLI-008 envelope (`{ok, data:{cameras}}`),
    // the bare `data`, or the bare `{cameras:[…]}` object; anything else is ignored. Returns how
    // many cameras were adopted.
    std::size_t apply_cameras_result(const contract::Json& reply);

    // Record a LOCAL camera change (a gesture, a `frame scene`) — the half that must reach the
    // daemon. Returns true when the stored camera actually moved.
    bool set_camera(const std::string& viewport_id, const render::View& view);

    // Put `viewport_id` back on the write queue WITHOUT changing its camera — the retry after a
    // failed `editor.camera-set`. Separate from `set_camera` because "the value moved" and "the
    // daemon has not heard the value" are different facts, and a retry that had to nudge the camera
    // to re-arm would be a write that changes what it is retrying. A no-op for an unknown id.
    void mark_camera_dirty(const std::string& viewport_id);

    // Viewport ids whose camera moved LOCALLY since the last call, in insertion order, cleared by
    // the call. The pump turns each into one `editor.camera-set`.
    [[nodiscard]] std::vector<std::string> take_dirty();
    [[nodiscard]] std::size_t dirty_count() const noexcept { return dirty_.size(); }

    // --- the per-frame producer -------------------------------------------------------------------

    // Take the window's published region map and rebuild the viewport layer stack from it.
    //
    // ONE call does all four halves of the producer: the target lifecycle (create / resize in place
    // / release the ones whose region went away), the render pass per live viewport, the layer
    // publish, and the damage. Idempotent: publishing the same regions twice draws twice (the scene
    // may have moved) but reports `changed == false` and marks only CONTENT damage, not layout.
    ViewportPublishStats publish(const std::vector<ShellRegion>& regions,
                                 const render::RenderSnapshot& snapshot,
                                 WindowCompositor& compositor);

    // The layer stack the last `publish()` produced — what went to the compositor, kept so a caller
    // (and the T1 suite) can assert on it without reaching into the compositor.
    [[nodiscard]] const std::vector<ViewportLayer>& layers() const noexcept { return layers_; }

    // The render slot `viewport_id` holds, or 0 when it holds none. Session-local; see the header's
    // three-ids note before using it for anything that outlives the process.
    [[nodiscard]] std::uint32_t render_slot(const std::string& viewport_id) const;

    // Live target count (the registry's own), so a test can prove a closed viewport gave its GPU
    // memory back rather than merely stopping being drawn.
    [[nodiscard]] std::size_t live_targets() const;

    // How many `publish()` calls have run, and how many of those moved the published SET. The pair
    // is what distinguishes "republished identically" from "never republished".
    [[nodiscard]] std::size_t publishes() const noexcept { return publishes_; }
    [[nodiscard]] std::size_t layout_changes() const noexcept { return layout_changes_; }

    // A device came or went since the last `publish()`, so the compositor's layer stack MUST be
    // rebuilt even if no rect moved.
    //
    // WHY IT IS A PUBLIC FACT AND NOT AN INTERNAL FLAG. `WindowCompositor` holds the published
    // layers by RAW `ITextureView*`, and those views belong to the registry `attach_device` /
    // `detach_device` destroys. The owner loop only calls `publish()` when the region map's
    // generation moved, so after a device change with an unchanged layout NOTHING would republish
    // and the compositor would keep compositing pointers into a destroyed registry. The caller's
    // publish gate reads this so a device change is a publish trigger in its own right.
    [[nodiscard]] bool needs_publish() const noexcept { return force_publish_; }

private:
    // Everything the binding knows about one live viewport.
    struct Entry
    {
        std::uint32_t slot = 0;      // the render-side id (view.h's `View::viewport_id`)
        render::View view{};         // the camera, in the units the pass wants
        render::Rect2D rect{};       // PHYSICAL client px, as published
        bool present = false;        // seen in the CURRENT publish
    };

    [[nodiscard]] Entry& entry(const std::string& viewport_id);

    // Void every render slot and arm the unconditional republish — the shared tail of
    // `attach_device` and `detach_device`, which face the same invariant for the same reason.
    void invalidate_targets();

    render::IDevice* device_ = nullptr;
    // Rebuilt on every `attach_device`; null while no device is attached, which is what makes the
    // degraded path structural rather than a flag someone can forget to check.
    std::unique_ptr<render::ViewportTargetRegistry> targets_;
    // ORDERED (std::map, not unordered): `take_dirty` and `publish` both walk it, and a stable order
    // makes the camera-set sequence and the layer order reproducible across runs and platforms.
    std::map<std::string, Entry> entries_;
    std::vector<std::string> dirty_;
    std::vector<ViewportLayer> layers_;
    std::uint32_t next_slot_ = 1; // 0 is view.h's reserved "unassigned"
    std::size_t publishes_ = 0;
    std::size_t layout_changes_ = 0;
    // Set by attach_device / detach_device, consumed by publish(): the next publish republishes
    // unconditionally, so no layer pointing into a destroyed registry survives it (see
    // `needs_publish`).
    bool force_publish_ = false;
};

} // namespace context::editor::shell
