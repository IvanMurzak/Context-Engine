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
#include "context/editor/shell/ext_scheme.h"

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
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h> // getpid()
#endif

namespace context::editor::shell::cef
{

// THE CEF-FREE SCHEME-OPTION MIRROR, CHECKED AGAINST THE REAL API (M9 e13a-1).
//
// ext_scheme.h pins `context-ext://` to STANDARD|SECURE|CORS_ENABLED using its own constants, so
// that pin is asserted by a unit test on all three default `build` legs — where CEF does not exist.
// A mirror nothing compares against the API it mirrors is a comment, so these are the comparison:
// a CEF bump that renumbered `cef_scheme_options_t` fails THIS build loudly instead of quietly
// registering the extension scheme with different security semantics. All seven are checked, not
// just the three that are set — the DENY half (CSP_BYPASSING, LOCAL, DISPLAY_ISOLATED,
// FETCH_ENABLED) is the half whose value being wrong would be invisible.
static_assert(kSchemeOptionStandard == static_cast<unsigned>(CEF_SCHEME_OPTION_STANDARD),
              "CEF renumbered CEF_SCHEME_OPTION_STANDARD — update the mirror in ext_scheme.h");
static_assert(kSchemeOptionLocal == static_cast<unsigned>(CEF_SCHEME_OPTION_LOCAL),
              "CEF renumbered CEF_SCHEME_OPTION_LOCAL — update the mirror in ext_scheme.h");
static_assert(kSchemeOptionDisplayIsolated ==
                  static_cast<unsigned>(CEF_SCHEME_OPTION_DISPLAY_ISOLATED),
              "CEF renumbered CEF_SCHEME_OPTION_DISPLAY_ISOLATED — update ext_scheme.h");
static_assert(kSchemeOptionSecure == static_cast<unsigned>(CEF_SCHEME_OPTION_SECURE),
              "CEF renumbered CEF_SCHEME_OPTION_SECURE — update the mirror in ext_scheme.h");
static_assert(kSchemeOptionCorsEnabled == static_cast<unsigned>(CEF_SCHEME_OPTION_CORS_ENABLED),
              "CEF renumbered CEF_SCHEME_OPTION_CORS_ENABLED — update the mirror in ext_scheme.h");
static_assert(kSchemeOptionCspBypassing == static_cast<unsigned>(CEF_SCHEME_OPTION_CSP_BYPASSING),
              "CEF renumbered CEF_SCHEME_OPTION_CSP_BYPASSING — update the mirror in ext_scheme.h");
static_assert(kSchemeOptionFetchEnabled == static_cast<unsigned>(CEF_SCHEME_OPTION_FETCH_ENABLED),
              "CEF renumbered CEF_SCHEME_OPTION_FETCH_ENABLED — update the mirror in ext_scheme.h");

namespace
{

// WHICH CONVENTION THIS PLATFORM'S SCREEN COORDINATES USE, in ONE place.
//
// CEF documents the same split three times over — `GetScreenInfo::rect`, `GetScreenPoint`, and (by
// its absence) `GetRootScreenRect`: Windows/Linux speak screen DEVICE pixels, macOS speaks screen
// DIP. All three callbacks below read this constant rather than each carrying their own `#if
// defined(__APPLE__)`, because three copies of one platform decision are three chances for the
// macOS branch — the one no CI job in this repo EXECUTES and the local gate cannot even compile —
// to disagree with itself. The ARITHMETIC each callback applies lives in dpi.h, tested on all three
// legs; this constant is only which side of it this build is on.
#if defined(__APPLE__)
constexpr bool kScreenCoordsAreDip = true;
#else
constexpr bool kScreenCoordsAreDip = false;
#endif

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

// The extension-scheme resolver (e13a-1), under the same single-assignment / const-resolve
// discipline as the app one above. UNLIKE the app resolver it is installed UNCONDITIONALLY at boot,
// even with no package mounted: an `ExtAssetResolver` with an empty mount table refuses every
// request, and having OUR deny-by-default handler answer a `context-ext://` request is strictly
// better than leaving the scheme handler-less and inheriting whatever Chromium does with it.
const ExtAssetResolver* g_ext_resolver = nullptr;

bool g_initialized = false;

// Opt-in verbose Chromium logging (CefShellOptions::verbose_logging). Set ONCE in the browser
// process before CefInitialize, read by OnBeforeCommandLineProcessing to append the logging
// switches; CEF then propagates the switches onto the renderer/GPU/utility subprocess command lines
// it builds from the browser process's, so the whole tree logs to stderr. Never mutated after boot.
bool g_verbose_logging = false;

// Opt-in OSCrypt keychain isolation (CefShellOptions::use_mock_keychain), latched exactly like
// g_verbose_logging above: set ONCE in the browser process before CefInitialize and read by
// OnBeforeCommandLineProcessing. Issue #437 — the field's own comment in cef_shell.h carries the
// mechanism.
bool g_use_mock_keychain = false;

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

// The e13a-2 extension-scheme request log (cef_shell.h § the extension-scheme request log).
//
// UNLIKE the three counters above this IS cross-thread: a scheme handler's `Open` runs on the CEF
// IO thread while the smoke reads these from the owner thread between pumps, so the mutex is
// load-bearing rather than defensive. Bounded, because the requester controls the URL.
constexpr std::size_t kExtLogMaxEntries = 64;
constexpr std::size_t kExtLogMaxUrlLength = 256;
std::mutex g_ext_log_mutex;
std::vector<std::string> g_ext_served_urls;
std::vector<std::string> g_ext_refused_urls;

// Truncate an untrusted URL for logging — the same bound the refusal's stderr line applies, for the
// same reason (an unbounded attacker-chosen string in a diagnostic channel).
std::string clamp_logged_url(const std::string& url)
{
    return url.size() <= kExtLogMaxUrlLength ? url
                                             : url.substr(0, kExtLogMaxUrlLength) + "...[truncated]";
}

void record_ext_request(const std::string& url, bool served)
{
    const std::lock_guard<std::mutex> lock(g_ext_log_mutex);
    std::vector<std::string>& log = served ? g_ext_served_urls : g_ext_refused_urls;
    if (log.size() >= kExtLogMaxEntries)
    {
        return;
    }
    log.push_back(clamp_logged_url(url));
}

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

// ------------------------------------------------- the custom-scheme resource handlers (app + ext)

// The BYTE PLUMBING both custom schemes share: buffer a resolved file, hand it to CEF a chunk at a
// time, and build the two response shapes (refused / served) from a resolution's status + a header
// list. It holds NO policy — which URLs are in bounds, what a path may contain, which media types
// exist, what the CSP says all live in the CEF-free resolvers next door, where they are
// adversarially unit-tested on all three default `build` legs.
//
// ONE COPY, deliberately: e13a-1 added a second scheme, and a near-verbatim second copy of a
// security-adjacent buffer/offset loop in the ONE translation unit the local dev gate cannot build
// is exactly where a divergence would go unseen. The derived handlers below are what remains —
// `Open` (ask MY resolver) and `GetResponseHeaders` (send MY policy), and nothing else.
class SchemeResourceHandlerBase : public CefResourceHandler
{
public:
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

protected:
    // Read the whole asset up front. These are bundled UI assets (hundreds of KB, not media), so
    // streaming would buy nothing and would leave the file handle open across callbacks for no
    // reason. Returns false when the file could not be read AT ALL or was read only PART WAY — a
    // truncated bundle otherwise presents as a baffling syntax error in the renderer rather than as
    // an IO failure, so the caller degrades it to not_found.
    bool load_body(const std::filesystem::path& file)
    {
        std::ifstream stream(file, std::ios::binary);
        if (!stream)
        {
            return false;
        }
        body_.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
        if (stream.bad())
        {
            body_.clear();
            return false;
        }
        return true;
    }

