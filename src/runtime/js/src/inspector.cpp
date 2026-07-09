// R-OBS-005 interactive CDP inspector session over V8's in-box inspector (issue #94 / L-61: this is
// CONFIGURATION of v8-inspector.h, NOT building a debugger from scratch). A CDP client can attach,
// set a source-mapped breakpoint, hit it, and resume/step. Compiled ONLY on toolchains that can link
// the rusty_v8 prebuilt (CONTEXT_JS_HAS_V8 — the 3-OS CI build legs); the local Strawberry-GCC
// Windows dev gate builds the js stub instead (js_host_stub.cpp), which offers no inspector.
//
// CROSS-ABI SEAM (issue #104 — the fix for the attach-time SEGV):
//   The rusty_v8 prebuilt is use_custom_libcxx=true — every v8_inspector class it defines carries a
//   vtable laid out and dispatched inside the archive's Chromium-libc++ (`__Cr`) ABI. DIRECTLY
//   subclassing v8_inspector::V8InspectorClient / V8Inspector::Channel from this system-STL TU (the
//   prior task-2b approach) emitted OUR vtable, and when V8's V8InspectorImpl ctor called back into
//   the client (e.g. m_client->generateUniqueId(), v8::debug::SetIsolateId()), the cross-ABI virtual
//   dispatch faulted — the SEGV read @0x100 this file previously mis-attributed to a lazy Debug
//   object. Instead we route through rusty_v8's OWN extern-"C" __BASE seam (identical technique to
//   the V8-embed backing-store shims in v8_shims.h): the archive-defined
//   v8_inspector__{V8InspectorClient,Channel}__BASE subclass is placement-constructed INSIDE the
//   archive (its vtable is the archive's, so V8's callbacks dispatch correctly), and every virtual it
//   overrides forwards OUT to an extern-"C" v8_inspector__..._BASE__<method> thunk — unmangled, no
//   by-value STL in the signature, hence ABI-namespace-immune from this system-STL TU. Those thunks
//   are IMPLEMENTED below and carry the real client/channel logic; they were previously fail-closed
//   traps in v8_rust_stubs.cpp (now removed from that trap set — a symbol is defined exactly once).
//   The __BASE object is the FIRST member (offset 0) of our SeamClient / SeamChannel, so the
//   V8InspectorClient* / Channel* V8 hands back to a thunk casts straight to our state — the same
//   containing-struct trick rusty_v8 uses when the client is implemented from Rust.
//
//   V8Inspector::create / connect ALSO cross through the archive's extern-"C" seam
//   (v8_inspector__V8Inspector__{create,connect}): they RETURN a std::unique_ptr, and consuming that
//   Chromium-libc++ smart-pointer return from this system-STL TU is NOT ABI-safe — the returned
//   pointer arrives null (issue #104: "V8Inspector::create returned null" on the macOS/Windows CI
//   legs, where the link succeeds but the attach fails at runtime; an earlier belief that a
//   unique_ptr return was ABI-immune was WRONG). The archive's __create / __connect shims .release()
//   the unique_ptr INSIDE the archive and hand back a bare pointer we own and free via
//   v8_inspector__V8Inspector__DELETE / v8_inspector__V8InspectorSession__DELETE (a system operator
//   delete would be a cross-heap free of an object allocated in the archive's libc++ heap). By
//   contrast contextCreated / contextDestroyed / dispatchProtocolMessage stay DIRECT virtual calls:
//   they return void and take only by-value-POD (V8ContextInfo) / single-slot-Local / StringView
//   args — no by-value STL, no smart-pointer return — so that direction genuinely is ABI-immune.
//   Net: smart-pointer RETURNS and the CALLBACK (V8-vtable-into-us) direction both go through the
//   seam; the void-returning, POD-argument outbound calls do not need it.
//
// Signatures of the archive-side seam functions were verified verbatim against
//   https://github.com/denoland/rusty_v8/blob/v149.4.0/src/binding.cc
// (the pin in tools/v8-prebuilt.json). Keep them in lockstep with that pin — a __BASE thunk
// signature is an ABI contract.

