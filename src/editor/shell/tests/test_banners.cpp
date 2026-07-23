// T1 for the two notification banners (M9 e14d): the notify-only update check and the daemon-lost
// reconnect surface — see banners.h.
//
// THE PRIVACY SUITE IS THE POINT OF THIS FILE (08 threat row "Update-check privacy": notify-only
// version GET, NO identifiers, no telemetry). The owner signed that commitment personally, so it is
// proven here rather than asserted in prose, and it is proven over the WHOLE outgoing request —
// method, URL, every header, and the body — never over the URL alone. A test that reads only the URL
// would pass with an install id in a header, which is exactly the failure mode this suite exists to
// make impossible.
//
// FOUR LAYERS, deliberately overlapping so that "update the golden" cannot silently retire the
// commitment:
//
//   1. GOLDEN EQUALITY (total). `canonical_request_text(build_release_check_request())` must equal a
//      literal spelled out below. ANY addition — a header, a query parameter, a body byte — fails.
//   2. HOST-LEAKAGE SCAN. No value this machine can supply (user name, computer name, home, temp,
//      host name) may appear anywhere in the request. Survives an intentional golden update.
//   3. IDENTIFIER-SHAPE SCAN. No identifier-shaped token (`install`, `machine`, `uuid`, `telemetry`,
//      `client_id`, …) may appear anywhere in the request. Also survives a golden update.
//   4. INVARIANCE. The request does not change when the RUNNING VERSION changes — the version is
//      compared LOCALLY and never sent (banners.h property (b)).
//
// PLANTED-VIOLATION VERIFICATION. Each layer was falsified by hand during development by planting a
// violation in `build_release_check_request()` (an `X-Install-Id` header, a `?v=<version>` query, and
// a version-bearing User-Agent) and confirming the corresponding CHECK failed. A gate never observed
// failing is not a gate.
//
// AND THE MOCK IS NOT MORE CAPABLE THAN THE REAL PATH. The recording transport below receives the
// request BY VALUE from the same `ReleaseNotice::request()` production uses, and layer 5 asserts the
// bytes it observed are byte-identical (modulo the endpoint override) to the golden — so a request
// that only looks clean when built directly, but grows a header on the way to the transport, fails.
// The remaining half — that the OS client adds nothing BELOW the seam — is not observable from C++
// at all, and is gated by the source scan `tools/check_release_request.py`
// (ctest `editor-shell-release-request`).

#include "context/editor/shell/banners.h"

#include "context/editor/shell/ipc_bridge.h"

#include "shell_test.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace context::editor::shell;
using Json = context::editor::contract::Json;

