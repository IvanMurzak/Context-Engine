// The windowed-OSR CEF binding — see cef_shell.h for the model and the owner ruling on the
// accelerated path.
//
// This is the ONLY CEF-dependent translation unit in the Shell. The cross-process / headless-boot
// carve-outs (subprocess re-entry, the per-PID root_cache_path, the Session-0 hard exit) mirror
// src/editor/gui/host/src/editor_host.cpp, which boots green on all three OS legs today.

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "context/editor/shell/cef/cef_shell.h"

#include "context/editor/shell/app_scheme.h"

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_command_line.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_load_handler.h"
#include "include/cef_render_handler.h"
#include "include/cef_render_process_handler.h"
#include "include/cef_request_handler.h"
#include "include/cef_resource_handler.h"
#include "include/cef_scheme.h"
#include "include/wrapper/cef_message_router.h"

#if defined(__APPLE__)
#include "include/wrapper/cef_library_loader.h"
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h> // getpid()
#endif

namespace context::editor::shell::cef
{
namespace
{

// The names the message router injects onto `window` in editor-core's frames.
//
// DELIBERATELY NOT CEF's default `cefQuery`: that name is what every CEF sample and every piece of
// drive-by injection probes for, and a distinctive one makes "is this the Context Shell" answerable
// rather than guessable. MUST match BRIDGE_QUERY_FUNCTION in src/editor/webui/core/src/bridge.ts —
// the `webui-scheme-contract` ctest re-checks that from the BUILT bundle, so a rename on either
// side reds CI instead of producing a bridge editor-core cannot find.
constexpr const char* kBridgeQueryFunction = "contextEditorQuery";
constexpr const char* kBridgeCancelFunction = "contextEditorQueryCancel";

// Read by the scheme handler factory on the IO thread. Set ONCE before the first browser exists and
// never mutated afterwards, which is what makes it safe without a lock; `AppAssetResolver::resolve`
// is const and holds no mutable state, so concurrent resolves are fine.
const AppAssetResolver* g_asset_resolver = nullptr;

bool g_initialized = false;

// Opt-in verbose Chromium logging (CefShellOptions::verbose_logging). Set ONCE in the browser
// process before CefInitialize, read by OnBeforeCommandLineProcessing to append the logging
// switches; CEF then propagates the switches onto the renderer/GPU/utility subprocess command lines
// it builds from the browser process's, so the whole tree logs to stderr. Never mutated after boot.
bool g_verbose_logging = false;

// The e10a containment counters (cef_shell.h § the containment counters). Both are written on the
// CEF UI thread — which IS the owner thread here (`multi_threaded_message_loop=false` + the
// integrated pump, so every callback runs inside the owner's CefDoMessageLoopWork) — and read by
// the owner thread between pumps. Plain ints, deliberately: making them atomic would advertise a
// cross-thread contract this single-threaded design does not have.
int g_browsers_created = 0;
int g_popups_suppressed = 0;
// The e10a frame-delivery tripwire (cef_shell.h § the containment counters). Written on the same
// thread as the two above, for the same reason it is a plain int.
int g_frames_dropped_without_sink = 0;

// --------------------------------------------------------------------------- modifier translation

std::uint32_t to_cef_modifiers(const Modifiers& modifiers)
{
    std::uint32_t flags = 0;
    if (modifiers.shift)
    {
        flags |= EVENTFLAG_SHIFT_DOWN;
    }
    if (modifiers.control)
    {
        flags |= EVENTFLAG_CONTROL_DOWN;
    }
    if (modifiers.alt)
    {
        flags |= EVENTFLAG_ALT_DOWN;
    }
    if (modifiers.meta)
    {
        flags |= EVENTFLAG_COMMAND_DOWN;
    }
    // The button flags matter for drag tracking: without them Chromium sees a move with no button
    // held and ends the drag it is in the middle of.
    if (modifiers.left_button_down)
    {
        flags |= EVENTFLAG_LEFT_MOUSE_BUTTON;
    }
    if (modifiers.middle_button_down)
    {
        flags |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
    }
    if (modifiers.right_button_down)
    {
        flags |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
    }
    return flags;
}

cef_mouse_button_type_t to_cef_button(MouseButton button)
{
    switch (button)
    {
    case MouseButton::right:
        return MBT_RIGHT;
    case MouseButton::middle:
        return MBT_MIDDLE;
    case MouseButton::left:
    case MouseButton::none:
    default:
        return MBT_LEFT;
    }
}

cef_key_event_type_t to_cef_key_type(KeyAction action)
{
    switch (action)
    {
    case KeyAction::key_down:
        return KEYEVENT_KEYDOWN;
    case KeyAction::key_up:
        return KEYEVENT_KEYUP;
    case KeyAction::character:
        return KEYEVENT_CHAR;
    case KeyAction::raw_key_down:
    default:
        return KEYEVENT_RAWKEYDOWN;
    }
}

// --------------------------------------------------------------- the app-scheme resource handler

// Serves ONE `context-editor://app/…` request from the built asset set.
//
// Every decision worth making — which URLs are in bounds, what a path may contain, which media
// types exist, what the CSP says — lives in the CEF-free `AppAssetResolver` next door, where it is
// adversarially unit-tested on all three default `build` legs. This class does exactly three
// things: ask the resolver, read the bytes, hand them to CEF. Keeping it that thin is the point,
// because this file is the one the local dev gate cannot build.
class AppSchemeResourceHandler final : public CefResourceHandler
{
public:
    bool Open(CefRefPtr<CefRequest> request, bool& handle_request,
              CefRefPtr<CefCallback>) override
    {
        // Synchronous: the response is fully decided inside this call, so CEF never has to wait on
        // a continuation. `handle_request = true` is what says so.
        handle_request = true;

        const std::string url = request->GetURL().ToString();
        if (g_asset_resolver == nullptr)
        {
            // No asset root was configured. 404 rather than a file:// fallback — see cef_shell.h.
            resolution_.status = AssetStatus::not_found;
            return true;
        }

        resolution_ = g_asset_resolver->resolve(url);
        if (!resolution_.ok())
        {
            // The REASON is logged, never sent: a refusal reason is a probe oracle for anything
            // that got script running in the renderer.
            std::fprintf(stderr, "[shell-cef] app scheme refused <%s>: %s\n", url.c_str(),
                         resolution_.reason.c_str());
            return true;
        }

        // Read the whole asset up front. These are the editor's own bundled assets (hundreds of KB,
        // not media), so streaming would buy nothing and would leave the file handle open across
        // callbacks for no reason.
        std::ifstream file(resolution_.file, std::ios::binary);
        if (!file)
        {
            resolution_.status = AssetStatus::not_found;
            return true;
        }
        body_.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        // A read that failed PART WAY would otherwise serve a truncated bundle, which presents as a
        // baffling syntax error in the renderer rather than as an IO failure.
        if (file.bad())
        {
            body_.clear();
            resolution_.status = AssetStatus::not_found;
        }
        return true;
    }

