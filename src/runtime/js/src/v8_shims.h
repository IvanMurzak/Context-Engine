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
class ArrayBuffer;
}  // namespace v8

// A 16-byte POD image of an ABI-namespace (`__Cr`) `std::shared_ptr<v8::BackingStore>` — the layout
// rusty_v8 itself boxes/unboxes shared_ptrs as (its `two_pointers_t`;
// `static_assert(sizeof(two_pointers_t) == sizeof(std::shared_ptr<v8::BackingStore>))`). Chromium's
// bundled libc++ lays a shared_ptr out as { element_type* __ptr_; __shared_weak_count* __cntrl_; },
// in that order, on EVERY target (the prebuilt is use_custom_libcxx=true, so the shared_ptr ABI is
// libc++'s on Linux/macOS/Windows alike — independent of the embedder's system STL). A control block
// of nullptr makes it a NON-OWNING shared_ptr: libc++'s copy/destroy guard every refcount op with
// `if (__cntrl_ != nullptr)`, so V8 co-owning this store via a null-cntrl shared_ptr never frees it
// — the host stays the sole owner of the raw BackingStore (freed by v8__BackingStore__DELETE). This
// lets the STL-free host hand a raw `v8::BackingStore*` to the STL-crossing ArrayBuffer factory
// below without ever constructing a system-STL shared_ptr in this translation unit.
struct BackingStoreSharedPtrImage
{
    void* ptr = nullptr;   // __ptr_  — the raw v8::BackingStore*
    void* cntrl = nullptr; // __cntrl_ — nullptr => non-owning (V8 never frees the store)
};

extern "C"
{
    // binding.cc:3266 — v8::platform::NewDefaultPlatform(thread_pool_size, idle?, ...).release()
    // Returns a process-owned raw v8::Platform* (kDisabled in-process stack dumping, no
    // tracing controller). thread_pool_size 0 = default to hardware concurrency. This is the
    // MULTI-THREADED default platform the host boots (v8_engine.cpp): it owns the background task
    // runner that Isolate teardown's delayed heap-release task requires. (The single-threaded
    // variant was tried to silence TSan and instead null-deref-SEGV'd during Isolate::Dispose on
    // every V8 leg; the TSan race is now handled by a suppressions file, not a platform swap.)
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

    // v8::ArrayBuffer::New(isolate, backing_store) — wraps an EXISTING backing store in a fresh
    // ArrayBuffer (the R-LANG-009 per-system attach over the stable shape-B store). rusty_v8's
    // signature is `const v8::ArrayBuffer* v8__ArrayBuffer__New__with_backing_store(v8::Isolate*,
    // const std::shared_ptr<v8::BackingStore>&)` (verified verbatim against
    // denoland/rusty_v8@v149.4.0 src/binding.cc). A `const T&` parameter is ABI-passed as a pointer,
    // so it is declared here taking a pointer to the 16-byte shared_ptr image above — keeping
    // std::shared_ptr out of this system-STL translation unit while matching the ABI exactly. The
    // returned pointer is the Local's handle-slot address (rusty_v8's `local_to_ptr`), reconstituted
    // into a v8::Local by the caller (valid only inside the live HandleScope).
    const v8::ArrayBuffer*
    v8__ArrayBuffer__New__with_backing_store(v8::Isolate* isolate,
                                             const BackingStoreSharedPtrImage* backing_store);
}
