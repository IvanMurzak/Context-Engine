// The `context-ext://<package-id>/` resolver + response policy — see ext_scheme.h for the trust
// model, the four deny-by-default gates and why none of this lives inside the CEF resource handler.

#include "context/editor/shell/ext_scheme.h"

#include <algorithm>
#include <system_error>

namespace context::editor::shell
{
namespace
{

bool is_lower_alnum(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

bool is_ascii_digit(unsigned char c)
{
    return c >= '0' && c <= '9';
}

// Does the id's LAST dot-separated label consist entirely of digits? Per the URL Standard's
// "ends in a number" check, such a host is handed to the IPv4 parser instead of being kept as a
// domain — see is_valid_package_id for why that makes the id unusable.
bool last_label_is_numeric(std::string_view id)
{
    const std::size_t dot = id.rfind('.');
    const std::string_view last = dot == std::string_view::npos ? id : id.substr(dot + 1);
    if (last.empty())
    {
        return false;
    }
    for (char ch : last)
    {
        if (!is_ascii_digit(static_cast<unsigned char>(ch)))
        {
            return false;
        }
    }
    return true;
}

} // namespace

// ------------------------------------------------------------------------------------ package ids

bool is_valid_package_id(std::string_view id)
{
    if (id.empty() || id.size() > kExtPackageIdMaxLength)
    {
        return false;
    }
    // First and last must be alphanumeric, which is what refuses ".", "..", ".hidden", "trailing-"
    // and every other shape that reads as a path fragment rather than a name.
    if (!is_lower_alnum(static_cast<unsigned char>(id.front())) ||
        !is_lower_alnum(static_cast<unsigned char>(id.back())))
    {
        return false;
    }
    for (char ch : id)
    {
        const auto c = static_cast<unsigned char>(ch);
        if (is_lower_alnum(c) || c == '-' || c == '.' || c == '_')
        {
            continue;
        }
        // Everything else — upper case (a standard URL's host arrives lower-cased, so an
        // upper-case id could never be matched again), '/', '\\', ':', '@', '%', a control byte,
        // any non-ASCII byte — is refused rather than normalized. A normalizing validator is how
        // two spellings end up naming one package.
        return false;
    }
    // No interior "..", even though an id never becomes a path component: an id that stays safe
    // under that future misuse costs one comparison.
    if (id.find("..") != std::string_view::npos)
    {
        return false;
    }
    // Refused here so an id a request could never name again is a loud mount-time failure rather
    // than a silently unreachable package — see last_label_is_numeric and ext_scheme.h for why such
    // a host does not come back the way it went in.
    return !last_label_is_numeric(id);
}

// ------------------------------------------------------------------------------- the mount table

bool ExtAssetResolver::mount(std::string_view package_id, const std::filesystem::path& root,
                             std::string& reason)
{
    reason.clear();
    if (!is_valid_package_id(package_id))
    {
        reason = "invalid package id";
        return false;
    }
    if (find(package_id) != nullptr)
    {
        // A second mount of one id would silently shadow the first, and "which one wins" is not a
        // question a security boundary should have an answer to.
        reason = "package id is already mounted";
        return false;
    }

    std::error_code ec;
    std::filesystem::path resolved = std::filesystem::weakly_canonical(root, ec);
    if (ec)
    {
        // DENY-BY-DEFAULT REACHES CANONICALIZATION ITSELF. Continuing with the raw `root` here — as
        // an earlier draft did — is the one fallback that silently voids the overlap refusal below:
        // that refusal is a LEXICAL comparison, so it only establishes disjointness when both sides
        // are canonical, and a non-canonical root would let a nested package mount and hand its
        // bytes to its parent. Every later containment check compares against this value too.
        reason = "package root could not be canonicalized";
        return false;
    }
    if (!std::filesystem::is_directory(resolved, ec))
    {
        // Deny-by-default reaches the mount table too: a package that is not installed is not
        // mounted, rather than mounted and 404-ing (which would leave a live origin with no owner).
        reason = "package root is not an existing directory";
        return false;
    }

    for (const ExtPackageMount& existing : mounts_)
    {
        if (path_contains_or_equals(existing.root, resolved) ||
            path_contains_or_equals(resolved, existing.root))
        {
            // NESTED ROOTS DEFEAT PER-ROOT CONTAINMENT (see ext_scheme.h): with B under A,
            // `context-ext://a/b/secret.js` is perfectly contained in A's root and would hand B's
            // bytes to A. Refused here so the disjointness the resolver relies on is established
            // rather than assumed.
            reason = "package root overlaps the root already mounted for '" + existing.id + "'";
            return false;
        }
    }

    mounts_.push_back(ExtPackageMount{std::string(package_id), std::move(resolved)});
    return true;
}

const ExtPackageMount* ExtAssetResolver::find(std::string_view package_id) const
{
    for (const ExtPackageMount& mount : mounts_)
    {
        if (mount.id == package_id)
        {
            return &mount;
        }
    }
    return nullptr;
}

bool ExtAssetResolver::is_mounted(std::string_view package_id) const
{
    return find(package_id) != nullptr;
}

// ------------------------------------------------------------------------------------- resolution

ExtResolution ExtAssetResolver::resolve(std::string_view url) const
{
    ExtResolution result;

    const std::string_view prefix{kExtUrlPrefix};
    if (url.size() < prefix.size() || url.substr(0, prefix.size()) != prefix)
    {
        result.status = AssetStatus::bad_request;
        result.reason = "not a " + std::string(kExtUrlPrefix) + " URL";
        return result;
    }
    const std::string_view rest = url.substr(prefix.size());

    // The AUTHORITY is everything up to the first '/', '?' or '#'. Taking it textually — rather
    // than trusting a URL library to have stripped userinfo or a port — is what makes
    // `context-ext://a@b/x` and `context-ext://pkg:9/x` fail the id grammar below instead of
    // resolving against some other host's mount.
    const std::size_t authority_end = rest.find_first_of("/?#");
    const std::string_view authority = rest.substr(0, authority_end);
    std::string_view path =
        authority_end == std::string_view::npos ? std::string_view{} : rest.substr(authority_end);

    if (authority.empty())
    {
        // `context-ext:///panel.js` — no package is named at all, so this is a malformed URL rather
        // than a refused package.
        result.status = AssetStatus::bad_request;
        result.reason = "no package id in the URL authority";
        return result;
    }
    // Recorded for the diagnostic channel BEFORE validation, so a refusal log names what was asked
    // for. It is never used to build a path.
    result.package_id = std::string(authority);

    if (!is_valid_package_id(authority))
    {
        result.status = AssetStatus::forbidden;
        result.reason = "invalid package id";
        return result;
    }

    const ExtPackageMount* mount = find(authority);
    if (mount == nullptr)
    {
        // NOT_FOUND, and the status matched here is the one that matters: an ABSENT ASSET inside a
        // package that IS mounted (below) also answers 404. Matching an unknown package to an
        // INVALID id instead — as an earlier draft did, with both at 403 — buys nothing, because a
        // caller can evaluate `is_valid_package_id` for itself without asking us; what it leaks is
        // the other half, since `context-ext://<id>/definitely-absent.js` would then answer 404 iff
        // `<id>` were installed. That is a package-enumeration oracle, so the two statuses that
        // must be equal are THIS one and "no such asset". The reason string carries the difference,
        // and it goes to the log, never to the frame.
        result.status = AssetStatus::not_found;
        result.reason = "package is not mounted";
        return result;
    }

    // Strip the query and fragment BEFORE decoding: they are not part of the path, and a '?' or '#'
    // left in would become a literal filename character.
    if (const std::size_t cut = path.find_first_of("?#"); cut != std::string_view::npos)
    {
        path = path.substr(0, cut);
    }

    std::string decoded;
    if (!percent_decode(path, decoded))
    {
        result.status = AssetStatus::bad_request;
        result.reason = "malformed percent-encoding";
        return result;
    }

    std::vector<std::string> segments;
    std::string reason;
    if (!split_safe_path_segments(decoded, segments, reason))
    {
        // FORBIDDEN, not bad_request: the path parsed fine, it just asked for something outside the
        // package. Keeping the two apart is what makes the traversal count in the tests real.
        result.status = AssetStatus::forbidden;
        result.reason = reason;
        return result;
    }

    // THE ONE SYNTHETIC ASSET (M9 e13b-1) — the port bootstrap, whose bytes are OURS rather than a
    // package file's (ext_scheme.h § the panel-port bootstrap).
    //
    // ITS POSITION IN THIS FUNCTION IS THE WHOLE OF ITS SECURITY ARGUMENT. It is reached only after a
    // valid package id, a MOUNTED package, a query/fragment strip, a successful percent-decode and a
    // clean `split_safe_path_segments` — so it is not a package-enumeration oracle (an unmounted
    // package answered 404 four checks up, indistinguishably from an absent asset) and it cannot be
    // used to walk past the path rules (a request that failed any of them never gets here). Placing
    // it EARLIER — before the mount lookup, say, since the bytes do not depend on the package —
    // would answer 200 for every syntactically valid id, which IS that oracle.
    //
    // Nothing below runs for it: there is no file to canonicalize, contain, or stat.
    if (segments.size() == 1 && segments.front() == kExtPortBootstrapAsset)
    {
        result.mime_type = media_type_for_extension(".js");
        if (result.mime_type.empty())
        {
            // Unreachable while `.js` is on the shared allowlist, and deliberately not an assert: if
            // a future edit dropped it, serving a script with no Content-Type under `nosniff` is a
            // silent blank panel, whereas refusing is the same answer every other unlistable media
            // type gets.
            result.status = AssetStatus::forbidden;
            result.reason = "media type not on the asset allowlist";
            return result;
        }
        result.status = AssetStatus::ok;
        result.synthetic = true;
        return result;
    }

    if (segments.empty())
    {
        segments.emplace_back(kExtDefaultDocument);
    }

    std::filesystem::path candidate = mount->root;
    for (const std::string& segment : segments)
    {
        candidate /= segment;
    }

    // Media type FIRST, before the filesystem is touched — the SAME allowlist the first-party
    // bundle is held to (app_scheme.h `asset_media_types`), because an extension may serve nothing
    // editor-core's own asset set could not. A request for an extension that is not on it is
    // refused whether or not the file exists, so the response cannot be used to probe a package for
    // files (`.env`, `.npmrc`, a private key) by their status code.
    result.mime_type = media_type_for_extension(candidate.extension().string());
    if (result.mime_type.empty())
    {
        result.status = AssetStatus::forbidden;
        result.reason = "media type not on the asset allowlist";
        return result;
    }

    // The SECOND, independent containment check. weakly_canonical resolves any `..` that survived
    // and — the case the textual pass genuinely cannot see — follows symlinks, so a link inside one
    // package pointing at another package (or at the user's home directory) is caught here.
    std::error_code ec;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(candidate, ec);
    if (ec)
    {
        result.status = AssetStatus::not_found;
        result.reason = "path could not be canonicalized";
        return result;
    }

    if (!path_contains_or_equals(mount->root, canonical))
    {
        result.status = AssetStatus::forbidden;
        result.reason = "resolved outside the package root";
        return result;
    }

    if (!std::filesystem::is_regular_file(canonical, ec))
    {
        result.status = AssetStatus::not_found;
        result.reason = "no such asset";
        return result;
    }

    result.status = AssetStatus::ok;
    result.file = canonical;
    return result;
}

// --------------------------------------------------------------------------------- response policy

const char* ext_csp_header()
{
    // One string, spelled once — see ext_scheme.h for why each directive is what it is, and in
    // particular why style-src carries NO 'unsafe-inline' here even though the trusted editor-core
    // policy next door does.
    return "default-src 'none'; "
           "script-src 'self'; "
           "style-src 'self'; "
           "img-src 'self' data:; "
           "font-src 'self'; "
           "connect-src 'none'; "
           "frame-src 'none'; "
           // SPELLED EXPLICITLY, because the CSP3 fallback chain does NOT reach `default-src` here:
           // `worker-src` falls back to `child-src` and then to `script-src`, which is `'self'` —
           // so omitting it silently GRANTS a panel Worker/SharedWorker on its own origin. Nothing
           // decided that; a capability an untrusted document gets by fallback is not a reviewed
           // one. `child-src` is spelled too so neither `worker-src` nor `frame-src` can reacquire
           // it through that link if either is ever edited.
           "child-src 'none'; "
           "worker-src 'none'; "
           "object-src 'none'; "
           "base-uri 'none'; "
           "form-action 'none'; "
           // The panel may be framed BY THE EDITOR AND BY NOTHING ELSE. Not 'none' (which would
           // block the frame the panel exists to be) and not '*'.
           //
           // TIGHTENED IN M9 e13a-2 from the scheme-source `context-editor:` to this HOST-source, as
           // ext_scheme.h's e13a-2 obligation required. The scheme form also authorized
           // `context-editor://ipc` (app_scheme.h declares that second host) — breadth nobody asked
           // for. It was left broad in e13a-1 only because no live iframe existed to prove the
           // host-source form does not silently collapse the directive and block panels outright;
           // `editor-cef-smoke-shell-iframe` is that proof, and its assertion is specifically that
           // the panel's OWN SUBRESOURCES were fetched — a frame the directive blocked never parses
           // its document, so it never requests them.
           "frame-ancestors context-editor://app";
}

// A second anonymous-namespace block rather than a jump up to the one at the top of the file: this
// helper is meaningless away from its single caller directly below, and the file's other four
// helpers are likewise anonymous — nothing here belongs on the library's link surface.
namespace
{

bool is_script_media_type(const std::string& mime_type)
{
    // Essence only — the allowlist's script entries are `text/javascript; charset=utf-8`, and the
    // charset is a separate response field (split_media_type). Compared against the ALLOWLIST's own
    // spelling rather than a set of guesses: app_scheme.cpp maps both `.js` and `.mjs` to exactly
    // this essence, so there is one string to agree with, not a family.
    //
    // THE GENERAL RULE IS "FETCHED IN CORS MODE", AND SCRIPTS ARE CURRENTLY ITS ONLY INSTANCE — the
    // narrowness is the decision, not an oversight. A JSON module (`import … with {type:"json"}`)
    // would be the next instance, and `.json` IS in the allowlist; it is deliberately NOT covered,
    // because the header's cost is paid in the other direction: every panel frame shares the opaque
    // origin `null`, so `Access-Control-Allow-Origin: null` on an asset makes it readable by every
    // OTHER package's frame. Scripts pay that price because without it the module graph dies
    // silently (ext_scheme.h's measurement); a data asset does not, and would simply become
    // cross-package-readable for a feature nothing uses yet. Widening this is e13b's call to make
    // WITH the capability model, not a one-line fix here.
    return split_media_type(mime_type).essence == "text/javascript";
}

} // namespace

std::vector<std::pair<std::string, std::string>> ext_response_headers(const std::string& mime_type)
{
    std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Type", mime_type},
        {"Content-Security-Policy", ext_csp_header()},
        // Without nosniff a served asset whose bytes look like HTML can be re-interpreted as a
        // document regardless of the Content-Type the allowlist chose.
        {"X-Content-Type-Options", "nosniff"},
        // NO X-Frame-Options — see ext_scheme.h. The app response's `DENY` would block the framing
        // this scheme exists for.
        //
        // A panel must never leak its URL (which names the package) to anywhere; with
        // connect-src 'none' there is nowhere to leak it to anyway, stated so the posture does not
        // depend on that coincidence.
        {"Referrer-Policy", "no-referrer"},
        // Package assets are replaced wholesale when the package updates; a cached copy across
        // versions would serve a stale panel against a fresh bridge contract.
        {"Cache-Control", "no-store"},
    };
    if (is_script_media_type(mime_type))
    {
        // M9 e13a-2, ADDED FROM A MEASUREMENT, not from the spec — see ext_scheme.h § the CORS
        // header for the full experiment. In one sentence: an ES MODULE is fetched in CORS mode, a
        // `sandbox="allow-scripts"` panel's origin is the opaque `null`, and without this header the
        // module is fetched and then DISCARDED — the document loads, its stylesheet applies, a
        // CLASSIC script even runs, and only the module graph silently dies. `null` is not a
        // permissive wildcard here; it is the literal serialization of the panel's own origin.
        //
        // SCRIPTS ONLY, deliberately, and this is the narrowest form that works (measured: the
        // module graph resolves with the header on `.js`/`.mjs` alone). Stylesheets, images and
        // fonts are fetched no-cors and need nothing, so withholding it from them keeps every
        // non-script asset unreadable cross-origin by the SAME-ORIGIN POLICY rather than only by
        // this scheme's CSP — which matters because every panel frame shares the opaque origin
        // `null`, so a blanket header would make one package's assets CORS-readable by another's
        // frame with the CSP as the only thing left standing.
        headers.emplace_back("Access-Control-Allow-Origin", "null");
    }
    return headers;
}

// ---------------------------------------------------- the e13b-1 panel-port bootstrap (ext_scheme.h)

// A THIRD anonymous block, on the same terms as the second: these three helpers are meaningless away
// from the two functions below them and nothing here belongs on the library's link surface.
namespace
{

bool is_html_media_type(const std::string& mime_type)
{
    // Essence only — the allowlist's entry is `text/html; charset=utf-8` and the charset is a separate
    // response field. Compared against the ALLOWLIST's own spelling, as `is_script_media_type` above
    // is, so there is one string to agree with rather than a family of guesses.
    return split_media_type(mime_type).essence == "text/html";
}

// The HTML spec's ASCII whitespace set — space, tab, LF, FF, CR. NOT `std::isspace`, which is
// locale-dependent and UB on a negative `char`.
bool is_ascii_space(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\f' || ch == '\r';
}

// The byte offset the bootstrap tag is spliced at. See ext_scheme.h for the RULE and why it is
// "the doctype must be FIRST for us to go second" rather than "find the doctype".
std::size_t bootstrap_splice_offset(std::string_view body)
{
    std::size_t at = 0;
    // A UTF-8 BOM, then ASCII whitespace: both may legitimately precede a doctype and neither is
    // content, so skipping them is what keeps a BOM'd or indented document in standards mode. Any
    // OTHER leading byte — a comment, a stray tag, a script — means the doctype is not first, and the
    // fall-through to offset 0 below is the answer that keeps the bootstrap ahead of package code.
    if (body.size() >= 3 && static_cast<unsigned char>(body[0]) == 0xEF &&
        static_cast<unsigned char>(body[1]) == 0xBB && static_cast<unsigned char>(body[2]) == 0xBF)
    {
        at = 3;
    }
    while (at < body.size() && is_ascii_space(body[at]))
    {
        ++at;
    }

    const std::string_view marker = "<!doctype";
    if (body.size() - at < marker.size() || !ascii_iequals(body.substr(at, marker.size()), marker))
    {
        return 0;
    }
    // BOUNDED: the body is third-party, so a `<!doctype` with no terminating '>' must not turn every
    // HTML response into a full-body scan. Giving up splices at 0, which is safe in the direction
    // that matters (the bootstrap still runs first) and costs only standards mode on a document that
    // is malformed anyway.
    const std::size_t limit = std::min(body.size(), at + kExtDoctypeScanLimit);
    for (std::size_t i = at; i < limit; ++i)
    {
        if (body[i] == '>')
        {
            return i + 1;
        }
    }
    return 0;
}

} // namespace

const char* ext_port_bootstrap_script()
{
    // ASSEMBLED FROM THE CONSTANTS, never written out as one literal: the tag, the global name and the
    // target origin each have a second declaration elsewhere (editor-core, and app_scheme.h), and a
    // hand-copied spelling here is how the served bytes come to name a vocabulary nobody listens on.
    //
    // Function-local static: built once, and C++11 makes the initialization thread-safe — which this
    // needs, because the CEF scheme handler calls it on the IO thread.
    static const std::string script =
        std::string("/* Context panel-port bootstrap (M9 e13b-1). Injected by the Shell as the FIRST\n"
                    "   script in every panel document; see ext_scheme.h for why this code, and not\n"
                    "   the package's, is what creates the channel. */\n"
                    "(function () {\n"
                    "    \"use strict\";\n"
                    "    /* Not framed: a panel document opened directly mints nothing. */\n"
                    "    if (window.parent === window) {\n"
                    "        return;\n"
                    "    }\n"
                    "    var channel = new MessageChannel();\n"
                    "    Object.defineProperty(window, \"") +
        kExtPortGlobalName +
        "\", {\n"
        "        value: channel.port1,\n"
        "        writable: false,\n"
        "        configurable: false,\n"
        "        enumerable: true\n"
        "    });\n"
        "    window.parent.postMessage({ ctx: \"" +
        kExtPortHandshakeTag +
        "\" }, \"" +
        kAppOrigin +
        "\", [channel.port2]);\n"
        "})();\n";
    return script.c_str();
}

std::string ext_inject_port_bootstrap(const std::string& mime_type, std::string_view body)
{
    if (!is_html_media_type(mime_type))
    {
        // EVERY other media type passes through byte-identical. Stated as the first branch because
        // "which responses are rewritten at all" is the question a reader of the CEF binding's single
        // unconditional call has to be able to answer here.
        return std::string(body);
    }
    const std::string tag = std::string("<script src=\"/") + kExtPortBootstrapAsset + "\"></script>";
    const std::size_t at = bootstrap_splice_offset(body);
    std::string out;
    out.reserve(body.size() + tag.size());
    out.append(body.substr(0, at));
    out.append(tag);
    out.append(body.substr(at));
    return out;
}

} // namespace context::editor::shell