    void GetResponseHeaders(CefRefPtr<CefResponse> response, int64_t& response_length,
                            CefString& redirectUrl) override
    {
        redirectUrl.clear();
        response->SetStatus(resolution_.http_status());

        if (!resolution_.ok())
        {
            // An error response still carries the CSP: an error page is a document too, and one
            // served without a policy is a hole that only shows up on the unhappy path.
            response->SetMimeType("text/plain");
            CefResponse::HeaderMap headers;
            headers.insert({"Content-Security-Policy", app_csp_header()});
            headers.insert({"X-Content-Type-Options", "nosniff"});
            response->SetHeaderMap(headers);
            response_length = 0;
            return;
        }

        // MIME ESSENCE AND CHARSET ARE TWO SEPARATE FIELDS — see split_media_type() in
        // app_scheme.h for the full trap. Passing the resolver's `text/css; charset=utf-8` straight
        // into SetMimeType() makes Chromium's by-essence comparison fail, and with the
        // `X-Content-Type-Options: nosniff` this response also sets, the stylesheet and the ES
        // module are then silently refused and the document is not parsed as HTML.
        const MediaType media = split_media_type(resolution_.mime_type);
        response->SetMimeType(media.essence);
        if (!media.charset.empty())
        {
            response->SetCharset(media.charset);
        }
        CefResponse::HeaderMap headers;
        for (const auto& [name, value] : app_response_headers(resolution_.mime_type))
        {
            // CEF derives the Content-Type from the mime type + charset set above; setting it in
            // the map as well makes the response carry it twice, which some parsers treat as a
            // conflict.
            if (name == "Content-Type")
            {
                continue;
            }
            headers.insert({name, value});
        }
        response->SetHeaderMap(headers);
        response_length = static_cast<int64_t>(body_.size());
    }

    bool Read(void* data_out, int bytes_to_read, int& bytes_read,
              CefRefPtr<CefResourceReadCallback>) override
    {
        bytes_read = 0;
        if (data_out == nullptr || bytes_to_read <= 0 || offset_ >= body_.size())
        {
            // false with bytes_read == 0 is CEF's "complete", not an error.
            return false;
        }
        const std::size_t remaining = body_.size() - offset_;
        const std::size_t count =
            std::min(remaining, static_cast<std::size_t>(bytes_to_read));
        std::memcpy(data_out, body_.data() + offset_, count);
        offset_ += count;
        bytes_read = static_cast<int>(count);
        return true;
    }

    // CEF's default Skip() reports -2 (ERR_FAILED), which turns any RANGE request into a load
    // failure. Chromium issues none for the current html/css/js set, so this is latent — but the
    // day a font, <audio> or <video> asset lands it would present as an unexplainable 404. The body
    // is already fully buffered and `offset_` already exists, so honouring it is free.
    bool Skip(int64_t bytes_to_skip, int64_t& bytes_skipped,
              CefRefPtr<CefResourceSkipCallback>) override
    {
        if (bytes_to_skip < 0 || offset_ >= body_.size())
        {
            bytes_skipped = -2; // ERR_FAILED: nothing left to skip over.
            return false;
        }
        const std::size_t remaining = body_.size() - offset_;
        const std::size_t count =
            std::min(remaining, static_cast<std::size_t>(bytes_to_skip));
        offset_ += count;
        bytes_skipped = static_cast<int64_t>(count);
        return true;
    }

    void Cancel() override {}

private:
    AssetResolution resolution_;
    std::string body_;
    std::size_t offset_ = 0;

    IMPLEMENT_REFCOUNTING(AppSchemeResourceHandler);
};

class AppSchemeFactory final : public CefSchemeHandlerFactory
{
public:
    CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                                         const CefString&, CefRefPtr<CefRequest>) override
    {
        return new AppSchemeResourceHandler();
    }

private:
    IMPLEMENT_REFCOUNTING(AppSchemeFactory);
};

// ------------------------------------------------------------------------- the IPC bridge handler

// Translates one CefMessageRouter query into one `BridgeRouter::dispatch` call.
//
// TWO gates before the router is even asked, both of which the router cannot apply itself because
// they are facts about the FRAME rather than about the message:
//
//   1. ORIGIN. The query must come from editor-core's own origin. A sandboxed third-party panel
//      (04 §5) lives on a different `context-ext://` origin and reaches the daemon through the
//      SCOPED panel bridge; this privileged channel is not for it. `Failure` rather than a JSON
//      error envelope, because a caller that is not editor-core is not owed a protocol reply.
//   2. NO PERSISTENT QUERIES. A persistent query is a subscription the renderer can open without
//      bound; the request/response bridge has no use for one, and refusing it keeps the channel's
//      lifetime model trivial (every query completes inside OnQuery).
//
// THREADING: OnQuery runs on the CEF UI thread, which — with external_message_pump and
// multi_threaded_message_loop=false (03 §1) — IS the shell's owner thread, inside
// CefDoMessageLoopWork() inside pump(). So the BridgeRouter is touched from exactly one thread,
// the same discipline OnPaint follows, and needs no locking.
class BridgeQueryHandler final : public CefMessageRouterBrowserSide::Handler
{
public:
    explicit BridgeQueryHandler(BridgeRouter* router) : router_(router) {}

    bool OnQuery(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int64_t /*query_id*/,
                 const CefString& request, bool persistent,
                 CefRefPtr<Callback> callback) override
    {
        return dispatch_query(frame, request.ToString(), persistent, callback);
    }

    // THE BINARY OVERLOAD IS NOT OPTIONAL. CefMessageRouter switches transports at
    // `CefMessageRouterConfig::message_size_threshold` — 16 KiB by default, which this Shell does
    // not override — and hands anything at or above it to THIS overload instead of the CefString
    // one. Leaving it to the base class (which returns false) means every request >= 16 KiB is
    // silently cancelled as unhandled: the router is never reached, so served()/refused() do not
    // move and the smoke stays green while real payloads — a document patch, a batch entity update,
    // a scene-tree listing — fail with a generic transport error. `kMaxBridgeMessageBytes`
    // advertises 1 MiB, so without this the live channel delivers 1/64th of its own contract.
    bool OnQuery(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int64_t /*query_id*/,
                 CefRefPtr<const CefBinaryBuffer> request, bool persistent,
                 CefRefPtr<Callback> callback) override
    {
        std::string payload;
        if (request != nullptr && request->GetData() != nullptr)
        {
            payload.assign(static_cast<const char*>(request->GetData()), request->GetSize());
        }
        return dispatch_query(frame, payload, persistent, callback);
    }

private:
    bool dispatch_query(CefRefPtr<CefFrame> frame, const std::string& request, bool persistent,
                        const CefRefPtr<Callback>& callback)
    {
        if (router_ == nullptr)
        {
            return false;
        }
        if (persistent)
        {
            callback->Failure(-1, "the context bridge does not accept persistent queries");
            return true;
        }
        const std::string frame_url = frame != nullptr ? frame->GetURL().ToString() : std::string();
        if (!is_trusted_bridge_origin(frame_url))
        {
            std::fprintf(stderr, "[shell-cef] bridge query REFUSED from untrusted origin <%s>\n",
                         frame_url.c_str());
            callback->Failure(-1, "this origin may not use the privileged bridge");
            return true;
        }

        // Everything past here is the CEF-free router's job: parse, validate, route, scan the
        // response for protected secrets, and produce an envelope. It never throws, so there is no
        // exception to contain at this boundary.
        const BridgeDispatch dispatch = router_->dispatch(request);
        // A refusal is still a well-formed JSON-RPC error envelope, so it goes back through
        // Success(): the QUERY succeeded, and what it carries is the protocol-level answer. Using
        // Failure() here would collapse "your message was malformed" into the same channel as "you
        // are not allowed to talk to me", which the JS client reports differently on purpose.
        callback->Success(dispatch.response);
        return true;
    }

