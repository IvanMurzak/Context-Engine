// R-OBS-005 interactive CDP inspector session over V8's in-box inspector (issue #94 / L-61: this is
// CONFIGURATION of v8-inspector.h, NOT building a debugger from scratch). Built out from the task-2a
// seam (inspector_seam.h) into a real V8Inspector + V8InspectorSession + Channel + Client with a
// synchronous nested pause loop, so a CDP client can attach, set a source-mapped breakpoint, hit it,
// and resume/step. Compiled ONLY on toolchains that can link the rusty_v8 prebuilt (CONTEXT_JS_HAS_V8
// — the 3-OS CI build legs); the local Strawberry-GCC Windows dev gate builds the js stub instead
// (js_host_stub.cpp), which offers no inspector. See src/runtime/js/README.md.
//
// HYBRID-libc++-ABI note (why direct C++ subclassing of the v8_inspector classes is link-safe across
// the __Cr boundary — the same boundary v8_shims.h documents for platform boot):
//   * V8Inspector::create / V8Inspector::connect / V8InspectorSession::stateJSON return
//     std::unique_ptr / std::shared_ptr. A RETURN type is NOT part of C++ name mangling, so the
//     symbol these resolve to is identical whether the caller's std::unique_ptr is libstdc++/MSVC-STL
//     or the archive's Chromium libc++ (__Cr). std::unique_ptr<T> with the default deleter is a
//     single pointer with a non-trivial destructor, so it is returned via the sret/invisible-reference
//     ABI on every target — one pointer, identical layout on both sides. So create()/connect() link
//     AND are ABI-correct directly, without an extern-"C" shim (the seam header's TODO note about a
//     shim is superseded: the return-type-immunity makes the shim unnecessary).
//   * The Channel virtuals take std::unique_ptr<StringBuffer> BY VALUE. A non-trivially-copyable
//     argument is passed by invisible reference (a pointer to a single-pointer temporary the CALLEE
//     destroys) on Itanium AND MSVC, so the archive's __Cr unique_ptr and our system-STL unique_ptr
//     are layout-identical across the call; our override destroys it (delete -> StringBuffer's virtual
//     dtor -> archive), never double-freeing. The Client virtuals we override carry NO STL types.
//   * Vtable slot ordering is fixed by v8-inspector.h, which the prebuilt archive was compiled
//     against too, so our overrides land in the slots V8 calls. Task 2a already subclassed
//     V8InspectorClient (inspector_seam.h) and shipped green on all three CI legs — proof the
//     subclass-and-link approach holds; this file only widens the same technique.

#include "inspector_session.h"

#include "context/runtime/js/js_engine.h"

#include <cstdint>
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

// The embedder-side CDP channel: every response + notification V8 produces is forwarded to the
// session's sink. Owned by the session; the sink pointer stays valid for its lifetime.
class ChannelImpl final : public v8_inspector::V8Inspector::Channel
{
public:
    explicit ChannelImpl(std::function<void(std::string_view)>* sink) : sink_(sink) {}

    void sendResponse(int /*callId*/,
                      std::unique_ptr<v8_inspector::StringBuffer> message) override
    {
        emit(*message);
    }
    void sendNotification(std::unique_ptr<v8_inspector::StringBuffer> message) override
    {
        emit(*message);
    }
    void flushProtocolNotifications() override {}

private:
    void emit(const v8_inspector::StringBuffer& buffer)
    {
        if (sink_ != nullptr && *sink_)
        {
            (*sink_)(fromStringView(buffer.string()));
        }
    }
    std::function<void(std::string_view)>* sink_;
};

// The V8InspectorClient: its runMessageLoopOnPause is the synchronous nested pause loop the embedder
// drives via the pump. When V8 stops at a breakpoint it calls runMessageLoopOnPause(groupId); we
// loop calling the pump (which dispatches the next CDP command) until V8 signals resume through
// quitMessageLoopOnPause (or the pump returns true, or the safety bound trips).
class ClientImpl final : public v8_inspector::V8InspectorClient
{
public:
    ClientImpl(v8::Isolate* isolate, v8::Global<v8::Context>* context,
               std::function<bool()>* pump)
        : isolate_(isolate), context_(context), pump_(pump)
    {
    }

