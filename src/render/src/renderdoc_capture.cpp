// The L-47 RenderDoc capture hook implementation (see renderdoc_capture.h). Pure module-handle +
// function-pointer plumbing; no GPU/wgpu. Binds only to an ALREADY-injected RenderDoc module
// (RenderDoc injects itself into the process it launches), so an ordinary/headless/CI run finds no
// module and stays a no-op.

#include "context/render/renderdoc_capture.h"

#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace context::render
{
namespace
{
// RENDERDOC_GetAPI(RENDERDOC_Version, void** outAPIPointers) -> int (1 on success). Version enum
// value eRENDERDOC_API_Version_1_1_2 == 10102.
using GetApiFn = int (*)(int version, void** out_api);
constexpr int kApiVersion_1_1_2 = 10102;

// Resolve RENDERDOC_GetAPI from the RenderDoc module if (and only if) it is already loaded into this
// process. Returns nullptr when RenderDoc is not attached — never loads it. The resolved symbol is
// copied into the function-pointer typedef with memcpy: a dlsym()/GetProcAddress() result crossing
// the object-pointer/function-pointer boundary would trip Clang/GCC -Wpedantic (function<->object
// reinterpret_cast) on the ubuntu/macOS legs, which the local Windows GCC gate cannot see; memcpy of
// two same-sized pointers is warning-clean on every toolchain.
GetApiFn resolve_get_api() noexcept
{
    GetApiFn fn = nullptr;
#if defined(_WIN32)
    // GetModuleHandle does NOT load the DLL — it returns a handle only if RenderDoc already injected
    // renderdoc.dll into this process.
    HMODULE mod = ::GetModuleHandleA("renderdoc.dll");
    if (mod == nullptr)
        return nullptr;
    FARPROC sym = ::GetProcAddress(mod, "RENDERDOC_GetAPI");
    if (sym == nullptr)
        return nullptr;
    std::memcpy(&fn, &sym, sizeof(fn));
#else
    // RTLD_NOLOAD resolves the handle ONLY if librenderdoc.so is already loaded (injected) — it does
    // not load it. RTLD_NOW binds symbols eagerly.
    void* mod = ::dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);
    if (mod == nullptr)
        return nullptr;
    void* sym = ::dlsym(mod, "RENDERDOC_GetAPI");
    // Drop our reference count (RTLD_NOLOAD took one); RenderDoc keeps the module alive.
    ::dlclose(mod);
    if (sym == nullptr)
        return nullptr;
    std::memcpy(&fn, &sym, sizeof(fn));
#endif
    return fn;
}
} // namespace

bool RenderDocCapture::load()
{
    if (api_ != nullptr)
        return true; // already bound (idempotent)

    const GetApiFn get_api = resolve_get_api();
    if (get_api == nullptr)
        return false;

    void* api = nullptr;
    if (get_api(kApiVersion_1_1_2, &api) != 1 || api == nullptr)
        return false;

    api_ = static_cast<Api*>(api);
    return true;
}

void RenderDocCapture::api_version(int& out_major, int& out_minor, int& out_patch) const noexcept
{
    out_major = 0;
    out_minor = 0;
    out_patch = 0;
    if (api_ != nullptr && api_->GetAPIVersion != nullptr)
        api_->GetAPIVersion(&out_major, &out_minor, &out_patch);
}

void RenderDocCapture::begin_frame_capture() noexcept
{
    if (api_ != nullptr && api_->StartFrameCapture != nullptr)
        api_->StartFrameCapture(nullptr, nullptr); // active device, all windows
}

void RenderDocCapture::end_frame_capture() noexcept
{
    if (api_ != nullptr && api_->EndFrameCapture != nullptr)
        api_->EndFrameCapture(nullptr, nullptr);
}

void RenderDocCapture::trigger_capture() noexcept
{
    if (api_ != nullptr && api_->TriggerCapture != nullptr)
        api_->TriggerCapture();
}

} // namespace context::render