    BridgeRouter* router_ = nullptr;
};

// ------------------------------------------------------------------------------- the CEF client

// The browser-side client: render handler (OSR frames + the popup), life-span (popup suppression),
// and load handler. It forwards frames into whatever sink the host is currently pumping with.
class ShellCefClient : public CefClient,
                       public CefRenderHandler,
                       public CefLifeSpanHandler,
                       public CefLoadHandler,
                       public CefDisplayHandler,
                       public CefRequestHandler
{
public:
    ShellCefClient(render::Extent2D logical_size, DpiScale dpi, BridgeRouter* bridge)
        : logical_size_(logical_size), dpi_(dpi)
    {
        if (bridge == nullptr)
        {
            // No bridge configured: the router is never created, so CEF injects NO query function
            // and editor-core reports itself detached. That is the honest state — injecting a
            // function that always fails would look like a broken bridge instead of an absent one.
            return;
        }
        CefMessageRouterConfig config;
        config.js_query_function = kBridgeQueryFunction;
        config.js_cancel_function = kBridgeCancelFunction;
        router_ = CefMessageRouterBrowserSide::Create(config);
        bridge_handler_ = std::make_unique<BridgeQueryHandler>(bridge);
        router_->AddHandler(bridge_handler_.get(), /*first*/ false);
    }

    ~ShellCefClient() override
    {
        // The router holds a raw Handler pointer, so it must let go BEFORE bridge_handler_ is
        // destroyed. Member destruction order alone does not guarantee that (router_ is a refcounted
        // handle CEF may still hold), so the removal is explicit.
        if (router_ != nullptr && bridge_handler_ != nullptr)
        {
            router_->RemoveHandler(bridge_handler_.get());
        }
    }

    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }

    // --- the message router's browser-side hooks ------------------------------------------------
    // All four are REQUIRED by CefMessageRouterBrowserSide; omitting any of them leaks pending
    // queries across a navigation, a renderer crash, or a browser close, and the router's own
    // documentation is explicit that the embedder must forward them.
    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                  CefProcessId source_process,
                                  CefRefPtr<CefProcessMessage> message) override
    {
        if (router_ != nullptr &&
            router_->OnProcessMessageReceived(browser, frame, source_process, message))
        {
            return true;
        }
        return false;
    }

    bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                        CefRefPtr<CefRequest> request, bool, bool) override
    {
        if (router_ != nullptr)
        {
            router_->OnBeforeBrowse(browser, frame);
        }
        // The MAIN frame may only ever be on the app origin. The CSP constrains what the document
        // may LOAD, but it has no `navigate-to`, so nothing else stops a compromised renderer from
        // navigating the top-level frame off `context-editor://app/` — after which the window is
        // showing content the Shell never served. Token isolation still held (the bridge refuses any
        // other origin), so this closes a broken-editor hole rather than a leak; it is cheap, and
        // `kAppUrlPrefix` is already the vocabulary this file routes on.
        //
        // Sub-frame navigations are not gated here: `frame-src 'none'` already denies them, and the
        // Shell creates no sub-frames of its own.
        if (frame != nullptr && frame->IsMain() && request != nullptr)
        {
            const std::string url = request->GetURL().ToString();
            if (url.rfind(kAppUrlPrefix, 0) != 0)
            {
                std::fprintf(stderr,
                             "[shell-cef] main-frame navigation BLOCKED to <%s>: the editor window "
                             "may only be on %s\n",
                             url.c_str(), kAppUrlPrefix);
                return true; // true == cancel the navigation.
            }
        }
        return false;
    }

    void OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser, TerminationStatus, int,
                                   const CefString&) override
    {
        if (router_ != nullptr)
        {
            router_->OnRenderProcessTerminated(browser);
        }
    }

    // --- CefRenderHandler --------------------------------------------------------------------
    void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override
    {
        // VIEW coordinates are DIP. Reporting physical pixels here lays the document out at the
        // wrong size on every non-100% monitor — the bug the spike's DPI-1.0 pin hid.
        rect.Set(0, 0, static_cast<int>(logical_size_.width),
                 static_cast<int>(logical_size_.height));
    }

    bool GetScreenInfo(CefRefPtr<CefBrowser>, CefScreenInfo& screen_info) override
    {
        // The other half of real DPI: the scale CEF multiplies the DIP view rect by to decide how
        // many PHYSICAL pixels to paint. Without it a 2x monitor gets a 1x-resolution UI.
        screen_info.device_scale_factor = dpi_.factor();
        // CefScreenInfo::rect is a RAW cef_rect_t (unlike CefRect it carries no Set()). Which
        // convention it wants is a per-platform choice; the ARITHMETIC lives in dpi.h so both
        // branches are compiled and tested on all three legs (see osr_screen_extent).
#if defined(__APPLE__)
        constexpr bool kScreenRectIsDip = true;
#else
        constexpr bool kScreenRectIsDip = false;
#endif
        const render::Extent2D screen = osr_screen_extent(logical_size_, dpi_, kScreenRectIsDip);
        screen_info.rect.x = 0;
        screen_info.rect.y = 0;
        screen_info.rect.width = static_cast<int>(screen.width);
        screen_info.rect.height = static_cast<int>(screen.height);
        screen_info.available_rect = screen_info.rect;
        return true;
    }

    void OnPopupShow(CefRefPtr<CefBrowser>, bool show) override
    {
        popup_visible_ = show;
        if (!show)
        {
            popup_rect_ = render::Rect2D{};
        }
        deliver_popup_state();
    }

    void OnPopupSize(CefRefPtr<CefBrowser>, const CefRect& rect) override
    {
        // CEF sends the rect and the visibility as separate callbacks with no guaranteed order, so
        // both are held here and the sink is told the COMBINED state — the sink keeps no partial
        // state of its own (see browser.h).
        popup_rect_ = to_rect(rect);
        if (popup_visible_)
        {
            deliver_popup_state();
        }
    }

    void OnPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList& dirty_rects,
                 const void* buffer, int width, int height) override
    {
        if (sink_ == nullptr)
        {
            // A LIVE, already-bound browser losing a frame is the e10a multi-window defect (see
            // `pump()` below): with N browsers sharing ONE process-wide message loop, a paint
            // delivered while this browser's sink was unbound vanished silently and its window
            // never composited anything. Counted so the condition is observable instead of being
            // inferred from a window that merely stays blank. The two excluded cases are normal and
            // not losses: before the owner's first `pump()` (nothing is driving this window yet)
            // and from `close()` onward (the window is going away; nothing would composite them).
            if (ever_bound_ && !closing_ && !closed_)
            {
                ++g_frames_dropped_without_sink;
            }
            return;
        }
        if (buffer == nullptr || width <= 0 || height <= 0)
        {
            return;
        }
        BrowserFrame frame;
        frame.layer = type == PET_POPUP ? BrowserLayer::popup : BrowserLayer::view;
        const auto w = static_cast<std::uint32_t>(width);
        const auto h = static_cast<std::uint32_t>(height);
        frame.frame.pixels = buffer;
        frame.frame.bytes_per_row = w * 4u;
        frame.frame.byte_size = static_cast<std::size_t>(frame.frame.bytes_per_row) * h;
        // CEF's OnPaint buffer IS the whole image, so the allocation and the visible area coincide.
        frame.frame.coded_size = render::Extent2D{w, h};
        frame.frame.visible_rect = render::Rect2D{render::Origin2D{}, render::Extent2D{w, h}};
        frame.frame.dirty.reserve(dirty_rects.size());
        for (const CefRect& rect : dirty_rects)
        {
            frame.frame.dirty.push_back(to_rect(rect));
        }
        // Delivered SYNCHRONOUSLY: OnPaint runs inside CefDoMessageLoopWork(), which runs inside
        // pump(), so the sink is live and the buffer is valid — no copy needed. CEF explicitly
        // documents the buffer as valid only for the duration of this call.
        sink_->on_browser_frame(frame);
    }

    // OnAcceleratedPaint is deliberately NOT overridden: the accelerated path is unreachable by
    // policy (owner ruling 2026-07-19 — see cef_shell.h) and shared_texture_enabled is left off, so
    // CEF never calls it. Overriding it to do nothing would advertise a path that does not exist.

    // --- CefLifeSpanHandler ------------------------------------------------------------------
    // `CefLifeSpanHandler::` on the disposition is REQUIRED, not decoration: e05c added
    // CefRequestHandler to this class's bases (the message router needs OnBeforeBrowse /
    // OnRenderProcessTerminated), and BOTH bases typedef `WindowOpenDisposition`. Unqualified, the
    // name is ambiguous, the signature does not match, and `override` fails — which is a compile
    // error ONLY on the CEF legs the local dev gate cannot build. It is the sole name these bases
    // collide on (TerminationStatus, ErrorCode and the render typedefs are each unique to one).
    bool OnBeforePopup(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int /*popup_id*/,
                       const CefString&, const CefString&,
                       CefLifeSpanHandler::WindowOpenDisposition, bool, const CefPopupFeatures&,
                       CefWindowInfo&, CefRefPtr<CefClient>&, CefBrowserSettings&,
                       CefRefPtr<CefDictionaryValue>&, bool*) override
    {
        // SUPPRESS every stray window.open (03 §1). Tear-out does NOT ride window.open — it is a
        // PanelHost/Shell mechanism (04 §2) — so a popup reaching here is an accident, and letting
        // CEF create a default popup window would put an un-composited native window on screen.
        //
        // e10a: counted AND logged. With N windows the Shell is now genuinely in the business of
        // creating windows, so "a window appeared that the Shell did not create" stops being an
        // impossible state and becomes the exact thing this boundary exists to prevent — and a
        // containment boundary nothing can observe is one nothing can prove.
        ++g_popups_suppressed;
        std::fprintf(stderr, "[shell-cef] popup SUPPRESSED: window.open may not create an "
                             "unmanaged window (03 §1)\n");
        return true;
    }

    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override
    {
        browser_ = browser;
        // Every browser this process creates passes through here — including one CEF might create
        // for a popup, which is why this is the honest denominator for the suppression assertion.
        ++g_browsers_created;
    }

    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override
    {
        // Cancel any query still in flight BEFORE the browser reference is dropped: the router
        // needs the browser to match its pending set, and a query left pending past close is a
        // callback into a destroyed context.
        if (router_ != nullptr)
        {
            router_->OnBeforeClose(browser);
        }
        browser_ = nullptr;
        closed_ = true;
    }

    // --- CefLoadHandler ----------------------------------------------------------------------
    void OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int) override
    {
        if (frame->IsMain())
        {
            load_ended_ = true;
        }
    }

    void OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, ErrorCode error_code,
                     const CefString& error_text, const CefString& failed_url) override
    {
        if (!frame->IsMain())
        {
            return;
        }
        // A failed main-frame load still ENDS the load, and it is REPORTED here rather than only
        // recorded: the live CEF smoke waits on a composited frame, so a page that never loaded
        // presents as an undiagnosable 30-second stall unless the cause reaches stderr. The state
        // is not reachable from the smoke (this class is TU-local), so the log is the channel.
        load_ended_ = true;
        load_failed_ = true;
        std::fprintf(stderr, "[shell-cef] main-frame load FAILED (%d): %s <%s>\n",
                     static_cast<int>(error_code), error_text.ToString().c_str(),
                     failed_url.ToString().c_str());
    }

    // --- CefDisplayHandler ---------------------------------------------------------------------
    bool OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t level, const CefString& message,
                          const CefString& source, int line) override
    {
        // THE RENDERER'S SIDE OF THE STORY, which nothing else in this Shell can see. A CSP refusal
        // ("Refused to apply stylesheet…"), a blocked ES module, a module that threw before it could
        // reach the bridge — all of them are console messages and NONE of them fail a load, so
        // without this the live smoke reports only that its assertions did not come true, with no
        // cause. Diagnosing one such failure from CI logs alone cost a full round-trip; a page that
        // cannot boot should say why.
        std::fprintf(stderr, "[shell-cef] console(%d) %s <%s:%d>\n", static_cast<int>(level),
                     message.ToString().c_str(), source.ToString().c_str(), line);
        // false = let CEF log it too; we are observing, not suppressing.
        return false;
    }

    // --- driving it ---------------------------------------------------------------------------
    // Bind (or unbind) the sink this browser's frames are delivered into. The binding OUTLIVES the
    // pump call that made it — see `CefBrowserHostImpl::pump` for why that is load-bearing rather
    // than a convenience, and `close()`/`~CefBrowserHostImpl` for the two places that clear it.
    void set_sink(IBrowserFrameSink* sink)
    {
        if (closing_)
        {
            // Once closing, the sink stays unbound for good. This makes the post-close
            // use-after-free impossible BY CONSTRUCTION rather than by caller discipline: a stray
            // `pump()` after `close()` cannot re-arm a pointer into a compositor that is gone.
            return;
        }
        sink_ = sink;
        ever_bound_ = ever_bound_ || sink != nullptr;
    }

    // The host is closing: drop the sink and stop treating a sink-less paint as a lost frame. Both
    // halves matter — see `CefBrowserHostImpl::close()`, which calls this before it pumps.
    void begin_close()
    {
        sink_ = nullptr;
        closing_ = true;
    }
    void set_view(render::Extent2D logical_size, DpiScale dpi)
    {
        logical_size_ = logical_size;
        dpi_ = dpi;
    }

    [[nodiscard]] CefRefPtr<CefBrowser> browser() const { return browser_; }
    [[nodiscard]] bool closed() const { return closed_; }

