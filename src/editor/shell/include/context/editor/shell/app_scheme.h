// The `context-editor://` app scheme (M9 e05c, design 04 §1 / 08 §1-§2) — URL -> on-disk asset
// resolution and the response headers that go with it, with NO CEF in any of it.
//
// WHY THIS IS CEF-FREE, like browser.h next door: CEF is a CI-only dependency path (the
// MSVC/Clang-ABI prebuilt cannot link under the local Strawberry-GCC dev gate), so a resolver
// written inside a `CefResourceHandler` would be exercised by nothing that runs locally and by one
// CI job remotely. This is the half that decides WHICH BYTES A RENDERER MAY READ — precisely the
// last thing that should go unverified. The CEF binding (src/editor/shell/cef/) is left as a thin
// translator from CefRequest to `AppAssetResolver::resolve` and back.
//
// ASSETS SHIP IN-APP AND ARE SERVED OVER THIS SCHEME — never `file://` temp files (04 §1). A
// `file://` document would take on a file origin, which defeats the origin + CSP semantics this
// scheme exists to give the trusted editor-core zone, and would leave the bundle readable and
// WRITABLE on disk by anything running as the user.
//
// DENY-BY-DEFAULT, TWICE. A request must (1) name a media type on the allowlist below and (2) land
// inside the asset root after canonicalization. Neither check trusts the other: the textual
// traversal rejection catches `..` before the filesystem is touched, and the canonical containment
// check independently catches anything the textual pass could miss (a symlink out of the root, a
// decoder gap, an OS path quirk). A resolver with only one of the two is one bug away from serving
// the user's home directory to a renderer.

#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace context::editor::shell
{

// ------------------------------------------------------------------------------ scheme vocabulary

// The scheme itself. Registered as STANDARD|SECURE|CORS_ENABLED in EVERY process (08 §2): standard
// so it gets ordinary origin semantics (without it Chromium treats it as opaque and CSP, module
// scripts and `fetch` all behave differently), secure so it counts as a trustworthy origin and is
// not downgraded/blocked as mixed content.
inline constexpr const char* kAppScheme = "context-editor";

// The two hosts under the scheme. They are deliberately DIFFERENT hosts rather than paths: the
// asset host is a plain static-file origin, while the IPC host is the privileged channel's
// identity, and keeping them apart makes "which requests may the resource handler serve" a
// host comparison rather than a prefix match on a path.
inline constexpr const char* kAppHost = "app";
inline constexpr const char* kIpcHost = "ipc";

// `context-editor://app` — editor-core's origin, and the ONLY origin the IPC bridge accepts a
// query from (see ipc_bridge.h).
inline constexpr const char* kAppOrigin = "context-editor://app";
// `context-editor://app/` — the asset URL prefix.
inline constexpr const char* kAppUrlPrefix = "context-editor://app/";
// The privileged bridge endpoint (04 §1). Named here so the Shell, the binding and the TS client
// all spell it once.
inline constexpr const char* kIpcEndpoint = "context-editor://ipc";

// What a bare `context-editor://app/` resolves to.
inline constexpr const char* kDefaultDocument = "index.html";

// The document editor-core boots from.
inline constexpr const char* kAppEntryUrl = "context-editor://app/index.html";

// ------------------------------------------------------------------------------------- resolution

enum class AssetStatus
{
    ok,          // 200 — serve `file`
    bad_request, // 400 — not a context-editor://app/ URL, or an unparseable path
    forbidden,   // 403 — traversal, an escape from the root, or a media type not on the allowlist
    not_found,   // 404 — a well-formed, permitted path with nothing behind it
};

struct AssetResolution
{
    AssetStatus status = AssetStatus::bad_request;
    // Only meaningful when `status == ok`.
    std::filesystem::path file;
    std::string mime_type;
    // Why it was refused — for the diagnostic channel, never for the renderer (a refusal reason is
    // a probe oracle; the handler returns the status alone to the page).
    std::string reason;

    [[nodiscard]] bool ok() const { return status == AssetStatus::ok; }
    // The HTTP status the resource handler reports for this resolution.
    [[nodiscard]] int http_status() const;
};

// The media types editor-core's asset set may contain. ALLOWLIST, not a blocklist: an unknown
// extension is refused rather than guessed at, so dropping a stray file into the asset dir can
// never make it reachable, and a wrong `Content-Type` can never turn a served byte stream into an
// executable document.
[[nodiscard]] const std::vector<std::pair<std::string, std::string>>& asset_media_types();

// The media type for `extension` (with the leading dot, lower-cased by the caller or not), or an
// empty string when it is not on the allowlist.
[[nodiscard]] std::string media_type_for_extension(std::string_view extension);

// Percent-decode ONE pass over a URL path (`%2e` -> `.`). Returns false when the input contains an
// invalid escape, a NUL, or a control character — all of which are rejected rather than sanitized,
// because a sanitizing decoder is exactly how a traversal survives its own check.
//
// Exposed (rather than kept private) because it is the first link in the containment chain and is
// directly adversarially tested.
[[nodiscard]] bool percent_decode(std::string_view input, std::string& out);

// Resolves `context-editor://app/...` URLs against a fixed asset root.
class AppAssetResolver
{
public:
    // `asset_root` is the directory the editor-core asset set was built into. It is canonicalized
    // once here, so every containment check afterwards compares canonical against canonical.
    explicit AppAssetResolver(std::filesystem::path asset_root);

    [[nodiscard]] AssetResolution resolve(std::string_view url) const;

    [[nodiscard]] const std::filesystem::path& asset_root() const { return asset_root_; }
    // False when the root did not exist at construction — every resolve() then reports not_found
    // rather than pretending, and the Shell reports the gap once at boot.
    [[nodiscard]] bool root_exists() const { return root_exists_; }

private:
    std::filesystem::path asset_root_;
    bool root_exists_ = false;
};

// --------------------------------------------------------------------------------- response policy

// The strict no-inline-script CSP the trusted editor-core zone loads under (04 §1, 04 §4, 08 §2).
//
// `default-src 'none'` is the deny-by-default base; every widening below is deliberate:
//   script-src 'self'   — the bundle only. No inline script, no eval: this is the backstop that
//                         makes hostile PROJECT content (entity names, `notes`) rendered through
//                         the hydration runtime unable to execute even if an escaping bug lands
//                         (C-F6 — the escaping contract is the primary control, this is the net).
//   style-src  'self'   — the stylesheet only; no inline <style> and no style="" in markup.
//   connect-src 'none'  — NO NETWORK. This is what makes the 08 §2 "token leakage via the web
//                         layer" row hold end to end: even a fully compromised renderer that got
//                         hold of a secret has nowhere to send it. The bridge is not `fetch`, so
//                         it is unaffected.
//   frame-src / object-src / base-uri / form-action / frame-ancestors 'none'
//                       — no framing in or out, no <base> rewrite of the module URLs, no form
//                         posts. Third-party panels (04 §5) are sandboxed iframes on their OWN
//                         `context-ext://` origins; when they land, THIS is the line that widens
//                         (frame-src context-ext:), and widening it is a reviewed change.
[[nodiscard]] const char* app_csp_header();

// The full response header set for a served asset. Ordered, and returned as a list rather than a
// blob so the CEF binding can hand CEF a map without re-parsing a string it just built.
[[nodiscard]] std::vector<std::pair<std::string, std::string>>
app_response_headers(const std::string& mime_type);

} // namespace context::editor::shell
