// Pinned extern-"C" ABI shims from the rusty_v8 static archive (v149.4.0 / V8 14.9.207.2).
//
// The hybrid host (issue #76 TD ruling): every published rusty_v8 prebuilt is
// use_custom_libcxx=true — Chromium's bundled libc++ with the `__Cr` ABI namespace. So any
// v8:: C++ API whose signature embeds an STL type (mangled std::__Cr) CANNOT be linked by a
// system-STL embedder (libstdc++ / MSVC-STL / system libc++) on ANY CI leg. These extern-"C"
// shims are DEFINED in the same static archive, their names are unmangled, and their
// signatures carry no by-value STL types — so they are ABI-namespace-immune and link from a
// system-STL translation unit. They cover exactly the two STL-crossing ops task 2a needs:
// platform boot, and the shape-B VM-allocated backing store. The STL-free MAJORITY of the
// v8:: API (Isolate / HandleScope / Context / Script / String / FunctionTemplate) links fine
// directly and is used as the real v8:: API in v8_engine.cpp.
//
// Signatures verified verbatim against
//   https://github.com/denoland/rusty_v8/blob/v149.4.0/src/binding.cc
// (line numbers below are that file's). Keep this header in lockstep with the pinned
// rusty_v8 version in tools/v8-prebuilt.json — a shim signature is an ABI contract.

#pragma once

#include <cstddef>

namespace v8
{
class Isolate;
class Platform;
class BackingStore;
}  // namespace v8

extern "C"
{
    // binding.cc:3266 — v8::platform::NewDefaultPlatform(thread_pool_size, idle?, ...).release()
    // Returns a process-owned raw v8::Platform* (kDisabled in-process stack dumping, no
    // tracing controller). thread_pool_size 0 = default to hardware concurrency.
    v8::Platform* v8__Platform__NewDefaultPlatform(int thread_pool_size, bool idle_task_support);

    // binding.cc:1046 — v8::ArrayBuffer::NewBackingStore(isolate, byte_length).release()
    // Returns a raw, caller-owned v8::BackingStore* allocated INSIDE the V8 sandbox address
    // space (the shape-B requirement). Free with v8__BackingStore__DELETE.
    v8::BackingStore* v8__ArrayBuffer__NewBackingStore__with_byte_length(v8::Isolate* isolate,
                                                                         std::size_t byte_length);

    // binding.cc:1092 / 1096 — accessors on the raw backing store.
    void* v8__BackingStore__Data(const v8::BackingStore& self);
    std::size_t v8__BackingStore__ByteLength(const v8::BackingStore& self);

    // binding.cc:1104 — `delete self` (runs the deleter inside the archive's ABI, freeing the
    // sandbox-interior allocation).
    void v8__BackingStore__DELETE(v8::BackingStore* self);
}
