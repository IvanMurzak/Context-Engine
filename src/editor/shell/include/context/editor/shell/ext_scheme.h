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
// ⚠ WHAT CHECK (4) RESTS ON, MEASURED RATHER THAN ASSUMED: it catches a link out of the package
// only insofar as `std::filesystem::weakly_canonical` RESOLVES that link, and the STLs diverge on
// Windows. MSVC resolves reparse points through `GetFinalPathNameByHandle` — both symlinks and
// DIRECTORY JUNCTIONS — so the shipped Windows binary (MSVC on every CI leg and every export) is
// covered. The MinGW libstdc++ used by the local dev gate does NOT resolve a junction: measured
// here, `weakly_canonical(<pkg-a>/link-to-b/private.js)` returns the path unchanged, so a junction
// planted inside a package root would read as contained. That matters because a junction, unlike a
// symlink, needs NO privilege to create on Windows. It is a dev-toolchain gap rather than a shipped
// one — but it is why check (4) is not the only line of defence, and it is a hard E13B OBLIGATION:
// the install path must refuse a package root containing a reparse point it did not create, rather
// than leaving the property to the STL that happened to compile the resolver.
//
// ⚠ E13B OBLIGATION (RE-HOMED FROM e13a-2, WHICH SHIPPED NO TRANSPORT) — BIND THE BRIDGE TO A
// VERIFIED ORIGIN, NOT TO THE FRAME. editor-core's widened `frame-src context-ext:` is a
// SCHEME-source, so it permits every package; and a `sandbox="allow-scripts"` frame may always
// navigate ITSELF (sandbox restricts navigating the TOP). Panel A can therefore set
// `location = 'context-ext://b/index.html'`, and the frame element editor-core believes hosts A now
// hosts B. No bytes leak today — A's own script is destroyed by the navigation, and this scheme
// still serves each origin only its own root — but any capability or MessagePort hands to "the A
// frame" would land in B's document. The bridge must key off the handshake's verified
// `event.origin`, never off the element it was granted to.
//
// e13a-2 landed the iframe HOST and deliberately installs NO `message` listener at all
// (panelhost.ts `IframePanelRenderer`): an editor that listened before the port handshake existed
// would be accepting unauthenticated postMessages from untrusted code for no capability in return.
// So the hazard above is inert until e13b, which is exactly why it is recorded on e13b.
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
// PINNED to (design 04 §5 / 08 §2: `STANDARD|SECURE|CORS_ENABLED`) is asserted by a unit test on
// all three default `build` legs — where CEF is not built at all — instead of only being
// declared in the one translation unit the local gate cannot compile.
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
//   [a-z0-9] ( [a-z0-9._-]* [a-z0-9] )?   , 1..kExtPackageIdMaxLength bytes, no ".." anywhere,
//   and the last '.'-separated label is not all digits
//
// TWO OF THOSE RULES ARE CORRECTNESS, NOT STYLE — both refuse an id that a request could never name
// again, turning a package that would be SILENTLY unreachable into a loud mount-time failure:
//   * LOWERCASE ONLY — Chromium canonicalizes a standard URL's host to lower case, so an id
//     registered with an upper-case letter can never be matched by an incoming request.
//   * LAST LABEL NOT ALL DIGITS — the URL Standard's "ends in a number" check sends such a host to
//     the IPv4 parser instead of keeping it a domain, so `12345` arrives back as `0.0.48.57` and
//     `pkg.2` fails host parsing outright. (`pkg2` and `context.hello-panel_2` are unaffected: the
//     rule is about a label that is ENTIRELY digits, not about digits appearing in one.)
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
    //   * the root cannot be CANONICALIZED. There is deliberately no "fall back to the raw path"
    //     branch: the overlap refusal below is a LEXICAL comparison, so it establishes disjointness
    //     only when both sides are canonical, and every containment check afterwards compares
    //     against this stored value;
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
    //
    // ⚠ SECOND E13B OBLIGATION — CASE. The overlap refusal compares paths with `path::compare`,
    // which is case-SENSITIVE, while NTFS is not. Canonicalization is what closes the gap in
    // practice (MSVC's `weakly_canonical` case-normalizes through `GetFinalPathNameByHandle`; the
    // MinGW libstdc++ used by the local dev gate does NOT), so two differently-cased spellings of
    // one directory could both mount under a non-MSVC Windows build. Refusing an uncanonicalizable
    // root above is what keeps this narrow; an install path that can produce two spellings of one
    // root must dedupe them before mounting.
    [[nodiscard]] bool mount(std::string_view package_id, const std::filesystem::path& root,
                             std::string& reason);

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
//   child-src / worker-src 'none'
//                       — SPELLED OUT rather than left to the fallback chain, which does NOT end at
//                         `default-src` for these: `worker-src` falls back to `child-src` and then
//                         to `script-src 'self'`, so omitting both would GRANT a panel a Worker and
//                         a SharedWorker rendezvous on its own origin. Nothing decided that; a
//                         capability acquired by fallback is not a reviewed one.
//   frame-src / object-src / base-uri / form-action 'none'
//                       — a panel may not nest further frames, load plugins, rewrite its module
//                         base URL, or post a form off-origin.
//   frame-ancestors context-editor://app
//                       — THE PANEL MAY ONLY BE FRAMED BY THE EDITOR. It is emphatically NOT
//                         `'none'` (which would block the panel from being framed at all) and not
//                         `*` (which would let any page that can reach the scheme frame it).
//                         ⚠ A HOST-source since M9 e13a-2, TIGHTENED from e13a-1's scheme-source
//                         `context-editor:`. The scheme form also authorized
//                         `context-editor://ipc` to frame a panel (app_scheme.h declares that
//                         SECOND host) — empty in practice, since only `kAppHost` gets a
//                         `CefRegisterSchemeHandlerFactory` and `OnBeforeBrowse` independently pins
//                         the main frame to `kAppUrlPrefix`, but breadth nobody asked for. It was
//                         left broad only until a live iframe smoke could prove the host-source form
//                         does not silently collapse the directive and block panels outright; that
//                         smoke is `editor-cef-smoke-shell-iframe`, and the assertion which
//                         discharges this obligation is deliberately NOT "the frame exists" but "the
//                         panel's OWN SUBRESOURCES were requested over the scheme" — a frame this
//                         directive blocked never parses its document, so it never asks for them.
//                         ⚠ The value is `kAppOrigin`'s TEXT, not the constant: this policy is one
//                         spelled-once string (ext_scheme.cpp), and `test_ext_scheme.cpp` pins the
//                         whole thing by exact compare PLUS an `ends_with` — because the old,
//                         broader value is a strict PREFIX of this one, so a substring probe passes
//                         identically before and after and would prove nothing.
//
// ⚠ THE TWO e13a-1 HYPOTHESES ABOUT A BLANK PANEL ARE NOW SETTLED BY MEASUREMENT — and the answer
// was NEITHER of them. Both are recorded with their evidence, because the true cause looks exactly
// like hypothesis (1) from the outside and would otherwise be re-derived as one:
//
//   (1) SETTLED YES — `'self'` DOES match inside a document framed `sandbox="allow-scripts"` (an
//       OPAQUE origin). CSP computes `'self'` from the POLICY's self-origin, which is the RESPONSE
//       URL's origin, not from the document's opaque origin. Measured: with the policy exactly as
//       written above, a sandboxed panel's own `panel.css` loads and its own `panel.js` is fetched.
//       So the policy needs no relaxation, and an `'unsafe-inline'` at a call site would still be
//       the wrong fix for any future blank panel.
//   (2) SETTLED IRRELEVANT — `FETCH_ENABLED` is not the missing piece, and withholding it (see
//       kExtSchemeOptions) costs the module graph nothing.
//
// ⚠ THE ACTUAL CAUSE, and why `ext_response_headers` now emits a CORS header on SCRIPTS. An ES
// module is ALWAYS fetched in CORS mode; a sandboxed frame's origin is the opaque `null`; so a
// module response with no `Access-Control-Allow-Origin` is fetched and then DISCARDED by the CORS
// check. The failure is silent and deeply misleading: the document loads, the stylesheet applies, a
// CLASSIC `<script>` even executes, and ONLY the module graph dies — no directive is named, so every
// symptom points at CSP. Measured on a real headless Chromium against a byte-identical policy, with
// a no-sandbox control frame isolating the sandbox as the variable and a no-CSP run isolating CSP
// out of it:
//
//     sandboxed + this CSP                  -> panel.css ✓  panel.js ✓  its imports ✗
//     sandboxed + NO CSP AT ALL             -> panel.css ✓  panel.js ✓  its imports ✗   (not CSP)
//     sandboxed + CSP + a classic <script>  -> the classic script RUNS ✓                (not scripting)
//     NOT sandboxed + this CSP              -> everything ✓                             (it is the sandbox)
//     sandboxed + this CSP + ACAO: null     -> everything ✓                             (it is CORS)
//
// The header is emitted for SCRIPT media types ONLY — the narrowest form that works, also measured.
// Stylesheets, images and fonts are no-cors fetches and need nothing, so withholding it from them
// keeps every NON-script package asset unreadable cross-origin by the same-origin policy itself
// rather than by this scheme's CSP alone. That distinction is load-bearing: EVERY panel frame has
// the same opaque origin `null`, so a blanket `Access-Control-Allow-Origin: null` would make one
// package's assets CORS-readable by another package's frame, leaving `script-src 'self'` (which
// resolves to the REQUESTING package's own origin, so it does refuse the cross-package script load)
// as the only remaining control — exactly the "resting on the CSP alone" posture kExtSchemeOptions
// declines elsewhere.
//
// ⚠ STILL NOT a `sandbox` directive, and hypothesis (1) settling does NOT by itself change that.
// Response-enforced `sandbox` would make containment independent of the embedder, which is
// genuinely attractive now that `'self'` is known to keep matching. What it would ALSO do is make
// the response's own sandbox flags and the EMBEDDER's `sandbox` attribute two independent policies
// that must agree, and the failure mode of a disagreement is a panel that works in the editor and
// breaks in a devtools-opened tab (or vice versa) with no diagnostic. That is a reviewed change
// belonging with the capability model (e13b), where there is a second policy surface to reconcile it
// against, rather than a one-line hardening bolted on here.
//
// ⚠ THE OTHER EXTENSION CSP IN THIS REPO — read before assuming this is the only one. The M5
// gui-host tier has `kDefaultExtensionCsp` (`context/editor/gui/contract/sandbox.h`), the default
// of `SandboxPolicy::csp`, emitted as a `<meta http-equiv>` by `uitree::render_document` for
// BUILT-IN headless panels. It governs a DIFFERENT document on a DIFFERENT trust tier (first-party
// panels rendered by the editor's own uitree, not third-party bundles on their own origin), which
// is why
// its `connect-src 'self'` does not contradict this policy's `'none'`. They are not
// interchangeable and neither should be edited to "match" the other by reflex — but a change to the
// third-party trust model belongs in BOTH or in neither, so reconciling the two (or deleting one)
// is an explicit e13b obligation once the package install path exists.
[[nodiscard]] const char* ext_csp_header();