    // The REFUSED response. WHICH headers a refusal carries is policy and lives in the CEF-free
    // `refusal_headers()` (app_scheme.h), where both scheme suites pin it on all three `build`
    // legs; this is only the translation into CEF's map.
    void write_refusal(CefRefPtr<CefResponse> response, const char* csp, int64_t& response_length)
    {
        response->SetMimeType("text/plain");
        CefResponse::HeaderMap headers;
        for (const auto& [name, value] : refusal_headers(csp))
        {
            headers.insert({name, value});
        }
        response->SetHeaderMap(headers);
        response_length = 0;
    }

    // The SERVED response.
    //
    // MIME ESSENCE AND CHARSET ARE TWO SEPARATE FIELDS — see split_media_type() in app_scheme.h for
    // the full trap. Passing the resolver's `text/css; charset=utf-8` straight into SetMimeType()
    // makes Chromium's by-essence comparison fail, and with the `X-Content-Type-Options: nosniff`
    // these responses also set, the stylesheet and the ES module are then silently refused and the
    // document is not parsed as HTML.
    void write_asset(CefRefPtr<CefResponse> response, const std::string& mime_type,
                     const std::vector<std::pair<std::string, std::string>>& header_list,
                     int64_t& response_length)
    {
        const MediaType media = split_media_type(mime_type);
        response->SetMimeType(media.essence);
        if (!media.charset.empty())
        {
            response->SetCharset(media.charset);
        }
        CefResponse::HeaderMap headers;
        for (const auto& [name, value] : header_list)
        {
            // CEF derives the Content-Type from the mime type + charset set above; setting it in
            // the map as well makes the response carry it twice, which some parsers treat as a
            // conflict. Compared case-INSENSITIVELY (header names are) via the CEF-free, unit-tested
            // `ascii_iequals` rather than a comparison loop written in this TU, which neither the
            // local gate nor any unit suite can exercise.
            if (ascii_iequals(name, "content-type"))
            {
                continue;
            }
            headers.insert({name, value});
        }
        response->SetHeaderMap(headers);
        response_length = static_cast<int64_t>(body_.size());
    }