private:
    static render::Rect2D to_rect(const CefRect& rect)
    {
        render::Rect2D out;
        // CEF rects are signed; a negative origin cannot be represented and would wrap. Clamp
        // rather than wrap — the import driver clips against the allocation anyway.
        out.origin.x = rect.x > 0 ? static_cast<std::uint32_t>(rect.x) : 0u;
        out.origin.y = rect.y > 0 ? static_cast<std::uint32_t>(rect.y) : 0u;
        out.size.width = rect.width > 0 ? static_cast<std::uint32_t>(rect.width) : 0u;
        out.size.height = rect.height > 0 ? static_cast<std::uint32_t>(rect.height) : 0u;
        return out;
    }

    void deliver_popup_state()
    {
        if (sink_ != nullptr)
        {
            sink_->on_popup_state(popup_visible_, popup_rect_);
        }
    }

    IBrowserFrameSink* sink_ = nullptr;
    CefRefPtr<CefBrowser> browser_;
    render::Extent2D logical_size_;
    DpiScale dpi_;
    render::Rect2D popup_rect_{};
    // The browser-side message router + the handler bridging it to the CEF-free BridgeRouter. Both
    // null when no bridge was configured, which is what keeps the query function uninjected.
    CefRefPtr<CefMessageRouterBrowserSide> router_;
    std::unique_ptr<BridgeQueryHandler> bridge_handler_;
    bool popup_visible_ = false;
    // True once a sink has ever been bound. It is what lets the OnPaint tripwire above tell a
    // genuine lost frame from the benign paints CEF can produce before the owner loop first pumps
    // this window.
    bool ever_bound_ = false;
    // Set by `begin_close()`, ahead of `closed_` (which only lands once CEF calls OnBeforeClose).
    // The gap between the two is the close pump, and it is the whole reason this flag exists.
    bool closing_ = false;
    bool closed_ = false;
    bool load_ended_ = false;
    bool load_failed_ = false;

    IMPLEMENT_REFCOUNTING(ShellCefClient);
};

// ---------------------------------------------------------------------------------- the CEF app

// The browser-process app. Its one real job beyond command-line flags is the INTEGRATED PUMP hook:
// with external_message_pump on, CEF asks to be driven via OnScheduleMessagePumpWork instead of
// owning a loop, which is what lets the shell's single thread own frame pacing (03 §1).
class ShellCefApp : public CefApp, public CefBrowserProcessHandler, public CefRenderProcessHandler
{
public:
    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
    CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }

    // --- the custom scheme, registered in EVERY process ------------------------------------------
    //
    // CefApp::OnRegisterCustomSchemes runs in the browser process AND in every subprocess, and it
    // MUST agree everywhere: the renderer is where origin comparisons, CSP evaluation and module
    // loading actually happen, so a scheme registered only browser-side would leave editor-core's
    // documents on an opaque origin with subtly different security semantics — the classic
    // "works until you load a module" failure. `g_app` is the same object on both paths
    // (execute_subprocess creates it too), which is what makes that automatic here.
    void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override
    {
        // The pinned flag set (design 04 §5 / 08 §2):
        //   STANDARD      — ordinary origin semantics. Without it Chromium treats the scheme as
        //                   opaque, and CSP, module scripts and same-origin checks all misbehave.
        //   SECURE        — a trustworthy origin, so the document is not treated as insecure
        //                   content and downgraded or blocked.
        //   CORS_ENABLED  — CORS requests are meaningful for the scheme (required for module
        //                   script loading to resolve the way a normal origin's does).
        //   FETCH_ENABLED — the Fetch API may target it. NOTE this does NOT widen the network
        //                   surface: the CSP sends `connect-src 'none'`, so nothing can actually
        //                   fetch anything. It is here so the scheme behaves like a real origin
        //                   rather than a special case.
        // NOT set, deliberately: CSP_BYPASSING (the whole point is that the CSP APPLIES) and
        // LOCAL (which would grant file-like privileges — the opposite of what this scheme is for).
        registrar->AddCustomScheme(kAppScheme, CEF_SCHEME_OPTION_STANDARD |
                                                   CEF_SCHEME_OPTION_SECURE |
                                                   CEF_SCHEME_OPTION_CORS_ENABLED |
                                                   CEF_SCHEME_OPTION_FETCH_ENABLED);
    }

    // --- the renderer side of the message router --------------------------------------------------
    // This is what actually injects the query function into editor-core's frames. Without these
    // three forwards the browser side is wired to nothing and `contextEditorQuery` is undefined.
    void OnContextCreated(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                          CefRefPtr<CefV8Context> context) override
    {
        ensure_renderer_router();
        renderer_router_->OnContextCreated(browser, frame, context);
    }

    void OnContextReleased(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                           CefRefPtr<CefV8Context> context) override
    {
        if (renderer_router_ != nullptr)
        {
            renderer_router_->OnContextReleased(browser, frame, context);
        }
    }

    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                  CefProcessId source_process,
                                  CefRefPtr<CefProcessMessage> message) override
    {
        ensure_renderer_router();
        return renderer_router_->OnProcessMessageReceived(browser, frame, source_process, message);
    }

    void OnBeforeCommandLineProcessing(const CefString&,
                                       CefRefPtr<CefCommandLine> command_line) override
    {
        // Matches src/editor/gui/host: no sandbox (ContextCef.cmake:91 builds USE_SANDBOX OFF), and
        // the GPU disabled because the editor composites CEF's SOFTWARE OSR output itself — the
        // shipping Windows path per the owner ruling.
        command_line->AppendSwitch("no-sandbox");
        command_line->AppendSwitch("disable-gpu");
        command_line->AppendSwitch("disable-gpu-compositing");
        // Skip DirectComposition entirely (issue #381). Unlike src/editor/gui/host this Shell
        // app keeps the software rasterizer enabled so the OSR software-present path (OnPaint
        // BGRA readback) still produces frames — but that drives CEF/Chromium through the Windows
        // software compositor, which probes DirectComposition (DCompositionCreateDevice3). On a
        // Session-0 self-hosted Windows CI runner (a LocalSystem service session) that probe is
        // ACCESS-DENIED (0x80070005), and CEF's failure path then re-enters a ref-counted
        // destructor and aborts with `Check failed: !in_dtor_.` (cef_ref_counted.h) — crashing
        // even single-window smokes (editor-cef-smoke-shell-palette AND -shell-multiwindow both
        // died on runner context-engine-win-3 in main job 89341674600, each preceded by the DComp
        // denial, while the SAME tree passed all 6 CEF smokes on runner context-engine-win-2 with
        // zero DComp lines). The OSR CPU-present path genuinely does not need DirectComposition:
        // Chromium's InitializeDirectComposition() (ui/gl/direct_composition_support.cc) honours
        // this switch and returns BEFORE any DXGI/dcomp work, so the denied call — and thus the
        // crash path, which fires only on that call's FAILURE — is never reached on ANY runner
        // (verified against the pinned CEF build's Chromium 149 source; tools/cef-prebuilt.json).
        command_line->AppendSwitch("disable-direct-composition");
        // Opt-in full-tree diagnostics (CefShellOptions::verbose_logging). Runs in the browser
        // process (g_verbose_logging set before CefInitialize) AND, via the switches CEF copies onto
        // each subprocess command line, in every renderer/GPU/utility child — so a fault that lives
        // in a subprocess names itself on stderr instead of surfacing only as the parent's exit code.
        if (g_verbose_logging)
        {
            command_line->AppendSwitchWithValue("enable-logging", "stderr");
            command_line->AppendSwitchWithValue("v", "1");
        }
    }

    void OnScheduleMessagePumpWork(int64_t delay_ms) override
    {
        // CEF is asking to be pumped in `delay_ms`. Recorded rather than acted on: the shell's own
        // thread owns the loop, and calling CefDoMessageLoopWork() from here would re-enter it from
        // whatever thread scheduled the work. CEF may call this from ANY thread, which is why the
        // policy + state live in the portable, atomic, unit-tested PumpSchedule rather than here.
        schedule_.schedule(delay_ms, now_ms());
    }

    // Should the owner thread pump now? Delegates to the portable policy (due, or the unconditional
    // floor when nothing is scheduled) — see PumpSchedule in browser.h.
    [[nodiscard]] bool should_pump() { return schedule_.should_pump(now_ms()); }

