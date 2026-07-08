// Internal factory for the R-OBS-005 interactive CDP inspector session (issue #94 / L-61). Kept
// out of the public js_engine.h so the STL-only seam header never names a v8:: type: this header
// is compiled ONLY into the V8 backend (CONTEXT_JS_HAS_V8 — inspector.cpp + v8_engine.cpp), so it
// may carry v8:: forward declarations. The session it builds subclasses V8's in-box inspector
// (v8-inspector.h) and shares the caller's Isolate + Context; it MUST NOT outlive them.

#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "context/runtime/js/js_engine.h"

namespace v8
{
class Isolate;
class Context;
class TryCatch;
template <class T>
class Local;
template <class T>
class Global;
} // namespace v8

namespace context::runtime::js::detail
{

// Describe a caught exception as a diagnostic string, preferring the full error.stack (so the
// R-OBS-005 TS-source-map remapper sees the raw JS frames and can resolve them to authored .ts
// positions) and falling back to the bare message / stringified non-Error primitive. Shared by
// V8Engine::eval and the inspector session's run(); requires an active Isolate + Context scope at
// the call site (both callers hold one).
std::string describeException(v8::Isolate* isolate, v8::Local<v8::Context> context,
                              const v8::TryCatch& tryCatch);

// Compile `code` and run it in `context` (the engine's own Global handle). Establishes the
// Isolate / Handle / Context scopes internally. When `resourceName` is non-empty it is attached as
// the ScriptOrigin — the CDP script URL Debugger.setBreakpointByUrl targets and
// Debugger.scriptParsed reports; when empty no origin is attached. When `numResult` is non-null and
// the result is numeric it is written there. Returns false + fills `err` (via describeException) on
// a compile or run failure. Shared by V8Engine::eval and InspectorSession::run.
bool compileAndRun(v8::Isolate* isolate, const v8::Global<v8::Context>& context,
                   std::string_view code, std::string_view resourceName, double* numResult,
                   std::string& err);

// Build a CDP inspector session over `isolate` + `context` (the engine's own handles). Returns
// nullptr + fills `err` on failure (V8Inspector::connect returning null). The returned session
// registers `context` with the inspector, connects a fully-trusted CDP session, and drives a
// synchronous nested pause loop — see InspectorSession (js_engine.h) for the embedder contract.
std::unique_ptr<InspectorSession> createInspectorSession(v8::Isolate* isolate,
                                                         const v8::Global<v8::Context>& context,
                                                         std::string& err);

} // namespace context::runtime::js::detail