    std::string body_;
    std::size_t offset_ = 0;
};

// Serves ONE `context-editor://app/…` request from the built asset set.
class AppSchemeResourceHandler final : public SchemeResourceHandlerBase
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

        if (!load_body(resolution_.file))
        {
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
            write_refusal(response, app_csp_header(), response_length);
            return;
        }
        write_asset(response, resolution_.mime_type, app_response_headers(resolution_.mime_type),
                    response_length);
    }

private:
    AssetResolution resolution_;

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

// Serves ONE `context-ext://<package-id>/…` request from ONE mounted package (M9 e13a-1).
//
// The thinnest possible translator, and for a sharper reason than the app handler's: the code on
// the other end of this response is UNTRUSTED third-party panel code, so every judgement about it
// — is that a real package, may that path leave the package root, may that media type be served at
// all — belongs in `ExtAssetResolver`, which the local dev gate and all three `build` legs
// adversarially test. Nothing here decides anything.
//
// A null `g_ext_resolver` refuses with 403, NOT 404: the resolver is installed unconditionally at
// boot, so a null one means the Shell is not serving this scheme in this process at all, and
// "forbidden" is the honest answer. It also keeps the refusal indistinguishable from an unknown
// package, which is the property `ExtAssetResolver` maintains on purpose.
class ExtSchemeResourceHandler final : public SchemeResourceHandlerBase
{
public:
    bool Open(CefRefPtr<CefRequest> request, bool& handle_request,
              CefRefPtr<CefCallback>) override
    {
        handle_request = true;

        const std::string url = request->GetURL().ToString();
        if (g_ext_resolver == nullptr)
        {
            resolution_.status = AssetStatus::forbidden;
            resolution_.reason = "no extension resolver is installed in this process";
        }
        else
        {
            resolution_ = g_ext_resolver->resolve(url);
            // IS CEF ABOUT TO MAKE A DOCUMENT OUT OF THESE BYTES? (M9 e13b-1.)
            //
            // The ONE fact this TU knows that the resolver cannot. The resolver sees a URL and a
            // media type, never the resource type — and that distinction is what the panel-port
            // mechanism rests on, because the bootstrap is spliced into `text/html` and nothing else.
            // A scriptable non-HTML document (`image/svg+xml` is both scriptable and on the shared
            // asset allowlist) would run package code with NO bootstrap ahead of it and could then
            // navigate the frame onward to claim a grant it never earned — ext_scheme.h property 1
            // carries the full attack.
            //
            // ONE BIT IS PASSED AND NOTHING ELSE IS DECIDED: which media types may be documents, and
            // the status + reason of the refusal, both live in `ext_apply_document_gate` where all
            // three `build` legs assert them. Narrow on purpose — only an explicit main/sub-frame
            // navigation is gated, so every subresource a panel legitimately loads (an `<img>` of
            // that same `.svg`, a script, a style) is untouched and still served.
            const cef_resource_type_t resource_type = request->GetResourceType();
            ext_apply_document_gate(resolution_, resource_type == RT_MAIN_FRAME ||
                                                    resource_type == RT_SUB_FRAME);
        }
        if (!resolution_.ok())
        {
            // Logged, never sent — a refusal reason is a probe oracle, and this scheme's whole
            // point is that the requester may be hostile.
            //
            // TRUNCATED for the same reason `kExtPackageIdMaxLength` exists: the requester here is
            // untrusted BY CONSTRUCTION and controls this string, so an unbounded URL is an
            // unbounded attacker-chosen write into the operator's diagnostic channel on every
            // refusal. The package id is bounded by the grammar; the path is not.
            const std::string logged = clamp_logged_url(url);
            std::fprintf(stderr, "[shell-cef] ext scheme refused <%s>: %s\n", logged.c_str(),
                         resolution_.reason.c_str());
            record_ext_request(url, /*served*/ false);
            return true;
        }

        if (resolution_.synthetic)
        {
            // The ONE asset this scheme serves out of ITSELF (M9 e13b-1): the panel-port bootstrap.
            // Its bytes are ours, so there is no file to read — ext_scheme.h § the panel-port
            // bootstrap, and `ExtAssetResolver::resolve` is where the request earned this branch.
            body_ = ext_port_bootstrap_script();
        }
        else if (!load_body(resolution_.file))
        {
            resolution_.status = AssetStatus::not_found;
            record_ext_request(url, /*served*/ false);
            return true;
        }
        else
        {
            // The e13b-1 bootstrap splice, called with NO MEDIA-TYPE BRANCH of its own: which media
            // types are rewritten and where the tag lands are BOTH decided inside the CEF-free
            // `ext_inject_port_bootstrap`, which the local dev gate and all three `build` legs
            // unit-test against adversarial document prefixes. A media-type COMPARISON written HERE
            // would be policy in the one TU nothing local can compile — the mistake this whole
            // handler is shaped to avoid, and why the navigation gate above asks a predicate rather
            // than spelling `text/html` out a second time.
            body_ = ext_inject_port_bootstrap(resolution_.mime_type, body_);
        }
        // Recorded only once the BYTES are in hand, so `ext_served_urls()` means "this asset was
        // actually delivered" rather than "the resolver approved of it" — the smoke's whole chain of
        // inference (the document PARSED, therefore its subresources were requested; its module RAN,
        // therefore its own import resolved) rests on the served list meaning the former.
        record_ext_request(url, /*served*/ true);
        return true;
    }

