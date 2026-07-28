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
// one — but it is why check (4) is not the only line of defence, and it was a hard E13B OBLIGATION:
// the install path must refuse a package root containing a reparse point it did not create, rather
// than leaving the property to the STL that happened to compile the resolver.
//
// ✅ PARTLY DISCHARGED IN M9 e13c-3 — and the SCOPE is stated here rather than left to be inferred,
// because the obligation as worded above says "a package root CONTAINING a reparse point" and what
// landed is narrower than that sentence. See § mount PROVENANCE below. `path_is_os_link` asks the OS
// for the reparse-point bit (`GetFileAttributesW`) / the symlink type (`lstat`) instead of asking a
// canonicalizer, so the property no longer varies by STL, and a JUNCTION becomes a testable case rather
// than one this file had to talk about — the refusal is the same on MinGW and MSVC, which is exactly
// what `test_traversal_refused` says it could not assert while the answer came from `weakly_canonical`.
//
// ⚠ WHAT IS AND IS NOT CLOSED, precisely:
//   * CLOSED — the path FROM the store root TO the package root, inclusive. `mount()` refuses a root
//     REACHED THROUGH any link, and a root that IS a link, whether it leads out of the store or stays
//     inside it. That covers the `~/.ssh`-by-symlink case and the DoD's "a link AT the root".
//   * STILL OPEN — a reparse point planted INSIDE an already-accepted package root
//     (`<store>/pkg-a/link-to-b`, the worked example above). The provenance walk stops AT the root, and
//     nothing here descends into it; the one file this module opens itself, `context-package.json`, IS
//     link-refused by name (`read_package_manifest`), but nothing else under the root is. Such a path is
//     caught only by resolver check (4) — i.e. still by `weakly_canonical`, so still MSVC-yes /
//     MinGW-junction-no, which is the dev-toolchain gap measured above, unchanged.
//     A recursive subtree walk is deliberately NOT the fix: it is unbounded work over untrusted input on
//     a path taken at every editor start. Closing it belongs to whatever WRITES the store (e13c-4's
//     install + consent surface), which is also the only layer that can see a HARD link
//     (package_store.h § THE RESIDUALS).
// Check (4) remains in the resolver, unchanged and still not alone.
//
// ⚠ E13B OBLIGATION — DISCHARGED IN M9 e13b-1. Recorded in full because the SHAPE of the answer is
// the reviewable part, and because the obligation as originally written names a control that does
// not exist.
//
// THE HAZARD. editor-core's widened `frame-src context-ext:` is a SCHEME-source, so it permits every
// package; and a `sandbox="allow-scripts"` frame may always navigate ITSELF (sandbox restricts
// navigating the TOP). Panel A can therefore set `location = 'context-ext://b/index.html'`, and the
// frame element editor-core believes hosts A now hosts B — so any capability or MessagePort handed to
// "the A frame" would land in B's document.
//
// ⚠ THE OBLIGATION'S OWN PRESCRIPTION ("key off the handshake's verified `event.origin`") IS NOT
// IMPLEMENTABLE, and e13b-1 measured why rather than working around it silently. `IFRAME_SANDBOX`
// (extpanel.ts) never carries `allow-same-origin`, so EVERY panel document reports the opaque origin
// `"null"` — one string shared by every package, which distinguishes nothing. The reflexive
// substitute is no better: an iframe's `WindowProxy` is STABLE ACROSS SAME-SLOT NAVIGATIONS (a
// web-platform property, not a bug), so an incoming `MessageEvent.source` compares EQUAL to
// `frame.contentWindow` whether the message came from the first document ever loaded into that frame
// or the third. Neither origin nor source/element identity can name a DOCUMENT INSTANCE.
//
// WHAT ACTUALLY DISCRIMINATES, and it is this file's own layer: only the SHELL knows which package's
// bytes it served into a frame. So the bridge is bound not to an origin string but to a
// SHELL-INJECTED, ALWAYS-FIRST BOOTSTRAP: every `text/html` document this scheme serves gets
// `<script src="/<kExtPortBootstrapAsset>">` spliced in ahead of any SCRIPT the document itself
// carries (`ext_inject_port_bootstrap` — a leading doctype is stepped over, the ONE thing that may
// precede the tag, and it carries no code), and that script — OUR code, not the package's — is what
// creates the `MessageChannel` and transfers one port UP to the editor
// (`ext_port_bootstrap_script`). Three properties follow, and the design needs all three:
//
//   1. IT RUNS FIRST, BY BROWSER-ENFORCED DOCUMENT ORDER — an external CLASSIC script in the head
//      blocks parsing until it has executed, and it is spliced ahead of any script the package
//      wrote. So the FIRST handshake a frame ever emits comes from the document the host navigated
//      to, not from whatever the package later navigates to. A package cannot decline to emit it in
//      order to leave the one-shot grant unclaimed for a second document.
//      ⚠ THIS HOLDS ONLY BECAUSE EVERY PANEL DOCUMENT IS `text/html`, and that is ENFORCED
//      (`ext_document_media_type_permitted`), not assumed. The splice is keyed on the media type, so
//      a SCRIPTABLE document of any other type would run package code with no bootstrap at all —
//      and `image/svg+xml` is both scriptable and on the shared asset allowlist. Such a document
//      could then `location.replace()` to a second `context-ext://` document DURING PARSE, whose
//      bootstrap would emit the first handshake the host ever sees, while the aborted navigation
//      means the frame's own `load` never fires and editor-core's revocation never runs. That is
//      verbatim the hazard this file declares discharged, so the media type of a DOCUMENT
//      NAVIGATION is refused at the binding rather than left to the panel manifest.
//   2. THE CHILD CREATES THE CHANNEL AND TRANSFERS THE PORT UPWARD, so the host NEVER posts a port
//      DOWN to a Window. That removes a real race the reverse direction cannot close: a host that
//      answered a handshake by posting a port to `event.source` would be naming a stable WindowProxy,
//      and if a navigation completed between the handshake being queued and the reply being
//      delivered, the port would land in the NEW document. Here the port the host keeps is entangled
//      with a port that only ever existed inside the handshaking document's realm; that document's
//      navigation destroys its end, and no other document can obtain it (a `MessagePort` is not
//      structured-cloneable, `frame-src`/`child-src`/`worker-src 'none'` leave the panel no second
//      realm to hand it to, and the panel has no handle on any other frame).
//   3. IT NEEDS NO CSP RELAXATION. An INLINE bootstrap would have required a `'nonce-…'` source on
//      `script-src`, and a nonce is readable by the document's own script (`HTMLScriptElement.nonce`
//      is not hidden from the IDL attribute), which would hand every panel an effective
//      `'unsafe-inline'` — a real loss for a package that renders untrusted data of its own. An
//      EXTERNAL script on the package's own origin is already authorized by `script-src 'self'`, so
//      the policy below is unchanged, byte for byte.
//
// The editor-core half (panelport.ts) supplies the two remaining halves of the grant: the ONE-SHOT
// (the first conforming handshake per frame is accepted and every later one refused, which is what
// makes property 1 load-bearing) and REVOCATION ON RE-NAVIGATION (a second `load` event on the frame
// element closes the port), so a document that somehow did obtain one holds a dead channel.
//
// ⚠ WHAT IS STILL NOT CLAIMED. This binds the port to the FIRST DOCUMENT LOADED INTO THE FRAME, and
// that document is the one the host navigated to — it does NOT prove to the host WHICH PACKAGE that
// was. Nothing in the browser can: the host chose the URL, so it knows the package it ASKED for, and
// the Shell served exactly that URL. The residual gap is therefore a Shell/editor-core agreement,
// not a browser-verified fact, and it is recorded as such rather than dressed up as authentication.
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

