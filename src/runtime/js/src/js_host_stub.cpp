// Stub in-process JS host for toolchains that cannot link the rusty_v8 MSVC/Clang-ABI
// prebuilt — the local Strawberry-GCC Windows `dev` gate (profile setup.md/test.md carve-out:
// Strawberry GCC cannot link an MSVC-ABI V8 archive). This keeps the local NON-dependency
// build + `ctest --preset dev` green; the 3-OS CI build legs (Linux-x64 clang / macOS-ARM64
// clang / Win-x64 MSVC) are the AUTHORITATIVE gate for the real backend. Compiled only when
// CONTEXT_JS_HAS_V8 is NOT defined.

#include "context/runtime/js/js_host.h"

namespace context::runtime::js
{

bool v8BackendAvailable() { return false; }

std::unique_ptr<JsEngine> createV8Engine(std::string& err)
{
    err = "V8 backend not built on this toolchain (the rusty_v8 prebuilt is MSVC/Clang-ABI; "
          "the 3-OS CI build legs are the authoritative gate)";
    return nullptr;
}

}  // namespace context::runtime::js