    void GetResponseHeaders(CefRefPtr<CefResponse> response, int64_t& response_length,
                            CefString& redirectUrl) override
    {
        redirectUrl.clear();
        response->SetStatus(resolution_.http_status());
        if (!resolution_.ok())
        {
            write_refusal(response, ext_csp_header(), response_length);
            return;
        }
        write_asset(response, resolution_.mime_type, ext_response_headers(resolution_.mime_type),
                    response_length);
    }

private:
    ExtResolution resolution_;

    IMPLEMENT_REFCOUNTING(ExtSchemeResourceHandler);
};

class ExtSchemeFactory final : public CefSchemeHandlerFactory
{
public:
    CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                                         const CefString&, CefRefPtr<CefRequest>) override
    {
        return new ExtSchemeResourceHandler();
    }

private:
    IMPLEMENT_REFCOUNTING(ExtSchemeFactory);
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
        // Sub-frame navigations are not gated here — but the REASON changed with e13a-1 and the
        // old one ("`frame-src 'none'` already denies them") is no longer true. editor-core's
        // policy is now `frame-src context-ext:`, so sub-frames DO exist: sandboxed third-party
        // panels. What still bounds them is that SAME directive — CSP `frame-src` is re-evaluated
        // on every navigation of a nested browsing context, not only on its first load, so a panel
        // that tries to navigate ITSELF anywhere off `context-ext:` is refused by the parent's
        // policy; and a `sandbox="allow-scripts"` frame without `allow-top-navigation` cannot move
        // the main frame, which the check just above independently pins to the app origin anyway.
        // A belt-and-braces sub-frame allowlist here would be a reasonable e13a-2 hardening once a
        // live iframe smoke exists to prove it does not over-block; adding one blind, in the TU the
        // local gate cannot build, would be a guess.
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