// The full response header set for a served extension asset. Ordered, and returned as a list rather
// than a blob so the CEF binding can hand CEF a map without re-parsing a string it just built.
//
// DELIBERATELY NO `X-Frame-Options`, which is the ONE header the app response carries that this one
// must not: the app sends `DENY` because the editor window is never framed, whereas a panel exists
// precisely to BE framed. `frame-ancestors` above is the modern, spec'd control and is what states
// by WHOM; adding a legacy `X-Frame-Options` alongside it would at best be redundant and at worst
// (on any consumer that honours the legacy header first) block the panel outright.
//
// `Access-Control-Allow-Origin: null` IS emitted, but ONLY on script media types — see the CORS
// note above for the measurement that forced it and for why narrowing it to scripts is not
// fastidiousness but the difference between the same-origin policy and the CSP being what keeps one
// package's assets away from another's frame.
//
// TWO MORE REVIEWED OMISSIONS, recorded so neither is re-proposed as an easy win:
//   * `Cross-Origin-Resource-Policy` — it LOOKS like the obvious hardening for an untrusted origin
//     and it is actively WRONG here. The CORP check runs on no-cors fetches AND on navigation
//     requests for NESTED browsing contexts, so `same-origin` (or `same-site`) on a panel document
//     would refuse the very cross-origin framing editor-core exists to do; `cross-origin` is the
//     only value that does not break it, and it asserts nothing. It is omitted because it cannot
//     help, not because it was missed.
//   * `Permissions-Policy` — a CROSS-ORIGIN iframe already gets every powerful feature denied by
//     default unless the embedder delegates with `allow=`, so the response header would restate a
//     default. The decision that actually binds is which features e13a-2's embedder delegates, and
//     that belongs at the iframe element, not here.
[[nodiscard]] std::vector<std::pair<std::string, std::string>>
ext_response_headers(const std::string& mime_type);

} // namespace context::editor::shell
