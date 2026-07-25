// The `context-ext://<package-id>/…` PANEL-ENTRY GRAMMAR, editor-core's half (M9 e13a-2, design
// 04 §5 / 08 §1-§2) — plus the sandbox/embedding attributes a third-party panel frame is created
// with.
//
// WHY THIS IS A SEPARATE MODULE rather than three lines inside `panelhost.ts`. The value this
// validates ends up in an `<iframe src=…>`: it is the single attacker-reachable string in the whole
// panel path, and it arrives from a package MANIFEST — which, from e13b onward, is authored outside
// this repo. A grammar that lives inside a renderer class is a grammar with no unit tests, so this
// file holds pure, total functions the `webui-ts-unit` tier drives directly (extpanel.test.ts) and
// `panelhost.ts` holds only the DOM.
//
// IT IS A MIRROR, AND THE MIRROR IS GATED. `EXT_SCHEME` / `EXT_URL_PREFIX` are byte-compared against
// the C++ `kExtScheme` / `kExtUrlPrefix` (ext_scheme.h) by `tools/check_webui_assets.py
// --scheme-contract` (ctest `webui-scheme-contract`), reading the values out of the BUILT bundle —
// the same cross-language discipline e05c applied to the app scheme. Without that gate a rename on
// either side would leave editor-core building frame URLs on a scheme the Shell does not serve, and
// every panel would come up blank with no build error anywhere.
//
// ⚠ THE GRAMMAR IS DELIBERATELY REDUNDANT WITH THE SHELL'S. `ExtAssetResolver::resolve` performs the
// authoritative check — it is the thing that decides which BYTES leave the disk, and nothing here
// can grant a panel anything the resolver refuses. This half exists for the property the resolver
// structurally cannot provide: the resolver only ever sees a URL the browser already NAVIGATED to,
// so an entry of `javascript:…`, `data:…`, `https://evil.example/` or `file:///…` would never reach
// it at all — it would simply be loaded, by the browser, from somewhere else. Refusing a
// non-`context-ext:` entry is therefore not defence in depth; it is the ONLY defence at that layer.
// editor-core's own `frame-src context-ext:` CSP is the backstop, in the same "the control, not the
// backstop" relationship hydration.ts's escaping has with its CSP.

/**
 * The extension scheme. MIRRORS C++ `kExtScheme` (ext_scheme.h) and is gated against it — see the
 * header. The PACKAGE IS THE HOST, which is what gives every package a distinct origin.
 */
export const EXT_SCHEME = "context-ext";

/** The URL prefix every panel entry must carry. MIRRORS C++ `kExtUrlPrefix`, gated identically. */
export const EXT_URL_PREFIX = "context-ext://";

/**
 * The longest package id accepted. MIRRORS C++ `kExtPackageIdMaxLength`; a bound is part of the
 * grammar, not decoration (ext_scheme.h states why).
 */
export const EXT_PACKAGE_ID_MAX_LENGTH = 64;

/**
 * The `sandbox` token list a panel frame is created with — the WHOLE list, spelled as one closed
 * constant so that widening it is an edit to a reviewed value rather than a token appended at a call
 * site.
 *
 * `allow-scripts` ALONE (04 §5). What is NOT here matters more than what is:
 *
 *   * `allow-same-origin` is the one that must NEVER appear. A sandboxed frame WITHOUT it has an
 *     OPAQUE origin, which is what makes a third-party panel origin-isolated from every other
 *     package AND from the editor even though they share a scheme. Adding it — the reflexive fix
 *     when a panel's `localStorage` or `fetch` throws — hands the panel its real
 *     `context-ext://<pkg>` origin back, and `allow-scripts` + `allow-same-origin` together let a
 *     frame remove its own sandbox attribute from the embedding document. It is the single highest-
 *     value token an attacker could get us to add, so `assertSandbox` below refuses it explicitly
 *     rather than relying on this string staying short.
 *   * `allow-top-navigation` / `-by-user-activation`: a panel may not navigate the editor away.
 *   * `allow-popups`, `allow-modals`, `allow-downloads`, `allow-forms`, `allow-pointer-lock`,
 *     `allow-presentation`: each is a capability nobody asked for. `OnBeforePopup` independently
 *     refuses popups Shell-side (03 §1), which is why omitting `allow-popups` costs a panel nothing
 *     it could otherwise have had.
 */
export const IFRAME_SANDBOX = "allow-scripts";

/** The sandbox token that must never be granted — see `IFRAME_SANDBOX`. */
export const FORBIDDEN_SANDBOX_TOKEN = "allow-same-origin";