    // a1 (audit D12, docs/shell.md § 16): the two members that say WHERE the view is. Unimplemented,
    // both default to `false` and CEF then treats view coordinates as SCREEN coordinates — which is
    // the reported offset context menu: the menu opens at the cursor's position measured from the
    // screen origin instead of the window's.
    //
    // ⚠ THE TWO DO NOT SHARE A CONVENTION (pinned cef_render_handler.h): `GetScreenPoint` takes the
    // per-platform device/DIP split, `GetRootScreenRect` is DIP everywhere. Both call into dpi.h
    // rather than deciding here, so the difference is pinned by `editor-shell-test_dpi` on all three
    // legs instead of living in the one TU no local gate compiles.
    bool GetRootScreenRect(CefRefPtr<CefBrowser>, CefRect& rect) override
    {
        const ScreenRect root =
            osr_root_screen_rect(client_origin_, logical_size_, dpi_, kScreenCoordsAreDip);
        // CefRect::Set takes ints and the origin is legitimately NEGATIVE (a monitor left of or
        // above the primary one), which is why the dpi.h rect carries a signed origin rather than
        // render::Rect2D's unsigned one — clamping here would report a window on a left-hand monitor
        // as if it sat at the screen origin.
        rect.Set(root.origin.x, root.origin.y, static_cast<int>(root.size.width),
                 static_cast<int>(root.size.height));
        return true;
    }

    bool GetScreenPoint(CefRefPtr<CefBrowser>, int viewX, int viewY, int& screenX,
                        int& screenY) override
    {
        const PointI screen =
            osr_screen_point(PointI{viewX, viewY}, client_origin_, dpi_, kScreenCoordsAreDip);
        screenX = screen.x;
        screenY = screen.y;
        return true;
    }

