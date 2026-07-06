// R-OBS-005 CDP inspector seam (STUB — task 2a wires the header + reserves a home; it does
// NOT build the debugger).
//
// v8-inspector.h ships in the pinned V8 headers (v8-149.4.0.crate) and rusty_v8's archive
// carries the V8 inspector implementation. This is the minimal, REAL, instantiable
// V8InspectorClient the future TypeScript source-mapped debugger plugs into: the debugger
// task adds a v8_inspector::V8Inspector (created via V8Inspector::create — an std::unique_ptr
// return, i.e. an STL-crossing op that belongs to that task), a v8_inspector::V8Inspector
// ::Channel over a CDP/WebSocket transport, and the runMessageLoopOnPause / quitMessageLoop
// OnPause pause loop. Task 2a deliberately instantiates nothing beyond this client stub, so
// the seam stays link-safe across the hybrid libc++ ABI boundary (no by-value STL crossing).

#pragma once

#include <v8-inspector.h>

namespace context::runtime::js::detail
{

class V8InspectorSeam final : public v8_inspector::V8InspectorClient
{
public:
    // A single link-safe override (double return, no STL-crossing signature) so the subclass
    // is a real, instantiable client. The debugger task fills in the pause loop + a channel.
    double currentTimeMS() override { return 0.0; }
};

}  // namespace context::runtime::js::detail