// ------------------------------------------------------- the e13b-1 panel-port bootstrap vocabulary

// The ONE asset path this scheme serves out of ITSELF rather than out of a package (M9 e13b-1): the
// port bootstrap the injected `<script src>` fetches. RESERVED — a package file at this path is
// unreachable, which is stated rather than enforced because enforcing it would mean a filesystem
// probe at mount time for a name no package has any reason to use.
//
// The leading '.' is load-bearing as a NAMESPACE MARKER, not as a hidden-file convention: it is one
// segment (so `split_safe_path_segments` accepts it — only the exact `.` and `..` segments are
// refused), and the injected tag names it ABSOLUTELY (`/` + this) so it resolves against the package
// ORIGIN. A relative spelling would resolve against the entry document's DIRECTORY, which silently
// 404s the bootstrap for any package whose entry is not at the root (`context-ext://pkg/ui/index.html`
// would fetch `/ui/.context-panel-port.js`) — a whole class of package that would come up portless
// with nothing naming the cause.
inline constexpr const char* kExtPortBootstrapAsset = ".context-panel-port.js";

// The handshake message's `ctx` discriminator — MIRRORED in editor-core (`EXT_PORT_HANDSHAKE_TAG`,
// panelport.ts) and byte-compared against it by `tools/check_webui_assets.py --scheme-contract`
// (ctest `webui-scheme-contract`), exactly as the scheme vocabulary above is.
//
// THE PROTOCOL VERSION IS IN THE STRING ON PURPOSE. A separate numeric constant is the obvious
// spelling and it is the one that rots: the contract gate reads STRING constants out of the built
// bundle, so a numeric version drifting between the injected script and the host's check would be
// invisible to it — and the failure mode is total (the host refuses every handshake, every panel is
// portless, nothing names why). Folding the version into the gated string makes a version bump a
// RENAME, which the gate cannot miss.
inline constexpr const char* kExtPortHandshakeTag = "context.panel-port.v1";

