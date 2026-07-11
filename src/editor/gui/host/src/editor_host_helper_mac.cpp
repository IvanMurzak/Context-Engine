// src/editor/gui/host/src/editor_host_helper_mac.cpp — macOS subprocess helper for the CEF editor
// host boot smoke (M5-F0b). Identical role to src/editor/cef/src/cef_boot_smoke_helper_mac.cpp: on
// macOS every CEF subprocess (renderer, GPU, utility) runs from a SEPARATE helper executable inside a
// "<app> Helper.app" bundle; this helper loads the framework for a non-main process and hands control
// to CefExecuteProcess(). No browser / host logic. See host/CMakeLists.txt for bundle assembly.

#include "include/cef_app.h"
#include "include/wrapper/cef_library_loader.h"

int main(int argc, char* argv[])
{
    CefScopedLibraryLoader library_loader;
    if (!library_loader.LoadInHelper())
    {
        return 1;
    }

    CefMainArgs main_args(argc, argv);
    return CefExecuteProcess(main_args, nullptr, nullptr);
}