namespace
{

// The complete request the update check makes, spelled out. This literal IS the privacy commitment
// in machine-checkable form: read it and you know every byte Context Editor puts on the wire.
constexpr const char* kGoldenRequest =
    "GET https://api.github.com/repos/IvanMurzak/Context-Engine/releases/latest\n"
    "Accept: application/vnd.github+json\n"
    "User-Agent: Context-Editor\n"
    "\n";

// Env read, MSVC-safe (the same two-call getenv_s split keybindings_bridge.cpp uses — std::getenv is
// C4996 under /W4 /WX).
[[nodiscard]] std::optional<std::string> read_env(const char* name)
{
#if defined(_MSC_VER)
    std::size_t required = 0;
    if (::getenv_s(&required, nullptr, 0, name) != 0 || required == 0)
    {
        return std::nullopt;
    }
    std::string value(required, '\0');
    if (::getenv_s(&required, value.data(), value.size(), name) != 0)
    {
        return std::nullopt;
    }
    value.resize(std::strlen(value.c_str()));
    if (value.empty())
    {
        return std::nullopt;
    }
    return value;
#else
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0')
    {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

[[nodiscard]] std::string lowered(const std::string& text)
{
    std::string out = text;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

[[nodiscard]] bool contains(const std::string& haystack, const std::string& needle)
{
    return !needle.empty() && haystack.find(needle) != std::string::npos;
}

// The request text WITHOUT the constant endpoint, lowercased — what layers 2 and 3 scan.
//
// WHY THE ENDPOINT IS EXCISED. It is a compile-time constant already pinned byte-for-byte by layer 1,
// so scanning it adds no coverage — and it would add FALSE POSITIVES: the repository owner's name is
// IN the URL, so a developer whose account name is a prefix of it would fail the host-leakage scan on
// a request that leaks nothing. Everything a leak could actually be added to — a query string
// appended past the endpoint, any header, the body — is still scanned.
[[nodiscard]] std::string scannable_request_text()
{
    std::string text = canonical_request_text(build_release_check_request());
    const std::string endpoint(kReleaseCheckEndpoint);
    const std::size_t at = text.find(endpoint);
    if (at != std::string::npos)
    {
        text.erase(at, endpoint.size());
    }
    return lowered(text);
}

// A release document shaped like the one the endpoint returns.
[[nodiscard]] std::string release_body(const std::string& tag)
{
    return "{\"tag_name\": \"" + tag + "\", \"name\": \"Context Editor\", \"draft\": false}";
}

Json dispatch(BridgeRouter& router, const char* method, bool& refused)
{
    Json request = Json::object();
    request.set("jsonrpc", Json("2.0"));
    request.set("id", Json(7));
    request.set("method", Json(method));
    request.set("params", Json::object());
    const BridgeDispatch result = router.dispatch(request.dump());
    const Json response = Json::parse(result.response);
    refused = response.contains("error");
    return refused ? response.at("error") : response.at("result");
}

// ---------------------------------------------------------------- 1. the request is a CONSTANT

void test_request_matches_the_golden()
{
    const HttpRequest request = build_release_check_request();
    const std::string text = canonical_request_text(request);
    CHECK(text == kGoldenRequest);
    if (text != kGoldenRequest)
    {
        std::fprintf(stderr, "  actual request was:\n%s\n", text.c_str());
    }

    // The structural facts the golden encodes, stated separately so a failure names the property
    // rather than only "the bytes moved".
    CHECK(request.method == "GET");
    CHECK(request.body.empty());
    CHECK(request.headers.size() == 2);
    CHECK(request.url.rfind("https://", 0) == 0);
    // No query string at ALL. A version query is the most natural way to leak the running build.
    CHECK(request.url.find('?') == std::string::npos);
    CHECK(request.url.find('&') == std::string::npos);
    CHECK(request.url.find('#') == std::string::npos);

    // The header set is CLOSED and named — an added header changes this count and this listing.
    if (request.headers.size() == 2)
    {
        CHECK(request.headers[0].name == "Accept");
        CHECK(request.headers[1].name == "User-Agent");
        // The user agent is a bare product name: no version, no OS, no build id. GitHub refuses a
        // request with no agent at all, so this is the minimum the endpoint requires — and keeping
        // it digit-free is what stops "just the version" from creeping back in.
        const std::string& agent = request.headers[1].value;
        CHECK(agent == kReleaseCheckUserAgent);
        CHECK(std::none_of(agent.begin(), agent.end(),
                           [](unsigned char c) { return std::isdigit(c) != 0; }));
    }

    // Deterministic: the same bytes every call, on every machine.
    CHECK(canonical_request_text(build_release_check_request()) == text);
}

// ------------------------------------------------------------------- 2. no HOST value is present

void test_request_leaks_no_host_value()
{
    const std::string text = scannable_request_text();

    // Everything this machine could contribute that identifies its user or itself. Whatever this
    // host's values are, none of them may appear in the request — which is a real assertion on any
    // machine, and the CI runners each supply different values.
    for (const char* name : {"USERNAME", "USER", "LOGNAME", "COMPUTERNAME", "HOSTNAME",
                             "USERPROFILE", "HOME", "USERDOMAIN", "TEMP", "TMPDIR"})
    {
        const std::optional<std::string> value = read_env(name);
        if (!value.has_value() || value->size() < 3)
        {
            continue; // too short to be a meaningful needle (a 1-2 char value matches by accident)
        }
        CHECK(!contains(text, lowered(*value)));
    }

    std::error_code ec;
    const std::string temp = std::filesystem::temp_directory_path(ec).string();
    if (!ec && temp.size() >= 3)
    {
        CHECK(!contains(text, lowered(temp)));
    }
}

// -------------------------------------------------------- 3. no identifier-SHAPED token is present

void test_request_carries_no_identifier_shape()
{
    const std::string text = scannable_request_text();

    // Deliberately broad. Anything an analytics/telemetry/identity field is plausibly SPELLED must
    // not appear anywhere in the request — header name, header value, path, or body alike. This is
    // what survives an intentional golden update: a future author who legitimately re-points the
    // endpoint still cannot bring an identifier along without this failing.
    for (const char* token : {"install", "machine", "device", "client-id", "client_id", "clientid",
                              "user-id", "user_id", "userid", "uid=", "guid", "uuid", "session",
                              "telemetry", "analytics", "fingerprint", "serial", "hwid", "anon",
                              "cookie", "authorization", "token", "x-", "locale", "accept-language"})
    {
        CHECK(!contains(text, token));
    }
}

// --------------------------------------------------- 4. the RUNNING VERSION is never on the wire

void test_request_is_invariant_to_the_running_version()
{
    ReleaseNotice a;
    a.set_current_version("0.0.1");
    ReleaseNotice b;
    b.set_current_version("99.44.12-with-a-very-distinctive-suffix");

    const std::string first = canonical_request_text(a.request());
    const std::string second = canonical_request_text(b.request());
    CHECK(first == second);
    CHECK(first == kGoldenRequest);
    CHECK(!contains(lowered(first), "99.44.12"));
}

// ------------------------------------- 5. the TRANSPORT receives exactly that request, unmodified

void test_transport_receives_the_golden_request()
{
    HttpRequest observed;
    std::size_t calls = 0;

    ReleaseNotice notice;
    notice.set_current_version("0.0.1");
    notice.bind_transport(
        [&observed, &calls](const HttpRequest& request) -> HttpResponse
        {
            observed = request;
            ++calls;
            HttpResponse response;
            response.ran = true;
            response.status = 200;
            response.body = release_body("v0.0.2");
            return response;
        });
    notice.check_blocking();

    CHECK(calls == 1);
    // The bytes the transport ACTUALLY saw — not a request rebuilt for the assertion.
    CHECK(canonical_request_text(observed) == kGoldenRequest);
    CHECK(observed.body.empty());
    CHECK(observed.headers.size() == 2);
}

// ------------------------------------------------------------------------ version comparison

void test_version_comparison()
{
    CHECK(compare_versions("0.0.1", "0.0.2") == -1);
    CHECK(compare_versions("0.0.2", "0.0.1") == 1);
    CHECK(compare_versions("0.0.1", "0.0.1") == 0);
    CHECK(compare_versions("1.2", "1.2.0") == 0);       // a missing component reads as 0
    CHECK(compare_versions("1.2", "1.2.1") == -1);
    CHECK(compare_versions("0.9.9", "0.10.0") == -1);   // numeric, not lexicographic
    CHECK(compare_versions("0.0.1", "v0.0.2") == -1);   // a leading `v` is tolerated
    CHECK(compare_versions("1.0.0", "1.0.0-rc1") == 0); // a prerelease suffix is ignored, not older

    // Unparseable on EITHER side yields "no opinion" — a garbled release tag must never be able to
    // manufacture an update prompt.
    CHECK(compare_versions("0.0.1", "not-a-version") == 0);
    CHECK(compare_versions("", "1.0.0") == 0);
    CHECK(compare_versions("nightly", "nightly") == 0);

    CHECK(parse_version("v1.2.3").size() == 3);
    CHECK(parse_version("1.2.3").size() == 3);
    CHECK(parse_version("1.2.x").size() == 2); // stops at the first non-numeric segment
    CHECK(parse_version("1.").size() == 1);
    CHECK(parse_version("").empty());
    CHECK(parse_version("v").empty());
    // Saturating rather than throwing: a hostile tag must degrade, never abort the owner thread.
    CHECK(!parse_version("999999999999999999999.0.0").empty());
}

void test_release_tag_parsing()
{
    CHECK(parse_latest_release_tag(release_body("v1.2.3")) == "v1.2.3");
    CHECK(parse_latest_release_tag("").empty());
    CHECK(parse_latest_release_tag("{").empty());                        // malformed
    CHECK(parse_latest_release_tag("[1,2,3]").empty());                  // not an object
    CHECK(parse_latest_release_tag("{\"name\":\"x\"}").empty());         // no tag_name
    CHECK(parse_latest_release_tag("{\"tag_name\": 3}").empty());        // tag_name not a string
    CHECK(parse_latest_release_tag(std::string(kMaxReleaseBodyBytes + 1, 'a')).empty()); // oversized
}

// ----------------------------------------------------------------- the update-notice state machine

[[nodiscard]] HttpTransport answering(int status, std::string body)
{
    return [status, body](const HttpRequest&) -> HttpResponse
    {
        HttpResponse response;
        response.ran = true;
        response.status = status;
        response.body = body;
        return response;
    };
}

void test_update_states()
{
    { // a NEWER release => the banner is offered
        ReleaseNotice notice;
        notice.set_current_version("0.0.1");
        notice.bind_transport(answering(200, release_body("v0.1.0")));
        notice.check_blocking();
        CHECK(notice.checked());
        CHECK(notice.update_available());
        CHECK(notice.latest() == "v0.1.0");
        CHECK(notice.error().empty());
        CHECK(notice.checks_completed() == 1);

        const Json state = notice.state_json();
        CHECK(state.at("checked").as_bool());
        CHECK(state.at("updateAvailable").as_bool());
        CHECK(state.at("current").as_string() == "0.0.1");
        CHECK(state.at("latest").as_string() == "v0.1.0");
        CHECK(state.at("downloadsUrl").as_string() == kDownloadsPageUrl);
        // The click-through target is the human downloads page, over https, and is NOT the API
        // endpoint the check used.
        CHECK(state.at("downloadsUrl").as_string().rfind("https://", 0) == 0);
        CHECK(state.at("downloadsUrl").as_string() != std::string(kReleaseCheckEndpoint));

        // A second poll with nothing pending changes nothing (the owner loop calls it every frame).
        CHECK(!notice.poll());
    }
    { // the SAME release => no banner
        ReleaseNotice notice;
        notice.set_current_version("1.4.0");
        notice.bind_transport(answering(200, release_body("v1.4.0")));
        notice.check_blocking();
        CHECK(notice.checked());
        CHECK(!notice.update_available());
    }
    { // an OLDER published release (a dev build ahead of the feed) => no banner
        ReleaseNotice notice;
        notice.set_current_version("2.0.0");
        notice.bind_transport(answering(200, release_body("v1.9.9")));
        notice.check_blocking();
        CHECK(notice.checked());
        CHECK(!notice.update_available());
    }
    { // an HTTP error => honest "not checked" with a legible reason, never a silent "you are current"
        ReleaseNotice notice;
        notice.set_current_version("0.0.1");
        notice.bind_transport(answering(503, ""));
        notice.check_blocking();
        CHECK(!notice.checked());
        CHECK(!notice.update_available());
        CHECK(!notice.error().empty());
        CHECK(shelltest::mentions(notice.error(), "503"));
    }
    { // the transport did not run at all (no wiring on this OS) => honest failure
        ReleaseNotice notice;
        notice.bind_transport(
            [](const HttpRequest&) -> HttpResponse
            {
                HttpResponse response;
                response.ran = false;
                response.error = "not wired on this platform yet";
                return response;
            });
        notice.check_blocking();
        CHECK(!notice.checked());
        CHECK(shelltest::mentions(notice.error(), "not wired"));
    }
    { // a transport that THROWS is contained, not propagated onto the owner thread
        ReleaseNotice notice;
        notice.bind_transport([](const HttpRequest&) -> HttpResponse
                              { throw std::runtime_error("socket exploded"); });
        notice.check_blocking();
        CHECK(!notice.checked());
        CHECK(shelltest::mentions(notice.error(), "socket exploded"));
    }
    { // a 200 with an unreadable body => "we learned nothing", not "you are current"
        ReleaseNotice notice;
        notice.set_current_version("0.0.1");
        notice.bind_transport(answering(200, "<html>rate limited</html>"));
        notice.check_blocking();
        CHECK(!notice.checked());
        CHECK(!notice.update_available());
        CHECK(!notice.error().empty());
    }
    { // NO transport bound at all => the check never runs and nothing is claimed
        ReleaseNotice notice;
        notice.check_blocking();
        CHECK(!notice.checked());
        CHECK(notice.checks_completed() == 0);
        CHECK(!notice.state_json().at("checked").as_bool());
    }
}

void test_dismiss_is_session_scoped()
{
    ReleaseNotice notice;
    notice.set_current_version("0.0.1");
    notice.bind_transport(answering(200, release_body("v9.0.0")));
    notice.check_blocking();
    CHECK(notice.update_available());

    CHECK(notice.dismiss());  // the first dismissal changes the state
    CHECK(!notice.dismiss()); // a second is idempotent
    CHECK(notice.dismissed());
    CHECK(!notice.update_available());

    const Json state = notice.state_json();
    CHECK(state.at("dismissed").as_bool());
    CHECK(!state.at("updateAvailable").as_bool());
    // The FACT is retained even while the banner is hidden — Settings still shows what is available.
    CHECK(state.at("latest").as_string() == "v9.0.0");
    CHECK(state.at("checked").as_bool());
}

void test_open_downloads()
{
    { // no opener wired => an honest false, never a pretend success
        ReleaseNotice notice;
        CHECK(!notice.open_downloads());
        CHECK(notice.downloads_opened() == 0);
    }
    {
        std::string opened;
        ReleaseNotice notice;
        notice.bind_url_opener(
            [&opened](const std::string& url)
            {
                opened = url;
                return true;
            });
        CHECK(notice.open_downloads());
        CHECK(opened == kDownloadsPageUrl);
        CHECK(opened.rfind("https://", 0) == 0);
        CHECK(notice.downloads_opened() == 1);
    }
    { // an opener that fails is reported, not swallowed
        ReleaseNotice notice;
        notice.bind_url_opener([](const std::string&) { return false; });
        CHECK(!notice.open_downloads());
        CHECK(notice.downloads_opened() == 0);
    }
}

// ------------------------------------------------------------------------ the daemon-lost banner

void test_daemon_link_probe_over_a_real_lifecycle()
{
    // A lifecycle that never attached: read-only, unowned, no attempts yet. This is exactly what the
    // banner must render on a bare launch, and it is provable with no daemon at all.
    DaemonLifecycle lifecycle;
    const DaemonLinkProbe probe = make_daemon_link_probe(lifecycle);
    const DaemonLinkStatus status = probe();
    CHECK(status.read_only);
    CHECK(status.ownership == kDaemonOwnershipNone);
    CHECK(status.reconnect_attempts == 0);

    CHECK(std::string(daemon_ownership_token(DaemonOwnership::none)) == kDaemonOwnershipNone);
    CHECK(std::string(daemon_ownership_token(DaemonOwnership::attached_external)) ==
          kDaemonOwnershipExternal);
    CHECK(std::string(daemon_ownership_token(DaemonOwnership::spawned_owned)) ==
          kDaemonOwnershipOwned);
}

void test_daemon_link_state_json()
{
    { // a LOST daemon mid-reconnect — what the banner is for
        BannerBridge banners;
        banners.bind_link_probe(
            []
            {
                DaemonLinkStatus status;
                status.read_only = true;
                status.reconnect_attempts = 3;
                status.ownership = kDaemonOwnershipOwned;
                status.last_error = "the daemon wire closed";
                return status;
            });
        const Json state = banners.daemon_link_state();
        CHECK(state.at("readOnly").as_bool());
        CHECK(state.at("reconnectAttempts").as_int() == 3);
        CHECK(state.at("ownership").as_string() == kDaemonOwnershipOwned);
        CHECK(state.at("lastError").as_string() == "the daemon wire closed");
        CHECK(banners.link_states_served() == 1);
    }
    { // a live link — no banner
        BannerBridge banners;
        banners.bind_link_probe(
            []
            {
                DaemonLinkStatus status;
                status.read_only = false;
                status.ownership = kDaemonOwnershipExternal;
                return status;
            });
        const Json state = banners.daemon_link_state();
        CHECK(!state.at("readOnly").as_bool());
        CHECK(state.at("reconnectAttempts").as_int() == 0);
    }
    { // NO probe bound (a smoke / headless drill): not read-only. A host with no daemon-link concept
      // is not a host whose daemon is lost, and reporting otherwise paints a permanent banner.
        BannerBridge banners;
        CHECK(!banners.daemon_link_state().at("readOnly").as_bool());
    }
}

// ------------------------------------------------------------------------------ the bridge wiring

void test_bridge_surface()
{
    ReleaseNotice notice;
    notice.set_current_version("0.0.1");
    notice.bind_transport(answering(200, release_body("v0.2.0")));
    std::string opened;
    notice.bind_url_opener(
        [&opened](const std::string& url)
        {
            opened = url;
            return true;
        });
    notice.check_blocking();

    BannerBridge banners;
    banners.bind_release_notice(&notice);
    banners.bind_link_probe(
        []
        {
            DaemonLinkStatus status;
            status.read_only = true;
            status.reconnect_attempts = 1;
            status.ownership = kDaemonOwnershipOwned;
            return status;
        });

    BridgeRouter router;
    CHECK(banners.install(router));
    // A second install must be refused wholesale — a silent double-bind would leave two handlers
    // fighting over one method.
    BannerBridge duplicate;
    CHECK(!duplicate.install(router));

    bool refused = false;
    const Json state = dispatch(router, kUpdateStateMethod, refused);
    CHECK(!refused);
    CHECK(state.at("updateAvailable").as_bool());
    CHECK(state.at("latest").as_string() == "v0.2.0");

    const Json link = dispatch(router, kDaemonLinkStateMethod, refused);
    CHECK(!refused);
    CHECK(link.at("readOnly").as_bool());
    CHECK(link.at("reconnectAttempts").as_int() == 1);

    const Json download = dispatch(router, kUpdateOpenDownloadsMethod, refused);
    CHECK(!refused);
    CHECK(download.at("opened").as_bool());
    CHECK(opened == kDownloadsPageUrl);

    const Json dismissed = dispatch(router, kUpdateDismissMethod, refused);
    CHECK(!refused);
    CHECK(dismissed.at("dismissed").as_bool());
    // The dismissal reached the notice, not just the envelope.
    const Json after = dispatch(router, kUpdateStateMethod, refused);
    CHECK(!refused);
    CHECK(!after.at("updateAvailable").as_bool());
    CHECK(after.at("dismissed").as_bool());

    // NOTHING was refused: the smokes assert `bridge.refused() == 0`, so every method editor-core
    // calls at boot must be routable here.
    CHECK(router.refused() == 0);
    CHECK(banners.update_states_served() >= 2);
}

void test_bridge_without_a_notice_is_honest()
{
    // The shape a live smoke installs: the surface exists (so nothing is refused at the envelope
    // level) but no update channel is wired.
    BannerBridge banners;
    BridgeRouter router;
    CHECK(banners.install(router));

    bool refused = false;
    const Json state = dispatch(router, kUpdateStateMethod, refused);
    CHECK(!refused);
    CHECK(!state.at("checked").as_bool());
    CHECK(!state.at("updateAvailable").as_bool());
    CHECK(!state.at("error").as_string().empty());

    // Asking to open downloads with no channel is a legible REFUSAL, not a pretend success.
    const Json error = dispatch(router, kUpdateOpenDownloadsMethod, refused);
    CHECK(refused);
    CHECK(shelltest::mentions(error.dump(), kErrUpdateNoOpener));
    // A handler's own error is NOT an envelope refusal — the smokes' invariant stays intact.
    CHECK(router.refused() == 0);
}

// The production transport is the one piece no unit test can drive (there is no network in CI, and
// the OS client is the thing under test). What IS assertable is that it refuses everything it must:
// a non-HTTPS URL, and an unencodable one — fail-closed, with `ran:false` and a reason.
void test_native_transport_fails_closed()
{
    HttpRequest plaintext = build_release_check_request();
    plaintext.url = "http://example.invalid/latest";
    const HttpResponse refused = native_https_get(plaintext);
    CHECK(!refused.ran);
    CHECK(!refused.error.empty());

    HttpRequest empty = build_release_check_request();
    empty.url.clear();
    const HttpResponse unencodable = native_https_get(empty);
    CHECK(!unencodable.ran);
    CHECK(!unencodable.error.empty());

    // The URL opener refuses anything that is not an https page — a compromised release feed must
    // not be able to name a scheme the OS would happily launch.
    CHECK(!native_open_url("file:///c:/windows/system32/calc.exe"));
    CHECK(!native_open_url("javascript:alert(1)"));
    CHECK(!native_open_url(""));
}

} // namespace

int main()
{
    test_request_matches_the_golden();
    test_request_leaks_no_host_value();
    test_request_carries_no_identifier_shape();
    test_request_is_invariant_to_the_running_version();
    test_transport_receives_the_golden_request();
    test_version_comparison();
    test_release_tag_parsing();
    test_update_states();
    test_dismiss_is_session_scoped();
    test_open_downloads();
    test_daemon_link_probe_over_a_real_lifecycle();
    test_daemon_link_state_json();
    test_bridge_surface();
    test_bridge_without_a_notice_is_honest();
    test_native_transport_fails_closed();
    SHELL_TEST_MAIN_END();
}