// Where the bootstrap publishes the panel's end of the channel, for the package's own code:
// `window.contextPanelPort`. A PACKAGE-facing name, so it is deliberately NOT in the cross-language
// contract gate — editor-core never reads it (it lives inside a document editor-core cannot touch),
// and the only consumer is third-party code in the panel's own realm.
inline constexpr const char* kExtPortGlobalName = "contextPanelPort";

// The BUDGET for both prefix scans `ext_inject_port_bootstrap` runs before splicing: how far the
// leading-whitespace skip may walk, and how far past the start of a `<!doctype` the terminating '>'
// may sit. Declared with its siblings above rather than beside the function, so the header has one
// home for this vocabulary.
//
// Bounded because the body is THIRD-PARTY. Unbounded, either scan is a byte-at-a-time full-body walk
// on the CEF IO thread for every HTML response — a document that is nothing but spaces, or a
// `<!doctype` with no '>', would pay it and then fall through to the same offset anyway. Exceeding
// either budget gives up and splices at the BOM floor, which is the direction that keeps the
// bootstrap ahead of package code; the only thing lost is standards mode on a document already
// pathological enough to spend a kilobyte before saying anything.
inline constexpr std::size_t kExtDoctypeScanLimit = 1024;

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

// -------------------------------------------------------------------- mount PROVENANCE (M9 e13c-3)
//
// THE HALF e13a-1 DECLARED MISSING, and the reason it could not be written there: "a root must be
// inside the package store it claims to come from" needs a package store, and e13a-1 had none. It
// does now (package_store.h), so `mount()` below takes the store root and this section is what it
// enforces. The whole point is that the ANSWER IS OURS: every refusal here is decided by code in
// this file, and NOT ONE of them is delegated to `std::filesystem::weakly_canonical`.
//
// ⚠ WHY THAT MATTERS, RESTATED FROM THE MEASUREMENT IN THE FILE HEADER. Canonicalization resolves a
// reparse point only on the STLs that choose to: MSVC does (`GetFinalPathNameByHandle`), the MinGW
// libstdc++ of the local dev gate does NOT, and a DIRECTORY JUNCTION needs no privilege to create on
// Windows. A containment check built on canonicalization therefore holds a different property per
// toolchain — which is not a property at all. `path_is_os_link` asks the OS directly instead, so the
// refusal is identical on every build of every toolchain.
//
// GREP-STABLE LOCAL CODES, not R-CLI-008 catalog rows (the discipline `registry.h` states for the
// same reason): a mount refusal is reported to the operator on stderr and asserted in this module's
// own suite, it never crosses the CLI/protocol surface, so protocolMajor and the contract-freeze
// gate stay untouched. SIX distinct codes because they are six distinct FAULTS, and a package
// author (or an operator reading stderr) must be able to tell them apart — the same reasoning
// `package_sessions.h` records for its four.