private:
    static std::int64_t now_ms()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    // Created lazily, in the RENDERER process only. The config MUST be byte-identical to the
    // browser side's or the two halves talk past each other (the router derives its internal
    // message names from the function names), which is why both read the same two constants.
    void ensure_renderer_router()
    {
        if (renderer_router_ != nullptr)
        {
            return;
        }
        CefMessageRouterConfig config;
        config.js_query_function = kBridgeQueryFunction;
        config.js_cancel_function = kBridgeCancelFunction;
        renderer_router_ = CefMessageRouterRendererSide::Create(config);
    }

    PumpSchedule schedule_;
    CefRefPtr<CefMessageRouterRendererSide> renderer_router_;

    IMPLEMENT_REFCOUNTING(ShellCefApp);
};

CefRefPtr<ShellCefApp> g_app;

// ------------------------------------------------------------------------------- the host

class CefBrowserHostImpl final : public IBrowserHost
{
public:
    CefBrowserHostImpl(CefRefPtr<ShellCefClient> client, CefRefPtr<ShellCefApp> app)
        : client_(client), app_(app)
    {
    }

    // `close()` is what UNBINDS the frame sink, which is why destruction must go through it even
    // for a host the caller never closed explicitly (an `EditorWindow` that simply goes out of
    // scope — the shape the other Shell smokes and the app itself use). CEF outlives this host: it
    // keeps its own reference to the client and finishes tearing the browser down inside
    // `CefShutdown()` (CE #319), so a client left holding a sink pointer into a destroyed
    // compositor would be that same use-after-free one layer down. It is airtight here because
    // `EditorWindow`'s compositor and its browser host are adjacent members of one object: nothing
    // can pump CEF between the compositor's destructor and this one.
    ~CefBrowserHostImpl() override { close(); }

    [[nodiscard]] const char* name() const override { return "cef-windowed-osr"; }

    void resize(render::Extent2D logical_size, DpiScale dpi) override
    {
        client_->set_view(logical_size, dpi);
        CefRefPtr<CefBrowser> browser = client_->browser();
        if (browser == nullptr)
        {
            return;
        }
        // The resize protocol (03 §4): WasResized makes CEF re-read GetViewRect + GetScreenInfo and
        // repaint. Reconfiguring the swapchain without this leaves the browser painting at the old
        // size and the composite sampling a UV sub-rect that no longer matches the window.
        browser->GetHost()->WasResized();
    }

    void send_pointer(const PointerDispatch& dispatch, const PointerEvent& event) override
    {
        CefRefPtr<CefBrowser> browser = client_->browser();
        if (browser == nullptr)
        {
            return;
        }
        CefMouseEvent mouse;
        // DIP, from the arbiter — CEF view coordinates are DIP, not physical pixels.
        mouse.x = dispatch.logical_position.x;
        mouse.y = dispatch.logical_position.y;
        mouse.modifiers = to_cef_modifiers(event.modifiers);

        CefRefPtr<CefBrowserHost> host = browser->GetHost();
        switch (event.action)
        {
        case PointerAction::move:
            host->SendMouseMoveEvent(mouse, /*mouseLeave*/ false);
            break;
        case PointerAction::leave:
            // The explicit leave is what stops a control staying hover-highlighted after the
            // pointer has left the window.
            host->SendMouseMoveEvent(mouse, /*mouseLeave*/ true);
            break;
        case PointerAction::down:
            host->SendMouseClickEvent(mouse, to_cef_button(event.button), /*mouseUp*/ false,
                                      event.click_count);
            break;
        case PointerAction::up:
            host->SendMouseClickEvent(mouse, to_cef_button(event.button), /*mouseUp*/ true,
                                      event.click_count);
            break;
        case PointerAction::wheel:
            host->SendMouseWheelEvent(mouse, event.wheel_delta_x, event.wheel_delta_y);
            break;
        default:
            break;
        }
    }

    void send_key(const KeyEvent& event) override
    {
        CefRefPtr<CefBrowser> browser = client_->browser();
        if (browser == nullptr)
        {
            return;
        }
        CefKeyEvent key;
        key.type = to_cef_key_type(event.action);
        key.modifiers = to_cef_modifiers(event.modifiers);
        key.windows_key_code = event.windows_key_code;
        key.native_key_code = event.native_key_code;
        key.is_system_key = event.is_system_key ? 1 : 0;
        key.character = static_cast<char16_t>(event.character);
        key.unmodified_character = key.character;
        browser->GetHost()->SendKeyEvent(key);
    }