#include "inspector_session.h"

#include "context/runtime/js/js_engine.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <v8-context.h>
#include <v8-exception.h>
#include <v8-inspector.h>
#include <v8-isolate.h>
#include <v8-local-handle.h>
#include <v8-message.h>
#include <v8-persistent-handle.h>
#include <v8-primitive.h>
#include <v8-script.h>

// rusty_v8 archive-provided extern-"C" seam entry points we CALL. CONSTRUCT placement-builds the
// archive-side __BASE subclass into our buffer (declared void* here — the archive's parameter is an
// `uninit_t<__BASE>*`, ABI-identical to a plain pointer). __create / __connect wrap
// V8Inspector::create / V8Inspector::connect: they .release() the archive's std::unique_ptr and
// return a BARE pointer — the ABI-safe alternative to consuming a Chromium-libc++ unique_ptr RETURN
// in this system-STL TU (which arrives null, issue #104). The __DELETE entry points free objects
// allocated in the archive's libc++ heap (a system operator delete would be a cross-heap mismatch);
// StringBuffer__DELETE does the same for a StringBuffer. All are defined inside the pinned librusty_v8
// archive — signatures verified verbatim against the v149.4.0 binding.cc pin (tools/v8-prebuilt.json,
// the SAME source the __BASE thunk signatures below were checked against); do not redefine.
extern "C"
{
    void v8_inspector__V8InspectorClient__BASE__CONSTRUCT(void* buf);
    void v8_inspector__V8Inspector__Channel__BASE__CONSTRUCT(void* buf);
    void v8_inspector__StringBuffer__DELETE(v8_inspector::StringBuffer* self);

    v8_inspector::V8Inspector* v8_inspector__V8Inspector__create(
        v8::Isolate* isolate, v8_inspector::V8InspectorClient* client);
    void v8_inspector__V8Inspector__DELETE(v8_inspector::V8Inspector* self);
    v8_inspector::V8InspectorSession* v8_inspector__V8Inspector__connect(
        v8_inspector::V8Inspector* self, int context_group_id,
        v8_inspector::V8Inspector::Channel* channel, v8_inspector::StringView state,
        v8_inspector::V8Inspector::ClientTrustLevel client_trust_level);
    void v8_inspector__V8InspectorSession__DELETE(v8_inspector::V8InspectorSession* self);
}