/** No store root was supplied. FAIL-CLOSED: an unbound store cannot vouch for any root. */
inline constexpr const char* kErrMountStoreRootUnset = "package.store_root_unset";
/** The store root itself does not resolve to an existing directory. */
inline constexpr const char* kErrMountStoreRootInvalid = "package.store_root_invalid";
/** The candidate root is relative, or carries a `..` component — refused TEXTUALLY (`.` is not). */
inline constexpr const char* kErrMountRootTraversal = "package.root_traversal";
/** The candidate root is not lexically beneath the store root (an absolute path elsewhere). */
inline constexpr const char* kErrMountRootOutsideStore = "package.root_outside_store";
/** A path component from the store root down to the candidate IS AN OS LINK — see path_is_os_link. */
inline constexpr const char* kErrMountRootLink = "package.root_link";
/** The canonical form of the candidate escapes the store root — the second, independent line. */
inline constexpr const char* kErrMountRootEscapesStore = "package.root_escapes_store";

// Is `path` an OS-level LINK — something whose name resolves to bytes stored under another name?
//
// ASKED OF THE OS, NEVER OF THE STL'S CANONICALIZER. That is the entire reason this function exists,
// and the two platform answers are deliberately different questions:
//
//   * WINDOWS — `GetFileAttributesW` and the `FILE_ATTRIBUTE_REPARSE_POINT` bit. TRUE for a symlink,
//     TRUE for a DIRECTORY JUNCTION (`IO_REPARSE_TAG_MOUNT_POINT`), and true for every other reparse
//     tag, because the tag is NOT inspected: an install path has no business accepting a package root
//     that is any kind of indirection it did not create, and enumerating "safe" tags would be a
//     denylist over a set Microsoft extends. This is the branch where the STLs diverge, so it is the
//     branch that had to stop being the STL's.
//   * POSIX — `::lstat` and `S_ISLNK`. A raw `lstat`, not `std::filesystem::symlink_status`, for
//     symmetry of provenance rather than necessity: libc++ and libstdc++ agree here, so the STL call
//     would have been correct — but a security predicate whose two arms come from two different
//     authorities is one arm away from a silent divergence, and the cost of the raw call is one
//     `#include`.
//
// FALSE when the path does not exist. FALSE ALSO when the query itself failed — but that case sets
// `*query_failed` (when a pointer is given), and the CALLER MUST FAIL CLOSED ON IT.
//
// ⚠ THE TWO ARE NOT INTERCHANGEABLE, and an earlier draft of this comment claimed they were — that
// "a link this cannot see is not thereby admitted, because the caller also requires an existing
// directory and canonical containment". That argument is FALSE, and this file's own suite is what
// disproves it: for a link pointing INSIDE the store, canonical containment PASSES (the target really
// is contained), which is precisely why `test_ext_scheme.cpp` treats that case as the one that
// discriminates this predicate from a canonical compare. So a query failure that read as "not a link"
// would be a FAIL-OPEN in exactly the case the predicate exists for. It is also reachable on Windows
// with no privilege: `GetFileAttributesW` fails on a path at or past `MAX_PATH` unless the process is
// long-path aware (this one ships no such manifest), while MSVC's `std::filesystem` handles long paths
// internally — so every other check keeps working and only this walk goes blind.
//
// Hence the split: ABSENT stays FALSE with `*query_failed` clear, so a missing package is still
// refused as missing by the caller's existing-directory check and a transient blip never mislabels one
// as a link; UNDECIDED sets `*query_failed`, and `package_root_provenance_ok` refuses the component
// (`kErrMountRootLink`) when the path nonetheless EXISTS. A caller passing `nullptr` gets the old
// two-valued answer and MUST NOT be a security decision.
[[nodiscard]] bool path_is_os_link(const std::filesystem::path& path, bool* query_failed = nullptr);

