// The two NOTIFICATION BANNERS (M9 e14d, design 07 §4 / 08 threat row "Update-check privacy"):
//
//   1. THE UPDATE BANNER — notify-only (owner-confirmed O3, 2026-07-22). One HTTPS GET against the
//      project's latest published release, a local version compare, and — when a newer release
//      exists — a banner whose action opens the DOWNLOADS PAGE in the user's browser. There is no
//      in-app updater, no download, no install; that is post-M9.
//   2. THE DAEMON-LOST BANNER — the read-only reconnect notice over e14a's reconnect STATE
//      (daemon_lifecycle.h: `read_only()` + the bounded backoff ladder). This module OWNS no
//      lifecycle; it only PROBES it, so the banner cannot perturb the thing it reports on.
//
// ⚠ THE PRIVACY COMMITMENT IS THE FEATURE, and it is asserted rather than stated (08 threat row: "no
// identifiers ... no telemetry anywhere"). Three properties make it checkable instead of aspirational:
//
//   (a) THE REQUEST IS A CONSTANT. `build_release_check_request()` takes NO arguments and reads NO
//       host state — not the running version, not the OS, not a machine/install/user id, not a
//       locale. Every Context Editor on every machine emits the BYTE-IDENTICAL request, which is the
//       strongest privacy claim available: the server cannot distinguish two users because the two
//       requests are the same bytes. `canonical_request_text()` renders the WHOLE request (method,
//       url, every header, body) so a test can assert byte-equality against a golden — an assertion
//       that fails on ANY added header, query parameter, or body, not merely on a changed URL.
//   (b) THE COMPARISON IS LOCAL. The server is asked "what is the latest release", never told what
//       we are running. `tag_name` comes back, `compare_versions` runs HERE.
//   (c) THE TRANSPORT ADDS NOTHING. `native_https_get` sends exactly the headers in the request and
//       no others — no default user agent, no cookies, no auth, no automatic credentials. That is
//       enforced by the source gate `tools/check_release_request.py` (ctest
//       `editor-shell-release-request`), which also refuses a SECOND request builder and any
//       network call from editor-core — because the renderer's `fetch()` would carry Chromium's own
//       `User-Agent` / `Accept-Language` / client hints, i.e. exactly the identifiers this design
//       forbids, and no assertion written in TypeScript could see them.
//
// WHY THIS IS NOT A NEW DEPENDENCY. The transport is the OPERATING SYSTEM's own HTTPS client
// (WinHTTP on Windows, from the platform SDK). No third-party library, no prebuilt, nothing added to
// `src/vcpkg.json` or the license allowlist — the standing owner-approval gate for a new dependency
// (08 §3) is therefore not reached. macOS/Linux get an HONEST "not wired on this OS yet" until e12
// brings their shells up, exactly as `native_pick_folder()` (e14c) and the window backend do; the
// banner then reports `checked:false` with a legible reason instead of pretending.
//
// D10 BOUNDARY-CLEAN. Plain OS-SDK calls plus `contract::Json`; nothing from the authored-project
// machinery. `context_assert_shell_boundary`'s FORBIDDEN list is untouched and stays non-vacuous.
//
// CEF-FREE, like every sibling bridge (ipc_bridge.h / themes_bridge.h / welcome.h): the router
// handlers run nowhere the local dev gate can reach, so the logic lives HERE where
// tests/test_banners.cpp drives the SAME code the renderer reaches, on all three `build` legs.

#pragma once

#include "context/editor/contract/json.h"
#include "context/editor/shell/daemon_lifecycle.h"
#include "context/editor/shell/ipc_bridge.h"

