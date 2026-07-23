// The two notification banners (M9 e14d) — see banners.h for the design and the privacy argument.
//
// EVERYTHING NETWORK-FACING IN THIS FILE IS A VALUE, NOT AN ACTION. `build_release_check_request()`
// is the ONLY place an outgoing request is constructed anywhere in the repository (enforced by
// `tools/check_release_request.py`), so "what does the editor send" is answerable by reading one
// function and asserting on one string.

#include "context/editor/shell/banners.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace context::editor::shell
{
namespace
{

// Case-insensitive-free, allocation-light digit walk. Deliberately not `std::stoi`: that throws on
// overflow, and a hostile/garbled tag must degrade to "unknown", never to an exception on the owner
// thread. Saturates instead, which keeps the comparison total.
[[nodiscard]] bool read_component(const std::string& text, std::size_t& index, int& out)
{
    if (index >= text.size() || std::isdigit(static_cast<unsigned char>(text[index])) == 0)
    {
        return false;
    }
    long long value = 0;
    while (index < text.size() && std::isdigit(static_cast<unsigned char>(text[index])) != 0)
    {
        if (value < 1000000000LL)
        {
            value = value * 10 + (text[index] - '0');
        }
        ++index;
    }
    // Clamp rather than wrap: a 30-digit "version" from a hostile feed must read as "very large",
    // never as a negative number that would compare as OLDER and suppress a real update notice.
    constexpr long long kMaxComponent = 2000000000LL;
    out = static_cast<int>(value > kMaxComponent ? kMaxComponent : value);
    return true;
}

} // namespace

// ------------------------------------------------------------------------------- the build's version

const char* editor_version() noexcept
{
#if defined(CONTEXT_EDITOR_VERSION)
    return CONTEXT_EDITOR_VERSION;
#else
    // Only reachable if the build system stopped supplying the define. "0.0.0" compares as older
    // than any real release, so the banner would over-offer rather than silently go quiet — the
    // safe direction for a notify-only surface.
    return "0.0.0";
#endif
}

// ------------------------------------------------------------------------------- the request

HttpRequest build_release_check_request()
{
    // ⚠ READ NOTHING HERE. No version, no OS, no locale, no machine/user/install id, no query
    // string, no body. Every field below is a compile-time constant, which is what makes the request
    // byte-identical for every user on every machine (banners.h property (a)). A test asserts that
    // by golden-comparing `canonical_request_text()`, so ANY addition — even an apparently innocent
    // `X-Client-Version` — fails the build's own gate rather than shipping.
    HttpRequest request;
    request.method = "GET";
    request.url = kReleaseCheckEndpoint;
    request.headers.push_back(HttpHeader{"Accept", kReleaseCheckAccept});
    request.headers.push_back(HttpHeader{"User-Agent", kReleaseCheckUserAgent});
    request.body.clear();
    return request;
}

std::string canonical_request_text(const HttpRequest& request)
{
    std::string text = request.method;
    text += ' ';
    text += request.url;
    text += '\n';
    for (const HttpHeader& header : request.headers)
    {
        text += header.name;
        text += ": ";
        text += header.value;
        text += '\n';
    }
    text += '\n';
    text += request.body;
    return text;
}

// --------------------------------------------------------------------------- version comparison

std::vector<int> parse_version(const std::string& text)
{
    std::size_t index = 0;
    if (index < text.size() && (text[index] == 'v' || text[index] == 'V'))
    {
        ++index;
    }
    std::vector<int> parts;
    int component = 0;
    if (!read_component(text, index, component))
    {
        return {}; // no leading number at all => unknown, never "older"
    }
    parts.push_back(component);
    while (index < text.size() && text[index] == '.')
    {
        const std::size_t after_dot = index + 1;
        std::size_t cursor = after_dot;
        if (!read_component(text, cursor, component))
        {
            break; // `1.2.x` / a trailing dot: stop at the last numeric component
        }
        parts.push_back(component);
        index = cursor;
    }
    return parts;
}

int compare_versions(const std::string& a, const std::string& b)
{
    const std::vector<int> left = parse_version(a);
    const std::vector<int> right = parse_version(b);
    if (left.empty() || right.empty())
    {
        return 0; // unknown on either side => "no opinion", so no banner is ever manufactured
    }
    const std::size_t count = left.size() > right.size() ? left.size() : right.size();
    for (std::size_t i = 0; i < count; ++i)
    {
        const int lv = i < left.size() ? left[i] : 0;
        const int rv = i < right.size() ? right[i] : 0;
        if (lv != rv)
        {
            return lv < rv ? -1 : 1;
        }
    }
    return 0;
}

std::string parse_latest_release_tag(const std::string& body)
{
    if (body.empty() || body.size() > kMaxReleaseBodyBytes)
    {
        return {};
    }
    try
    {
        const contract::Json document = contract::Json::parse(body);
        if (!document.is_object())
        {
            return {};
        }
        const contract::Json& tag = document.at("tag_name");
        return tag.is_string() ? tag.as_string() : std::string{};
    }
    catch (const std::exception&)
    {
        return {}; // a malformed release document means we learned nothing, not that we failed
    }
}

// ------------------------------------------------------------------------------- ReleaseNotice

ReleaseNotice::~ReleaseNotice()
{
    wait_for_check();
}

HttpRequest ReleaseNotice::request() const
{
    HttpRequest built = build_release_check_request();
    built.url = endpoint_; // the ONLY overridable field — see the header's warning
    return built;
}

void ReleaseNotice::begin_check()
{
    if (!transport_ || worker_.joinable())
    {
        return;
    }
    // The worker holds COPIES of everything it touches plus the shared handoff — no reference into
    // this object's mutable state, so an owner-thread field cannot race a worker read.
    HttpTransport transport = transport_;
    const HttpRequest outgoing = request();
    worker_ = std::thread(
        [this, transport, outgoing]()
        {
            HttpResponse response;
            try
            {
                response = transport(outgoing);
            }
            catch (const std::exception& error)
            {
                response.ran = false;
                response.error = error.what();
            }
            catch (...)
            {
                response.ran = false;
                response.error = "release check threw";
            }
            const std::lock_guard<std::mutex> guard(mutex_);
            pending_ = std::move(response);
            pending_ready_ = true;
        });
}

void ReleaseNotice::wait_for_check()
{
    if (worker_.joinable())
    {
        worker_.join();
    }
}

bool ReleaseNotice::poll()
{
    HttpResponse response;
    {
        const std::lock_guard<std::mutex> guard(mutex_);
        if (!pending_ready_)
        {
            return false;
        }
        pending_ready_ = false;
        response = std::move(pending_);
        pending_ = HttpResponse{};
    }
    // Join AFTER releasing the lock: the worker sets `pending_ready_` as its last act, so this
    // returns immediately and never holds the owner loop.
    wait_for_check();
    adopt(response);
    return true;
}

void ReleaseNotice::check_blocking()
{
    begin_check();
    wait_for_check();
    (void)poll();
}

void ReleaseNotice::adopt(const HttpResponse& response)
{
    ++checks_;
    if (!response.ran)
    {
        checked_ = false;
        available_ = false;
        latest_.clear();
        error_ = response.error.empty() ? std::string("the release check did not run") : response.error;
        return;
    }
    if (response.status < 200 || response.status >= 300)
    {
        checked_ = false;
        available_ = false;
        latest_.clear();
        error_ = "the release feed answered HTTP " + std::to_string(response.status);
        return;
    }
    const std::string tag = parse_latest_release_tag(response.body);
    if (tag.empty())
    {
        checked_ = false;
        available_ = false;
        latest_.clear();
        error_ = "the release feed carried no readable version";
        return;
    }
    checked_ = true;
    error_.clear();
    latest_ = tag;
    available_ = compare_versions(current_, tag) < 0;
}

bool ReleaseNotice::dismiss()
{
    const bool changed = !dismissed_;
    dismissed_ = true;
    return changed;
}

bool ReleaseNotice::open_downloads()
{
    if (!opener_ || downloads_.empty())
    {
        return false;
    }
    const bool opened = opener_(downloads_);
    if (opened)
    {
        ++opens_;
    }
    return opened;
}

contract::Json ReleaseNotice::state_json() const
{
    contract::Json state = contract::Json::object();
    state.set("checked", contract::Json(checked_));
    state.set("current", contract::Json(current_));
    state.set("latest", contract::Json(latest_));
    state.set("updateAvailable", contract::Json(available_ && !dismissed_));
    state.set("dismissed", contract::Json(dismissed_));
    state.set("downloadsUrl", contract::Json(downloads_));
    state.set("error", contract::Json(error_));
    return state;
}

// ------------------------------------------------------------------------- the daemon-link probe

const char* daemon_ownership_token(DaemonOwnership ownership) noexcept
{
    switch (ownership)
    {
    case DaemonOwnership::attached_external:
        return kDaemonOwnershipExternal;
    case DaemonOwnership::spawned_owned:
        return kDaemonOwnershipOwned;
    case DaemonOwnership::none:
        break;
    }
    return kDaemonOwnershipNone;
}

DaemonLinkProbe make_daemon_link_probe(const DaemonLifecycle& lifecycle)
{
    const DaemonLifecycle* target = &lifecycle;
    return [target]() -> DaemonLinkStatus
    {
        DaemonLinkStatus status;
        status.read_only = target->read_only();
        status.reconnect_attempts = target->reconnect_attempts();
        status.ownership = daemon_ownership_token(target->ownership());
        status.last_error = target->last_error();
        return status;
    };
}

// ------------------------------------------------------------------------------- BannerBridge

contract::Json BannerBridge::update_state() const
{
    ++update_states_;
    if (notice_ == nullptr)
    {
        // No update notice wired (a smoke, a headless drill): report the honest "we have not
        // checked" shape rather than refusing, so the renderer's banner logic is exercised unchanged.
        contract::Json state = contract::Json::object();
        state.set("checked", contract::Json(false));
        state.set("current", contract::Json(std::string{}));
        state.set("latest", contract::Json(std::string{}));
        state.set("updateAvailable", contract::Json(false));
        state.set("dismissed", contract::Json(false));
        state.set("downloadsUrl", contract::Json(std::string{}));
        state.set("error", contract::Json(std::string("no update channel is wired in this build")));
        return state;
    }
    return notice_->state_json();
}

contract::Json BannerBridge::daemon_link_state() const
{
    ++link_states_;
    DaemonLinkStatus status;
    if (probe_)
    {
        status = probe_();
    }
    else
    {
        // No lifecycle to report on => not read-only. A host with no daemon-link concept is not a
        // host whose daemon is LOST, and reporting otherwise would paint a permanent banner on
        // every smoke.
        status.read_only = false;
    }
    contract::Json state = contract::Json::object();
    state.set("readOnly", contract::Json(status.read_only));
    state.set("reconnectAttempts", contract::Json(static_cast<std::int64_t>(status.reconnect_attempts)));
    state.set("ownership", contract::Json(status.ownership));
    state.set("lastError", contract::Json(status.last_error));
    return state;
}

bool BannerBridge::install(BridgeRouter& router)
{
    bool ok = router.register_method(kUpdateStateMethod,
                                     [this](const BridgeRequest&) -> BridgeResult
                                     { return BridgeResult::ok(update_state()); });
    ok = router.register_method(kDaemonLinkStateMethod,
                                [this](const BridgeRequest&) -> BridgeResult
                                { return BridgeResult::ok(daemon_link_state()); }) &&
         ok;
    ok = router.register_method(
             kUpdateDismissMethod,
             [this](const BridgeRequest&) -> BridgeResult
             {
                 if (notice_ != nullptr)
                 {
                     (void)notice_->dismiss();
                 }
                 contract::Json result = contract::Json::object();
                 result.set("dismissed", contract::Json(true));
                 return BridgeResult::ok(std::move(result));
             }) &&
         ok;
    ok = router.register_method(
             kUpdateOpenDownloadsMethod,
             [this](const BridgeRequest&) -> BridgeResult
             {
                 if (notice_ == nullptr)
                 {
                     return BridgeResult::error(kErrUpdateNoOpener,
                                                "this build has no downloads page wired");
                 }
                 const bool opened = notice_->open_downloads();
                 if (!opened)
                 {
                     return BridgeResult::error(
                         kErrUpdateNoOpener,
                         "opening the downloads page is not wired on this platform yet");
                 }
                 contract::Json result = contract::Json::object();
                 result.set("opened", contract::Json(true));
                 return BridgeResult::ok(std::move(result));
             }) &&
         ok;
    return ok;
}

} // namespace context::editor::shell