// May `package_root` be mounted as a package installed in `store_root`? The PROVENANCE check —
// "where did this root come from" — as opposed to `mount()`'s existing "what SHAPE is this root".
//
// Sets `error_code` (one of the constants above) + a human `message` and returns false on refusal.
// SIX CODES AT FIVE REFUSAL POINTS, IN THIS ORDER (item 5 below is deliberately NOT a refusal of this
// function — `mount()` keeps its own), and the order is part of the contract: each fault is reported by
// the FIRST layer that can see it, so a diagnostic never blames a later layer for an earlier layer's
// input.
//
//   1. `store_root` EMPTY -> `kErrMountStoreRootUnset`. Fail-closed, and it is the DEFAULT state of a
//      `CefShellOptions` that names no store: a caller who forgets the store root gets every mount
//      refused, never every mount admitted. This is why the store root is a REQUIRED PARAMETER of
//      `mount()` rather than a member with a setter — a member can be left unset, and the failure
//      mode of an unset member is only fail-closed until someone "helpfully" defaults it.
//   2. `store_root` does not canonicalize to an existing directory -> `kErrMountStoreRootInvalid`.
//      The store root IS canonicalized (and this is the ONLY canonicalization in the function that
//      participates in a decision): it is the editor's OWN configuration — `~/.context/packages`, or
//      a smoke's temp fixture dir — not an attacker-influenced value, and on macOS the system temp
//      dir genuinely reaches through a `/var -> private/var` symlink, so refusing a link ABOVE the
//      store would refuse every test fixture and every default install on that OS. What a package
//      author can influence is everything BELOW it, and that is what step 4 walks.
//   3. `package_root` is relative, or carries a `..` component -> `kErrMountRootTraversal`, BEFORE the
//      filesystem is touched (a `.` component is NOT an escape and is normalized away instead). Then a
//      LEXICAL containment test -> `kErrMountRootOutsideStore`, which is what refuses an absolute path
//      pointing anywhere else (`~/.ssh` is the worked example e13a-1's obligation names). Equality with
//      the store root is ALSO refused: the store is not a package.
//      ⚠ THE CANDIDATE IS NEVER CANONICALIZED TO MAKE IT FIT, and that is load-bearing rather than an
//      omission: canonicalizing it would resolve the very link step 4 exists to SEE, and the link
//      refusal would go with it. So the containment test is lexical, and it is tried against the store
//      root AS GIVEN and against its canonical form — either spelling is a correct anchor, and trying
//      both is what lets a caller pass an UNCANONICAL store root (measured: `temp_directory_path()` on
//      macOS is `/var/folders/…` whose canonical form is `/private/var/…`, which is the live
//      `editor-cef-smoke-shell-iframe`'s own situation).
//      ⚠ MIXED PAIRS — a store root in one spelling with a candidate in the other — are NOT symmetric,
//      and the REAL producer relies on that. An AS-GIVEN store root with a CANONICAL candidate is
//      ACCEPTED, by the second `lexically_relative` attempt; only the inverse fails CLOSED
//      (`kErrMountRootOutsideStore`). The accepted direction is exactly what production produces:
//      `package_mounts` hands out `InstalledPackage::root`, which is the CANONICAL root the scan
//      recorded, while `editor_main.cpp` passes `package_store_root()` as given. So both anchors are
//      load-bearing — dropping the as-given attempt breaks a store root spelled non-canonically
//      (macOS `/var`, and an 8.3-expanded path on the MSVC leg), and dropping the canonical attempt
//      breaks the scan→mount producer itself. (An earlier version of this note claimed a mixed pair was
//      simply unreachable "because every producer BUILDS the candidate as `store_root / <id>`"; that is
//      not what `package_mounts` does.)
//   4. ANY path component from the store root (exclusive) down to `package_root` (INCLUSIVE) is an OS
//      link -> `kErrMountRootLink`. THE SECURITY CORE, and it is refuse-by-construction rather than
//      refuse-if-it-escapes: a link is refused whether it points outside the store or inside it.
//      TWO reasons, and the second is why the weaker rule would be a bug rather than a taste:
//        (a) A link's TARGET is not a stable fact. A check that admits `<store>/a -> <store>/b`
//            because b is contained has admitted a NAME whose bytes whoever can write that link may
//            repoint afterwards, at any time, with no further consent. The mount decision would then
//            be about a state of the world that no longer holds.
//        (b) It is what makes the property TESTABLE at all, and testable IDENTICALLY on every leg.
//            A refuse-if-it-escapes rule is exactly what `weakly_canonical` already gives you where
//            it resolves links — so a test of it measures the STL on POSIX and MSVC, and measures
//            NOTHING on MinGW. A link INSIDE the store discriminates: canonicalization calls it
//            contained, this rule refuses it. `test_ext_scheme.cpp` asserts precisely that case, and
//            that is the assertion which would go GREEN — i.e. red as a test — if this check were
//            ever replaced by a canonical compare.
//      The walk starts BELOW the canonical store root for the reason step 2 gives, and INCLUDES the
//      candidate itself, which is the DoD's "a link AT the root" case (`<store>/pkg` is a symlink).
//   5. `package_root` must be an existing DIRECTORY -> `kErrMountStoreRootInvalid` is NOT reused;
//      `mount()` keeps its own pre-existing "not an existing directory" refusal for that, so this
//      function is purely about provenance and the shape checks stay where they were.
//   6. The canonical form of `package_root` must STILL be inside the canonical store root ->
//      `kErrMountRootEscapesStore`. REDUNDANT WITH 3+4 ON EVERY TOOLCHAIN WE KNOW OF, and kept
//      deliberately, exactly as the resolver keeps its textual and canonical passes both: this one
//      catches what a decoder gap or an OS path quirk could smuggle past a lexical comparison, and
//      it is the only layer that would notice a link the OS declined to report.
//
// `out_canonical` receives the canonical root on success — the value `mount()` stores, so the
// canonicalization is not paid twice and cannot disagree between the check and the record.
[[nodiscard]] bool package_root_provenance_ok(const std::filesystem::path& store_root,
                                              const std::filesystem::path& package_root,
                                              std::filesystem::path& out_canonical,
                                              std::string& error_code, std::string& message);

