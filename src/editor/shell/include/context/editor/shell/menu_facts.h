// The Shell-published MENU ACTIVATION fact (editor-window-chrome d3, menu structure 03) — exactly
// one: "a native menu item carrying command id X was activated in window W".
//
// WHAT THIS CLOSES. On macOS the d3 menu renders as the native global NSMenu bar (cocoa_menu.h),
// so an activation happens in AppKit, on the Shell side of the bridge — and the command it names
// must be EXECUTED by editor-core through the SAME e07b registry a web-menubar click dispatches
// through, or a second dispatch system would exist (the one thing 03 forbids). 03 binds the return
// channel: the activation comes back "as a fact on the EXISTING editor.ui mirror relay carrying the
// command id". This relay turns the Cocoa callback into that envelope, queued for the window's next
// `ui.mirror-poll`, which editor-core drains onto its bus; menu.ts's subscriber (`subscribeMenuFacts`)
// reads the command id off it and calls `registry.execute` — one registry, one dispatch path.
//
// THE PRECEDENT IS chrome_facts.h, deliberately, down to the section names: a NINTH built-in
// `editor.ui` topic (the set stays CLOSED — one more declared member with a design authority, 03's
// "an activated item comes back as a fact on the EXISTING editor.ui relay", not a door left open),
// the `shell` origin that can never collide with a window id, the UNICAST delivery (an activation
// is a fact about ONE window's menu — today always the primary, which owns the app menu bar), and
// the `webui-panel-contract` byte-compare against the TS constant out of the BUILT bundle.
//
// D7 IS UNTOUCHED: tier-2 chrome moving Shell -> its own window; nothing here reaches the daemon.
// (The COMMAND the fact names may itself write daemon-ward once editor-core executes it — but that
// is the registry's ordinary dispatch, indistinguishable from a palette run of the same command.)
// CEF-FREE and D10 BOUNDARY-CLEAN like chrome_facts.h: pure data movement over `contract::Json`, so
// tests/test_menu_facts.cpp drives the SAME relay the real Shell runs, on all three `build` legs.

#pragma once

#include "context/editor/contract/json.h"
#include "context/editor/shell/ui_mirror.h"
#include "context/editor/shell/window_registry.h" // WindowId

#include <cstddef>
#include <cstdint>
#include <string>

namespace context::editor::shell
{

// The NINTH built-in `editor.ui` topic (uibus.ts `UI_TOPIC_MENU`). Cross-checked byte-for-byte
// against the TS constant out of the BUILT bundle by `tools/check_webui_assets.py --panel-contract`
// (ctest `webui-panel-contract`): a drift makes `receiveMirrored` refuse every activation as an
// unknown topic, and the native menu silently stops doing anything — restored by a typo, with both
// builds green.
inline constexpr const char* kUiTopicMenu = "editor.ui.menu";

// The `origin` stamped on a Shell-published menu envelope — the shared `kShellUiOrigin`
// (ui_mirror.h), aliased under this topic's own name so it can never drift from its two siblings'
// (write_notice.h / chrome_facts.h own the full origin rationale: a fact stamped with the target
// window's own id would be swallowed by that window's own-origin echo drop).
inline constexpr const char* kMenuFactOrigin = kShellUiOrigin;

// Build the `{seq, topic, origin, payload}` mirror envelope for one activation. PURE and separately
// exposed so the T1 suite asserts the wire shape without a store — the chrome_maximized_envelope
// discipline. Payload: `{windowId, commandId}`, both written unconditionally.
[[nodiscard]] contract::Json menu_activation_envelope(WindowId window,
                                                      const std::string& command_id,
                                                      std::uint64_t seq);

// The relay: turn a native menu activation into an `editor.ui` fact in the affected window's mirror
// queue.
//
// UNBOUND IS AN HONEST, SUPPORTED STATE (the chrome_facts discipline): with no store,
// `publish_activation` reaches nobody and answers 0, while `published()` still moves — so a test can
// tell "no activation was reported" from "reported to nobody", and no caller has to know whether the
// transport exists.
class MenuActivationRelay
{
public:
    MenuActivationRelay() = default;

    // Non-copyable and non-movable, like every sibling relay: the composition root hands the Cocoa
    // menu an activation callback that captures it, and nothing a std::function captured may be
    // relocated out from under it.
    MenuActivationRelay(const MenuActivationRelay&) = delete;
    MenuActivationRelay& operator=(const MenuActivationRelay&) = delete;
    MenuActivationRelay(MenuActivationRelay&&) = delete;
    MenuActivationRelay& operator=(MenuActivationRelay&&) = delete;

    void bind_store(UiMirrorStore* store) noexcept { store_ = store; }
    [[nodiscard]] bool has_store() const noexcept { return store_ != nullptr; }

    // Queue "window `window`'s menu activated command `command_id`" for that window's next
    // `ui.mirror-poll`. An EMPTY command id is refused (counted in `refused()`, delivered nowhere):
    // a nameless activation could only dispatch nothing, and the registry must never be handed one.
    // Returns how many queues it reached (1, or 0 with no store bound / a refused id).
    std::size_t publish_activation(WindowId window, const std::string& command_id);

    // --- what it saw (the T1 assertion surface) --------------------------------------------------
    [[nodiscard]] std::size_t published() const noexcept { return published_; }
    [[nodiscard]] std::size_t delivered() const noexcept { return delivered_; }
    [[nodiscard]] std::size_t refused() const noexcept { return refused_; }
    [[nodiscard]] std::uint64_t seq() const noexcept { return seq_; }

private:
    UiMirrorStore* store_ = nullptr;
    std::size_t published_ = 0;
    std::size_t delivered_ = 0;
    std::size_t refused_ = 0;
    std::uint64_t seq_ = 0;
};

} // namespace context::editor::shell