    bool GetScreenInfo(CefRefPtr<CefBrowser>, CefScreenInfo& screen_info) override
    {
        // The other half of real DPI: the scale CEF multiplies the DIP view rect by to decide how
        // many PHYSICAL pixels to paint. Without it a 2x monitor gets a 1x-resolution UI.
        screen_info.device_scale_factor = dpi_.factor();
        // CefScreenInfo::rect is a RAW cef_rect_t (unlike CefRect it carries no Set()). Which
        // convention it wants is a per-platform choice; the ARITHMETIC lives in dpi.h so both
        // branches are compiled and tested on all three legs (see osr_screen_extent), and WHICH
        // branch is the file-scope kScreenCoordsAreDip this callback shares with the two below.
        const render::Extent2D screen = osr_screen_extent(logical_size_, dpi_, kScreenCoordsAreDip);
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

    // a1: where the view sits on screen, in this platform's screen convention. STORED ONLY — CEF
    // pulls it through GetScreenPoint / GetRootScreenRect when it needs it (opening a menu), so
    // there is nothing to notify. `NotifyScreenInfoChanged` is a separate audited gap
    // (docs/shell.md § 16) and stays one: nothing here claims to close it.
    void set_client_origin(PointI origin) { client_origin_ = origin; }

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
    // The window's client origin on screen (a1). Zero until the owner loop's first push, which is
    // the honest pre-boot state and reproduces the old behaviour exactly rather than inventing a
    // position; `EditorWindow::sync_browser_size` pushes it before the first pump.
    PointI client_origin_{};
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
        //
        // THE RETURN VALUE IS CHECKED on both schemes below. `AddCustomScheme` returns false if the
        // name is already registered or registration failed, and the consequence is invisible and
        // severe: documents on that scheme silently get OPAQUE origins instead of the pinned
        // semantics. The static_asserts above pin the flag VALUES at compile time; this is the one
        // runtime step that actually applies them, so a failure must not pass unremarked.
        if (!registrar->AddCustomScheme(kAppScheme, CEF_SCHEME_OPTION_STANDARD |
                                                        CEF_SCHEME_OPTION_SECURE |
                                                        CEF_SCHEME_OPTION_CORS_ENABLED |
                                                        CEF_SCHEME_OPTION_FETCH_ENABLED))
        {
            std::fprintf(stderr,
                         "[shell-cef] AddCustomScheme(%s) FAILED — documents on this scheme will "
                         "have opaque origins\n",
                         kAppScheme);
        }

        // The EXTENSION scheme (M9 e13a-1, design 04 §5 / 08 §1-§2). Registered here, in the same
        // every-process hook and for the same reason: a per-package origin only behaves like an
        // origin if the RENDERER agrees it is one, and a sandboxed panel's whole containment story
        // is origin-based.
        //
        // The flag set is `kExtSchemeOptions` from the CEF-free ext_scheme.h rather than a second
        // spelling of the CEF enumerators here: that constant is what the unit suite asserts on the
        // three legs where CEF does not exist, and the file-scope static_asserts above are what
        // keep it equal to CEF's own values. Writing the bits out again here would leave the tested
        // constant and the registered value free to drift — which, on a security boundary whose
        // only other witness is one CI job, is precisely the failure worth engineering out.
        //
        // NOTE the deliberate difference from the app scheme one line up: NO FETCH_ENABLED (a panel
        // has `connect-src 'none'` and gets no Fetch surface of its own) and, as there, no
        // CSP_BYPASSING and no LOCAL. ext_scheme.h documents each omission.
        if (!registrar->AddCustomScheme(kExtScheme, static_cast<int>(kExtSchemeOptions)))
        {
            std::fprintf(stderr,
                         "[shell-cef] AddCustomScheme(%s) FAILED — panel documents will have "
                         "opaque origins, not the pinned STANDARD|SECURE|CORS_ENABLED\n",
                         kExtScheme);
        }
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
        // Opt-in OSCrypt keychain isolation (CefShellOptions::use_mock_keychain, issue #437). MUST
        // be a command-line switch rather than a CefSettings field: Chromium's own
        // chrome_browser_main_mac layer reads it off the command line to install the fake keychain
        // BEFORE the first OSCrypt use, and there is no CEF API surface for it. Appended for the
        // whole process tree exactly like the logging switches above.
        if (g_use_mock_keychain)
        {
            command_line->AppendSwitch("use-mock-keychain");
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

    void set_client_origin(PointI origin) override
    {
        // No WasResized()/notify: the browser has not resized, and CEF re-reads the screen mapping
        // on demand. This is what makes pushing on every window-move step cheap (browser.h).
        client_->set_client_origin(origin);
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

int execute_helper_process(int argc, char** argv)
{
#if defined(__APPLE__)
    // A helper process must load the framework from the PARENT app's Frameworks dir (the helper
    // variant of the search), before any CEF call. Scoped to this function: `CefExecuteProcess` blocks
    // for the whole life of the subprocess, so the unload happens as this function returns — which is
    // BEFORE static teardown, and is why `g_app` is released explicitly below rather than left to it.
    CefScopedLibraryLoader library_loader;
    if (!library_loader.LoadInHelper())
    {
        std::fprintf(stderr, "[shell-cef] helper: LoadInHelper() failed — the Chromium Embedded "
                             "Framework is not embedded in the parent app bundle's "
                             "Contents/Frameworks\n");
        return 1;
    }

    // The REAL app object, not nullptr — see cef_shell.h's contract for the three renderer-process
    // duties that would otherwise never run. Same TU-global instance the browser process creates, so
    // the two halves of `OnRegisterCustomSchemes` and the message router cannot drift.
    if (g_app == nullptr)
    {
        g_app = new ShellCefApp;
    }
    CefMainArgs main_args(argc, argv);
    const int exit_code = CefExecuteProcess(main_args, g_app.get(), nullptr);

    // ⚠ RELEASE THE APP HERE, NOT AT STATIC TEARDOWN — an ordering bug, not tidiness. `g_app` is a
    // TU-global, so left alone it is released AFTER this function returns, i.e. after
    // `~CefScopedLibraryLoader` has already called `cef_unload_library()` (dlclose). `~ShellCefApp`
    // drops `renderer_router_`, and a `CefMessageRouterRendererSide` can still hold library-side
    // `CefRefPtr`s (the V8 context / callbacks of any query that had not completed) whose `Release()`
    // is a function pointer INSIDE the image just unmapped. Releasing while the framework is still
    // loaded makes the order well-defined.
    //
    // Neither precedent helper can reach this: both pass `nullptr` and hold no app object at all
    // (`src/editor/cef/src/cef_boot_smoke_helper_mac.cpp`,
    // `src/editor/gui/host/src/editor_host_helper_mac.cpp`) — the real `CefApp` this helper must pass
    // (see cef_shell.h) is exactly what introduces the hazard. The BROWSER process was already safe
    // the same way: `shutdown()` nulls `g_app` before `CefShutdown()`.
    g_app = nullptr;
    return exit_code;
#else
    (void)argc;
    (void)argv;
    std::fprintf(stderr, "[shell-cef] execute_helper_process() is macOS-only — Windows and Linux "
                         "re-exec the main binary (execute_subprocess). Refusing.\n");
    return 1;
#endif
}

int execute_subprocess(int argc, char** argv)
{
#if defined(__APPLE__)
    // On macOS the framework is LOADED at runtime, never linked, and the helper processes run from
    // their own bundles — so this entry point is not the subprocess path there. `execute_helper_process`
    // above is that path.
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
        // Same latch-before-CefInitialize reason as the line above: OnBeforeCommandLineProcessing
        // runs INSIDE this CefInitialize, and it is the only place the switch can be appended.
        g_use_mock_keychain = options.use_mock_keychain;
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

    // --- the extension scheme (e13a-1) -------------------------------------------------------------
    // UNCONDITIONAL, unlike the app scheme above: it is installed whether or not any package is
    // mounted, because an `ExtAssetResolver` with an empty mount table is a complete configuration
    // that refuses everything. Leaving `context-ext://` handler-less until the first package
    // installs (e13b+) would mean the deny-by-default answer came from Chromium's unhandled-scheme
    // path rather than from the boundary this task exists to build — and would make the boundary
    // untestable in exactly the window where nothing is installed.
    //
    // Registered with an EMPTY domain, so it answers for EVERY host under the scheme. That is the
    // right shape here: the package is the host, hosts are not known at CefInitialize time, and
    // "which package is real" is the resolver's mount-table decision, not CEF's routing decision.
    if (g_ext_resolver == nullptr)
    {
        // Leaked ON PURPOSE, exactly like the app resolver: the factory CEF holds may answer on the
        // IO thread at any point up to CefShutdown, so it must outlive every browser. shutdown()
        // frees it.
        auto* ext_resolver = new ExtAssetResolver();
        for (const ExtPackageMount& package : options.ext_packages)
        {
            std::string reason;
            // The store root is passed on EVERY mount (M9 e13c-3): `mount()` refuses a root that did
            // not come from it, and refuses everything when it is empty. See
            // CefShellOptions::ext_store_root on why that is a required argument and not a member the
            // resolver could be left holding.
            if (!ext_resolver->mount(package.id, package.root, options.ext_store_root, reason))
            {
                // REPORTED, not fatal: the editor still boots and every other package still works.
                // A refused mount means that ONE origin serves 403 — the deny-by-default outcome —
                // and the operator is told which package and why instead of watching an empty panel.
                std::fprintf(stderr,
                             "[shell-cef] extension package NOT mounted: id=<%s> root=<%s>: %s — "
                             "context-ext://%s/ will refuse every request\n",
                             package.id.c_str(), package.root.string().c_str(), reason.c_str(),
                             package.id.c_str());
            }
        }
        // PUBLISHED BEFORE the factory is registered, and that ordering is load-bearing rather than
        // incidental: until `CefRegisterSchemeHandlerFactory` returns there is no factory, so no
        // handler, so no IO-thread reader of this pointer — the registration call is what carries
        // the write across to that thread. Do NOT "fix" the failure path below by moving this line
        // after the registration.
        g_ext_resolver = ext_resolver;
        if (!CefRegisterSchemeHandlerFactory(kExtScheme, /*domain*/ CefString(),
                                             new ExtSchemeFactory()))
        {
            // UNPUBLISH on failure. Leaving a non-null resolver with no factory registered would
            // permanently trip the `g_ext_resolver == nullptr` guard above, so a retry would skip
            // this whole block and leave `context-ext://` handler-less for the process lifetime —
            // the one state this block exists to make impossible.
            delete ext_resolver;
            g_ext_resolver = nullptr;
            error = "CefRegisterSchemeHandlerFactory failed for context-ext://";
            return nullptr;
        }
    }
    else if (!options.ext_packages.empty() || !options.ext_store_root.empty())
    {
        // ⚠ THE STORE ROOT COUNTS TOO, now that it exists: the two fields are one decision
        // (editor_main.cpp says so), so a second window that differs in EITHER is being ignored, and a
        // caller who passed only a store root would otherwise be dropped in total silence.
        // The resolver is PROCESS-GLOBAL while `ext_packages` is a per-call option, so a second
        // window's package list is silently dropped. Said out loud rather than swallowed: it would
        // otherwise present as "the second window's panels all 403 and nothing explains why".
        // See CefShellOptions::ext_packages.
        std::fprintf(stderr,
                     "[shell-cef] %zu extension package mount(s) IGNORED: the ext resolver is "
                     "process-global and was already built by an earlier browser; only the first "
                     "make_cef_browser_host() call's ext_packages take effect\n",
                     options.ext_packages.size());
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

std::vector<std::string> ext_served_urls()
{
    const std::lock_guard<std::mutex> lock(g_ext_log_mutex);
    return g_ext_served_urls;
}

std::vector<std::string> ext_refused_urls()
{
    const std::lock_guard<std::mutex> lock(g_ext_log_mutex);
    return g_ext_refused_urls;
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
    delete g_ext_resolver;
    g_ext_resolver = nullptr;
    // The request log outlives shutdown deliberately: the smokes read their verdict AFTER calling
    // shutdown() (the CE #319 lifetime invariant above forces that order), so clearing it here would
    // erase exactly the evidence they are about to assert on.
}

} // namespace context::editor::shell::cef