    void runMessageLoopOnPause(int /*contextGroupId*/) override
    {
        paused_ = true;
        int guard = 0;
        while (paused_ && guard++ < kMaxPauseIterations)
        {
            if (pump_ == nullptr || !*pump_)
            {
                break; // no pump installed: return so V8 continues rather than deadlocking
            }
            if ((*pump_)())
            {
                break; // pump reports it issued a resume/step
            }
        }
        paused_ = false;
    }

    void quitMessageLoopOnPause() override { paused_ = false; }

    void runIfWaitingForDebugger(int /*contextGroupId*/) override {}

    v8::Local<v8::Context> ensureDefaultContextInGroup(int /*contextGroupId*/) override
    {
        return context_->Get(isolate_);
    }

private:
    v8::Isolate* isolate_;
    v8::Global<v8::Context>* context_;
    std::function<bool()>* pump_;
    bool paused_ = false;
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
        // MATERIALIZE the isolate's debug subsystem BEFORE wiring the inspector — the actual fix for
        // the attach-time SEGV (isolate-level, NOT context-level: entering an Isolate/Handle/Context
        // scope, done below, is necessary for the handle ops but does NOT create the Debug object).
        //
        // V8's V8InspectorImpl constructor (invoked from V8Inspector::create) runs, in its body,
        // v8::debug::SetIsolateId(isolate, ...) -> isolate->debug()->SetIsolateId(id). On this
        // rusty_v8 prebuilt the isolate's Debug object is NOT allocated eagerly by Isolate::New; it
        // is created lazily the first time a script is compiled/run on the isolate. Our attach path
        // (attachInspector on an engine that has only done Context::New) targets a BARE isolate that
        // has never compiled a script, so isolate->debug() is still null and SetIsolateId writes
        // isolate_id_ through a null Debug — the ASan "SEGV 0x100" (null base + field offset). Every
        // canonical rusty_v8 inspector consumer (Deno bootstraps JS; d8 runs scripts) attaches only
        // AFTER JS has run, so none of them hit this. Compiling+running a trivial script here forces
        // V8 down its debug-aware compile path, which lazily instantiates isolate->debug() before
        // V8Inspector::create dereferences it. (An earlier fix that only added a Context::Scope was
        // disproven — it never materializes Debug.) The rusty_v8 host is CI-only-buildable, so this
        // is verified on the 3-OS + sanitize CI legs, not the local Strawberry-GCC stub gate.
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
        // V8Engine's eval/getFunction/...). Still required — just not, by itself, sufficient: the
        // Debug object it operates against is what the warm-up above materialized.
        v8::Context::Scope contextScope(context);

        client_ = std::make_unique<ClientImpl>(isolate_, &context_, &pump_);
        inspector_ = v8_inspector::V8Inspector::create(isolate_, client_.get());
        if (!inspector_)
        {
            err = "v8_inspector::V8Inspector::create returned null";
            return false;
        }
        channel_ = std::make_unique<ChannelImpl>(&sink_);

        const std::string name = "context-engine";
        v8_inspector::V8ContextInfo info(context, kContextGroupId, toStringView(name));
        inspector_->contextCreated(info);

        session_ = inspector_->connect(kContextGroupId, channel_.get(), v8_inspector::StringView(),
                                       v8_inspector::V8Inspector::kFullyTrusted,
                                       v8_inspector::V8Inspector::kNotWaitingForDebugger);
        if (!session_)
        {
            err = "v8_inspector::V8Inspector::connect returned null";
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
        // same per-context inspector/debug state they were registered against (and ~V8InspectorImpl's
        // own SetIsolateId runs against a live, entered context).
        v8::Context::Scope contextScope(context);
        session_.reset();
        if (inspector_)
        {
            inspector_->contextDestroyed(context);
        }
        inspector_.reset();
        context_.Reset();
    }

private:
    v8::Isolate* isolate_;
    v8::Global<v8::Context> context_;
    std::function<void(std::string_view)> sink_;
    std::function<bool()> pump_;
    std::unique_ptr<ClientImpl> client_;
    std::unique_ptr<v8_inspector::V8Inspector> inspector_;
    std::unique_ptr<ChannelImpl> channel_;
    std::unique_ptr<v8_inspector::V8InspectorSession> session_;
};

} // namespace

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
