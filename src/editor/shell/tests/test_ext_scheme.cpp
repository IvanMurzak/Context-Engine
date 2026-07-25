// ctest `editor-shell-test_ext_scheme` — the `context-ext://<package-id>/` extension-scheme
// resolver + response policy (M9 e13a-1, design 04 §5 / 08 §1-§3).
//
// This resolver decides WHICH BYTES A THIRD-PARTY PANEL MAY READ, and the code on the other side of
// it is untrusted by construction — so this suite is adversarial first and functional second.
// Every deny axis the DoD names has its own block: traversal in each encoding the URL layer
// permits, cross-package reads (including the nested-root shape that per-root containment alone
// does NOT close), unknown packages, absent and syntactically invalid package ids, and media types
// off the allowlist. It runs on all three default `build` legs because ext_scheme.h is CEF-free by
// design.
//
// NON-VACUITY IS ASSERTED, NOT ASSUMED. Each refusal block is paired with a positive: the file a
// traversal reaches for is proven READABLE from its own package's origin, and the nested-read block
// proves the same bytes are servable to their owner. A suite that only ever refuses would pass just
// as happily against a resolver that refuses everything, which is not what is being claimed. The
// gate was additionally verified by PLANTING weakenings (dropping the `..` rejection, dropping the
// mount-table lookup, dropping the overlapping-root refusal, and clearing a pinned scheme option)
// and watching the corresponding block go red before reverting byte-exact.

#include "context/editor/shell/ext_scheme.h"

#include "shell_test.h"

#include <cstddef>
#include <fstream>
#include <string>
#include <system_error>

namespace shell = context::editor::shell;

