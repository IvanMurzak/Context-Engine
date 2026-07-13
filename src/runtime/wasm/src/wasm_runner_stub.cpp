// WasmRunner — the honest STUB backend (issue #71 PR3), built where the MSVC/Clang-ABI wasmtime
// prebuilt cannot link (the local Strawberry-GCC Windows dev host — the context_js stub split;
// setup.md § Preconditions). It links NO wasm runtime and runs NO wasm: create() REFUSES with a
// clear problem, so an embedder on this toolchain injects nothing and the migrate seam keeps its
// honest migration.runner_unavailable refusal — a stub must never impersonate a sandbox. The
// run_step/map_path bodies exist only so the class is complete; they are unreachable through
// create() and answer honestly if constructed by force.

#include "context/runtime/wasm/wasm_runner.h"

#include <utility>

namespace context::runtime::wasm
{

struct WasmRunner::Impl
{
};

std::unique_ptr<WasmRunner> WasmRunner::create(ModuleResolver resolver, std::string& problem)
{
    (void)resolver;
    problem = "this build carries no wasmtime runtime (CONTEXT_BUILD_WASM stub backend — the "
              "prebuilt cannot link under this toolchain; the 3-OS CI wasm-runner legs are the "
              "authoritative gate). Inject no runner: the migrate seam's "
              "migration.runner_unavailable refusal is the honest behavior here.";
    return nullptr;
}

bool WasmRunner::runtime_available() noexcept
{
    return false;
}

WasmRunner::WasmRunner(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

WasmRunner::~WasmRunner() = default;

editor::migrate::SandboxedStepResult
WasmRunner::run_step(const editor::migrate::SandboxedStep& step, std::string_view input)
{
    (void)step;
    (void)input;
    editor::migrate::SandboxedStepResult result;
    result.detail = "wasm runtime unavailable (stub backend)";
    return result;
}

std::optional<std::string> WasmRunner::map_path(const editor::migrate::SandboxedStep& step,
                                                std::string_view pointer)
{
    (void)step;
    return std::string(pointer);
}

} // namespace context::runtime::wasm
