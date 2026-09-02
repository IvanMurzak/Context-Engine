// The LIVE Scene-viewport feed (M9 editor-UX e3, D7) — the panel-host half of the viewport: ONE
// `ViewportPanel` model per live COPY, the camera round trip over `editor.camera-set` /
// `editor.cameras-get`, and the honest degraded summary when this build has no rendering adapter.
//
// WHAT DIFFERS FROM EVERY SIBLING FEED, and it is the whole reason this file is shaped the way it is:
//
//   * IT BINDS `provide_factory`, NOT `provide` (c3). `builtin.viewport` is the ONE built-in declared
//     `instances.mode: "unlimited"` — several scene views at once IS the panel — and two copies must
//     hold DIFFERENT cameras. `provide()` binds one provider that every copy shares, which is right
//     for Problems (one diagnostics set however many views) and WRONG here: both viewports would
//     render the same camera. So this feed keeps a MAP of models keyed by instance id and hands the
//     host a factory (panel_host.h § provide_factory).
//
//   * ⚠ `panel.list` REPORTS ONE REVISION PER KIND, and c3 recorded that limit naming e3 as its
//     owner. The consequence, stated plainly so nobody rediscovers it as a bug: touching the kind
//     because copy #1 changed makes copy #2's next `panel.render` a no-op ROUND TRIP — the runtime
//     re-fetches a tree that is byte-identical and its id-keyed patch applies nothing. That is
//     wasted work, never wrong pixels, and it is bounded by the number of open copies. The
//     alternative (a per-instance revision on the wire) is a `panel.list` shape change, i.e. a
//     contract edit, which this task explicitly does not make. Designed around knowingly.
//
//   * THE CAMERA IS NOT PANEL STATE. `get_state` / `restore_state` are deliberately absent, so
//     `panel.list` reports `persists: false` for the viewport. The camera lives in the DAEMON
//     (`EditorSessionState::set_camera` -> `.editor/session.json`), reached through verbs that
//     already exist, and a D6 blob carrying it too would be a second source of truth free to
//     disagree with the persisted one across two windows. What the viewport persists, it persists
//     where cameras already persist — which is what the task asked for.
//
//   * `viewport.frame-scene` IS THE CAMERA WRITER. The Shell's pointer arm for
//     `InputTarget::viewport` is still empty (orbit/pan/zoom is later work), so the one thing that
//     moves a camera today is the panel's own keyboard-reachable "frame scene" affordance — which
//     arrives over `panel.invoke` and lands here. That makes the camera round trip a REAL path with
//     a REAL trigger rather than a seam nothing drives (R-CLI-001: every GUI action reachable
//     without a pointer).
//
// The feed owns NO device and NO rects: those are `shell::ViewportBinding`'s (viewport_binding.h),
// which this feed reads for the adapter verdict and the per-copy composited size, and writes local
// camera moves into. One binding, one feed, no third copy of the camera.

#pragma once