namespace context::runtime::js::detail
{
namespace
{

// The single context group this host exposes to the inspector (must be non-zero, V8ContextInfo).
constexpr int kContextGroupId = 1;

// Bounds the nested pause loop so a pump that never resumes cannot hang the process (defensive; the
// documented contract is that the pump dispatches a resume/step command each pause). Far above any
// legitimate step count for a single breakpoint interaction.
constexpr int kMaxPauseIterations = 100000;

v8_inspector::StringView toStringView(std::string_view s)
{
    return v8_inspector::StringView(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

// Convert an inspector StringBuffer's contents to a UTF-8 std::string. V8 emits its CDP JSON as
// 8-bit (Latin-1/UTF-8) buffers in practice; the 16-bit branch encodes the BMP (+ surrogate pairs)
// defensively so a non-ASCII payload never corrupts.
std::string fromStringView(const v8_inspector::StringView& v)
{
    if (v.is8Bit())
    {
        return std::string(reinterpret_cast<const char*>(v.characters8()), v.length());
    }
    const std::uint16_t* p = v.characters16();
    std::string out;
    out.reserve(v.length());
    for (std::size_t i = 0; i < v.length(); ++i)
    {
        std::uint32_t c = p[i];
        // Combine a high+low surrogate pair into a single astral code point.
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < v.length())
        {
            const std::uint32_t lo = p[i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF)
            {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            }
        }
        if (c < 0x80)
        {
            out.push_back(static_cast<char>(c));
        }
        else if (c < 0x800)
        {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
        else if (c < 0x10000)
        {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
        else
        {
            out.push_back(static_cast<char>(0xF0 | (c >> 18)));
            out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

// The embedder-side CDP client. Its `base` member is the archive-constructed
// v8_inspector__V8InspectorClient__BASE (a real V8InspectorClient whose vtable lives in the archive);
// V8's virtual callbacks land in the archive vtable and forward through the extern-"C"
// v8_inspector__V8InspectorClient__BASE__* thunks below, which recover this struct from the client
// pointer (base is at offset 0). runMessageLoopOnPause is the synchronous nested pause loop the
// embedder drives via the pump.
struct SeamClient
{
    // MUST be the first member (offset 0). sizeof/alignof match the base class because the archive's
    // __BASE subclass adds no data members (it only overrides virtuals), so `sizeof(__BASE)` ==
    // `sizeof(V8InspectorClient)` — one vtable pointer.
    alignas(v8_inspector::V8InspectorClient) unsigned char base[sizeof(v8_inspector::V8InspectorClient)];

    v8::Isolate* isolate = nullptr;
    v8::Global<v8::Context>* context = nullptr;
    std::function<bool()>* pump = nullptr;
    bool paused = false;

    SeamClient(v8::Isolate* iso, v8::Global<v8::Context>* ctx, std::function<bool()>* p)
        : isolate(iso), context(ctx), pump(p)
    {
        v8_inspector__V8InspectorClient__BASE__CONSTRUCT(&base);
    }
    ~SeamClient() { asClient()->~V8InspectorClient(); }

    SeamClient(const SeamClient&) = delete;
    SeamClient& operator=(const SeamClient&) = delete;

    v8_inspector::V8InspectorClient* asClient()
    {
        return reinterpret_cast<v8_inspector::V8InspectorClient*>(&base);
    }
};
static_assert(offsetof(SeamClient, base) == 0, "the __BASE storage must be at offset 0");

// The embedder-side CDP channel: every response + notification V8 produces is forwarded to the
// session's sink. Same __BASE seam as SeamClient.
struct SeamChannel
{
    alignas(v8_inspector::V8Inspector::Channel) unsigned char
        base[sizeof(v8_inspector::V8Inspector::Channel)];

    std::function<void(std::string_view)>* sink = nullptr;

    explicit SeamChannel(std::function<void(std::string_view)>* s) : sink(s)
    {
        v8_inspector__V8Inspector__Channel__BASE__CONSTRUCT(&base);
    }
    ~SeamChannel() { asChannel()->~Channel(); }

    SeamChannel(const SeamChannel&) = delete;
    SeamChannel& operator=(const SeamChannel&) = delete;

    v8_inspector::V8Inspector::Channel* asChannel()
    {
        return reinterpret_cast<v8_inspector::V8Inspector::Channel*>(&base);
    }

    void emit(const v8_inspector::StringBuffer* buffer)
    {
        if (sink != nullptr && *sink && buffer != nullptr)
        {
            (*sink)(fromStringView(buffer->string()));
        }
    }
};
static_assert(offsetof(SeamChannel, base) == 0, "the __BASE storage must be at offset 0");

// v8_inspector__V8Inspector__{create,connect} hand back objects allocated inside the archive's libc++
// heap (the shims .release() the archive's std::unique_ptr). They MUST be freed by the archive's
// matching __DELETE — a system operator delete would be a cross-heap free. These custom deleters route
// unique_ptr teardown back through that archive seam.
struct InspectorDeleter
{
    void operator()(v8_inspector::V8Inspector* p) const
    {
        if (p != nullptr)
        {
            v8_inspector__V8Inspector__DELETE(p);
        }
    }
};

struct SessionDeleter
{
    void operator()(v8_inspector::V8InspectorSession* p) const
    {
        if (p != nullptr)
        {
            v8_inspector__V8InspectorSession__DELETE(p);
        }
    }
};

class SessionImpl final : public InspectorSession
{
public:
    SessionImpl(v8::Isolate* isolate, const v8::Global<v8::Context>& context) : isolate_(isolate)
    {
        context_.Reset(isolate_, context);
    }

    bool init(std::string& err)
    {
        // Materialize the isolate's Debug subsystem before wiring the inspector. V8's
        // V8InspectorImpl ctor runs v8::debug::SetIsolateId(isolate, ...) ->
        // isolate->debug()->SetIsolateId(id); on this rusty_v8 prebuilt isolate->debug() is created
        // lazily on the first compile, and our attach path targets an isolate that may not have run
        // any script yet. Compiling+running a trivial script forces V8 down its debug-aware compile
        // path so isolate->debug() exists before V8Inspector::create touches it. Cheap insurance,
        // retained ALONGSIDE the primary fix (the __BASE ABI seam, issue #104 — the header comment):
        // the earlier belief that this warm-up alone resolved the attach SEGV was wrong (main stayed
        // red), but a live Debug object is still a genuine precondition of a clean attach.
        {
            std::string warmErr;
            if (!compileAndRun(isolate_, context_, "0", {}, nullptr, warmErr))
            {
                err = "failed to materialize the isolate debug subsystem before inspector attach: " +
                      warmErr;
                return false;
            }
        }

        v8::Isolate::Scope isolateScope(isolate_);
        v8::HandleScope handleScope(isolate_);
        v8::Local<v8::Context> context = context_.Get(isolate_);
        // Enter the engine's context so the V8Inspector::create / contextCreated / connect handle
        // operations below run against a live, entered context (matching dispatch()/run() and
        // V8Engine's eval/getFunction/...).
        v8::Context::Scope contextScope(context);

        client_ = std::make_unique<SeamClient>(isolate_, &context_, &pump_);
        // create() returns std::unique_ptr from the archive's Chromium-libc++ ABI; consuming that
        // return in this system-STL TU arrives null (issue #104). Go through the archive's raw-pointer
        // shim, which .release()s inside the archive; InspectorDeleter frees it via the archive's
        // __DELETE (header comment).
        inspector_.reset(v8_inspector__V8Inspector__create(isolate_, client_->asClient()));
        if (!inspector_)
        {
            err = "v8_inspector__V8Inspector__create returned null";
            return false;
        }
        channel_ = std::make_unique<SeamChannel>(&sink_);

        const std::string name = "context-engine";
        // contextCreated takes a by-value V8ContextInfo (trivially copyable, no by-value STL) and
        // returns void, so the direct virtual call into the archive is ABI-safe (unlike the
        // unique_ptr-returning create/connect above/below).
        v8_inspector::V8ContextInfo info(context, kContextGroupId, toStringView(name));
        inspector_->contextCreated(info);

        // connect() likewise returns std::unique_ptr — same ABI hazard, same raw-pointer shim. The
        // shim omits the trailing SessionPauseState arg, defaulting it to kNotWaitingForDebugger inside
        // the archive (the value the prior direct call passed explicitly).
        session_.reset(v8_inspector__V8Inspector__connect(
            inspector_.get(), kContextGroupId, channel_->asChannel(), v8_inspector::StringView(),
            v8_inspector::V8Inspector::kFullyTrusted));
        if (!session_)
        {
            err = "v8_inspector__V8Inspector__connect returned null";
            return false;
        }
        return true;
    }

    void onMessage(std::function<void(std::string_view)> sink) override { sink_ = std::move(sink); }
    void onPause(std::function<bool()> pump) override { pump_ = std::move(pump); }

    bool dispatch(std::string_view cdpMessage, std::string& err) override
    {
        if (cdpMessage.empty())
        {
            err = "empty CDP message";
            return false;
        }
        v8::Isolate::Scope isolateScope(isolate_);
        v8::HandleScope handleScope(isolate_);
        v8::Local<v8::Context> context = context_.Get(isolate_);
        v8::Context::Scope contextScope(context);
        session_->dispatchProtocolMessage(toStringView(cdpMessage));
        return true;
    }

    bool run(std::string_view code, std::string_view resourceName, std::string& err) override
    {
        // Shares the compile+run skeleton with V8Engine::eval; the non-empty resourceName attaches
        // the ScriptOrigin (CDP script URL) and no numeric result is extracted.
        return compileAndRun(isolate_, context_, code, resourceName, nullptr, err);
    }

    ~SessionImpl() override
    {
        // Tear the inspector down inside an Isolate scope and BEFORE the Isolate is disposed (the
        // engine, which owns the Isolate, outlives this session per the attachInspector contract).
        // Disconnect the session, then pair init()'s contextCreated with a contextDestroyed so the
        // inspector releases its per-context bookkeeping before the inspector itself is destroyed.
        v8::Isolate::Scope isolateScope(isolate_);
        v8::HandleScope handleScope(isolate_);
        v8::Local<v8::Context> context = context_.Get(isolate_);
        // Mirror init(): enter the context so the session disconnect + contextDestroyed unwind the
        // same per-context inspector/debug state they were registered against.
        v8::Context::Scope contextScope(context);
        session_.reset();
        if (inspector_)
        {
            inspector_->contextDestroyed(context);
        }
        inspector_.reset();
        context_.Reset();
        // Destroy the channel + client (the SeamClient/SeamChannel) HERE, while the Isolate/Handle/
        // Context scope guards declared above are still alive: ~SeamChannel()/~SeamClient() dispatch
        // virtually into the archive's __BASE vtable, so they must run inside the same isolate scope as
        // the rest of teardown — NOT in the compiler's member-destruction epilogue, which fires after
        // these guards unwind (at the closing brace) and after context_ is reset. Reset after
        // inspector_ so the inspector is never holding them when it is torn down; the unique_ptrs are
        // left null, so the trailing epilogue destruction is a no-op.
        channel_.reset();
        client_.reset();
    }

private:
    v8::Isolate* isolate_;
    v8::Global<v8::Context> context_;
    std::function<void(std::string_view)> sink_;
    std::function<bool()> pump_;
    std::unique_ptr<SeamClient> client_;
    std::unique_ptr<v8_inspector::V8Inspector, InspectorDeleter> inspector_;
    std::unique_ptr<SeamChannel> channel_;
    std::unique_ptr<v8_inspector::V8InspectorSession, SessionDeleter> session_;
};

} // namespace

// ---- extern-"C" __BASE callback implementations ------------------------------------------------
// The archive's __BASE subclass forwards V8's virtual calls here across the STL-free, unmangled
// seam. `self` is the address of the __BASE object, which is the offset-0 `base` member of our
// SeamClient/SeamChannel, so the reinterpret_cast recovers the owning struct. These REPLACE the
// fail-closed traps that lived in v8_rust_stubs.cpp for these ten symbols (the header comment).
//
// LINK-ORDER SAFETY (why defining them here, not in the WHOLE_ARCHIVE'd stub lib, is safe): unlike
// the pure trap-only stub TU — which nothing else references, so the single-pass GNU-ld/lld legs
// need --whole-archive to keep it — inspector.o is unconditionally pulled into the link by
// createInspectorSession (referenced from v8_engine.cpp's attachInspector). Once inspector.o is in,
// all ten definitions are present, and librusty_v8.a's undefined references to them resolve as
// usual. Verified against the CI build + sanitize legs (the authoritative gate for the V8 path).
extern "C"
{
    std::int64_t v8_inspector__V8InspectorClient__BASE__generateUniqueId(
        v8_inspector::V8InspectorClient* /*self*/)
    {
        // Monotonic, process-global, positive. V8 uses these only to tag scripts/objects uniquely;
        // any never-repeating value is correct.
        static std::atomic<std::int64_t> counter{1};
        return counter.fetch_add(1, std::memory_order_relaxed);
    }

    void v8_inspector__V8InspectorClient__BASE__runMessageLoopOnPause(
        v8_inspector::V8InspectorClient* self, int /*contextGroupId*/)
    {
        auto* c = reinterpret_cast<SeamClient*>(self);
        c->paused = true;
        int guard = 0;
        while (c->paused && guard++ < kMaxPauseIterations)
        {
            if (c->pump == nullptr || !*c->pump)
            {
                break; // no pump installed: return so V8 continues rather than deadlocking
            }
            if ((*c->pump)())
            {
                break; // pump reports it issued a resume/step
            }
        }
        c->paused = false;
    }

    void v8_inspector__V8InspectorClient__BASE__quitMessageLoopOnPause(
        v8_inspector::V8InspectorClient* self)
    {
        reinterpret_cast<SeamClient*>(self)->paused = false;
    }

    void v8_inspector__V8InspectorClient__BASE__runIfWaitingForDebugger(
        v8_inspector::V8InspectorClient* /*self*/, int /*contextGroupId*/)
    {
    }

    void v8_inspector__V8InspectorClient__BASE__consoleAPIMessage(
        v8_inspector::V8InspectorClient* /*self*/, int /*contextGroupId*/,
        v8::Isolate::MessageErrorLevel /*level*/, const v8_inspector::StringView& /*message*/,
        const v8_inspector::StringView& /*url*/, unsigned /*lineNumber*/, unsigned /*columnNumber*/,
        v8_inspector::V8StackTrace* /*stackTrace*/)
    {
        // Console output routing is out of scope for the attach/breakpoint DoD; intentionally no-op.
    }

    v8::Context* v8_inspector__V8InspectorClient__BASE__ensureDefaultContextInGroup(
        v8_inspector::V8InspectorClient* self, int /*context_group_id*/)
    {
        // Return the engine's context in rusty_v8's raw-slot form (the __BASE override wraps it back
        // into a v8::Local via ptr_to_local). A v8::Local is a single slot pointer, so the raw form
        // is its bit pattern — the same Local<->ptr reinterpret the v8_shims.h backing-store seam uses.
        static_assert(sizeof(v8::Local<v8::Context>) == sizeof(v8::Context*),
                      "v8::Local must be a single-slot pointer for the raw-slot round-trip");
        auto* c = reinterpret_cast<SeamClient*>(self);
        v8::Local<v8::Context> ctx = c->context->Get(c->isolate);
        v8::Context* raw = nullptr;
        std::memcpy(&raw, &ctx, sizeof(raw));
        return raw;
    }

    v8_inspector::StringBuffer* v8_inspector__V8InspectorClient__BASE__resourceNameToUrl(
        v8_inspector::V8InspectorClient* /*self*/,
        const v8_inspector::StringView& /*resource_name_view*/)
    {
        // nullptr => no transformation: V8 keeps the resource name as the script URL (our CDP client
        // sets breakpoints against that same URL, e.g. context://breakpoint.js).
        return nullptr;
    }

    void v8_inspector__V8Inspector__Channel__BASE__sendResponse(
        v8_inspector::V8Inspector::Channel* self, int /*callId*/,
        v8_inspector::StringBuffer* message)
    {
        // `message` is a raw, caller-owned StringBuffer (the __BASE override called message.release()
        // before crossing the seam): emit its contents, then free it via the archive's deleter.
        reinterpret_cast<SeamChannel*>(self)->emit(message);
        v8_inspector__StringBuffer__DELETE(message);
    }

    void v8_inspector__V8Inspector__Channel__BASE__sendNotification(
        v8_inspector::V8Inspector::Channel* self, v8_inspector::StringBuffer* message)
    {
        reinterpret_cast<SeamChannel*>(self)->emit(message);
        v8_inspector__StringBuffer__DELETE(message);
    }

    void v8_inspector__V8Inspector__Channel__BASE__flushProtocolNotifications(
        v8_inspector::V8Inspector::Channel* /*self*/)
    {
    }
}

// Describe a caught exception, preferring the full error.stack (so the same TS-source-map remapper
// runtime/ts uses can resolve the JS frames), falling back to the bare message / stringified
// non-Error primitive. Shared by V8Engine::eval (which delegates its own describe() member here)
// and compileAndRun below.
std::string describeException(v8::Isolate* isolate, v8::Local<v8::Context> context,
                             const v8::TryCatch& tryCatch)
{
    v8::Local<v8::Value> stack;
    if (tryCatch.StackTrace(context).ToLocal(&stack) && !stack.IsEmpty() && stack->IsString())
    {
        v8::String::Utf8Value utf8Stack(isolate, stack);
        if (*utf8Stack != nullptr)
        {
            return *utf8Stack;
        }
    }
    v8::Local<v8::Value> ex = tryCatch.Exception();
    if (ex.IsEmpty())
    {
        return "<no exception>";
    }
    v8::Local<v8::String> str;
    if (!ex->ToString(context).ToLocal(&str))
    {
        return "<unprintable exception>";
    }
    v8::String::Utf8Value utf8(isolate, str);
    return *utf8 != nullptr ? *utf8 : "<utf8 conversion failed>";
}

bool compileAndRun(v8::Isolate* isolate, const v8::Global<v8::Context>& contextGlobal,
                   std::string_view code, std::string_view resourceName, double* numResult,
                   std::string& err)
{
    v8::Isolate::Scope isolateScope(isolate);
    v8::HandleScope handleScope(isolate);
    v8::Local<v8::Context> context = contextGlobal.Get(isolate);
    v8::Context::Scope contextScope(context);
    v8::TryCatch tryCatch(isolate);

    v8::Local<v8::String> src;
    if (!v8::String::NewFromUtf8(isolate, code.data(), v8::NewStringType::kNormal,
                                 static_cast<int>(code.size()))
             .ToLocal(&src))
    {
        err = "source string allocation failed";
        return false;
    }

    v8::Local<v8::Script> script;
    if (resourceName.empty())
    {
        if (!v8::Script::Compile(context, src).ToLocal(&script))
        {
            err = describeException(isolate, context, tryCatch);
            return false;
        }
    }
    else
    {
        v8::Local<v8::String> resource;
        if (!v8::String::NewFromUtf8(isolate, resourceName.data(), v8::NewStringType::kNormal,
                                     static_cast<int>(resourceName.size()))
                 .ToLocal(&resource))
        {
            err = "resource-name string allocation failed";
            return false;
        }
        // A ScriptOrigin carrying the resource name = the CDP script URL Debugger.setBreakpointByUrl
        // targets and Debugger.scriptParsed reports.
        v8::ScriptOrigin origin(resource);
        if (!v8::Script::Compile(context, src, &origin).ToLocal(&script))
        {
            err = describeException(isolate, context, tryCatch);
            return false;
        }
    }

    v8::Local<v8::Value> result;
    if (!script->Run(context).ToLocal(&result))
    {
        err = describeException(isolate, context, tryCatch);
        return false;
    }
    if (numResult != nullptr && result->IsNumber())
    {
        *numResult = result.As<v8::Number>()->Value();
    }
    return true;
}

std::unique_ptr<InspectorSession> createInspectorSession(v8::Isolate* isolate,
                                                         const v8::Global<v8::Context>& context,
                                                         std::string& err)
{
    auto session = std::make_unique<SessionImpl>(isolate, context);
    if (!session->init(err))
    {
        return nullptr;
    }
    return session;
}

} // namespace context::runtime::js::detail