    void set_focus(bool focused) override
    {
        CefRefPtr<CefBrowser> browser = client_->browser();
        if (browser != nullptr)
        {
            browser->GetHost()->SetFocus(focused);
        }
    }

    bool pump(IBrowserFrameSink& sink) override
    {
        // THE SINK STAYS BOUND PAST THIS CALL (browser.h § IBrowserHost::pump). It is tempting to
        // scope it to the call — one browser, one pump, one sink — and that is what e10a shipped
        // first. It is WRONG as soon as there are two windows, because `CefDoMessageLoopWork()` is
        // PROCESS-WIDE: it drains the pending work of EVERY browser in the process, not just this
        // one. Window 0's pump therefore dispatches window 1's `OnPaint` — at which point window 1's
        // own sink was still null, so its frame was dropped on the floor (OnPaint above). The owner
        // loop pumps window 0 first and every tick's work accumulates during the inter-tick sleep,
        // so window 0's call won that race essentially every time: window 1 never composited a
        // single frame in 30 seconds, deterministically, on both CI legs.
        //
        // Keeping the binding live means whichever browser's pump happens to drain the loop, each
        // frame reaches ITS OWN window's compositor. Delivery is still synchronous and still
        // copy-free (the sink consumes CEF's buffer inside the callback), and still single-threaded:
        // every `CefDoMessageLoopWork()` in this process runs on the one owner thread.
        //
        // LIFETIME: the caller must keep `sink` alive until `close()` or this host's destruction,
        // both of which unbind it. `EditorWindow` satisfies that by construction — the sink is its
        // own `compositor_` member and the host is its `browser_` member, so no pump can run between
        // the compositor's destruction and the unbind in `~CefBrowserHostImpl`.
        client_->set_sink(&sink);
        // The integrated pump. PumpSchedule::should_pump carries the whole policy — run when CEF's
        // scheduled work is due, and run anyway on the UNCONDITIONAL floor when nothing is
        // scheduled, which keeps the browser live if a schedule is ever missed. Both are cheap:
        // DoMessageLoopWork with no work pending returns immediately.
        if (app_ == nullptr || app_->should_pump())
        {
            CefDoMessageLoopWork();
        }
        return !client_->closed();
    }

    void execute_script(std::string_view source) override
    {
        CefRefPtr<CefBrowser> browser = client_->browser();
        if (browser == nullptr)
        {
            return;
        }
        CefRefPtr<CefFrame> frame = browser->GetMainFrame();
        if (frame == nullptr)
        {
            return;
        }
        // The script URL is what the renderer attributes errors to; naming this seam (rather than
        // passing the app origin) keeps a Shell-injected script distinguishable from editor-core's
        // own code in a console trace.
        frame->ExecuteJavaScript(CefString(std::string(source)), "context-editor://shell/inject",
                                 0);
    }

    void request_close() override
    {
        // Phase 1 of a serialised teardown (browser.h § teardown). UNBIND FIRST — `client_->begin_close()`
        // drops the sink and latches `closing_`, so a frame delivered during the drain that follows is
        // dropped instead of dispatched into a compositor that is going away (CE #319's shape). Then ask
        // CEF to close, but DO NOT pump: the WindowManager pumps the shared loop exactly once for the
        // whole teardown, so no per-window close-drain can advance another window into a re-entrant
        // final destruction (the e10a Windows `!in_dtor_` abort). Idempotent.
        client_->begin_close();
        CefRefPtr<CefBrowser> browser = client_->browser();
        // Closing after CefShutdown is UB (a host destroyed during static teardown); guarding on
        // g_initialized makes a late close a no-op. `close_requested_` keeps a second call — the
        // destructor's `close()` after the manager already closed us — from re-issuing CloseBrowser.
        if (browser == nullptr || !g_initialized || close_requested_)
        {
            return;
        }
        browser->GetHost()->CloseBrowser(/*force_close*/ true);
        close_requested_ = true;
    }

    [[nodiscard]] bool is_closed() const override
    {
        // Closed once CEF's OnBeforeClose has released the browser reference (`client_->closed()`),
        // or once CEF itself is gone (a close during static teardown is a no-op that is already done).
        return !g_initialized || client_->closed();
    }

    void pump_teardown() override
    {
        // Phase 2: one slice of the PROCESS-WIDE loop, no sink bound. Unconditional (not gated on
        // should_pump) — teardown must drain every pending OnBeforeClose, not wait for a schedule.
        // DoMessageLoopWork with nothing pending returns immediately, so an extra slice is free.
        if (g_initialized)
        {
            CefDoMessageLoopWork();
        }
    }

    void detach() override
    {
        // Retire mid-process WITHOUT closing (browser.h § IBrowserHost::detach). `begin_close()` drops
        // the sink and latches `closing_`, so this browser stops painting into a compositor that is
        // going away AND a later paint is not miscounted as a lost frame — but it does NOT issue
        // `CloseBrowser`, so NO `CefDoMessageLoopWork()` runs here and CEF is not asked to tear this
        // browser down while sibling browsers are live. The actual close + drain is deferred to the
        // WindowManager's shared, all-closing `shutdown()` (the e10a Windows `!in_dtor_` fix). Pumping
        // nothing here is the whole point; a stray pump is exactly the interleaving this avoids.
        client_->begin_close();
    }

    void close() override
    {
        // The SINGLE-window / destructor path: request the close and drain THIS browser closed in one
        // call, exactly as before. `~CefBrowserHostImpl` relies on it for a host that simply goes out
        // of scope, and the sibling single-window smokes + the app's window 0 reach teardown through
        // it. Multi-window teardown instead calls request_close()/pump_teardown() so the drain is
        // SHARED across all windows (see the WindowManager) rather than run once per browser — which is
        // the interleaving that faulted `!in_dtor_` on Windows. The unbind-before-pump invariant is
        // preserved: request_close() calls begin_close() before this drain runs.
        request_close();
        // OnBeforeClose is what releases the browser reference; leaving it pending would leak the
        // browser past CefShutdown. A no-op once request_close() already saw g_initialized false.
        if (!g_initialized)
        {
            return;
        }
        for (int i = 0; i < 200 && !client_->closed(); ++i)
        {
            CefDoMessageLoopWork();
        }
    }

private:
    CefRefPtr<ShellCefClient> client_;
    CefRefPtr<ShellCefApp> app_;
    // Set once CloseBrowser has been issued, so a second close request (the destructor's `close()`
    // after the manager already tore us down) does not re-issue it.
    bool close_requested_ = false;
};

} // namespace

