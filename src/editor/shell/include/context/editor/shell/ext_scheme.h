// The `context-ext://<package-id>/…` extension scheme (M9 e13a-1, design 04 §5 / 08 §1-§3) — the
// per-package origin third-party panels are served from, its DENY-BY-DEFAULT resolver, and the
// response policy that goes with it. NO CEF in any of it.
//
// WHY THIS IS CEF-FREE, exactly as app_scheme.h next door: CEF is a CI-only dependency path (the
// MSVC/Clang-ABI prebuilt cannot link under the local Strawberry-GCC dev gate), so a resolver
// written inside a `CefResourceHandler` would be exercised by nothing that runs locally and by one
// CI job remotely. This is the half that decides WHICH BYTES A THIRD-PARTY PANEL MAY READ — the
// single most security-critical decision in the panel system, and the last thing that should go
// unverified. The CEF binding (src/editor/shell/cef/cef_shell.cpp) is left as a thin translator
// from CefRequest to `ExtAssetResolver::resolve` and back.
//
// THE TRUST MODEL (08 §1). A third-party panel is UNTRUSTED CODE running in a sandboxed iframe on
// its own origin. It holds no socket, no attach token, no ambient filesystem. Everything it can
// reach it reaches through (a) this scheme, for its own bundled assets, and (b) the scoped panel
// bridge (e13a-2 and later, out of scope here). So this file's whole job is: given a URL, hand back
// AT MOST one file inside ONE mounted package's own root, or refuse.
//
// DENY-BY-DEFAULT, FOUR TIMES OVER. A request must (1) name a syntactically valid package id, (2)
// name a package that is actually MOUNTED — an unknown package is refused, never guessed at and
// NEVER path-joined from the id (deriving a root by joining an attacker-influenced id onto a base
// dir is itself the traversal), (3) name a media type on the shared asset allowlist, and (4) land
// inside that package's canonicalized root. Each check is independent: the textual traversal
// rejection catches `..` before the filesystem is touched, and the canonical containment check
// independently catches what the textual pass cannot see (a symlink out of the package, a decoder
// gap, an OS path quirk).
//
// CROSS-PACKAGE READS ARE THE AXIS THIS EXISTS TO CLOSE, and containment-per-root is NOT
// sufficient on its own: if package B's root sat INSIDE package A's root, then
// `context-ext://a/b/secret.js` would be perfectly contained in A's root and would serve B's bytes
// to A. `mount()` therefore refuses any root that contains — or is contained by — an already
// mounted root, so the "one package, one disjoint subtree" property that the containment check
// relies on is established at mount time rather than assumed.

#pragma once

