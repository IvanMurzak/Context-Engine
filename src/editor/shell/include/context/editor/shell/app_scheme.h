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
//
// THIS HEADER IS ALSO THE SHELL'S SHARED SCHEME/ASSET-SERVING PRIMITIVE SET (M9 e13a-1). The
// sibling `context-ext://` extension scheme (ext_scheme.h) resolves URLs against a DIFFERENT root
// per package, but the containment chain it walks is the SAME one — `percent_decode`,
// `split_safe_path_segments`, `media_type_for_extension`, `http_status_for`. They are declared here
// rather than duplicated there ON PURPOSE: two copies of a traversal rejection drift, and the copy
// that drifts is the one that ships the hole. One implementation, adversarially tested by BOTH
// suites, is the whole point.

#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace context::editor::shell
{

// ------------------------------------------------------------------------------ scheme vocabulary

// The scheme itself. Registered as STANDARD|SECURE|CORS_ENABLED|FETCH_ENABLED in EVERY process
// (08 §2): standard so it gets ordinary origin semantics (without it Chromium treats it as opaque
// and CSP, module scripts and `fetch` all behave differently), secure so it counts as a trustworthy
// origin and is not downgraded/blocked as mixed content, CORS/FETCH enabled so the module graph and
// same-origin `fetch` resolve normally. FETCH_ENABLED widens nothing in practice: the served CSP
// carries `connect-src 'none'`, so there is no network for a fetch to reach — it only lets
// same-origin asset requests take the ordinary path instead of a special-cased one.
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

// The boot-URL query flag that PINS editor-core's first-run theme (`theme.ts`'s THEME_PIN_FLAG).
//
// CROSS-LANGUAGE VOCABULARY, which is why it is declared HERE beside the rest of the scheme's:
// the C++ smokes WRITE it into the boot URL and editor-core's `bootThemeId` READS it, so a rename on
// either side would silently restore the ambient-`prefers-color-scheme` behaviour it exists to
// remove — a live per-pixel assertion would then go back to depending on whether the HOST prefers
// dark, with no build error anywhere. `webui-scheme-contract` compares this string against the one
// compiled into the built bundle.
inline constexpr const char* kThemePinFlag = "ctx-smoke-theme";

// ------------------------------------------------------------------------------------- resolution

enum class AssetStatus
{
    ok,          // 200 — serve `file`
    bad_request, // 400 — not a context-editor://app/ URL, or an unparseable path
    forbidden,   // 403 — traversal, an escape from the root, or a media type not on the allowlist
    not_found,   // 404 — a well-formed, permitted path with nothing behind it
};

// The HTTP status a resource handler reports for `status`. Free function so the ONE mapping serves
// both this scheme's `AssetResolution` and the extension scheme's `ExtResolution` (ext_scheme.h) —
// a second copy could disagree, and a resolution that refuses with a 200 refuses nothing.
[[nodiscard]] int http_status_for(AssetStatus status);

// ASCII case-insensitive equality. Declared HERE rather than open-coded in the CEF binding for the
// reason the whole layering exists: the binding is a translator the local dev gate cannot build and
// no unit suite exercises, so a comparison loop written there is verified by nothing. HTTP header
// names are case-insensitive, which is what needs it (see `refusal_headers` below and the CEF
// binding's header de-duplication).
[[nodiscard]] bool ascii_iequals(std::string_view a, std::string_view b);

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

// Split an ALREADY-DECODED relative URL path into segments on '/', refusing every shape that could
// escape a root: a `.` or `..` segment, a backslash (a separator on Windows, so `..\..\x` is a
// traversal a '/'-only split would hand through as ONE innocent segment), a control character, and
// a COLON anywhere in a segment (both `C:` / `C:x`, which re-roots onto that drive on Windows
// instead of appending, and `panel.js:evil` / `x::$DATA`, an NTFS alternate data stream whose bytes
// no directory walk enumerates while `path::extension()` reports the BASE name's extension — see
// the impl for why that one is refused textually rather than left to the media allowlist). Empty
// segments (`//`, a leading or trailing slash) are skipped: they cannot escape, and refusing them
// would refuse the legitimate bare root.
//
// Returns false with `reason` set on refusal. NOTHING here "cleans up" a suspicious path — a
// cleaning pass is exactly how `....//` becomes `../`.
//
// This is the SECOND link in the containment chain, and — like `percent_decode` — it is public
// because both schemes walk it and both suites attack it directly.
[[nodiscard]] bool split_safe_path_segments(std::string_view path, std::vector<std::string>& out,
                                            std::string& reason);

// Is `inner` the same path as `outer`, or somewhere beneath it? A PURE path operation on two
// ALREADY-CANONICAL paths — `lexically_relative` yields a path starting with ".." exactly when
// `inner` escapes `outer`, and an EMPTY path when the two have different root names (a different
// drive, or drive-vs-UNC), so both non-containment shapes fail CLOSED.
//
// The LAST link in the containment chain, and public for the same reason as the two above: it is
// the check that catches what the textual pass structurally cannot (a symlink out of the root, a
// decoder gap, an OS path quirk), both schemes rest on it, and a second copy is a second place for
// a de-Morgan slip to land in only one of them.
[[nodiscard]] bool path_contains_or_equals(const std::filesystem::path& outer,
                                           const std::filesystem::path& inner);

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
//   style-src  'self' 'unsafe-inline'
//                       — same-origin stylesheets, PLUS inline CSS. This is the ONE inline
//                         relaxation, and it is confined to style. The pinned, SHA-verified
//                         dockview-core engine needs inline CSS two ways at runtime: it INJECTS a
//                         <style> element for its base rules (the webpack style-loader
//                         `.dv-scrollable{...}` insert — governed by style-src-elem) AND it POSITIONS
//                         panels/sashes by writing inline style through the CSSOM
//                         (`element.style.setProperty` — governed by style-src-attr). Both fall back
//                         to style-src. The narrower `style-src-attr 'unsafe-inline'` split was tried
//                         and does NOT hold on this pinned CEF/Chromium build: it could never cover
//                         the <style>-element path (that is style-src-elem, not -attr), and the build
//                         did not honour -attr as a distinct directive, enforcing `style-src 'self'`
//                         on the CSSOM writes too — so the docking layout never manifested and
//                         editor-cef-smoke-shell stayed red on ubuntu + windows. `'unsafe-inline'` on
//                         style-src is what this build actually enforces for both paths. It stays
//                         DEFENSIBLY strict: script-src carries NO widening (the C-F6 backstop is
//                         intact — no inline/`eval` script), and connect-src 'none' + img-src
//                         'self' data: + font-src 'self' leave an inline style nowhere to exfiltrate
//                         to (no external url(), font or fetch). Hashes/nonces are no alternative:
//                         dockview's CSSOM positioning cannot carry a nonce at all, and its inline
//                         values vary by panel count and window size, so the set is not bounded.
//                         Editor-core's own authored index.html still carries NO inline <style> or
//                         style="" (the webui-scheme-contract gate enforces that as defense-in-depth);
//                         this widening exists for the vendored engine's runtime DOM, not authored markup.
//   connect-src 'none'  — NO NETWORK. This is what makes the 08 §2 "token leakage via the web
//                         layer" row hold end to end: even a fully compromised renderer that got
//                         hold of a secret has nowhere to send it. The bridge is not `fetch`, so
//                         it is unaffected.
//   frame-src  context-ext:
//                       — THE ONE FRAMING WIDENING (M9 e13a-1, design 04 §5 / 08 §1-§2), and the
//                         reviewed change the previous `frame-src 'none'` comment foreshadowed.
//                         Third-party panels are sandboxed iframes on their OWN
//                         `context-ext://<package-id>` origins (ext_scheme.h), so editor-core must
//                         be allowed to frame that SCHEME — and nothing else. It is a
//                         SCHEME-source, not a host-source, because a per-package origin list
//                         would have to be regenerated on every install; the containment that
//                         matters is not "which package may be framed" (editor-core only ever
//                         frames packages the user installed) but "which BYTES that frame can
//                         read", which the deny-by-default `ExtAssetResolver` owns. No `http:` /
//                         `https:` / `*` / `data:` source is added here or anywhere else in this
//                         policy: a widening to `frame-src *` would let a hydration-escaping bug
//                         frame the open web, which is the hole this directive exists to close.
//   object-src / base-uri / form-action / frame-ancestors 'none'
//                       — no plugins, no framing IN (the editor window is never someone else's
//                         frame), no <base> rewrite of the module URLs, no form posts. These stay
//                         at 'none': e13a-1 widened framing OUT only.
[[nodiscard]] const char* app_csp_header();

// The full response header set for a served asset. Ordered, and returned as a list rather than a
// blob so the CEF binding can hand CEF a map without re-parsing a string it just built.
[[nodiscard]] std::vector<std::pair<std::string, std::string>>
app_response_headers(const std::string& mime_type);

// The response header set for a REFUSED request (403/404), on either scheme — `csp` is the caller's
// own policy (`app_csp_header()` or `ext_csp_header()`).
//
// IT LIVES HERE, not in the CEF binding that sends it, for the same reason every other policy
// decision does. An error page IS a document: it loads on the requesting origin and it is the ONE
// response an attacker can always elicit, so its policy deserves at least the scrutiny the happy
// path gets. Spelled inside the binding it had neither a test nor a local build — deleting
// `no-store` from it failed nothing — while the served-path twin next door is pinned by both scheme
// suites on all three `build` legs. Now they are pinned side by side.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> refusal_headers(const char* csp);

// A media type split into the two fields a response actually carries SEPARATELY.
struct MediaType
{
    std::string essence; // "text/css"  — type/subtype, no parameters
    std::string charset; // "utf-8"     — the charset parameter's value, empty when absent
};

// Splits `text/css; charset=utf-8` into {"text/css", "utf-8"}.
//
// WHY THIS EXISTS (a CI-only trap that costs a full round-trip): a response's mime type and its
// charset are two DIFFERENT fields, not one string. CEF says so in its own API — `CefResponse` has
// separate `SetMimeType()` and `SetCharset()` accessors — and CEF's own reference resource server
// (`CefResourceManager`) fills the first from `CefGetMimeType(extension)`, which returns the
// ESSENCE only. Chromium then compares that field BY ESSENCE, so handing it the full
// `text/css; charset=utf-8` makes the comparison fail: with this response's own
// `X-Content-Type-Options: nosniff`, the stylesheet and the ES module are refused and the document
// is not treated as HTML. Nothing local catches it — it compiles clean, the CEF-free unit suites
// assert the resolver's own convention, and pre-push audit check 9 is `-fsyntax-only`. Only the live
// `editor-cef-smoke-shell` boot sees it.
//
// Lives HERE, not in the CEF binding, for the reason the whole layering exists: the binding is a
// translator with no judgement, and this is a rule about our own response policy that is testable on
// all three default `build` legs.
[[nodiscard]] MediaType split_media_type(std::string_view media_type);

} // namespace context::editor::shell
