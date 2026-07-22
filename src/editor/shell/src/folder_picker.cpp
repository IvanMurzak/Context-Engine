// The native OS folder picker (M9 e14c). Isolated in its own TU because the Windows implementation
// pulls <windows.h> + the shell COM headers, which must not reach a wgpu-native / CEF header (the
// `near`/`far` macro landmine, conventions.md); nothing here is a header, and no identifier is named
// `near`/`far`, so the include is contained. Boundary-clean: pure OS-SDK calls, no engine-internal link
// edge — `context_assert_shell_boundary` is unaffected.
//
// This is the un-testable half of the welcome folder-picker seam (like the real window backend and the
// live CEF path): a genuine OS dialog cannot run headless in CI, so `WelcomeBridge` reaches it through
// an injectable `FolderPicker` and the unit tests inject a deterministic fake. Only this literal OS
// call is outside automated coverage.

#include "context/editor/shell/welcome.h"

#if defined(_WIN32)
// NOMINMAX: <windows.h> otherwise macro-defines min/max and mangles std::min/std::max at every later
// include (mirrors win32_window.cpp). WIN32_LEAN_AND_MEAN drops the winsock headers this file has no use
// for; the COM/shell headers needed for the dialog are included explicitly below.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// clang-format off
#include <windows.h>
#include <objbase.h>   // CoInitializeEx / CoCreateInstance / CoTaskMemFree
#include <shobjidl.h>  // IFileOpenDialog / IShellItem / FOS_PICKFOLDERS
// clang-format on
#include <string>
#endif

namespace context::editor::shell
{

#if defined(_WIN32)

std::optional<std::filesystem::path> native_pick_folder()
{
    // Initialize COM on THIS thread (the Shell's single-threaded owner loop, so this is synchronous and
    // safe). RPC_E_CHANGED_MODE means COM is already up in another mode on this thread — we still use it.
    const HRESULT init = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool owns_com = SUCCEEDED(init);
    const bool com_usable = owns_com || init == RPC_E_CHANGED_MODE;
    if (!com_usable)
        return std::nullopt;

    std::optional<std::filesystem::path> chosen;

    IFileOpenDialog* dialog = nullptr;
    HRESULT hr = ::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&dialog));
    if (SUCCEEDED(hr) && dialog != nullptr)
    {
        DWORD flags = 0;
        if (SUCCEEDED(dialog->GetOptions(&flags)))
            (void)dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                                     FOS_PATHMUSTEXIST);

        hr = dialog->Show(nullptr);
        if (SUCCEEDED(hr))
        {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item)) && item != nullptr)
            {
                PWSTR wide_path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &wide_path)) &&
                    wide_path != nullptr)
                {
                    chosen = std::filesystem::path(std::wstring(wide_path));
                    ::CoTaskMemFree(wide_path);
                }
                item->Release();
            }
        }
        // A user cancel returns HRESULT_FROM_WIN32(ERROR_CANCELLED); `chosen` simply stays empty.
        dialog->Release();
    }

    if (owns_com)
        ::CoUninitialize();
    return chosen;
}

#else

std::optional<std::filesystem::path> native_pick_folder()
{
    // The packaged macOS (NSOpenPanel) and Linux (XDG portal / GTK) dialogs are e15's, exactly as the
    // window backend leaves non-Windows an honest gap until e12. Returning nullopt reads on the welcome
    // screen as "the picker is not available on this platform yet" rather than a silent failure; the
    // recents list and "New from template" still work everywhere.
    return std::nullopt;
}

#endif

} // namespace context::editor::shell
