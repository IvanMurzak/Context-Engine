// The panel COMPOSITION ROOT (M9 e05d1, design 04 §3-§4): the one place that binds the Shell's
// panel-agnostic `PanelHost` to the concrete C++ panel libraries this build can actually link.
//
// WHY THIS IS A SEPARATE LIBRARY FROM `context_editor_shell`. The D10 shell-boundary gate
// (`context_assert_shell_boundary`, src/CMakeLists.txt) audits the transitive link closure of
// `context_editor_shell` and `context_editor` and FATAL_ERRORs at configure time on any EditorKernel
// internal module. Panel libraries are exactly where that bites: `context_gui_panel_scenetree` and
// `context_gui_panel_inspector` both link `context_compose`, which IS forbidden. Keeping the
// provider bindings HERE — in a library only the executable links — means `context_editor_shell`
// itself never touches a panel library, so the runtime half stays boundary-clean by construction and
// the gate's FORBIDDEN list needs no edit. Resolving the two violations so those panels CAN be
// hosted is e05d3's whole task; this file is the seam it plugs into.
//
// WHAT "HOSTABLE TODAY" MEANS. Two of the ten rostered panels are hostable in this build:
//
//   * `placeholder`      — `gui/uitree/builtin.h`, in `context_gui_uitree`. Clean.
//   * `builtin.problems` — `context_gui_panel_problems` -> uitree + bridge. Clean.
//
// The other eight have no provider yet and are LISTED-BUT-UNHOSTED (panel_host.h explains why that
// is an honest state rather than a hidden one). Two panels rather than one is deliberate: hosting a
// single panel would leave "the runtime is panel-agnostic" resting on a claim, while two panels from
// two different libraries with two different shapes exercise it.

#pragma once

#include "context/editor/shell/panel_host.h"
#include "context/editor/shell/panels/problems_feed.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace context::editor::shell::panels
{

// The roster ids this build can render. Exposed so a caller (and the T1 suite) can assert what was
// bound WITHOUT restating the list — the one enumeration lives in `install_builtin_panels`.
[[nodiscard]] const std::vector<std::string>& hostable_panel_ids();

// Everything `install_builtin_panels` created and the caller must keep alive.
//
// THE LIFETIME IS THE POINT. Providers are std::functions capturing the models below, and the
// PanelHost holds them for as long as it routes. Returning the owners in one bag makes "these must
// outlive the host" a thing the type system participates in, rather than a comment someone violates
// by letting a local ProblemsFeed go out of scope and leaving the host with dangling captures.
struct BuiltinPanels
{
    std::unique_ptr<ProblemsFeed> problems;

    // How many providers actually bound. Checked by the caller: a silently dropped binding presents
    // later as a panel that mysteriously reports `hosted: false`.
    std::size_t bound = 0;
};

// Bind every hostable provider on `host`. `host` must outlive the returned bag.
//
// Returns the owners; `bound` reports how many bindings succeeded, which the caller compares against
// `hostable_panel_ids().size()`. A partial result is REPORTED rather than fatal: an editor that
// refused to start because one panel could not bind would be less useful than one that opens with
// the rest and says so.
[[nodiscard]] BuiltinPanels install_builtin_panels(PanelHost& host);

} // namespace context::editor::shell::panels