#include "context/editor/contract/json.h"
#include "context/editor/gui/compositor/surface.h"
#include "context/editor/gui/viewport/viewport_panel.h"
#include "context/editor/shell/panel_host.h"
#include "context/editor/shell/viewport_binding.h"
#include "context/render/view.h"

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace context::editor::shell::panels
{

namespace viewport = gui::viewport;
// The L-41 compositor surface vocabulary. Aliased rather than spelled `gui::compositor` at each use:
// inside `shell::panels` the bare name `compositor` does NOT resolve to it (there is no
// `shell::compositor` namespace, and `shell::Compositor*` types are in scope), so the alias is what
// keeps this file reading like `viewport_panel.h`, whose own namespace makes `compositor::` work.
namespace gui_compositor = gui::compositor;

// THE REAL DRIFT GATE for the adapter-absent code, and it lives HERE because this is the only header
// that sees BOTH spellings: `context_editor_shell` cannot link `context_gui_viewport` (it would
// widen the `-fno-rtti` CEF-smoke closure — the same reason `builtin_panels.h` forward-declares
// `ViewportBinding`), so the shell keeps its own copy. `viewport_binding.h` used to claim this check
// was performed by `editor-shell-test_viewport_binding`; that test compares the shell constant to a
// STRING LITERAL and never references the GUI one, so the two could in fact drift. A compile-time
// assertion in the one translation-unit set that can see both makes the claim true at zero cost.
static_assert(std::string_view(kViewportAdapterAbsentCode) ==
                  std::string_view(viewport::kViewportAdapterAbsentCode),
              "the shell's adapter-absent code must stay byte-identical to the GUI viewport's");

// ------------------------------------------------------------------------------- pure helpers

// The camera "frame scene" returns to: the DEFAULT framing pose, keeping the projection the human
// chose (mode, fov / ortho height, near / far) and resetting only the placement.
//
// ⚠ WHY IT IS NOT A REAL FIT. Framing the scene's actual bounds needs bounds, and nothing delivers
// them: `ViewportSceneSummary` carries COUNTS (drawables / lights) and the Shell holds an empty
// `RenderSnapshot` because no daemon scene-data read exists (viewport_binding.h § SCENE DATA,
// HONESTLY). Inventing a distance from a drawable count would be a fabricated camera dressed as a
// computed one. Returning to the default pose is the honest, deterministic, testable rule, and it is
// a real camera CHANGE — which is the property the round trip needs.
[[nodiscard]] render::View framed_scene_view(const render::View& current);

// ------------------------------------------------------------------------------------ the feed

class ViewportFeed
{
public:
    // Non-owning: `host` and `binding` must outlive the feed. `panel_id` is passed rather than
    // hardcoded — the feed is a MECHANISM the composition root points at a roster id (the
    // ProblemsFeed/FilesFeed convention). A null `binding` is legal and means "no producer wired":
    // the models still render (a headless harness, a build with no window), the camera is kept
    // locally and nothing is pushed.
    ViewportFeed(PanelHost& host, std::string panel_id, ViewportBinding* binding = nullptr);

    ViewportFeed(const ViewportFeed&) = delete;
    ViewportFeed& operator=(const ViewportFeed&) = delete;
    ViewportFeed(ViewportFeed&&) = delete;
    ViewportFeed& operator=(ViewportFeed&&) = delete;

    // The per-instance factory to bind on the PanelHost (c3). Captures `this` — the feed must
    // OUTLIVE the binding. Safe for the host's bind-time PROBE (an empty instance id), which
    // materialises no model.
    [[nodiscard]] PanelProviderFactory make_factory();

    // Re-point the feed at the window's viewport producer (the `bind_write_client` pattern).
    //
    // NEEDED because of an ORDERING FACT in the composition root, not a preference: the panel bag is
    // built at boot, BEFORE the window and therefore before the `render::IDevice` its producer binds
    // to exists. Re-derived per frame like the wire gateways, so a window whose present path is
    // re-attached (a device rebuild, a GPU-less fallback) is picked up with no bookkeeping. `nullptr`
    // detaches: the models keep rendering and nothing is pushed.
    //
    // Also refreshes the ADAPTER verdict, because the two facts arrive together and a feed that took
    // a new producer while keeping the old verdict would report `viewport.adapter_absent` for a
    // window that has just acquired a device (or, worse, hide the code for one that has lost it).
    void bind_binding(ViewportBinding* binding);

    // --- the camera round trip -------------------------------------------------------------------

    // The pump's contract, identical to FilesFeed's: `fetch_due()` says an `editor.cameras-get` is
    // wanted; the pump calls `mark_fetched()` BEFORE issuing the RPC (claiming the fetch). Re-armed
    // when a new copy opens, because that copy's camera may be one the daemon already holds.
    [[nodiscard]] bool fetch_due() const noexcept { return fetch_due_; }
    void mark_fetched() noexcept { fetch_due_ = false; }

    // Adopt an `editor.cameras-get` reply into the binding. Returns how many cameras were adopted.
    // Does NOT arm a write back (viewport_binding.h § apply_camera: the echo-suppression posture).
    std::size_t apply_cameras_result(const contract::Json& reply);

    // The `editor.camera-set` calls the pump owes the daemon: one params object per locally-moved
    // camera, in a stable order, cleared by the call.
    [[nodiscard]] std::vector<contract::Json> take_camera_writes();

    // Put one camera BACK on the write queue after its `editor.camera-set` failed. The read/write
    // asymmetry the pump's header states, made mechanical: a lost read is retried on the next
    // reason to ask, a lost WRITE is a camera move the human made and the daemon never heard, so it
    // is re-armed immediately. Idempotent, and a no-op for a copy that has since closed.
    void rearm_camera_write(const std::string& viewport_id);

    // --- the present environment (the degraded summary) -------------------------------------------

    // Whether this build has a rendering adapter for the viewports. Pushed from the composition root
    // off `ViewportBinding::adapter_available()`; false makes every live model report
    // `viewport.adapter_absent` in its rendered summary (R-HEAD-002).
    void set_adapter_available(bool available);
    [[nodiscard]] bool adapter_available() const noexcept { return adapter_available_; }

    // --- observables ------------------------------------------------------------------------------

    [[nodiscard]] std::size_t instances() const noexcept { return models_.size(); }
    // The model of one live copy, materialising it on first sight (what `build` does). Null only for
    // an EMPTY instance id — the host's bind-time probe, which must materialise nothing.
    [[nodiscard]] viewport::ViewportPanel* model(const std::string& instance_id);
    [[nodiscard]] std::size_t frame_scene_requests() const noexcept { return frame_scene_requests_; }
    [[nodiscard]] std::size_t cameras_applied() const noexcept { return cameras_applied_; }

private:
    // Push the current present environment into ONE model: the platform + probed caps, the adapter
    // verdict, and the composited size the producer published for this copy (0x0 until it has one).
    void refresh_present(const std::string& instance_id, viewport::ViewportPanel& panel) const;

    PanelHost& host_;
    std::string panel_id_;
    ViewportBinding* binding_ = nullptr;
    // ORDERED, like the binding's own map and for the same reason: `take_camera_writes` walks it and
    // a stable order makes the RPC sequence reproducible across runs and platforms.
    std::map<std::string, viewport::ViewportPanel> models_;
    gui_compositor::SurfaceCapabilities caps_{};
    bool adapter_available_ = true;
    bool fetch_due_ = true; // born due: the first pump hydrates every camera the daemon holds
    std::size_t frame_scene_requests_ = 0;
    std::size_t cameras_applied_ = 0;
};

} // namespace context::editor::shell::panels