/**
 * The Permissions-Policy delegation (`allow=`). EMPTY — delegate NOTHING.
 *
 * A cross-origin iframe is denied every powerful feature (camera, microphone, geolocation, …) BY
 * DEFAULT unless its embedder delegates with this attribute, which is exactly why ext_scheme.h
 * omits a `Permissions-Policy` RESPONSE header: the decision that actually binds is made here, at
 * the element. Empty-string is set EXPLICITLY rather than left off, so the intent is legible in the
 * rendered DOM and a test can assert it (an absent attribute and an empty one both deny, but only
 * one of them says so).
 */
export const IFRAME_ALLOW = "";

/**
 * `referrerpolicy` — send NO referrer with the frame's own subresource requests. The URL names the
 * package, and a panel must not leak which package it is (nor which document framed it) even to its
 * own origin's request log. Mirrors the `Referrer-Policy: no-referrer` the Shell's response carries.
 */
export const IFRAME_REFERRER_POLICY = "no-referrer";

/** The `loading` attribute — panels mount eagerly; a docked panel is visible by construction. */
export const IFRAME_LOADING = "eager";

/**
 * Is `id` a syntactically valid package id (and therefore usable as this scheme's host)?
 *
 * A LINE-FOR-LINE MIRROR of C++ `is_valid_package_id` (ext_scheme.cpp), including the two rules that
 * are correctness rather than style:
 *
 *   * LOWERCASE ONLY — Chromium canonicalizes a standard URL's host to lower case, so an id carrying
 *     an upper-case letter could never be matched by an incoming request.
 *   * LAST DOT-LABEL NOT ALL DIGITS — the URL Standard's "ends in a number" check hands such a host
 *     to the IPv4 parser instead of keeping it a domain, so `12345` arrives back as `0.0.48.57` and
 *     `pkg.2` fails host parsing outright.
 *
 * Divergence from the C++ side is not merely a style bug: this one decides what editor-core will put
 * in a frame, and a looser copy here would build URLs the Shell then refuses (a panel that silently
 * never loads), while a TIGHTER copy would hide packages the Shell would happily serve. The two are
 * kept honest by `extpanel.test.ts`, which drives the same table of shapes the C++ suite does.
 */
export function isValidPackageId(id: string): boolean {
    if (id.length === 0 || id.length > EXT_PACKAGE_ID_MAX_LENGTH) {
        return false;
    }
    // First and last must be alphanumeric — refuses ".", "..", ".hidden", "trailing-" and every
    // other shape that reads as a path fragment rather than as a name.
    if (!isLowerAlnum(id[0] ?? "") || !isLowerAlnum(id[id.length - 1] ?? "")) {
        return false;
    }
    for (const ch of id) {
        if (isLowerAlnum(ch) || ch === "-" || ch === "." || ch === "_") {
            continue;
        }
        // Everything else — upper case, '/', '\\', ':', '@', '%', a control byte, any non-ASCII
        // code point — is REFUSED rather than normalized. A normalizing validator is how two
        // spellings end up naming one package.
        return false;
    }
    if (id.includes("..")) {
        return false;
    }
    return !lastLabelIsNumeric(id);
}

/** One parsed panel entry: the package it names and the URL to hand the frame. */
export interface ExtPanelEntry {
    /** The URL authority — a validated package id. This is the frame's ORIGIN host. */
    readonly packageId: string;
    /** The entry VERBATIM, as validated. Never rebuilt — see `parseExtPanelEntry`. */
    readonly url: string;
}

/**
 * Parse a manifest's `content.entry` into a mountable panel entry. `null` — never a throw, never a
 * "best effort" URL — for anything it cannot fully validate.
 *
 * TOTAL AND FAIL-CLOSED, the same contract every parser in `panels.ts` follows, and here it is the
 * security boundary itself: the ONLY way a string reaches an `<iframe src>` is by coming back out of
 * this function.
 *
 * HAND-PARSED, NOT `new URL(...)`, deliberately. For a non-special scheme the WHATWG parser keeps
 * the host OPAQUE — it does not lower-case it, and it percent-encodes rather than rejects — so
 * `new URL("context-ext://PKG/x").host` is `"PKG"`, which this grammar must refuse but a URL-object
 * round trip would happily hand on. Taking the authority textually (up to the first `/`, `?` or `#`)
 * is also what makes `context-ext://a@b/x` and `context-ext://pkg:9/x` fail the id grammar instead
 * of resolving against some other host, and it mirrors `ExtAssetResolver::resolve` byte for byte.
 *
 * THE RETURNED URL IS THE INPUT, UNMODIFIED. Normalizing it here would mean the string editor-core
 * validated and the string the browser requests are two different values produced by two different
 * pieces of code — precisely the gap a validator must not open.
 */