// ------------------------------------------------------------------------------------- resolution

// The outcome of resolving one `context-ext://…` URL. Mirrors `AssetResolution` (app_scheme.h) and
// shares its `AssetStatus` + `http_status_for` mapping, plus the package the URL named.
struct ExtResolution
{
    AssetStatus status = AssetStatus::bad_request;
    // The package the URL named, when the URL was well-formed enough to carry one. Empty otherwise.
    // Diagnostic only — it is NEVER used to build a path.
    std::string package_id;
    // Only meaningful when `status == ok`. EMPTY when `synthetic` is set — see below.
    std::filesystem::path file;
    std::string mime_type;
    // Set for the ONE asset the scheme serves out of ITSELF: the e13b-1 port bootstrap
    // (`kExtPortBootstrapAsset`). The caller supplies the bytes from `ext_port_bootstrap_script()`
    // instead of reading `file`.
    //
    // A FLAG RATHER THAN A SECOND STATUS, deliberately: `AssetStatus` is shared with the app scheme
    // and is what `http_status_for` maps, so a synthetic member there would force every switch over
    // it — in both schemes — to grow an arm for a case only this one has. Keeping it orthogonal means
    // a caller that ignores the flag serves an EMPTY 200 rather than something wrong.
    bool synthetic = false;
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

