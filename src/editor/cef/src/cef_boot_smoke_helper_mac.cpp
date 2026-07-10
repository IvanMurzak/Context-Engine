// src/editor/cef/src/cef_boot_smoke_helper_mac.cpp — macOS subprocess helper for the CEF boot smoke.
//
// On macOS every CEF subprocess (renderer, GPU, utility) runs from a SEPARATE helper executable
// inside a "<app> Helper.app" bundle — the main app bundle cannot be re-exec'd as a subprocess the
// way it can on Windows/Linux. This helper does the minimum: load the Chromium Embedded Framework
// for a non-main process, then hand control to CefExecuteProcess(). It has NO browser logic.
//
// Mirrors the CEF sample "process_helper_mac" pattern. See src/editor/cef/CMakeLists.txt for how the
// helper bundle is assembled and embedded into the main app's Contents/Frameworks.

#include "include/cef_app.h"
#include "include/wrapper/cef_library_loader.h"

int main(int argc, char* argv[]) {
    // Load the framework from the parent app's Frameworks dir (helper-process variant).
    CefScopedLibraryLoader library_loader;
    if (!library_loader.LoadInHelper()) {
        return 1;
    }

    CefMainArgs main_args(argc, argv);
    // No CefApp is needed for the boot-smoke subprocesses (no custom renderer-side handlers).
    return CefExecuteProcess(main_args, nullptr, nullptr);
}