#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace context::editor::shell
{

// --------------------------------------------------------------------------- the wire vocabulary
//
// Grep-stable, and MIRRORED by the TS side (src/editor/webui/core/src/banners.ts). The
// `webui-welcome-contract` gate re-reads these values out of the BUILT bundle and byte-compares them
// to the C++ constants here — the same cross-language discipline the panel / welcome / editor-state
// surfaces ride, so a rename on either side reds a ctest instead of silently unbinding a banner.

inline constexpr const char* kUpdateStateMethod = "update.state";
inline constexpr const char* kUpdateDismissMethod = "update.dismiss";
inline constexpr const char* kUpdateOpenDownloadsMethod = "update.openDownloads";
inline constexpr const char* kDaemonLinkStateMethod = "daemon.linkState";

// The ownership tokens `daemon.linkState` reports. Mirrored by the TS `DAEMON_OWNERSHIP_*` constants.
inline constexpr const char* kDaemonOwnershipNone = "none";
inline constexpr const char* kDaemonOwnershipExternal = "external";
inline constexpr const char* kDaemonOwnershipOwned = "owned";

// Local refusal codes (NOT R-CLI-008 catalog codes — same rationale as welcome.h / panel_host.h:
// these classify a HOST-side caller/wiring error, not a daemon-contract failure).
inline constexpr const char* kErrUpdateNoOpener = "update.no_opener";

// ------------------------------------------------------------------- the release-check endpoint
//
// The published release feed and the human downloads page, as plain constants so BOTH are visible at
// one glance and neither can be assembled from host state at call time.
//
// `kReleaseCheckUserAgent` is deliberately a bare PRODUCT name with NO version, OS, or build id.
// GitHub refuses an API request that carries no user agent at all, so this is the minimum the
// endpoint requires; keeping it constant is what preserves property (a) above. Adding the running
// version here would leak exactly the fact (b) exists to avoid sending.
inline constexpr const char* kReleaseCheckEndpoint =
    "https://api.github.com/repos/IvanMurzak/Context-Engine/releases/latest";
inline constexpr const char* kDownloadsPageUrl =
    "https://github.com/IvanMurzak/Context-Engine/releases/latest";
inline constexpr const char* kReleaseCheckUserAgent = "Context-Editor";
inline constexpr const char* kReleaseCheckAccept = "application/vnd.github+json";

// The largest release document the Shell will read. A release payload is a few KB; anything past
// this is treated as unreadable rather than pulled into memory (the same fail-closed cap the
// keybindings / themes / config surfaces apply).
inline constexpr std::size_t kMaxReleaseBodyBytes = 512u * 1024u;

// Total time budget for the whole release check, in milliseconds. A notify-only banner is never
// worth a long stall, and the bound is what makes the worker thread's join at shutdown finite.
inline constexpr int kReleaseCheckTimeoutMs = 5000;

// ------------------------------------------------------------------------------- the HTTP seam

struct HttpHeader
{
    std::string name;
    std::string value;
};

// One outgoing request, COMPLETE: there is no other channel through which a byte can reach the wire.
// That completeness is what makes `canonical_request_text()` a sound privacy assertion — a header
// added anywhere below the seam would not appear here, which is precisely what the source gate
// `tools/check_release_request.py` exists to refuse.
struct HttpRequest
{
    std::string method = "GET";
    std::string url;
    std::vector<HttpHeader> headers;
    std::string body; // ALWAYS empty for the release check — a notify-only GET sends nothing
};

struct HttpResponse
{
    bool ran = false; // the request was issued and a response came back
    int status = 0;
    std::string body;
    std::string error; // why `ran` is false (no transport on this OS, DNS, TLS, timeout, …)
};

// The injectable transport. Production binds `native_https_get`; tests bind a recording fake that
// captures the request VALUE — the same value the real transport receives, which is what keeps the
// mock from being more capable (or more forgiving) than reality.
using HttpTransport = std::function<HttpResponse(const HttpRequest&)>;

// The injectable "open this URL in the user's browser" seam. Production binds `native_open_url`;
// tests bind a recorder.
using UrlOpener = std::function<bool(const std::string& url)>;

// THE request the update check makes. Pure, argument-free, host-state-free — see property (a).
[[nodiscard]] HttpRequest build_release_check_request();

// A deterministic, complete rendering of `request`: the request line, every header in order, a blank
// line, then the body. This is the artifact the privacy assertions compare and scan; it exists so a
// test can never be accidentally narrower than the request it is meant to police.
[[nodiscard]] std::string canonical_request_text(const HttpRequest& request);

// The production transport: the OS HTTPS client. Sends EXACTLY `request.headers` and nothing else.
// On a platform with no wiring yet (macOS/Linux until e12) returns `ran:false` with an honest error
// rather than a silent success. NEVER throws.
[[nodiscard]] HttpResponse native_https_get(const HttpRequest& request);

// The production URL opener (Win32 `ShellExecuteW`; an honest false elsewhere). NEVER throws.
[[nodiscard]] bool native_open_url(const std::string& url);

// The version THIS build reports as running — the compiled-in `CONTEXT_EDITOR_VERSION`, which the
// build system fills from the project's single `PROJECT_VERSION`.
//
// A FUNCTION rather than a header constant on purpose: the define is PRIVATE to this library, so a
// `#if defined(...)` default member initializer in this header would expand differently in the
// library's own TUs and in a consumer's — a quiet ODR violation across the very class the tests
// construct. One definition, in banners.cpp, cannot disagree with itself.
[[nodiscard]] const char* editor_version() noexcept;

// ------------------------------------------------------------------------ pure: version compare

// Split a version into its numeric components. Tolerates a leading `v`, and stops at the first
// non-numeric segment so `1.2.3-rc1` reads as {1,2,3}. Returns an EMPTY vector for anything with no
// leading number at all — which `compare_versions` treats as "unknown", never as "older".
[[nodiscard]] std::vector<int> parse_version(const std::string& text);

// -1 when `a` < `b`, 0 when equal (or either is unparseable), 1 when `a` > `b`. Component-wise, with
// a missing component reading as 0 (`1.2` == `1.2.0`). TOTAL: never throws, never reads host state.
// An unparseable side yields 0 so a garbled release tag can never manufacture an update prompt.
[[nodiscard]] int compare_versions(const std::string& a, const std::string& b);

// Read `tag_name` out of a release document. Empty when the body is oversized, unparseable, not an
// object, or carries no string `tag_name` — every one of which means "we learned nothing".
[[nodiscard]] std::string parse_latest_release_tag(const std::string& body);

// ---------------------------------------------------------------- the update-notice state machine

// The notify-only update check + the banner state it produces.
//
// THREADING. The check runs on ONE short-lived worker thread so a boot-time network round trip never
// stalls the Shell's single-threaded owner loop; `poll()` adopts the result from the owner thread.
// Everything else is owner-thread-only. The worker holds a COPY of the transport and the request and
// touches nothing else, so there is one mutex and one shared field.
class ReleaseNotice
{
public:
    ReleaseNotice() = default;
    ~ReleaseNotice();

    // Non-copyable / non-movable: it owns a thread and is captured by `this` in the bridge handlers.
    ReleaseNotice(const ReleaseNotice&) = delete;
    ReleaseNotice& operator=(const ReleaseNotice&) = delete;
    ReleaseNotice(ReleaseNotice&&) = delete;
    ReleaseNotice& operator=(ReleaseNotice&&) = delete;

    // --- configuration (before begin_check) ---
    // The version this build reports as "running". Defaults to the compiled-in `CONTEXT_EDITOR_VERSION`.
    void set_current_version(std::string version) { current_ = std::move(version); }
    // Where the check points. Defaults to `kReleaseCheckEndpoint`; tests point it at a mock endpoint.
    // ⚠ This overrides ONLY the URL — every other property of the request is fixed by
    // `build_release_check_request()`, so a test cannot accidentally exercise a laxer request shape
    // than production emits.
    void set_endpoint(std::string endpoint) { endpoint_ = std::move(endpoint); }
    void set_downloads_url(std::string url) { downloads_ = std::move(url); }
    void bind_transport(HttpTransport transport) { transport_ = std::move(transport); }
    void bind_url_opener(UrlOpener opener) { opener_ = std::move(opener); }

    // The exact request this instance will issue — `build_release_check_request()` with the endpoint
    // override applied. What the privacy tests assert on, and what the transport receives verbatim.
    [[nodiscard]] HttpRequest request() const;

    // --- the check ---
    // Start the check on a worker thread. A no-op when no transport is bound (no network wiring on
    // this OS / a smoke that deliberately binds none) or a check is already in flight.
    void begin_check();
    // Join a check in flight, if any. Bounded by the transport's own timeout.
    void wait_for_check();
    // Adopt a finished check's result. Returns true when the state changed this call. Owner-loop safe
    // and cheap: an integer compare when nothing is pending.
    [[nodiscard]] bool poll();
    // begin + wait + poll, for tests and any caller that wants the answer now.
    void check_blocking();

    // --- the banner actions ---
    // Hide the banner for THIS SESSION. Deliberately not persisted: the per-user config document has
    // a closed settable vocabulary and ONE writer (user_config.h / C-F14), and a dismissal that
    // outlives the session would have to answer "dismissed until which version?" — a product question
    // the notify-only design (07 §4) does not pose. A restart re-offers the notice, which is the
    // honest behaviour for a build that is genuinely out of date.
    bool dismiss();
    // Open the downloads page in the user's browser. False when no opener is wired.
    [[nodiscard]] bool open_downloads();

    // `{ checked, current, latest, updateAvailable, dismissed, downloadsUrl, error }`.
    [[nodiscard]] contract::Json state_json() const;

    // --- observation (tests / smoke) ---
    [[nodiscard]] bool checked() const noexcept { return checked_; }
    [[nodiscard]] bool update_available() const noexcept { return available_ && !dismissed_; }
    [[nodiscard]] bool dismissed() const noexcept { return dismissed_; }
    [[nodiscard]] const std::string& latest() const noexcept { return latest_; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] std::size_t checks_completed() const noexcept { return checks_; }
    [[nodiscard]] std::size_t downloads_opened() const noexcept { return opens_; }

private:
    void adopt(const HttpResponse& response);

    std::string current_ = editor_version();
    std::string endpoint_ = kReleaseCheckEndpoint;
    std::string downloads_ = kDownloadsPageUrl;
    HttpTransport transport_;
    UrlOpener opener_;

    // Owner-thread state.
    bool checked_ = false;
    bool available_ = false;
    bool dismissed_ = false;
    std::string latest_;
    std::string error_;
    std::size_t checks_ = 0;
    std::size_t opens_ = 0;

    // The worker handoff: `pending_` is written once by the worker under `mutex_`, then adopted once
    // by `poll()` on the owner thread.
    std::thread worker_;
    mutable std::mutex mutex_;
    HttpResponse pending_;
    bool pending_ready_ = false;
};

// ------------------------------------------------------------------- the daemon-link banner probe

// What the daemon-lost banner renders. A SNAPSHOT, read from the lifecycle each time it is asked.
struct DaemonLinkStatus
{
    bool read_only = true;   // not live read-write: no daemon yet, or a lost one mid-reconnect
    int reconnect_attempts = 0;
    std::string ownership = kDaemonOwnershipNone;
    std::string last_error;
};

// The injectable probe. Production binds `make_daemon_link_probe(lifecycle)`; tests bind a fake so
// the banner surface is provable with no daemon at all.
using DaemonLinkProbe = std::function<DaemonLinkStatus()>;

// The one place `DaemonOwnership` is mapped to its wire token — kept here so the vocabulary has a
// single definition rather than an agreement between the composition root and the smokes.
[[nodiscard]] const char* daemon_ownership_token(DaemonOwnership ownership) noexcept;

// A probe reading `lifecycle`. The reference must outlive every call (the composition root owns both).
[[nodiscard]] DaemonLinkProbe make_daemon_link_probe(const DaemonLifecycle& lifecycle);

// ------------------------------------------------------------------------------- the bridge

// The `update.*` + `daemon.linkState` bridge surface. Mirrors the sibling bridges: bind collaborators
// after construction, then `install` on the router.
//
// ⚠ INSTALL IT IN EVERY LIVE SMOKE. editor-core asks for both banner states during boot, and the
// router denies unknown methods by DEFAULT — an uninstalled surface therefore trips the smokes'
// strict `bridge.refused() == 0` invariant even though the renderer degrades gracefully. That is the
// exact regression e06d shipped with its config surface; the cost of avoiding it is three lines per
// smoke.
class BannerBridge
{
public:
    BannerBridge() = default;

    BannerBridge(const BannerBridge&) = delete;
    BannerBridge& operator=(const BannerBridge&) = delete;
    BannerBridge(BannerBridge&&) = delete;
    BannerBridge& operator=(BannerBridge&&) = delete;

    // The update notice this surface reports. Must outlive the router.
    void bind_release_notice(ReleaseNotice* notice) noexcept { notice_ = notice; }
    // How the daemon link is read. Unbound => a permanently "attached" report, which is the correct
    // answer for a host that has no daemon link concept at all (a smoke, a headless drill).
    void bind_link_probe(DaemonLinkProbe probe) { probe_ = std::move(probe); }

    // --- the method bodies, exposed for direct testing ---
    [[nodiscard]] contract::Json update_state() const;
    [[nodiscard]] contract::Json daemon_link_state() const;

    // Bind every banner method on `router`. False when ANY binding was refused (a name collision) —
    // a wiring bug the caller must not ignore.
    [[nodiscard]] bool install(BridgeRouter& router);

    // --- observation ---
    [[nodiscard]] std::size_t update_states_served() const noexcept { return update_states_; }
    [[nodiscard]] std::size_t link_states_served() const noexcept { return link_states_; }

private:
    ReleaseNotice* notice_ = nullptr;
    DaemonLinkProbe probe_;
    mutable std::size_t update_states_ = 0;
    mutable std::size_t link_states_ = 0;
};

} // namespace context::editor::shell