    // Mount `root` as `package_id`, having come from the package store at `store_root`. Returns
    // false with `reason` set, and mounts nothing, when:
    //   * the id is not a valid package id (see is_valid_package_id);
    //   * the id is already mounted — a second mount would silently shadow the first, and which one
    //     wins is not a question a security boundary should have an answer to;
    //   * the root FAILS THE PROVENANCE CHECK against `store_root` — see
    //     `package_root_provenance_ok` above for all six refusals, which include the canonicalization
    //     failure this list used to name (there is deliberately no "fall back to the raw path"
    //     branch: the overlap refusal below is a LEXICAL comparison, so it establishes disjointness
    //     only when both sides are canonical, and every containment check afterwards compares
    //     against the stored value);
    //   * the root does not exist or is not a directory (a package that is not installed is not
    //     mounted — deny-by-default applies to the mount table too);
    //   * the canonical root CONTAINS, IS, or IS CONTAINED BY an already mounted root. See the file
    //     header: nested roots turn per-root containment into a cross-package read.
    //
    // `reason` is `"<code>: <message>"` for a provenance refusal, so the grep-stable code is in the
    // string the CEF binding prints to stderr and the suite asserts on; the four pre-existing
    // refusals keep their prose spellings byte-for-byte, so no existing assertion moves.
    //
    // ✅ E13B OBLIGATION — DISCHARGED IN M9 e13c-3. `store_root` is that discharge, and it is a
    // REQUIRED PARAMETER rather than a constructor argument or a settable member ON PURPOSE: the
    // obligation was that a root must be inside the package store it claims to come from, and a
    // parameter is the only spelling of that which a caller cannot forget. An EMPTY `store_root`
    // refuses every mount (`kErrMountStoreRootUnset`), so the fail-closed direction needs no
    // discipline either. A package whose manifest points its asset root at `~/.ssh` — the worked
    // example the obligation named — is now refused by `kErrMountRootOutsideStore`, and one that
    // tries to get there through a symlink or a Windows junction by `kErrMountRootLink`.
    //
    // ✅ SECOND E13B OBLIGATION — CASE — ALSO DISCHARGED, and NOT here: it is discharged where the
    // obligation said it had to be, in the install path. The overlap refusal below still compares
    // with `path::compare`, which is case-SENSITIVE while NTFS is not, so two differently-cased
    // spellings of one directory could still both mount under a non-MSVC Windows build if a caller
    // offered both. `scan_package_store` (package_store.h) is what makes sure no caller does, and it
    // needs NO dedupe step to do it: roots are BUILT from enumerated directory names, so each arrives
    // in exactly the one spelling the filesystem reports, and `is_valid_package_id` accepts LOWER CASE
    // ONLY — so a directory named `Pkg` is refused outright (`kErrPackageIdInvalid`, package_store.h)
    // and two ACCEPTED ids cannot differ by case at all. That argument's authority is
    // package_store.h § the scan; it is deliberately NOT restated here, so the two cannot drift.
    // ⚠ There is NO case-collision error code, and adding one would be dead code: an explicit
    // collision refusal was written during e13c-3 and REMOVED once review showed it could never fire.
    [[nodiscard]] bool mount(std::string_view package_id, const std::filesystem::path& root,
                             const std::filesystem::path& store_root, std::string& reason);

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

// ------------------------------------------------------------ the e13b-1 panel-port bootstrap (§ above)

// The bootstrap script's bytes — served as `kExtPortBootstrapAsset` on the package's own origin, and
// the ONLY code in a panel document that the editor wrote. It creates one `MessageChannel`, publishes
// `port1` on `window.<kExtPortGlobalName>` for the package, and transfers `port2` to the editor.
//
// FOUR PROPERTIES OF THE TEXT, each of which a rewrite must preserve (test_ext_scheme.cpp pins all
// four, so this comment cannot drift away from the code alone):
//   * It posts to `kAppOrigin` EXACTLY, never `"*"`. This is the one direction in which a precise
//     target origin is possible — `targetOrigin` constrains the RECEIVER, and the receiver is
//     editor-core on a known origin. (The REVERSE direction, editor->panel, is stuck with `"*"`
//     because the panel's origin is opaque; that is why `IframeThemeChannel` uses it and why the
//     asymmetry is correct rather than an oversight.) `frame-ancestors` already restricts who may
//     frame a panel; this makes the port undeliverable to anyone else even if that directive were
//     ever loosened.
//   * It returns EARLY when the document is not framed (`parent === window`), so a panel document
//     opened directly in a tab mints no channel and transfers nothing to itself.
//   * The global is installed NON-WRITABLE and NON-CONFIGURABLE, so a package's own bundle shim
//     cannot clobber the port with a look-alike and strand the real one unreferenced.
//   * It is pure ASCII and contains no `</script` sequence, because it is delivered as a separate
//     resource today but is spliced into a document's byte stream by NOTHING — if a future change
//     ever inlines it, an embedded closing tag would truncate the host document.
[[nodiscard]] const char* ext_port_bootstrap_script();

// Splice the bootstrap `<script src>` tag into an HTML document, and return every OTHER media type's
// body UNCHANGED. The whole decision lives here — in the CEF-free half — so the CEF binding's part is
// one call with no branch of its own (see the file header on why judgement never lives in that TU).
//
// WHERE IT SPLICES, and why the rule is this shape rather than "after `<head>`":
//   * Skip a UTF-8 BOM and up to `kExtDoctypeScanLimit` bytes of leading ASCII whitespace, then, IF
//     what follows is a case-insensitive
//     `<!doctype` whose terminating '>' is within `kExtDoctypeScanLimit` bytes, splice immediately
//     AFTER that '>'. This preserves standards mode, which a splice at offset 0 would destroy by
//     displacing the doctype — and quirks mode is a real, silent behaviour change forced on
//     third-party layout.
//   * OTHERWISE splice at offset 0 — but never ahead of a UTF-8 BOM, which stays byte 0 (see below).
//     Two of the three shapes this covers lose nothing: a document with no doctype is already
//     quirks, and a document that puts a `<script>` before the doctype is the case the rule EXISTS
//     for — searching for the doctype anywhere in the prefix instead would let exactly that shape
//     run package code ahead of the bootstrap, which is property 1 of the mechanism in the file
//     header. The rule is therefore "the doctype must be FIRST for us to go second", never "find
//     the doctype".
//     ⚠ THE THIRD SHAPE PAYS A REAL PRICE, ACCEPTED DELIBERATELY: `<!-- c --><!doctype html>` is
//     legal STANDARDS-mode HTML (the "before html" insertion mode ignores comments), and splicing
//     at 0 demotes it to quirks. Skipping a leading comment instead means scanning third-party
//     bytes for `-->` — a tokenizer's worth of judgement about adversarial input, in order to move
//     OUR script LATER. A package that wants standards mode puts its doctype first, as the spec
//     already tells it to.
//
// NOTHING IS PARSED, and nothing else in the document is examined. An HTML-aware insertion point (the
// end of `<head>`, after `<meta charset>`) would mean writing an HTML tokenizer's worth of judgement
// about adversarial bytes in order to move the script LATER, which is the wrong direction anyway. The
// `<meta charset>`-within-1024-bytes rule is unaffected for a different reason: these responses carry
// an explicit `charset` on `Content-Type` (`asset_media_types()`), and an HTTP charset OVERRIDES a
// meta declaration, so displacing that element cannot change the document's encoding.
[[nodiscard]] std::string ext_inject_port_bootstrap(const std::string& mime_type,
                                                    std::string_view body);

// May a response of this media type be served as a DOCUMENT NAVIGATION (a main or sub frame)?
//
// THE COMPANION GATE TO THE SPLICE, and the reason property 1 in the file header is a fact rather
// than a hope. `ext_inject_port_bootstrap` rewrites `text/html` and nothing else, so a scriptable
// document of ANY other type would run package code with no bootstrap ahead of it — and
// `image/svg+xml` is exactly that: scriptable, framable, and on the shared asset allowlist
// (`asset_media_types()`), which the panel-entry grammar does not constrain. From such a document a
// package can navigate the frame to a SECOND `context-ext://` document during parse; that
// document's bootstrap then emits the first handshake the host ever sees, and because the aborted
// navigation means the frame's own `load` never fires, editor-core's revocation never runs either.
// The grant would be live, and inside a document the host never chose.
//
// So a document navigation is refused unless its media type is one the splice covers. It is a
// PREDICATE here, in the CEF-free half that all three `build` legs adversarially test, rather than a
// media-type comparison written in the CEF binding — the binding only translates CEF's resource type
// into "is this a document?", which is the one thing it alone can know.
//
// SUBRESOURCES ARE UNAFFECTED: an `<img src="…/icon.svg">` inside a panel is not a navigation, so it
// still resolves and is served exactly as before. Only the entry (or a self-navigation) is narrowed.
[[nodiscard]] bool ext_document_media_type_permitted(const std::string& mime_type);

// Apply the document gate to an ALREADY-RESOLVED response, in place.
//
// THE REFUSAL ITSELF LIVES HERE, not in the CEF binding, for the same reason every other refusal in
// this file does: `!ok()` alone cannot tell a held boundary from a differently-held one, so the
// status AND the reason string are part of the contract and belong somewhere all three `build` legs
// can assert them. The binding's whole job is the ONE bit it alone knows — whether CEF is about to
// make a document out of these bytes — so it passes that bit and nothing else.
//
// FORBIDDEN, not not_found, and deliberately unlike the enumeration-defence statuses above: this
// refusal leaks nothing about the mount table (it is a property of the ASSET's declared media type,
// which the requester already chose), and a 404 here would tell a package its own file is missing
// when it is present and simply not a legal document.
//
// A resolution that already failed is left untouched — the first refusal is the honest one, and
// overwriting it would replace a mount/path/id diagnostic with a media-type one.
void ext_apply_document_gate(ExtResolution& resolution, bool is_document_navigation);

} // namespace context::editor::shell
