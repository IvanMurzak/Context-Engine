// Internal factory for the R-OBS-005 interactive CDP inspector session (issue #94 / L-61). Kept
// out of the public js_engine.h so the STL-only seam header never names a v8:: type: this header
// is compiled ONLY into the V8 backend (CONTEXT_JS_HAS_V8 — inspector.cpp + v8_engine.cpp), so it
// may carry v8:: forward declarations. The session it builds subclasses V8's in-box inspector
// (v8-inspector.h) and shares the caller's Isolate + Context; it MUST NOT outlive them.

#pragma once

#include <memory>
#include <string>

#include "context/runtime/js/js_engine.h"

namespace v8
{
class Isolate;
class Context;
template <class T>
class Global;
} // namespace v8

namespace context::runtime::js::detail
{

// Build a CDP inspector session over `isolate` + `context` (the engine's own handles). Returns
// nullptr + fills `err` on failure (V8Inspector::connect returning null). The returned session
// registers `context` with the inspector, connects a fully-trusted CDP session, and drives a
// synchronous nested pause loop — see InspectorSession (js_engine.h) for the embedder contract.
std::unique_ptr<InspectorSession> createInspectorSession(v8::Isolate* isolate,
                                                         const v8::Global<v8::Context>& context,
                                                         std::string& err);

} // namespace context::runtime::js::detail