export function parseExtPanelEntry(entry: string): ExtPanelEntry | null {
    if (!entry.startsWith(EXT_URL_PREFIX)) {
        // Every other scheme, including the dangerous ones a manifest could name: `javascript:`,
        // `data:`, `blob:`, `file:`, `https:`. See the file header on why this refusal is the only
        // control at this layer rather than defence in depth.
        return null;
    }
    // No whitespace, no control characters, no backslash ANYWHERE in the entry. None can appear in
    // a legitimate asset URL, all three are load-bearing in some parser's disagreement with another
    // (a leading/trailing space is stripped by the URL parser but not by a string comparison; a
    // backslash is a path separator on Windows and a slash to the URL parser), and refusing the
    // whole string is cheaper to reason about than deciding which position is safe.
    for (const ch of entry) {
        const code = ch.codePointAt(0) ?? 0;
        if (code <= 0x20 || code === 0x7f || ch === "\\") {
            return null;
        }
    }

    const rest = entry.slice(EXT_URL_PREFIX.length);
    const authorityEnd = firstIndexOfAny(rest, "/?#");
    const authority = authorityEnd === -1 ? rest : rest.slice(0, authorityEnd);
    if (!isValidPackageId(authority)) {
        // Covers the empty authority (`context-ext:///x`) too — `isValidPackageId("")` is false.
        return null;
    }

    // The path, with the query and fragment cut first exactly as the resolver does. A `..` that
    // survives here would be refused Shell-side anyway; refusing it now means editor-core never
    // creates a frame whose very first request is a traversal attempt, so the refusal is visible as
    // "this panel is unavailable" rather than as a blank frame and a 403 in the operator's log.
    const rawPath = authorityEnd === -1 ? "" : rest.slice(authorityEnd);
    const pathEnd = firstIndexOfAny(rawPath, "?#");
    const path = pathEnd === -1 ? rawPath : rawPath.slice(0, pathEnd);
    for (const segment of path.split("/")) {
        if (segment === "..") {
            return null;
        }
    }

    return { packageId: authority, url: entry };
}

/**
 * Assert a rendered frame's `sandbox` list is the reviewed one — used by `panelhost.ts` right after
 * it creates the element, and by the T1 tier against the REAL DOM.
 *
 * Checked on the ELEMENT rather than trusted from the constant, because `sandbox` is the attribute
 * an extension or a later refactor is most likely to widen, and because `DOMTokenList` normalizes:
 * asserting the parsed token set is what a browser will actually enforce, whereas asserting the
 * string is what someone wrote. Returns false if `allow-same-origin` appears by ANY route.
 */
export function isSandboxSafe(frame: HTMLIFrameElement): boolean {
    const tokens = frame.getAttribute("sandbox");
    if (tokens === null) {
        // No attribute at all is the WORST case (a fully-privileged frame), not the safest.
        return false;
    }
    const parsed = tokens.split(/\s+/u).filter((token) => token.length > 0);
    if (parsed.length === 0) {
        return false;
    }
    for (const token of parsed) {
        if (token.toLowerCase() === FORBIDDEN_SANDBOX_TOKEN) {
            return false;
        }
    }
    return parsed.includes("allow-scripts") && parsed.length === 1;
}

// ------------------------------------------------------------------------------------- internals

function isLowerAlnum(ch: string): boolean {
    return (ch >= "a" && ch <= "z") || (ch >= "0" && ch <= "9");
}

/** Does the id's LAST dot-separated label consist entirely of digits? Mirrors `last_label_is_numeric`. */
function lastLabelIsNumeric(id: string): boolean {
    const dot = id.lastIndexOf(".");
    const last = dot === -1 ? id : id.slice(dot + 1);
    if (last.length === 0) {
        return false;
    }
    for (const ch of last) {
        if (ch < "0" || ch > "9") {
            return false;
        }
    }
    return true;
}

/** The first index at which any character of `chars` appears, or -1. Mirrors `find_first_of`. */
function firstIndexOfAny(text: string, chars: string): number {
    for (let i = 0; i < text.length; i += 1) {
        if (chars.includes(text[i] as string)) {
            return i;
        }
    }
    return -1;
}