int execute_subprocess(int argc, char** argv)
{
#if defined(__APPLE__)
    // On macOS the framework is LOADED at runtime, never linked, and the helper processes run from
    // their own bundles — so this entry point is not the subprocess path there.
    (void)argc;
    (void)argv;
    return -1;
#else
#if defined(_WIN32)
    CefMainArgs main_args(::GetModuleHandleW(nullptr));
    (void)argc;
    (void)argv;
#else
    CefMainArgs main_args(argc, argv);
#endif
    if (g_app == nullptr)
    {
        g_app = new ShellCefApp;
    }
    return CefExecuteProcess(main_args, g_app.get(), nullptr);
#endif
}

std::unique_ptr<IBrowserHost> make_cef_browser_host(const CefShellOptions& options,
                                                    std::string& error)
{
#if defined(__APPLE__)
    static CefScopedLibraryLoader library_loader;
    static bool library_loaded = library_loader.LoadInMain();
    if (!library_loaded)
    {
        error = "failed to load the CEF framework (LoadInMain)";
        return nullptr;
    }
#endif

    if (g_app == nullptr)
    {
        g_app = new ShellCefApp;
    }

    if (!g_initialized)
    {
#if defined(_WIN32)
        CefMainArgs main_args(::GetModuleHandleW(nullptr));
#else
        CefMainArgs main_args(0, nullptr);
#endif
        CefSettings settings;
        settings.no_sandbox = true;
        settings.windowless_rendering_enabled = true;
        // The single-threaded owner loop (03 §1): CEF does NOT own a thread, and asks to be driven
        // through OnScheduleMessagePumpWork. The design REJECTS the spike's multi-threaded+mutex
        // caveat in favour of this.
        settings.multi_threaded_message_loop = false;
        settings.external_message_pump = true;
        // Latch the opt-in BEFORE CefInitialize: OnBeforeCommandLineProcessing (which reads the flag)
        // runs INSIDE this CefInitialize, and the browser-process log level is a CefSettings field.
        g_verbose_logging = options.verbose_logging;
        settings.log_severity =
            options.verbose_logging ? LOGSEVERITY_VERBOSE : LOGSEVERITY_WARNING;
        if (options.devtools_enabled && options.remote_debugging_port > 0)
        {
            // Dev loop ONLY (review B-F11): a naive DevTools pass-through from an OSR browser does
            // not display, so the port is the working route — and an open debugging port in a
            // shipped editor is a security hole, which is why it is off unless asked for twice.
            settings.remote_debugging_port = options.remote_debugging_port;
        }

        // Chromium takes a process-singleton lock on the cache root, so two editors sharing one
        // would deadlock on boot. Per-PID by default (mirrors editor_host.cpp).
        std::error_code ec;
        std::filesystem::path cache = options.cache_root;
        if (cache.empty())
        {
#if defined(_WIN32)
            const long long pid = static_cast<long long>(::GetCurrentProcessId());
#else
            const long long pid = static_cast<long long>(::getpid());
#endif
            cache = std::filesystem::temp_directory_path(ec) /
                    ("context-editor-shell-" + std::to_string(pid));
        }
        std::filesystem::create_directories(cache, ec);
#if defined(_WIN32)
        CefString(&settings.root_cache_path).FromWString(cache.wstring());
#else
        CefString(&settings.root_cache_path).FromString(cache.string());
#endif

        if (!CefInitialize(main_args, settings, g_app.get(), nullptr))
        {
            error = "CefInitialize failed";
            return nullptr;
        }
        g_initialized = true;
    }

    // --- the app scheme (e05c) --------------------------------------------------------------------
    // Registered AFTER CefInitialize (CefRegisterSchemeHandlerFactory requires an initialized
    // browser process) and only once. The SCHEME itself was already declared in every process by
    // ShellCefApp::OnRegisterCustomSchemes; this attaches the handler that answers for it.
    if (!options.app_asset_root.empty() && g_asset_resolver == nullptr)
    {
        // Leaked ON PURPOSE, to a TU-local pointer: the factory CEF holds may answer a request on
        // the IO thread at any point up to CefShutdown, so the resolver must outlive every browser.
        // A function-local static would be destroyed at exit, and a member of the host would die
        // with the first window closed. shutdown() clears the pointer.
        auto* resolver = new AppAssetResolver(options.app_asset_root);
        if (!resolver->root_exists())
        {
            // REPORTED, not fatal: the editor still boots, the scheme still answers (404), and the
            // operator gets told exactly which directory was missing rather than watching a blank
            // window and guessing.
            std::fprintf(stderr,
                         "[shell-cef] app asset root does not exist: %s — context-editor://app/ "
                         "will serve 404 (no file:// fallback exists by design)\n",
                         options.app_asset_root.string().c_str());
        }
        g_asset_resolver = resolver;
        if (!CefRegisterSchemeHandlerFactory(kAppScheme, kAppHost, new AppSchemeFactory()))
        {
            error = "CefRegisterSchemeHandlerFactory failed for context-editor://app";
            return nullptr;
        }
    }

    CefRefPtr<ShellCefClient> client(
        new ShellCefClient(options.logical_size, options.dpi, options.bridge));

    CefWindowInfo window_info;
#if defined(_WIN32)
    // WINDOWED-OSR: the native window OWNS the device context while rendering stays off-screen.
    // A null handle degrades to a fully windowless browser, which is the honest headless config.
    window_info.SetAsWindowless(static_cast<HWND>(options.native_window));
#else
    // The X11/NSView handles are e12's; until then the non-Windows browser is windowless.
    (void)options.native_window;
    window_info.SetAsWindowless(0);
#endif
    // shared_texture_enabled is deliberately LEFT AT ITS DEFAULT (off): the accelerated OSR path is
    // unreachable by policy on Windows per the owner ruling of 2026-07-19, and asking CEF for a
    // shared texture the RHI cannot import would produce frames nothing can composite.
    (void)options.accelerated_osr;

    CefBrowserSettings browser_settings;
    browser_settings.windowless_frame_rate =
        options.windowless_frame_rate > 0 ? options.windowless_frame_rate : 60;

    CefRefPtr<CefBrowser> browser = CefBrowserHost::CreateBrowserSync(
        window_info, client, options.url, browser_settings, nullptr, nullptr);
    if (browser == nullptr)
    {
        error = "CreateBrowserSync failed";
        return nullptr;
    }
    error.clear();
    return std::make_unique<CefBrowserHostImpl>(client, g_app);
}

int browsers_created()
{
    return g_browsers_created;
}

int popups_suppressed()
{
    return g_popups_suppressed;
}

int frames_dropped_without_sink()
{
    return g_frames_dropped_without_sink;
}

void shutdown()
{
    if (!g_initialized)
    {
        return;
    }
    g_initialized = false;
    g_app = nullptr;
    // Clear the factories BEFORE CefShutdown: a factory that answered after teardown would reach a
    // resolver this function is about to abandon.
    CefClearSchemeHandlerFactories();
    CefShutdown();
    // Freed only now — every browser is gone and no IO-thread request can still be in flight.
    delete g_asset_resolver;
    g_asset_resolver = nullptr;
}

} // namespace context::editor::shell::cef