namespace
{

void write_file(const std::filesystem::path& path, const std::string& content)
{
    std::filesystem::create_directories(path.parent_path());
    // C++ streams, never std::fopen: MSVC /W4 /WX rejects the C stdio family as C4996 and the local
    // GCC gate cannot see it (conventions.md § Coding conventions).
    std::ofstream out(path, std::ios::binary);
    out << content;
}

std::size_t count_occurrences(const std::string& haystack, const char* needle)
{
    std::size_t count = 0;
    const std::string key{needle};
    for (std::size_t at = haystack.find(key); at != std::string::npos;
         at = haystack.find(key, at + 1))
    {
        ++count;
    }
    return count;
}

// ------------------------------------------------------------------------------------ package ids

void test_package_id_grammar()
{
    // The shapes a real npm-published content package produces.
    CHECK(shell::is_valid_package_id("hello-panel"));
    CHECK(shell::is_valid_package_id("a"));
    CHECK(shell::is_valid_package_id("0"));
    CHECK(shell::is_valid_package_id("context.hello-panel_2"));
    CHECK(shell::is_valid_package_id("pkg2"));

    // Empty, and over the length bound.
    CHECK(!shell::is_valid_package_id(""));
    CHECK(shell::is_valid_package_id(std::string(shell::kExtPackageIdMaxLength, 'a')));
    CHECK(!shell::is_valid_package_id(std::string(shell::kExtPackageIdMaxLength + 1, 'a')));

    // UPPER CASE is refused, and this is correctness rather than style: Chromium canonicalizes a
    // standard URL's host to lower case, so an id mounted with a capital could never be matched by
    // a request again — a package that is silently unreachable instead of loudly unmountable.
    CHECK(!shell::is_valid_package_id("Hello-Panel"));
    CHECK(!shell::is_valid_package_id("HELLO"));

    // Every spelling that reads as a path fragment rather than a name.
    CHECK(!shell::is_valid_package_id("."));
    CHECK(!shell::is_valid_package_id(".."));
    CHECK(!shell::is_valid_package_id("..."));
    CHECK(!shell::is_valid_package_id(".hidden"));
    CHECK(!shell::is_valid_package_id("trailing."));
    CHECK(!shell::is_valid_package_id("-leading"));
    CHECK(!shell::is_valid_package_id("trailing-"));
    CHECK(!shell::is_valid_package_id("_leading"));
    CHECK(!shell::is_valid_package_id("a..b"));
    CHECK(!shell::is_valid_package_id("a/b"));
    CHECK(!shell::is_valid_package_id("a\\b"));

    // Authority syntax that is NOT a bare host: userinfo, a port, an npm scope, a percent escape,
    // a wildcard. Each would otherwise be a way to spell one package's id and reach another.
    CHECK(!shell::is_valid_package_id("user@pkg"));
    CHECK(!shell::is_valid_package_id("pkg:9229"));
    CHECK(!shell::is_valid_package_id("@scope/pkg"));
    CHECK(!shell::is_valid_package_id("pkg%2e%2e"));
    CHECK(!shell::is_valid_package_id("*"));
    CHECK(!shell::is_valid_package_id("pkg name"));

    // Control bytes and non-ASCII are refused rather than normalized.
    CHECK(!shell::is_valid_package_id(std::string("pkg\0x", 5)));
    CHECK(!shell::is_valid_package_id("pkg\tx"));
    CHECK(!shell::is_valid_package_id("pkg\xc3\xa9"));
}

// ------------------------------------------------------------------------------- the mount table

void test_mount_table()
{
    const std::filesystem::path root = shelltest::make_temp_project("ctx-ext-scheme", "mount");
    const std::filesystem::path pkg_a = root / "a";
    const std::filesystem::path pkg_b = root / "b";
    const std::filesystem::path nested = pkg_a / "inner";
    write_file(pkg_a / "index.html", "<!DOCTYPE html>");
    write_file(pkg_b / "index.html", "<!DOCTYPE html>");
    write_file(nested / "index.html", "<!DOCTYPE html>");

    shell::ExtAssetResolver resolver;
    std::string reason;

    // A fresh resolver is EMPTY, and an empty resolver is a complete, correct configuration: it
    // refuses everything. That is what lets the Shell register the handler before any install path
    // exists.
    CHECK(resolver.mounts().empty());
    CHECK(!resolver.is_mounted("pkg-a"));
    CHECK(resolver.resolve("context-ext://pkg-a/index.html").status ==
          shell::AssetStatus::forbidden);

    CHECK(resolver.mount("pkg-a", pkg_a, reason));
    CHECK(reason.empty());
    CHECK(resolver.is_mounted("pkg-a"));
    CHECK(resolver.mounts().size() == 1);

    // A syntactically invalid id never enters the table.
    CHECK(!resolver.mount("Pkg-A", pkg_b, reason));
    CHECK(!reason.empty());
    CHECK(!resolver.mount("..", pkg_b, reason));
    CHECK(resolver.mounts().size() == 1);

    // A second mount of the SAME id is refused — which one would win is not a question a security
    // boundary should have an answer to.
    CHECK(!resolver.mount("pkg-a", pkg_b, reason));
    CHECK(shelltest::mentions(reason, "already mounted"));
    CHECK(resolver.mounts().size() == 1);

    // A root that does not exist is not mounted (deny-by-default reaches the mount table too).
    CHECK(!resolver.mount("pkg-missing", root / "no" / "such" / "dir", reason));
    CHECK(!resolver.mounts().empty());
    CHECK(!resolver.is_mounted("pkg-missing"));
    // Nor is a FILE masquerading as a package root.
    CHECK(!resolver.mount("pkg-file", pkg_a / "index.html", reason));
    CHECK(!resolver.is_mounted("pkg-file"));

    // NESTED ROOTS, both directions plus identity. This is the shape per-root containment does NOT
    // close by itself: with `nested` under pkg-a's root, `context-ext://pkg-a/inner/index.html`
    // would be perfectly contained in pkg-a AND would be pkg-nested's bytes.
    CHECK(!resolver.mount("pkg-nested", nested, reason));
    CHECK(shelltest::mentions(reason, "overlaps"));
    CHECK(!resolver.is_mounted("pkg-nested"));
    CHECK(!resolver.mount("pkg-same", pkg_a, reason));
    CHECK(shelltest::mentions(reason, "overlaps"));
    CHECK(!resolver.mount("pkg-parent", root, reason));
    CHECK(shelltest::mentions(reason, "overlaps"));
    CHECK(!resolver.is_mounted("pkg-parent"));

    // A DISJOINT sibling still mounts — the overlap rule refuses nesting, not every second package.
    CHECK(resolver.mount("pkg-b", pkg_b, reason));
    CHECK(resolver.mounts().size() == 2);

    // The stored root is CANONICAL, so every later containment check compares canonical against
    // canonical. A `.`-laden spelling of the same directory is therefore still an overlap.
    CHECK(!resolver.mount("pkg-c", pkg_b / "." , reason));
    CHECK(shelltest::mentions(reason, "overlaps"));

    shelltest::cleanup(root);
}

// ------------------------------------------------------------------------------------- resolution

void test_resolution_happy_paths()
{
    const std::filesystem::path root = shelltest::make_temp_project("ctx-ext-scheme", "happy");
    const std::filesystem::path pkg = root / "hello-panel";
    write_file(pkg / "index.html", "<!DOCTYPE html>");
    write_file(pkg / "panel.js", "export const x = 1;");
    write_file(pkg / "panel.css", ".p {}");
    write_file(pkg / "sub" / "nested.js", "1;");

    shell::ExtAssetResolver resolver;
    std::string reason;
    CHECK(resolver.mount("hello-panel", pkg, reason));

    {
        const shell::ExtResolution r = resolver.resolve("context-ext://hello-panel/panel.js");
        CHECK(r.ok());
        CHECK(r.http_status() == 200);
        CHECK(r.package_id == "hello-panel");
        CHECK(r.mime_type == "text/javascript; charset=utf-8");
        CHECK(r.file.filename() == "panel.js");
        CHECK(r.reason.empty());
    }
    {
        // A bare package host resolves to the default document, with and without the slash.
        CHECK(resolver.resolve("context-ext://hello-panel/").ok());
        CHECK(resolver.resolve("context-ext://hello-panel").ok());
        CHECK(resolver.resolve("context-ext://hello-panel/").file.filename() == "index.html");
    }
    {
        // Sub-directories inside the package are legitimate — containment refuses ESCAPES, not
        // structure. (Without this the traversal blocks below would pass against a resolver that
        // simply refused every path with a slash in it.)
        CHECK(resolver.resolve("context-ext://hello-panel/sub/nested.js").ok());
        CHECK(resolver.resolve("context-ext://hello-panel/panel.js?v=2").ok());
        CHECK(resolver.resolve("context-ext://hello-panel/panel.js#top").ok());
        CHECK(resolver.resolve("context-ext://hello-panel?v=2").ok());
        CHECK(resolver.resolve("context-ext://hello-panel/panel%2Ecss").ok());
        CHECK(resolver.resolve("context-ext://hello-panel/panel.css").mime_type ==
              "text/css; charset=utf-8");
    }

    shelltest::cleanup(root);
}

void test_bad_urls()
{
    const std::filesystem::path root = shelltest::make_temp_project("ctx-ext-scheme", "badurl");
    const std::filesystem::path pkg = root / "pkg";
    write_file(pkg / "index.html", "<!DOCTYPE html>");

    shell::ExtAssetResolver resolver;
    std::string reason;
    CHECK(resolver.mount("pkg", pkg, reason));

    // Not a context-ext:// URL at all => bad_request. Note the app scheme and a `file://` URL are
    // both in here: neither may be served by THIS resolver.
    for (const char* url : {"", "pkg/index.html", "file:///etc/passwd",
                            "https://example.com/panel.js", "context-editor://app/index.html",
                            "context-ext:/pkg/index.html", "context-ext:pkg/index.html",
                            "CONTEXT-EXT://pkg/index.html", "xcontext-ext://pkg/index.html"})
    {
        const shell::ExtResolution r = resolver.resolve(url);
        CHECK(r.status == shell::AssetStatus::bad_request);
        CHECK(r.http_status() == 400);
        CHECK(r.file.empty());
    }

    // An authority that is EMPTY names no package at all — a malformed URL, not a refused package.
    for (const char* url : {"context-ext://", "context-ext:///index.html", "context-ext://?x=1",
                            "context-ext:///"})
    {
        const shell::ExtResolution r = resolver.resolve(url);
        CHECK(r.status == shell::AssetStatus::bad_request);
        CHECK(r.file.empty());
    }

    shelltest::cleanup(root);
}

void test_package_denial_is_indistinguishable()
{
    const std::filesystem::path root = shelltest::make_temp_project("ctx-ext-scheme", "deny");
    const std::filesystem::path pkg = root / "pkg";
    write_file(pkg / "index.html", "<!DOCTYPE html>");

    shell::ExtAssetResolver resolver;
    std::string reason;
    CHECK(resolver.mount("pkg", pkg, reason));

    // A syntactically INVALID package id.
    const shell::ExtResolution invalid = resolver.resolve("context-ext://PKG/index.html");
    CHECK(invalid.status == shell::AssetStatus::forbidden);
    CHECK(invalid.http_status() == 403);
    CHECK(invalid.file.empty());

    // A syntactically valid id that is simply not installed.
    const shell::ExtResolution unknown = resolver.resolve("context-ext://not-installed/index.html");
    CHECK(unknown.status == shell::AssetStatus::forbidden);
    CHECK(unknown.file.empty());

    // THE SAME STATUS for both, and the same status a mounted package returns for a file that
    // exists but is off the media allowlist — so a response cannot be used to enumerate which
    // packages a user has installed. The distinguishing detail lives in `reason`, which goes to the
    // log and never to the frame.
    CHECK(invalid.status == unknown.status);
    CHECK(invalid.reason != unknown.reason);

    // Every other authority shape that is not a bare, valid host.
    for (const char* url : {"context-ext://user@pkg/index.html", "context-ext://pkg:9229/index.html",
                            "context-ext://@scope/pkg/index.html", "context-ext://../index.html",
                            "context-ext://./index.html", "context-ext://pkg%2e%2e/index.html",
                            "context-ext://*/index.html", "context-ext://pkg./index.html"})
    {
        const shell::ExtResolution r = resolver.resolve(url);
        CHECK(r.status == shell::AssetStatus::forbidden);
        CHECK(r.file.empty());
    }

    shelltest::cleanup(root);
}

void test_traversal_refused()
{
    const std::filesystem::path root = shelltest::make_temp_project("ctx-ext-scheme", "traverse");
    const std::filesystem::path pkg = root / "pkg";
    write_file(pkg / "index.html", "<!DOCTYPE html>");
    write_file(pkg / "sub" / "nested.js", "1;");
    // The file a traversal is reaching FOR: a sibling of the package root, outside it.
    write_file(root / "secret.js", "TOKEN");

    shell::ExtAssetResolver resolver;
    std::string reason;
    CHECK(resolver.mount("pkg", pkg, reason));

    for (const char* url : {
             "context-ext://pkg/../secret.js",
             "context-ext://pkg/sub/../../secret.js",
             "context-ext://pkg/%2e%2e/secret.js",
             "context-ext://pkg/%2E%2E/secret.js",
             "context-ext://pkg/..%2fsecret.js",
             "context-ext://pkg/%2e%2e%2fsecret.js",
             "context-ext://pkg/sub/%2e%2e/%2e%2e/secret.js",
             "context-ext://pkg/./index.html",
             // A backslash is a separator on Windows, so this is a traversal a '/'-only split would
             // hand through as ONE innocent segment.
             "context-ext://pkg/..\\secret.js",
             "context-ext://pkg/%5c..%5csecret.js",
             // Drive-qualified: on Windows `root / "C:x"` re-roots onto that drive instead of
             // appending.
             "context-ext://pkg/C:/Windows/System32/drivers/etc/hosts",
             "context-ext://pkg/C:secret.js",
             // Double-encoding must NOT decode twice; one pass leaves the literal text `%2e%2e`,
             // which is a filename, not a traversal — and there is no such file.
             "context-ext://pkg/%252e%252e/secret.js",
         })
    {
        const shell::ExtResolution r = resolver.resolve(url);
        CHECK(!r.ok());
        CHECK(r.file.empty());
    }

    // Malformed percent-encoding is refused outright rather than sanitized.
    CHECK(resolver.resolve("context-ext://pkg/%zz.js").status == shell::AssetStatus::bad_request);
    CHECK(resolver.resolve("context-ext://pkg/a%00.js").status == shell::AssetStatus::bad_request);

    // NON-VACUITY: the very file every traversal above reached for IS readable when it is inside a
    // package that is mounted. So the refusals are about the boundary, not about the bytes being
    // unreachable in principle.
    shell::ExtAssetResolver sibling;
    CHECK(sibling.mount("outer", root, reason));
    CHECK(sibling.resolve("context-ext://outer/secret.js").ok());

    shelltest::cleanup(root);
}

void test_cross_package_refused()
{
    const std::filesystem::path root = shelltest::make_temp_project("ctx-ext-scheme", "cross");
    const std::filesystem::path pkg_a = root / "pkg-a";
    const std::filesystem::path pkg_b = root / "pkg-b";
    write_file(pkg_a / "index.html", "<!DOCTYPE html>");
    write_file(pkg_b / "index.html", "<!DOCTYPE html>");
    write_file(pkg_b / "private.js", "PKG-B SECRET");

    shell::ExtAssetResolver resolver;
    std::string reason;
    CHECK(resolver.mount("pkg-a", pkg_a, reason));
    CHECK(resolver.mount("pkg-b", pkg_b, reason));

    // NON-VACUITY FIRST: pkg-b's own asset IS servable, from pkg-b's own origin. Everything below
    // therefore proves a boundary rather than an absent file.
    CHECK(resolver.resolve("context-ext://pkg-b/private.js").ok());

    // pkg-a may not reach it, in any spelling.
    for (const char* url : {
             "context-ext://pkg-a/../pkg-b/private.js",
             "context-ext://pkg-a/%2e%2e/pkg-b/private.js",
             "context-ext://pkg-a/..%2fpkg-b%2fprivate.js",
             "context-ext://pkg-a/..\\pkg-b\\private.js",
         })
    {
        const shell::ExtResolution r = resolver.resolve(url);
        CHECK(!r.ok());
        CHECK(r.file.empty());
    }

    // A SYMLINK inside pkg-a pointing at pkg-b is the case the textual pass genuinely cannot see;
    // the canonical containment check is what catches it. Creating one needs a privilege the
    // default Windows host lacks (`WinError 1314`), so this asserts only where the link could
    // actually be made — ubuntu and macOS CI — and is inert here rather than failing
    // environmentally.
    std::error_code ec;
    std::filesystem::create_directory_symlink(pkg_b, pkg_a / "link-to-b", ec);
    if (!ec && std::filesystem::is_symlink(pkg_a / "link-to-b", ec))
    {
        const shell::ExtResolution r =
            resolver.resolve("context-ext://pkg-a/link-to-b/private.js");
        CHECK(!r.ok());
        CHECK(r.status == shell::AssetStatus::forbidden);
        CHECK(r.file.empty());
    }

    shelltest::cleanup(root);
}

void test_media_allowlist_and_absent()
{
    const std::filesystem::path root = shelltest::make_temp_project("ctx-ext-scheme", "media");
    const std::filesystem::path pkg = root / "pkg";
    write_file(pkg / "index.html", "<!DOCTYPE html>");
    write_file(pkg / "payload.exe", "MZ");
    write_file(pkg / ".env", "SECRET=1");
    write_file(pkg / "panel.ts", "const x: number = 1;");

    shell::ExtAssetResolver resolver;
    std::string reason;
    CHECK(resolver.mount("pkg", pkg, reason));

    // The SAME deny-by-default allowlist the first-party bundle is held to: an extension may serve
    // nothing editor-core's own asset set could not.
    for (const char* url : {"context-ext://pkg/payload.exe", "context-ext://pkg/.env",
                            "context-ext://pkg/panel.ts", "context-ext://pkg/absent.exe",
                            "context-ext://pkg/absent.ts"})
    {
        const shell::ExtResolution r = resolver.resolve(url);
        CHECK(r.status == shell::AssetStatus::forbidden);
        CHECK(r.file.empty());
    }
    // Present and absent give the SAME status, so the response cannot be used to probe a package
    // for files by their status code.
    CHECK(resolver.resolve("context-ext://pkg/payload.exe").status ==
          resolver.resolve("context-ext://pkg/absent.exe").status);

    // Well-formed, permitted, but absent inside a MOUNTED package => not_found. This is the ONE
    // place a status distinguishes anything, and it only tells a package about its own contents.
    CHECK(resolver.resolve("context-ext://pkg/missing.js").status == shell::AssetStatus::not_found);
    CHECK(resolver.resolve("context-ext://pkg/missing.js").http_status() == 404);
    // A directory is not a regular file.
    CHECK(!resolver.resolve("context-ext://pkg/sub").ok());

    shelltest::cleanup(root);
}

// ---------------------------------------------------------------------- the pinned scheme options

void test_scheme_registration_options()
{
    // The bit VALUES, mirrored CEF-free so the pin below is meaningful on a leg with no CEF.
    // cef_shell.cpp static_asserts each of these against the real CEF_SCHEME_OPTION_* enumerator,
    // so a CEF bump that renumbered them fails the CEF build rather than silently registering the
    // scheme with different semantics.
    CHECK(shell::kSchemeOptionStandard == 1u);
    CHECK(shell::kSchemeOptionLocal == 2u);
    CHECK(shell::kSchemeOptionDisplayIsolated == 4u);
    CHECK(shell::kSchemeOptionSecure == 8u);
    CHECK(shell::kSchemeOptionCorsEnabled == 16u);
    CHECK(shell::kSchemeOptionCspBypassing == 32u);
    CHECK(shell::kSchemeOptionFetchEnabled == 64u);

    // The PINNED set (design 04 §5 / 08 §2): STANDARD|SECURE|CORS_ENABLED, exactly.
    CHECK((shell::kExtSchemeOptions & shell::kSchemeOptionStandard) != 0u);
    CHECK((shell::kExtSchemeOptions & shell::kSchemeOptionSecure) != 0u);
    CHECK((shell::kExtSchemeOptions & shell::kSchemeOptionCorsEnabled) != 0u);

    // The DENY half, which is the half worth asserting: CSP_BYPASSING would void the panel CSP
    // outright, LOCAL would grant file-like privileges to untrusted third-party code,
    // DISPLAY_ISOLATED would stop editor-core framing panels at all, and FETCH_ENABLED is withheld
    // so the no-network posture does not rest on the CSP alone.
    CHECK((shell::kExtSchemeOptions & shell::kSchemeOptionCspBypassing) == 0u);
    CHECK((shell::kExtSchemeOptions & shell::kSchemeOptionLocal) == 0u);
    CHECK((shell::kExtSchemeOptions & shell::kSchemeOptionDisplayIsolated) == 0u);
    CHECK((shell::kExtSchemeOptions & shell::kSchemeOptionFetchEnabled) == 0u);

    // Equality, not just membership — a future edit that ORs a fourth option in trips this.
    CHECK(shell::kExtSchemeOptions == (shell::kSchemeOptionStandard | shell::kSchemeOptionSecure |
                                       shell::kSchemeOptionCorsEnabled));
}

void test_scheme_constants()
{
    CHECK(std::string(shell::kExtScheme) == "context-ext");
    CHECK(std::string(shell::kExtUrlPrefix) == "context-ext://");
    CHECK(std::string(shell::kExtDefaultDocument) == "index.html");
    // NO file:// anywhere in the vocabulary — a package's assets are served over this scheme or not
    // at all, exactly as editor-core's are.
    CHECK(!shelltest::mentions(std::string(shell::kExtUrlPrefix), "file:"));
}

// ------------------------------------------------------------------------------- response policy

void test_ext_csp_and_headers()
{
    const std::string csp = shell::ext_csp_header();

    CHECK(shelltest::mentions(csp, "default-src 'none'"));
    CHECK(shelltest::mentions(csp, "script-src 'self'"));
    CHECK(shelltest::mentions(csp, "connect-src 'none'"));
    CHECK(shelltest::mentions(csp, "object-src 'none'"));
    CHECK(shelltest::mentions(csp, "base-uri 'none'"));
    CHECK(shelltest::mentions(csp, "form-action 'none'"));
    // A panel may not nest further frames.
    CHECK(shelltest::mentions(csp, "frame-src 'none'"));

    // STRICTER than the trusted editor-core policy: NO inline anything, on ANY directive. The
    // dockview `'unsafe-inline'` carve-out is editor-core's, and untrusted code does not inherit it.
    CHECK(shelltest::mentions(csp, "style-src 'self'"));
    CHECK(count_occurrences(csp, "unsafe-inline") == 0);
    CHECK(!shelltest::mentions(csp, "unsafe-eval"));

    // A panel may load NOTHING from another package: no `context-ext:` scheme-source on any fetch
    // directive, which would be a cross-package escalation dressed up as a subresource load.
    CHECK(!shelltest::mentions(csp, "script-src 'self' context-ext:"));
    CHECK(count_occurrences(csp, "context-ext:") == 0);

    // FRAMED BY THE EDITOR AND NOTHING ELSE. Emphatically not 'none' (which would block the frame
    // the panel exists to be) and not '*'.
    CHECK(shelltest::mentions(csp, "frame-ancestors context-editor:"));
    CHECK(!shelltest::mentions(csp, "frame-ancestors 'none'"));

    // No external hosts anywhere (08 §2 "strict CSP, no external hosts").
    CHECK(!shelltest::mentions(csp, "http://"));
    CHECK(!shelltest::mentions(csp, "https://"));
    CHECK(!shelltest::mentions(csp, "*"));
    // `data:` is permitted for IMAGES only — inert bytes an icon in a bundled stylesheet needs.
    CHECK(shelltest::mentions(csp, "img-src 'self' data:"));
    CHECK(count_occurrences(csp, "data:") == 1);

    const auto headers = shell::ext_response_headers("text/html; charset=utf-8");
    bool has_type = false;
    bool has_csp = false;
    bool has_nosniff = false;
    bool has_referrer = false;
    bool has_cache = false;
    bool has_frame_options = false;
    for (const auto& [name, value] : headers)
    {
        if (name == "Content-Type")
        {
            has_type = value == "text/html; charset=utf-8";
        }
        else if (name == "Content-Security-Policy")
        {
            has_csp = value == csp;
        }
        else if (name == "X-Content-Type-Options")
        {
            has_nosniff = value == "nosniff";
        }
        else if (name == "Referrer-Policy")
        {
            has_referrer = value == "no-referrer";
        }
        else if (name == "Cache-Control")
        {
            has_cache = value == "no-store";
        }
        else if (name == "X-Frame-Options")
        {
            has_frame_options = true;
        }
    }
    CHECK(has_type);
    CHECK(has_csp);
    CHECK(has_nosniff);
    CHECK(has_referrer);
    CHECK(has_cache);
    // THE ONE HEADER THAT MUST BE ABSENT. The app response sends `X-Frame-Options: DENY` because
    // the editor window is never framed; a panel exists precisely to BE framed, and any consumer
    // that honours the legacy header ahead of frame-ancestors would block it outright.
    CHECK(!has_frame_options);
}

// The e13a-1 widening of the EDITOR-CORE policy, asserted from this suite as well as its own: the
// two sides of the seam must agree, and reading `frame-src context-ext:` next to
// `frame-ancestors context-editor:` is what makes that agreement legible.
void test_app_policy_permits_framing_this_scheme()
{
    const std::string app_csp = shell::app_csp_header();
    CHECK(shelltest::mentions(app_csp, "frame-src context-ext:"));
    CHECK(!shelltest::mentions(app_csp, "frame-src 'none'"));
    // And nothing ELSE was relaxed alongside it.
    CHECK(shelltest::mentions(app_csp, "object-src 'none'"));
    CHECK(shelltest::mentions(app_csp, "connect-src 'none'"));
    CHECK(shelltest::mentions(app_csp, "frame-ancestors 'none'"));
    CHECK(!shelltest::mentions(app_csp, "http://"));
    CHECK(!shelltest::mentions(app_csp, "https://"));
    CHECK(!shelltest::mentions(app_csp, "*"));
}

void test_http_status_mapping()
{
    CHECK(shell::http_status_for(shell::AssetStatus::ok) == 200);
    CHECK(shell::http_status_for(shell::AssetStatus::bad_request) == 400);
    CHECK(shell::http_status_for(shell::AssetStatus::forbidden) == 403);
    CHECK(shell::http_status_for(shell::AssetStatus::not_found) == 404);
}

} // namespace

int main()
{
    test_package_id_grammar();
    test_mount_table();
    test_resolution_happy_paths();
    test_bad_urls();
    test_package_denial_is_indistinguishable();
    test_traversal_refused();
    test_cross_package_refused();
    test_media_allowlist_and_absent();
    test_scheme_registration_options();
    test_scheme_constants();
    test_ext_csp_and_headers();
    test_app_policy_permits_framing_this_scheme();
    test_http_status_mapping();
    SHELL_TEST_MAIN_END();
}
