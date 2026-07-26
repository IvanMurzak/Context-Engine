// src/editor/shell/cef/src/shell_helper_mac.cpp — the macOS subprocess helper for every live Shell
// CEF bundle (M9 e12c-1, issue #436).
//
// On macOS a CEF subprocess (renderer, GPU, utility, alerts, plugin) runs from a SEPARATE executable
// inside a "<App> Helper*.app" bundle; the main app bundle cannot be re-exec'd as a subprocess the way
// it can on Windows/Linux. FIVE helper bundles are built per main app, one per CEF_HELPER_APP_SUFFIXES
// entry, and CEF resolves them by name from the main executable's own name — see
// src/editor/shell/cef/CMakeLists.txt for the assembly.
//
// ⚠ DELIBERATELY NOT A COPY of src/editor/cef/src/cef_boot_smoke_helper_mac.cpp or
// src/editor/gui/host/src/editor_host_helper_mac.cpp. Both of those inline the CEF calls and pass a
// NULL CefApp, which is legal only because their apps have no renderer duties. The Shell's app object
// registers `context-editor://` + `context-ext://` in EVERY process and injects `contextEditorQuery`
// into every frame from `OnContextCreated`, so this helper must hand CEF the real one. That object is
// internal to the binding, so the binding exports the entry point instead:
// `shell::cef::execute_helper_process` (cef_shell.h) does the framework load, the app and
// `CefExecuteProcess`. Keeping the body to one call is what stops a sixth helper from drifting.

#include "context/editor/shell/cef/cef_shell.h"

int main(int argc, char* argv[])
{
    return context::editor::shell::cef::execute_helper_process(argc, argv);
}
