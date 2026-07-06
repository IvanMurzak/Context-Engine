// Factory for the in-process V8 JS host (issue #76 / L-61). Kept apart from js_engine.h so
// consumers can depend on the backend-agnostic interface without naming the V8 backend.

#pragma once

#include <memory>
#include <string>

#include "context/runtime/js/js_engine.h"

namespace context::runtime::js
{

// true when the V8 backend was compiled into this build. It is CI-only for its dependency
// path: the rusty_v8 prebuilt is an MSVC/Clang-ABI static lib, so the local Strawberry-GCC
// Windows `dev` gate cannot link it and builds a stub (createV8Engine returns nullptr). The
// 3-OS CI build legs are the authoritative gate (profile setup.md/test.md carve-out).
bool v8BackendAvailable();

// Create the in-process V8 JS host. Returns nullptr and fills `err` when the backend is not
// built on this toolchain (v8BackendAvailable() == false).
std::unique_ptr<JsEngine> createV8Engine(std::string& err);

}  // namespace context::runtime::js
