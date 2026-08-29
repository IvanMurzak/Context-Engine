// The macOS NATIVE MENU seam (editor-window-chrome d3, menu structure 03 / target 02 §4) — how the
// composition root asks the Cocoa backend to build the global NSMenu bar from a published model,
// without widening IWindowBackend and without a single AppKit type in a cross-platform header.
//
// THE c1 PATTERN, THIRD APPLICATION (cocoa_chrome.h owns the rationale): the functions below are
// REAL in cocoa_window.mm — keyed on `IWindowBackend::name() == "cocoa"`, the one string only the
// live Cocoa backend answers — and honest refusals in cocoa_window.cpp on every other platform, so
// the off-platform behaviour is assertable as a VALUE on every leg (tests/test_menu_facts.cpp)
// rather than the functions not existing at all.
//
// WHAT THE .mm HALF DOES with a model: builds the NSMenu tree (the App menu as the main menu's
// first item — the AppKit convention), one NSMenuItem per command item with its label, its parsed
// key equivalent (menu_model.h `parse_menu_accelerator`; `Ctrl` maps onto the COMMAND key — the
// platform's primary modifier, the CmdOrCtrl reading), its enabled state (autoenablesItems is OFF —
// enablement is the published model's, the honest-degrade snapshot, never AppKit's guess), and the
// command id as its represented object; then installs it as `NSApp.mainMenu`. An activated item —
// clicked, or reached through its key equivalent — invokes the bound callback with its command id,
// which the composition root routes into `MenuActivationRelay` (menu_facts.h) and so back to
// editor-core's ONE registry. No web menubar renders on macOS (02 §4); this bar is the rendering.

#pragma once

#include "context/editor/shell/menu_model.h"

#include <cstddef>
#include <functional>
#include <string>

namespace context::editor::shell
{

class IWindowBackend;

// What an activated native menu item reports: its command id, verbatim from the published model.
using MenuActivationCallback = std::function<void(const std::string& command_id)>;

// What the backend's menu currently holds, for the windowed smoke's assertions (the same observable
// pattern CocoaCaptionStats serves the c1 smoke).
struct CocoaMenuStats
{
    std::size_t installs = 0;    // how many models were installed over this backend's life
    std::size_t items = 0;       // command items in the CURRENT menu (separators/submenu headers excluded)
    std::size_t activations = 0; // how many item activations invoked the callback
};

// Build + install the global menu bar from `model`, activation routed to `on_activate`. True ONLY
// when `backend` is the live Cocoa backend and the install happened; false for every other backend
// and in every non-macOS build — the honest refusal the win/linux composition root gets, which is
// what makes binding `menu.publish` unconditional and platform-free (editor_main.cpp). A SECOND
// install REPLACES the bar wholesale (a re-publish is a re-render, the mountChrome rule).
[[nodiscard]] bool cocoa_install_menu(IWindowBackend& backend, const MenuModel& model,
                                      MenuActivationCallback on_activate);

// Read what the menu has seen so far. True only for the live Cocoa backend, like the install above.
[[nodiscard]] bool cocoa_menu_stats(const IWindowBackend& backend, CocoaMenuStats& out);

// Perform the installed menu item carrying `command_id`, exactly as an activation would — the
// programmatic activation path the windowed smoke drives the round trip through (a CI runner cannot
// click the system menu bar; interactive verification of the REAL bar is named deferred work).
// False when there is no such item, the item is DISABLED (a disabled item is truly inert — the
// honesty the DoD pins), or `backend` is not the live Cocoa backend.
[[nodiscard]] bool cocoa_menu_perform(IWindowBackend& backend, const std::string& command_id);

} // namespace context::editor::shell