#include "context/editor/shell/app_scheme.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace context::editor::shell
{

// ------------------------------------------------------------------------------ scheme vocabulary

// The scheme itself. `context-ext://<package-id>/<asset-path>` — the PACKAGE IS THE HOST, which is
// what gives every package a distinct origin and therefore Chromium's per-origin isolation for
// free. A path-based layout (`context-ext://packages/<id>/…`) would put every package on ONE
// origin, where a single escaping bug is a cross-package read; the host layout makes that a
// cross-ORIGIN read the browser refuses on its own.
inline constexpr const char* kExtScheme = "context-ext";

// `context-ext://` — the URL prefix every request must carry.
inline constexpr const char* kExtUrlPrefix = "context-ext://";

// What a bare `context-ext://<package-id>/` resolves to.
inline constexpr const char* kExtDefaultDocument = "index.html";

// The longest package id the scheme accepts. A bound is part of the grammar, not decoration: an
// unbounded host is an unbounded key into the mount table and an unbounded string in every log line
// a refusal writes.
inline constexpr std::size_t kExtPackageIdMaxLength = 64;

// ------------------------------------------------------ the pinned CEF scheme-registration options
//
// A CEF-FREE MIRROR of CEF's `cef_scheme_options_t` bit values, so the flag set this scheme is
// PINNED to (design 04 §5 / 08 §2: `STANDARD|SECURE|CORS_ENABLED`) is asserted by a unit test on all
// three default `build` legs — where CEF is not built at all — instead of only being declared in the
// one translation unit the local gate cannot compile.
//
// The mirror is kept honest at the other end: cef_shell.cpp `static_assert`s each constant below
// against the real `CEF_SCHEME_OPTION_*` enumerator, so a CEF bump that renumbered them would fail
// the CEF build LOUDLY rather than silently registering the scheme with different semantics. That
// pairing is the point — a constant nothing checks against the API it mirrors is a comment.
inline constexpr unsigned kSchemeOptionStandard = 1u << 0;
inline constexpr unsigned kSchemeOptionLocal = 1u << 1;
inline constexpr unsigned kSchemeOptionDisplayIsolated = 1u << 2;
inline constexpr unsigned kSchemeOptionSecure = 1u << 3;
inline constexpr unsigned kSchemeOptionCorsEnabled = 1u << 4;
inline constexpr unsigned kSchemeOptionCspBypassing = 1u << 5;
inline constexpr unsigned kSchemeOptionFetchEnabled = 1u << 6;

// The pinned set, and every omission is deliberate:
//   STANDARD      — ordinary origin semantics. Without it Chromium treats the scheme as opaque and
//                   CSP, module scripts and same-origin checks all behave differently.
//   SECURE        — a trustworthy origin, so a panel document is not treated as insecure content
//                   and downgraded or blocked inside the secure editor-core page that frames it.
//   CORS_ENABLED  — CORS requests are meaningful for the scheme, which is what lets a panel's ES
//                   module graph resolve the way an ordinary origin's does.
//
// NOT set, and each one would undo something this scheme exists to do:
//   CSP_BYPASSING — the panel's strict CSP MUST apply; bypassing it is the whole hole.
//   LOCAL         — file-like privileges for untrusted third-party code. Never.
//   FETCH_ENABLED — deliberately NOT granted (the app scheme does take it). A panel's response
//                   carries `connect-src 'none'`, so there is no fetch for it to make; withholding
//                   the option means the refusal does not rest on the CSP alone.
//   DISPLAY_ISOLATED — would forbid a document on another scheme from displaying this one, i.e. it
//                   would stop editor-core framing panels at all. It is the opposite of the goal.
inline constexpr unsigned kExtSchemeOptions =
    kSchemeOptionStandard | kSchemeOptionSecure | kSchemeOptionCorsEnabled;

// ------------------------------------------------------------------------------------ package ids

// Is `id` a syntactically valid package id (and therefore usable as this scheme's HOST)?
//
//   [a-z0-9] ( [a-z0-9._-]* [a-z0-9] )?   , 1..kExtPackageIdMaxLength bytes, no ".." anywhere
//
// LOWERCASE ONLY, which is a correctness rule and not merely a style one: Chromium canonicalizes a
// standard URL's host to lower case, so an id registered with an upper-case letter could never be
// matched by a request again. Refusing it here makes that a loud mount-time failure instead of a
// package that is silently unreachable.
//
// The leading/trailing character rule refuses `.`, `..`, `-x`, `x.` and the empty string, and the
// `..` rule refuses any dotted spelling that would read as a parent directory if the id ever
// reached a filesystem path. It never does — roots come from the mount table, never from the id —
// but an id that is safe under that misuse costs nothing and removes a whole class of future bug.
[[nodiscard]] bool is_valid_package_id(std::string_view id);

// ------------------------------------------------------------------------------------- resolution

// The outcome of resolving one `context-ext://…` URL. Mirrors `AssetResolution` (app_scheme.h) and
// shares its `AssetStatus` + `http_status_for` mapping, plus the package the URL named.
struct ExtResolution
{
    AssetStatus status = AssetStatus::bad_request;
    // The package the URL named, when the URL was well-formed enough to carry one. Empty otherwise.
    // Diagnostic only — it is NEVER used to build a path.
    std::string package_id;
    // Only meaningful when `status == ok`.
    std::filesystem::path file;
    std::string mime_type;
    // Why it was refused — for the diagnostic channel, never for the panel (a refusal reason is a
    // probe oracle; the handler returns the status alone to the frame).
    std::string reason;

    [[nodiscard]] bool ok() const { return status == AssetStatus::ok; }
    [[nodiscard]] int http_status() const { return http_status_for(status); }
};

// One package's assets, mounted under the scheme.
struct ExtPackageMount
{
    std::string id;
    // CANONICAL after a successful mount — every containment check afterwards compares canonical
    // against canonical.
    std::filesystem::path root;
};

// Resolves `context-ext://<package-id>/…` URLs against the packages that are actually mounted.
//
// EMPTY IS THE DEFAULT AND IT IS A COMPLETE, CORRECT CONFIGURATION: a resolver with no mounts
// refuses every request. That is what lets the Shell register the scheme handler unconditionally at
// boot — before any package install path exists (e13b+) — so a `context-ext://` request is answered
// by OUR deny-by-default handler rather than by whatever Chromium does with an unhandled scheme.
class ExtAssetResolver
{
public:
    ExtAssetResolver() = default;

    // Mount `root` as `package_id`. Returns false with `reason` set, and mounts nothing, when:
    //   * the id is not a valid package id (see is_valid_package_id);
    //   * the id is already mounted — a second mount would silently shadow the first, and which one
    //     wins is not a question a security boundary should have an answer to;
    //   * the root does not exist or is not a directory (a package that is not installed is not
    //     mounted — deny-by-default applies to the mount table too);
    //   * the canonical root CONTAINS, IS, or IS CONTAINED BY an already mounted root. See the file
    //     header: nested roots turn per-root containment into a cross-package read.
    //
    // ⚠ E13B OBLIGATION — WHERE THE ROOT CAME FROM IS NOT CHECKED HERE, AND MUST BE CHECKED THERE.
    // This function validates the id and the SHAPE of the root; it cannot validate the root's
    // PROVENANCE, because e13a-1 has no package store to validate it against (the install flow is
    // e13b's). Today the only caller is the Shell's own options and the list is empty, so there is
    // nothing to get wrong. The moment an install path starts producing these mounts, that path
    // owns the missing half: a root must be inside the package store it claims to come from, or a
    // package whose manifest points its asset root at `~/.ssh` gets exactly what it asked for.
    bool mount(std::string_view package_id, const std::filesystem::path& root, std::string& reason);

    [[nodiscard]] ExtResolution resolve(std::string_view url) const;

    [[nodiscard]] const std::vector<ExtPackageMount>& mounts() const { return mounts_; }
    [[nodiscard]] bool is_mounted(std::string_view package_id) const;

private:
    [[nodiscard]] const ExtPackageMount* find(std::string_view package_id) const;

    std::vector<ExtPackageMount> mounts_;
};

// --------------------------------------------------------------------------------- response policy

// The strict CSP a third-party panel document loads under (04 §5, 08 §1-§2). STRICTER than the
// trusted editor-core policy next door, because the code it governs is untrusted:
//
//   default-src 'none'  — the deny-by-default base.
//   script-src 'self'   — the package's own bundle, and nothing else. No inline script, no eval,
//                         and NO `context-ext:` scheme-source: a panel may not load another
//                         package's script, which would be a cross-package escalation dressed up as
//                         a subresource load.
//   style-src  'self'   — the package's own stylesheets ONLY. NOTE this deliberately does NOT carry
//                         the `'unsafe-inline'` that the editor-core policy takes: that relaxation
//                         exists solely for the vendored dockview-core engine's runtime CSSOM
//                         writes, which live in editor-core, never in a panel frame. Untrusted code
//                         gets the strict form.
//   img-src / font-src 'self' (+ data: images)
//                       — own assets only; `data:` images are inert bytes and are what an icon in a
//                         bundled stylesheet needs.
//   connect-src 'none'  — NO NETWORK. The 08 §2 "malicious third-party panel exfiltrates project
//                         data" control: even a panel that got hold of something has nowhere to
//                         send it. The panel bridge is postMessage, not fetch, so it is unaffected.
//   frame-src / object-src / base-uri / form-action 'none'
//                       — a panel may not nest further frames, load plugins, rewrite its module
//                         base URL, or post a form off-origin.
//   frame-ancestors context-editor:
//                       — THE PANEL MAY ONLY BE FRAMED BY THE EDITOR. A scheme-source rather than
//                         `context-editor://app`, matching the `frame-src context-ext:` form on the
//                         other side of the same seam: `context-editor` carries exactly one
//                         document origin (`//app`), so the two spell the same permission, and the
//                         scheme form cannot be broken by a future origin rename. It is emphatically
//                         NOT `'none'` (which would block the panel from being framed at all) and
//                         not `*` (which would let any page that can reach the scheme frame it).
//
// ⚠ e13a-2 READ THIS BEFORE DEBUGGING A BLANK PANEL: `'self'` inside a document that editor-core
// framed with `sandbox="allow-scripts"` is resolved against the RESPONSE URL's origin, not the
// frame's (opaque) document origin — so it does match `context-ext://<pkg>/…`. If a live smoke ever
// shows a panel's own module refused, the console message from `OnConsoleMessage` names the
// directive, and the fix is a REVIEWED policy change here, never an `'unsafe-inline'` sprinkled at
// the call site. The same discipline the editor-core `style-src` saga above records.
[[nodiscard]] const char* ext_csp_header();

// The full response header set for a served extension asset. Ordered, and returned as a list rather
// than a blob so the CEF binding can hand CEF a map without re-parsing a string it just built.
//
// DELIBERATELY NO `X-Frame-Options`, which is the ONE header the app response carries that this one
// must not: the app sends `DENY` because the editor window is never framed, whereas a panel exists
// precisely to BE framed. `frame-ancestors` above is the modern, spec'd control and is what states
// by WHOM; adding a legacy `X-Frame-Options` alongside it would at best be redundant and at worst
// (on any consumer that honours the legacy header first) block the panel outright.
[[nodiscard]] std::vector<std::pair<std::string, std::string>>
ext_response_headers(const std::string& mime_type);

} // namespace context::editor::shell
