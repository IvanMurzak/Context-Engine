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
// NON-VACUITY IS ASSERTED, NOT ASSUMED, AND IT TAKES TWO SEPARATE DISCIPLINES:
//
//   (1) EVERY REFUSAL BLOCK IS PAIRED WITH A POSITIVE. The file a traversal reaches for is proven
//       READABLE from its own package's origin; the cross-package block proves the same bytes are
//       servable to their owner; the symlink case proves a link that stays INSIDE the root still
//       resolves. A suite that only ever refuses would pass just as happily against a resolver that
//       refuses everything, which is not what is being claimed.
//
//   (2) EVERY ADVERSARIAL CASE PINS THE GATE THAT MUST FIRE — its STATUS and its REASON — never a
//       bare `!ok()`. This is the discipline the four overlapping deny gates make mandatory, and it
//       was added after a review found the earlier `!ok()`-only form could not detect the deletion
//       of the textual traversal rejection AT ALL: with the `..` rule gone,
//       `context-ext://pkg/../secret.js` is still refused one layer later by canonical containment,
//       so the block stayed green while the gate it names no longer existed. `!ok()` cannot tell a
//       held boundary from a differently-held one, and on a security boundary that distinction is
//       the entire assertion.
//
// The gates were additionally verified by PLANTING weakenings (dropping the `..` rejection, the
// colon rejection, the mount-table lookup, the overlapping-root refusal, the mount-time
// canonicalization refusal, and a pinned scheme option) and watching the corresponding block go red
// before reverting byte-exact.

#include "context/editor/shell/ext_scheme.h"

#include "shell_test.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace shell = context::editor::shell;

namespace
{

using shelltest::write_file;

// ------------------------------------------------------------------------------------ package ids

void test_package_id_grammar()
{
    // The shapes a real npm-published content package produces.
    CHECK(shell::is_valid_package_id("hello-panel"));
    CHECK(shell::is_valid_package_id("a"));
    CHECK(shell::is_valid_package_id("context.hello-panel_2"));
    CHECK(shell::is_valid_package_id("pkg2"));

    // Empty, and over the length bound.
    CHECK(!shell::is_valid_package_id(""));
    CHECK(shell::is_valid_package_id(std::string(shell::kExtPackageIdMaxLength, 'a')));
    CHECK(!shell::is_valid_package_id(std::string(shell::kExtPackageIdMaxLength + 1, 'a')));

    // AN ALL-DIGIT LAST LABEL is refused for the same CORRECTNESS reason upper case is, not for
    // tidiness: the URL Standard's "ends in a number" rule hands such a host to the IPv4 parser, so
    // `12345` would arrive back from Chromium as `0.0.48.57` and `pkg.2` would fail host parsing —
    // either way the id that comes back is not the id that was mounted, and the package would be
    // permanently unreachable behind a 404 nobody could explain.
    CHECK(!shell::is_valid_package_id("0"));
    CHECK(!shell::is_valid_package_id("12345"));
    CHECK(!shell::is_valid_package_id("1.2.3.4"));
    CHECK(!shell::is_valid_package_id("pkg.2"));
    // …but a digit INSIDE a label is fine — the rule is about a label that is ENTIRELY digits.
    CHECK(shell::is_valid_package_id("pkg.2a"));
    CHECK(shell::is_valid_package_id("a2"));

    // UPPER CASE is refused, and this is correctness rather than style: Chromium canonicalizes a
    // standard URL's host to lower case, so an id mounted with a capital could never be matched by
    // a request again — a package that is silently unreachable instead of loudly unmountable.
    CHECK(!shell::is_valid_package_id("Hello-Panel"));
    CHECK(!shell::is_valid_package_id("HELLO"));
    // INTERIOR position is a DISTINCT axis from a leading one, and these two rows exist because both
    // rows above are refused by the FIRST/LAST-character check alone — they exercise the character
    // LOOP not at all, so a loop that stopped refusing upper case would leave every assertion in
    // this function green. The TS mirror carries the same row (`paCkage`, extpanel.test.ts) for
    // exactly this reason, added after that plant passed.
    CHECK(!shell::is_valid_package_id("paCkage"));
    CHECK(!shell::is_valid_package_id("hello-Panel"));

    // Every spelling that reads as a path fragment rather than a name.
    CHECK(!shell::is_valid_package_id("."));
    CHECK(!shell::is_valid_package_id(".."));
    CHECK(!shell::is_valid_package_id("..."));
    CHECK(!shell::is_valid_package_id(".hidden"));
    CHECK(!shell::is_valid_package_id("trailing."));
    CHECK(!shell::is_valid_package_id("-leading"));
    CHECK(!shell::is_valid_package_id("trailing-"));
    CHECK(!shell::is_valid_package_id("_leading"));
    CHECK(!shell::is_valid_package_id("trailing_"));
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

    // Control bytes and non-ASCII are refused rather than normalized. The first two land INSIDE the
    // id so they reach the character loop; the third is trailing, so it is the first/last check that
    // refuses it — `pkg\xc3\xa9x` is the interior twin that reaches the loop, added for the same
    // reason as the interior upper-case rows above.
    CHECK(!shell::is_valid_package_id(std::string("pkg\0x", 5)));
    CHECK(!shell::is_valid_package_id("pkg\tx"));
    CHECK(!shell::is_valid_package_id("pkg\xc3\xa9"));
    CHECK(!shell::is_valid_package_id("pkg\xc3\xa9x"));
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
    {
        // NOT_FOUND, matching an absent asset inside a mounted package — see
        // test_package_denial_is_indistinguishable for why that is the pairing that closes the
        // enumeration oracle.
        const shell::ExtResolution r = resolver.resolve("context-ext://pkg-a/index.html");
        CHECK(r.status == shell::AssetStatus::not_found);
        CHECK(shelltest::mentions(r.reason, "not mounted"));
    }

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
        CHECK(shelltest::mentions(r.reason, "not a context-ext:// URL"));
        CHECK(r.file.empty());
    }

    // An authority that is EMPTY names no package at all — a malformed URL, not a refused package.
    for (const char* url : {"context-ext://", "context-ext:///index.html", "context-ext://?x=1",
                            "context-ext:///"})
    {
        const shell::ExtResolution r = resolver.resolve(url);
        CHECK(r.status == shell::AssetStatus::bad_request);
        CHECK(shelltest::mentions(r.reason, "no package id in the URL authority"));
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

    // A syntactically INVALID package id => forbidden. Nothing is leaked by that: a caller can
    // evaluate `is_valid_package_id` for itself without asking us.
    const shell::ExtResolution invalid = resolver.resolve("context-ext://PKG/index.html");
    CHECK(invalid.status == shell::AssetStatus::forbidden);
    CHECK(invalid.http_status() == 403);
    CHECK(shelltest::mentions(invalid.reason, "invalid package id"));
    CHECK(invalid.file.empty());

    // A syntactically valid id that is simply not installed => NOT_FOUND, and THIS is the pairing
    // that matters. Matching it to the invalid id instead would leave the real oracle open: an
    // absent asset inside a MOUNTED package answers 404, so if an unknown package answered 403 then
    // `context-ext://<id>/definitely-absent.js` would answer 404 iff `<id>` were installed — a
    // package-enumeration probe. The two statuses asserted equal below are the two that must be.
    const shell::ExtResolution unknown = resolver.resolve("context-ext://not-installed/index.html");
    CHECK(unknown.status == shell::AssetStatus::not_found);
    CHECK(unknown.http_status() == 404);
    CHECK(shelltest::mentions(unknown.reason, "not mounted"));
    CHECK(unknown.file.empty());

    const shell::ExtResolution absent_in_mounted = resolver.resolve("context-ext://pkg/absent.js");
    CHECK(absent_in_mounted.status == unknown.status);
    // The distinguishing detail lives in `reason`, which goes to the log and never to the frame.
    CHECK(absent_in_mounted.reason != unknown.reason);

    // Every other authority shape that is not a bare, valid host. These are GRAMMAR refusals, so
    // they are forbidden rather than not_found — an id that cannot be spelled is not a package that
    // might or might not be installed.
    for (const char* url : {"context-ext://user@pkg/index.html",
                            "context-ext://pkg:9229/index.html",
                            "context-ext://@scope/pkg/index.html", "context-ext://../index.html",
                            "context-ext://./index.html", "context-ext://pkg%2e%2e/index.html",
                            "context-ext://*/index.html", "context-ext://pkg./index.html",
                            "context-ext://12345/index.html"})
    {
        const shell::ExtResolution r = resolver.resolve(url);
        CHECK(r.status == shell::AssetStatus::forbidden);
        CHECK(shelltest::mentions(r.reason, "invalid package id"));
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

    // EACH CASE PINS THE GATE THAT MUST FIRE — status AND reason — not merely `!ok()`.
    //
    // WHY THAT MATTERS, and it is the difference between an adversarial suite and a decorative one:
    // the four deny gates overlap, so a bare `!ok()` block stays GREEN with the textual traversal
    // rejection DELETED. Drop the `..` rule and `context-ext://pkg/../secret.js` still refuses —
    // the canonical containment check catches it one layer later and answers `forbidden` all the
    // same. The suite would report a passing gate that no longer exists. Pinning the reason is what
    // makes each layer's own removal visible, and it is why the non-vacuity claim in this file's
    // header is checkable rather than decorative.
    struct TraversalCase
    {
        const char* url;
        shell::AssetStatus status;
        const char* reason;
    };
    for (const TraversalCase& c : {
             TraversalCase{"context-ext://pkg/../secret.js", shell::AssetStatus::forbidden,
                           "'..' traversal segment"},
             TraversalCase{"context-ext://pkg/sub/../../secret.js", shell::AssetStatus::forbidden,
                           "'..' traversal segment"},
             TraversalCase{"context-ext://pkg/%2e%2e/secret.js", shell::AssetStatus::forbidden,
                           "'..' traversal segment"},
             TraversalCase{"context-ext://pkg/%2E%2E/secret.js", shell::AssetStatus::forbidden,
                           "'..' traversal segment"},
             TraversalCase{"context-ext://pkg/..%2fsecret.js", shell::AssetStatus::forbidden,
                           "'..' traversal segment"},
             TraversalCase{"context-ext://pkg/%2e%2e%2fsecret.js", shell::AssetStatus::forbidden,
                           "'..' traversal segment"},
             TraversalCase{"context-ext://pkg/sub/%2e%2e/%2e%2e/secret.js",
                           shell::AssetStatus::forbidden, "'..' traversal segment"},
             TraversalCase{"context-ext://pkg/./index.html", shell::AssetStatus::forbidden,
                           "'.' segment"},
             // A backslash is a separator on Windows, so this is a traversal a '/'-only split would
             // hand through as ONE innocent segment.
             TraversalCase{"context-ext://pkg/..\\secret.js", shell::AssetStatus::forbidden,
                           "backslash in path"},
             TraversalCase{"context-ext://pkg/%5c..%5csecret.js", shell::AssetStatus::forbidden,
                           "backslash in path"},
             // Drive-qualified: on Windows `root / "C:x"` re-roots onto that drive instead of
             // appending.
             TraversalCase{"context-ext://pkg/C:/Windows/System32/drivers/etc/hosts",
                           shell::AssetStatus::forbidden, "colon in path segment"},
             TraversalCase{"context-ext://pkg/C:secret.js", shell::AssetStatus::forbidden,
                           "colon in path segment"},
             // Double-encoding must NOT decode twice. ONE pass leaves the LITERAL text `%2e%2e`,
             // which is a filename and not a traversal — so this lands INSIDE the package and 404s.
             // Pinned to not_found precisely so a decoder that LOOPED until stable (turning it into
             // a real `..`) would flip the status to forbidden and go RED. `!ok()` cannot tell the
             // two apart, which is the whole point of pinning.
             TraversalCase{"context-ext://pkg/%252e%252e/secret.js", shell::AssetStatus::not_found,
                           "no such asset"},
         })
    {
        const shell::ExtResolution r = resolver.resolve(c.url);
        CHECK(r.status == c.status);
        CHECK(shelltest::mentions(r.reason, c.reason));
        CHECK(r.file.empty());
    }

    // Malformed percent-encoding is refused outright rather than sanitized.
    for (const char* url : {"context-ext://pkg/%zz.js", "context-ext://pkg/a%00.js"})
    {
        const shell::ExtResolution r = resolver.resolve(url);
        CHECK(r.status == shell::AssetStatus::bad_request);
        CHECK(shelltest::mentions(r.reason, "malformed percent-encoding"));
        CHECK(r.file.empty());
    }

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

    // pkg-a may not reach it, in any spelling. These are all TEXTUAL refusals — pinned by reason so
    // the layer that actually fires is named (see test_traversal_refused for why).
    for (const char* url : {
             "context-ext://pkg-a/../pkg-b/private.js",
             "context-ext://pkg-a/%2e%2e/pkg-b/private.js",
             "context-ext://pkg-a/..%2fpkg-b%2fprivate.js",
         })
    {
        const shell::ExtResolution r = resolver.resolve(url);
        CHECK(r.status == shell::AssetStatus::forbidden);
        CHECK(shelltest::mentions(r.reason, "'..' traversal segment"));
        CHECK(r.file.empty());
    }
    {
        const shell::ExtResolution r =
            resolver.resolve("context-ext://pkg-a/..\\pkg-b\\private.js");
        CHECK(r.status == shell::AssetStatus::forbidden);
        CHECK(shelltest::mentions(r.reason, "backslash in path"));
        CHECK(r.file.empty());
    }

    // A SYMLINK inside pkg-a pointing at pkg-b is the case the textual pass genuinely cannot see —
    // and it is the ONLY input in this entire suite that reaches the canonical containment check,
    // because the textual pass is total: no spelling of `..` survives it. So this block is that
    // gate's sole coverage, and a silent skip here means the gate ran nowhere.
    //
    // Creating a symlink needs a privilege the default Windows host lacks (`WinError 1314`), so the
    // assertion is conditional — but the SKIP is now LOUD on Windows and IMPOSSIBLE on POSIX, where
    // the link always creates. A security case that quietly stops running is worse than one that
    // was never written.
    //
    // MEASURED PER-LEG COVERAGE, stated because "it is tested" would be false: planting `if
    // (false)` over the containment call in ext_scheme.cpp leaves this suite GREEN on the windows
    // leg and
    // turns it RED on ubuntu/macOS. So the containment CALL SITE is covered end-to-end on two of
    // the three `build` legs, and on windows only `test_path_contains_or_equals_direct` (the
    // predicate itself) covers it. A junction would reach the call site without any privilege, but
    // it is deliberately NOT used here: MinGW's `weakly_canonical` does not resolve one (measured),
    // so such a test would assert opposite outcomes on the local gate and on the MSVC CI leg. See
    // ext_scheme.h's note on what check (4) rests on.
    std::error_code ec;
    std::filesystem::create_directory_symlink(pkg_b, pkg_a / "link-to-b", ec);
#ifndef _WIN32
    CHECK(!ec);
#endif
    if (!ec && std::filesystem::is_symlink(pkg_a / "link-to-b", ec))
    {
        const shell::ExtResolution r =
            resolver.resolve("context-ext://pkg-a/link-to-b/private.js");
        CHECK(r.status == shell::AssetStatus::forbidden);
        CHECK(shelltest::mentions(r.reason, "resolved outside the package root"));
        CHECK(r.file.empty());

        // NON-VACUITY FOR THIS GATE SPECIFICALLY: a symlink that stays INSIDE pkg-a still resolves,
        // so the refusal above is about LEAVING THE ROOT and not about symlinks per se. Without
        // this, a resolver that simply refused every symlink would pass the block above.
        std::error_code inner_ec;
        write_file(pkg_a / "own" / "inside.js", "OWN");
        std::filesystem::create_directory_symlink(pkg_a / "own", pkg_a / "link-to-own", inner_ec);
        if (!inner_ec && std::filesystem::is_symlink(pkg_a / "link-to-own", inner_ec))
        {
            CHECK(resolver.resolve("context-ext://pkg-a/link-to-own/inside.js").ok());
        }
    }
    else
    {
        // NEVER SILENT — the ctest log must say the gate did not run on this leg.
        std::fprintf(stderr,
                     "[test_ext_scheme] SKIPPED the symlink containment case (%s) — the canonical "
                     "containment gate is UNCOVERED on this leg\n",
                     ec.message().c_str());
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
        CHECK(shelltest::mentions(r.reason, "media type not on the asset allowlist"));
        CHECK(r.file.empty());
    }
    // Note the list above deliberately pairs PRESENT files with ABSENT ones (`payload.exe` vs
    // `absent.exe`): both refuse identically, and they refuse at the ALLOWLIST — before the
    // filesystem is touched — so the response cannot be used to probe a package for files by
    // their status code.

    // Well-formed, permitted, but absent inside a MOUNTED package => not_found — the same status an
    // unmounted package returns, which is what keeps the mount table unobservable from outside.
    const shell::ExtResolution missing = resolver.resolve("context-ext://pkg/missing.js");
    CHECK(missing.status == shell::AssetStatus::not_found);
    CHECK(missing.http_status() == 404);

    // A DIRECTORY is not a regular file — and reaching that branch at all takes a directory whose
    // name carries an ALLOWLISTED extension. A bare `pkg/sub` has an EMPTY extension, so it is
    // refused by the media allowlist several steps EARLIER and proves nothing about directories;
    // asserting on it (as an earlier draft did, against a directory the fixture never even created)
    // is a case that passes for two independent wrong reasons at once. Both spellings are pinned
    // here so the two gates cannot silently swap places.
    std::filesystem::create_directories(pkg / "bundle.js");
    const shell::ExtResolution dir = resolver.resolve("context-ext://pkg/bundle.js");
    CHECK(dir.status == shell::AssetStatus::not_found);
    CHECK(shelltest::mentions(dir.reason, "no such asset"));
    CHECK(dir.file.empty());

    const shell::ExtResolution bare = resolver.resolve("context-ext://pkg/sub");
    CHECK(bare.status == shell::AssetStatus::forbidden);
    CHECK(shelltest::mentions(bare.reason, "media type not on the asset allowlist"));

    shelltest::cleanup(root);
}

// An NTFS ALTERNATE DATA STREAM is a byte stream hanging off a base file — `dir` does not list it,
// `directory_iterator` does not enumerate it, and a manifest hash taken over a package's enumerated
// files does not cover it. So a package can be listed, hashed, signed and human-reviewed and STILL
// carry `panel.js:evil`.
//
// It reaches this resolver as an ordinary path segment, and the media allowlist does NOT stop it:
// MSVC's `path::extension()` reports `.js` for `panel.js:evil` (the SHIPPING Windows toolchain —
// measured), so the allowlist is satisfied by the BASE name while the bytes served are the
// unreviewed stream. libstdc++ splits it the other way and serves `hidden.env:x.js` instead. The
// two STLs disagree in OPPOSITE directions, which is exactly why the colon is refused textually,
// before the filesystem is touched, rather than left to whatever `extension()` happens to do.
void test_alternate_data_stream_refused()
{
    const std::filesystem::path root = shelltest::make_temp_project("ctx-ext-scheme", "ads");
    const std::filesystem::path pkg = root / "pkg";
    write_file(pkg / "index.html", "<!DOCTYPE html>");
    write_file(pkg / "panel.js", "export const x = 1;");

    shell::ExtAssetResolver resolver;
    std::string reason;
    CHECK(resolver.mount("pkg", pkg, reason));

    // NON-VACUITY: the base file IS servable, so the refusals below are about the STREAM spelling.
    CHECK(resolver.resolve("context-ext://pkg/panel.js").ok());

    for (const char* url : {
             "context-ext://pkg/panel.js:evil",          // MSVC: extension() == ".js"
             "context-ext://pkg/hidden.env:x.js",        // libstdc++: extension() == ".js"
             "context-ext://pkg/panel.js::$DATA",        // the canonical ADS spelling of the file
             "context-ext://pkg/sub:s.html",             // a DIRECTORY's stream
             "context-ext://pkg/panel.js:$INDEX_ALLOCATION",
             "context-ext://pkg/panel%2Ejs%3Aevil",      // and percent-encoded
         })
    {
        const shell::ExtResolution r = resolver.resolve(url);
        CHECK(r.status == shell::AssetStatus::forbidden);
        CHECK(shelltest::mentions(r.reason, "colon in path segment"));
        CHECK(r.file.empty());
    }

    shelltest::cleanup(root);
}

// The shared containment chain's SECOND link, attacked DIRECTLY. Both schemes walk it, and its
// header says both suites attack it — which was not true until this test existed: every earlier
// case reached it only THROUGH `resolve()`, which cannot reach two of its branches at all.
void test_split_safe_path_segments_direct()
{
    std::vector<std::string> out;
    std::string reason;

    CHECK(shell::split_safe_path_segments("a/b/c.js", out, reason));
    CHECK(out.size() == 3);

    // Empty segments are SKIPPED, not refused: they cannot escape, and refusing them would refuse
    // the legitimate bare root. Asserted so a future "tidy-up" cannot quietly turn a leading '/'
    // into a filesystem root instead.
    CHECK(shell::split_safe_path_segments("//a//b//", out, reason));
    CHECK(out.size() == 2);
    CHECK(out[0] == "a" && out[1] == "b");
    CHECK(shell::split_safe_path_segments("", out, reason));
    CHECK(out.empty());

    // NOTHING is cleaned up: `....//` stays a literal name and does not collapse to `../`.
    CHECK(shell::split_safe_path_segments("....//x.js", out, reason));
    CHECK(out.size() == 2);
    CHECK(out[0] == "....");

    CHECK(!shell::split_safe_path_segments("..", out, reason));
    CHECK(shelltest::mentions(reason, "'..' traversal segment"));
    CHECK(!shell::split_safe_path_segments(".", out, reason));
    CHECK(shelltest::mentions(reason, "'.' segment"));
    CHECK(!shell::split_safe_path_segments("a\\b", out, reason));
    CHECK(shelltest::mentions(reason, "backslash in path"));
    CHECK(!shell::split_safe_path_segments("C:x", out, reason));
    CHECK(shelltest::mentions(reason, "colon in path segment"));
    CHECK(!shell::split_safe_path_segments("panel.js:evil", out, reason));
    CHECK(shelltest::mentions(reason, "colon in path segment"));

    // The control-character branch that `resolve()` can NEVER reach — `percent_decode` refuses
    // control bytes first — but this is public surface and a future caller may decode differently.
    CHECK(!shell::split_safe_path_segments(std::string("a\x01/b", 4), out, reason));
    CHECK(shelltest::mentions(reason, "control character in path"));
}

// The chain's LAST link, attacked directly for a sharper reason than the one above: end-to-end it
// is reachable ONLY through a filesystem link (the textual pass is total, so no spelling of `..`
// survives to reach it), and creating one needs a privilege the windows leg lacks. So on that leg
// this test is the containment predicate's ONLY coverage — see test_cross_package_refused.
void test_path_contains_or_equals_direct()
{
    const std::filesystem::path root = shelltest::make_temp_project("ctx-ext-scheme", "contain");
    const std::filesystem::path pkg = root / "pkg";
    write_file(pkg / "index.html", "<!DOCTYPE html>");

    // Inside, and the boundary case: a path IS contained in itself ("or_equals").
    CHECK(shell::path_contains_or_equals(pkg, pkg));
    CHECK(shell::path_contains_or_equals(pkg, pkg / "index.html"));
    CHECK(shell::path_contains_or_equals(pkg, pkg / "a" / "b" / "c.js"));

    // Outside, in every shape that matters.
    CHECK(!shell::path_contains_or_equals(pkg, root));
    CHECK(!shell::path_contains_or_equals(pkg, root / "secret.js"));
    CHECK(!shell::path_contains_or_equals(pkg, root / "pkg-sibling" / "x.js"));
    // A SIBLING WHOSE NAME IS A PREFIX of the root's — the classic off-by-one a naive
    // `starts_with(root_string)` containment check gets wrong. `lexically_relative` is
    // per-COMPONENT, so this is refused; a string-prefix implementation would accept it.
    CHECK(!shell::path_contains_or_equals(pkg, std::filesystem::path(pkg.string() + "-evil") /
                                                   "x.js"));
    // Different root names (a different drive, or drive-vs-UNC) yield an EMPTY relative path, which
    // must read as NOT contained — the predicate has to fail CLOSED on a shape it cannot compare.
#ifdef _WIN32
    CHECK(!shell::path_contains_or_equals("C:/pkg", "D:/pkg/x.js"));
    CHECK(!shell::path_contains_or_equals("C:/pkg", "//server/share/pkg/x.js"));
#else
    CHECK(!shell::path_contains_or_equals("/pkg", "relative/x.js"));
#endif

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
    // NO `file:` source anywhere in the panel policy — a package's assets are served over this
    // scheme or not at all, exactly as editor-core's are. Pointed at the CSP, which can actually
    // regress; probing `kExtUrlPrefix` for it (as an earlier draft did) is tautological, since the
    // line above already pins that constant to an exact string.
    CHECK(!shelltest::mentions(std::string(shell::ext_csp_header()), "file:"));
}

// ------------------------------------------------------------------------------- response policy

void test_ext_csp_and_headers()
{
    const std::string csp = shell::ext_csp_header();

    // THE WHOLE POLICY, PINNED EXACTLY. Every directive-by-directive assertion below is still worth
    // having — a failure names WHAT moved — but only an exact match makes ANY edit to an untrusted
    // document's policy a reviewed edit, including one that ADDS a directive nothing thought to
    // probe for. ext_scheme.h carries the rationale for each clause.
    CHECK(csp == std::string("default-src 'none'; "
                             "script-src 'self'; "
                             "style-src 'self'; "
                             "img-src 'self' data:; "
                             "font-src 'self'; "
                             "connect-src 'none'; "
                             "frame-src 'none'; "
                             "child-src 'none'; "
                             "worker-src 'none'; "
                             "object-src 'none'; "
                             "base-uri 'none'; "
                             "form-action 'none'; "
                             "frame-ancestors context-editor://app"));

    CHECK(shelltest::mentions(csp, "default-src 'none'"));
    CHECK(shelltest::mentions(csp, "script-src 'self'"));
    CHECK(shelltest::mentions(csp, "connect-src 'none'"));
    CHECK(shelltest::mentions(csp, "object-src 'none'"));
    CHECK(shelltest::mentions(csp, "base-uri 'none'"));
    CHECK(shelltest::mentions(csp, "form-action 'none'"));
    CHECK(shelltest::mentions(csp, "font-src 'self'"));
    // A panel may not nest further frames.
    CHECK(shelltest::mentions(csp, "frame-src 'none'"));
    // NOR spawn workers. These two are spelled out because the CSP3 fallback chain for `worker-src`
    // runs through `child-src` to `script-src` — NOT to `default-src` — so omitting them would let
    // `script-src 'self'` silently GRANT an untrusted panel a Worker and a SharedWorker rendezvous.
    CHECK(shelltest::mentions(csp, "child-src 'none'"));
    CHECK(shelltest::mentions(csp, "worker-src 'none'"));

    // STRICTER than the trusted editor-core policy: NO inline anything, on ANY directive. The
    // dockview `'unsafe-inline'` carve-out is editor-core's; untrusted code does not inherit it.
    CHECK(shelltest::mentions(csp, "style-src 'self'"));
    CHECK(shelltest::count_occurrences(csp, "unsafe-inline") == 0);
    CHECK(!shelltest::mentions(csp, "unsafe-eval"));

    // A panel may load NOTHING from another package: no `context-ext:` scheme-source on any fetch
    // directive, which would be a cross-package escalation dressed up as a subresource load.
    CHECK(shelltest::count_occurrences(csp, "context-ext:") == 0);

    // FRAMED BY THE EDITOR AND NOTHING ELSE. Emphatically not 'none' (which would block the frame
    // the panel exists to be) and not '*'.
    //
    // ⚠ THE TIGHTENING M9 e13a-2 LANDED IS NOT SUBSTRING-CHECKABLE, and that is why the assertion
    // below is an ends_with rather than a `mentions`. The OLD, broader value —
    // `frame-ancestors context-editor:`, the scheme-source that also authorized
    // `context-editor://ipc` — is a strict PREFIX of the new one, so `mentions(csp,
    // "frame-ancestors context-editor:")` passes identically before and after and proves nothing.
    // Anchoring on the END of the policy is what actually distinguishes them: `frame-ancestors` is
    // the last directive, so a revert to the scheme form fails here immediately.
    CHECK(shelltest::mentions(csp, "frame-ancestors context-editor://app"));
    CHECK(csp.ends_with("frame-ancestors context-editor://app"));
    CHECK(!shelltest::mentions(csp, "frame-ancestors 'none'"));
    // The app's own origin appears EXACTLY ONCE, in that directive — a second occurrence would mean
    // some other directive had also been handed the editor's privileged origin.
    CHECK(shelltest::count_occurrences(csp, "context-editor:") == 1);

    // No external hosts anywhere (08 §2 "strict CSP, no external hosts"). Probed WITHOUT the
    // slashes: a CSP scheme-source is spelled `https:`, never `https://`, so a probe for `https://`
    // would sail straight past a `frame-src context-ext: https:` widening — the exact hole this
    // assertion exists to catch. `http:` as a substring catches `https:` too.
    CHECK(!shelltest::mentions(csp, "http:"));
    CHECK(!shelltest::mentions(csp, "*"));
    // `data:` is permitted for IMAGES only — inert bytes an icon in a bundled stylesheet needs.
    CHECK(shelltest::mentions(csp, "img-src 'self' data:"));
    CHECK(shelltest::count_occurrences(csp, "data:") == 1);

    // THE WHOLE HEADER SET, EXACTLY AND IN ORDER. One comparison pins the values, the documented
    // ORDER, and — the half a "look for what I expect" loop structurally cannot give — that the set
    // is CLOSED: an `Access-Control-Allow-Origin: *` quietly added to an untrusted panel's response
    // fails this, where a per-header search would wave it through.
    //
    // NOTE WHAT IS NOT IN THE LIST: `X-Frame-Options`. The app response sends `DENY` because the
    // editor window is never framed; a panel exists precisely to BE framed, and any consumer that
    // honours the legacy header ahead of `frame-ancestors` would block it outright. Its absence is
    // a deliberate asymmetry with app_response_headers, and this exact-set compare is what pins it
    // (test_app_scheme.cpp pins the other side).
    const std::vector<std::pair<std::string, std::string>> expected_headers = {
        {"Content-Type", "text/html; charset=utf-8"},
        {"Content-Security-Policy", csp},
        {"X-Content-Type-Options", "nosniff"},
        {"Referrer-Policy", "no-referrer"},
        {"Cache-Control", "no-store"},
    };
    CHECK(shell::ext_response_headers("text/html; charset=utf-8") == expected_headers);

    // --- the M9 e13a-2 CORS header: SCRIPTS ONLY -------------------------------------------------
    //
    // An ES module is fetched in CORS mode and a sandboxed panel's origin is the opaque `null`, so
    // without this header a module is fetched and then DISCARDED — measured; ext_scheme.h carries
    // the experiment. What is asserted here is the NARROWING, because that is the part a later edit
    // would "simplify" away: every NON-script asset must still carry NO CORS header at all, so one
    // package's stylesheet/image/font stays unreadable by another package's frame under the
    // same-origin policy rather than under this scheme's CSP alone (every panel frame shares the
    // opaque origin `null`, so a blanket header would make them mutually readable).
    const std::vector<std::pair<std::string, std::string>> expected_script_headers = {
        {"Content-Type", "text/javascript; charset=utf-8"},
        {"Content-Security-Policy", csp},
        {"X-Content-Type-Options", "nosniff"},
        {"Referrer-Policy", "no-referrer"},
        {"Cache-Control", "no-store"},
        {"Access-Control-Allow-Origin", "null"},
    };
    CHECK(shell::ext_response_headers("text/javascript; charset=utf-8") == expected_script_headers);

    // The charset is a SEPARATE response field, so the essence alone must classify identically —
    // the `.mjs` and `.js` allowlist entries both map to this essence.
    CHECK(shell::ext_response_headers("text/javascript").size() == expected_script_headers.size());

    // EVERY non-script media type on the shared allowlist carries NO CORS header. Driven from the
    // allowlist itself rather than from a hand-listed few, so a media type added there later cannot
    // quietly acquire one.
    for (const auto& [extension, mime] : shell::asset_media_types())
    {
        const auto headers = shell::ext_response_headers(mime);
        const bool has_cors =
            std::any_of(headers.begin(), headers.end(), [](const auto& header)
                        { return header.first == "Access-Control-Allow-Origin"; });
        const bool is_script = mime.rfind("text/javascript", 0) == 0;
        if (has_cors != is_script)
        {
            std::fprintf(stderr, "  [ext-cors] %s (%s) has_cors=%d is_script=%d\n",
                         extension.c_str(), mime.c_str(), has_cors ? 1 : 0, is_script ? 1 : 0);
        }
        CHECK(has_cors == is_script);
    }

    // And the REFUSAL response never carries one, whatever was asked for: a 403/404 body is not a
    // package asset, and a CORS-readable refusal would hand a probing frame a cross-origin oracle
    // for exactly the statuses `resolve` works to keep indistinguishable.
    const auto refusal = shell::refusal_headers(shell::ext_csp_header());
    CHECK(std::none_of(refusal.begin(), refusal.end(), [](const auto& header)
                       { return header.first == "Access-Control-Allow-Origin"; }));

    // The REFUSED response's policy, pinned next to the served one. It carries the same CSP (an
    // error page is a document too) and no Content-Type — the binding sets `text/plain` itself.
    const std::vector<std::pair<std::string, std::string>> expected_refusal = {
        {"Content-Security-Policy", csp},
        {"X-Content-Type-Options", "nosniff"},
        {"Cache-Control", "no-store"},
        {"Referrer-Policy", "no-referrer"},
    };
    CHECK(shell::refusal_headers(shell::ext_csp_header()) == expected_refusal);
}

// The e13a-1 widening of the EDITOR-CORE policy, asserted from this suite as well as its own: the
// two sides of the seam must agree, and reading `frame-src context-ext:` next to
// `frame-ancestors context-editor://app` is what makes that agreement legible.
void test_app_policy_permits_framing_this_scheme()
{
    const std::string app_csp = shell::app_csp_header();
    CHECK(shelltest::mentions(app_csp, "frame-src context-ext:"));
    CHECK(!shelltest::mentions(app_csp, "frame-src 'none'"));
    // And nothing ELSE was relaxed alongside it.
    CHECK(shelltest::mentions(app_csp, "object-src 'none'"));
    CHECK(shelltest::mentions(app_csp, "connect-src 'none'"));
    CHECK(shelltest::mentions(app_csp, "frame-ancestors 'none'"));
    // Probed WITHOUT slashes — see test_ext_csp_and_headers: a CSP scheme-source has none, so
    // `https://` would not catch a `frame-src context-ext: https:` over-widening.
    CHECK(!shelltest::mentions(app_csp, "http:"));
    CHECK(!shelltest::mentions(app_csp, "*"));
    // `data:` stays confined to img-src on this side of the seam too — the widening added a FRAME
    // source, not a data source, whatever order the directives end up in.
    CHECK(shelltest::count_occurrences(app_csp, "data:") == 1);
}

// ------------------------------------------------- the e13b-1 panel-port bootstrap (ext_scheme.h)

// The SYNTHETIC asset — the one path this scheme answers out of itself.
//
// Its whole security argument is WHERE it sits in `resolve`, so this block asserts that position from
// both sides: it is served for a mounted package, and it is refused — with the SAME status and reason
// an absent asset gets — for one that is not. A resolver that answered the bootstrap before consulting
// the mount table would be a package-enumeration oracle (200 for every syntactically valid id), which
// is exactly what the pairing below detects and what a bare `ok()` check on the positive case alone
// would not.
void test_port_bootstrap_asset()
{
    const std::filesystem::path root = shelltest::make_temp_project("ctx-ext-scheme", "bootstrap");
    const std::filesystem::path pkg = root / "hello-panel";
    write_file(pkg / "index.html", "<!DOCTYPE html><html></html>");

    shell::ExtAssetResolver resolver;
    std::string reason;
    CHECK(resolver.mount("hello-panel", pkg, reason));

    const std::string asset = std::string("context-ext://hello-panel/") + shell::kExtPortBootstrapAsset;
    {
        const shell::ExtResolution r = resolver.resolve(asset);
        CHECK(r.ok());
        CHECK(r.synthetic);
        CHECK(r.http_status() == 200);
        CHECK(r.package_id == "hello-panel");
        // Classified through the SHARED allowlist, not a literal, so it carries the same
        // `Access-Control-Allow-Origin: null` every other script response does.
        CHECK(r.mime_type == "text/javascript; charset=utf-8");
        // No file is named — the caller must take the bytes from `ext_port_bootstrap_script()`, and a
        // path here would invite a caller to read one.
        CHECK(r.file.empty());
    }
    {
        // AN UNMOUNTED PACKAGE GETS NOTHING, and gets it in the SAME shape an absent asset does.
        // `ctx.absent` is grammatically valid, so this refusal can only come from the mount table.
        const shell::ExtResolution r =
            resolver.resolve(std::string("context-ext://ctx.absent/") + shell::kExtPortBootstrapAsset);
        CHECK(!r.ok());
        CHECK(!r.synthetic);
        CHECK(r.status == shell::AssetStatus::not_found);
        CHECK(r.reason == "package is not mounted");
    }
    {
        // EXACTLY ONE SEGMENT, and exactly that name. A nested spelling is an ordinary path that
        // resolves against the package root and 404s there like any other absent file — asserted so
        // the synthetic branch cannot be reached by a request the containment rules never saw.
        const shell::ExtResolution nested = resolver.resolve(
            std::string("context-ext://hello-panel/sub/") + shell::kExtPortBootstrapAsset);
        CHECK(!nested.ok());
        CHECK(!nested.synthetic);
        CHECK(nested.status == shell::AssetStatus::not_found);
        CHECK(nested.reason == "no such asset");
        // And a traversal that ENDS in the reserved name is still a traversal: the textual pass runs
        // first, so this must report the traversal gate rather than the synthetic 200.
        const shell::ExtResolution up = resolver.resolve(
            std::string("context-ext://hello-panel/../") + shell::kExtPortBootstrapAsset);
        CHECK(!up.ok());
        CHECK(!up.synthetic);
        CHECK(up.status == shell::AssetStatus::forbidden);
        CHECK(up.reason == "'..' traversal segment");
    }
    {
        // The reserved name does NOT shadow a real file of another name, and the package's own assets
        // are unaffected by the branch existing at all.
        CHECK(resolver.resolve("context-ext://hello-panel/index.html").ok());
        CHECK(!resolver.resolve("context-ext://hello-panel/index.html").synthetic);
    }

    shelltest::cleanup(root);
}

// The bootstrap script's BYTES. Pinned because they are the only editor-authored code that ever runs
// inside a third-party document, and because each property below is one the header claims and nothing
// else enforces.
void test_port_bootstrap_script()
{
    const std::string script = shell::ext_port_bootstrap_script();

    // Assembled FROM the constants — so each of the three appears, and a rename that missed the
    // assembly would drop it here rather than at a silent runtime mismatch.
    CHECK(shelltest::mentions(script, shell::kExtPortHandshakeTag));
    CHECK(shelltest::mentions(script, shell::kExtPortGlobalName));
    CHECK(shelltest::mentions(script, shell::kAppOrigin));

    // IT TRANSFERS, and it transfers exactly one port of a channel it created itself. This is the
    // direction the whole design rests on (the child mints the channel; the host never posts a port
    // down), so its two halves are asserted separately.
    CHECK(shelltest::mentions(script, "new MessageChannel()"));
    CHECK(shelltest::mentions(script, "[channel.port2]"));
    CHECK(shelltest::mentions(script, "channel.port1"));

    // NEVER `"*"`. This is the ONE direction in which a precise target origin is possible, and a
    // wildcard here would make the port deliverable to any embedder that got hold of the frame.
    CHECK(!shelltest::mentions(script, "\"*\""));
    CHECK(!shelltest::mentions(script, "'*'"));

    // The not-framed early return, so a panel document opened directly transfers nothing to itself.
    CHECK(shelltest::mentions(script, "window.parent === window"));

    // The global is installed unwritable + unconfigurable, so a package bundle cannot clobber the
    // port with a look-alike and strand the real one.
    CHECK(shelltest::mentions(script, "writable: false"));
    CHECK(shelltest::mentions(script, "configurable: false"));

    // Pure ASCII and no embedded closing tag — the header's fourth property. `</script` would
    // truncate the host document the day this is ever inlined rather than served.
    CHECK(!shelltest::mentions(script, "</script"));
    for (const char ch : script)
    {
        CHECK(static_cast<unsigned char>(ch) < 0x80);
    }

    // STABLE ACROSS CALLS — it is served on every panel navigation and is built once behind a
    // function-local static, so an accidental per-call rebuild would be a silent allocation on the
    // CEF IO thread.
    CHECK(shell::ext_port_bootstrap_script() == script);
}

// WHERE THE TAG IS SPLICED — the adversarial half, and the one that decides whether the bootstrap
// really runs before package code.
//
// The table is built around the ONE property that matters: for every prefix shape a document can
// start with, our tag must end up ahead of ANY `<script>` the document carries. The cases are
// therefore chosen by what they put FIRST, not by what looks like valid HTML.
void test_port_bootstrap_injection()
{
    const std::string tag =
        std::string("<script src=\"/") + shell::kExtPortBootstrapAsset + "\"></script>";
    const std::string html = "text/html; charset=utf-8";

    // Helper: the offset of our tag, and of the document's own first script.
    const auto injected_before_own_script = [&tag](const std::string& out) {
        const std::size_t ours = out.find(tag);
        if (ours == std::string::npos)
        {
            return false;
        }
        // The document's own first `<script` is the next one after ours ends.
        const std::size_t theirs = out.find("<script", ours + tag.size());
        return theirs == std::string::npos || ours < theirs;
    };

    {
        // A LEADING DOCTYPE ⇒ spliced immediately after it, so standards mode survives. The tag must
        // NOT precede the doctype: displacing it is a silent switch to quirks mode for third-party
        // layout.
        const std::string out = shell::ext_inject_port_bootstrap(
            html, "<!DOCTYPE html>\n<html><head><script src=\"panel.js\"></script></head></html>");
        CHECK(out.rfind("<!DOCTYPE html>", 0) == 0);
        CHECK(out.find(tag) == std::string("<!DOCTYPE html>").size());
        CHECK(injected_before_own_script(out));
    }
    {
        // Case-insensitive, lower case, and with attributes + a BOM + leading whitespace — all shapes
        // that may legitimately precede content, none of which may cost us the doctype.
        for (const std::string& prefix :
             {std::string("<!doctype html>"),
              std::string("<!DocType HTML SYSTEM \"about:legacy-compat\">"),
              std::string("\xEF\xBB\xBF<!doctype html>"), std::string("\n\t  <!doctype html>")})
        {
            const std::string out =
                shell::ext_inject_port_bootstrap(html, prefix + "<script src=\"p.js\"></script>");
            CHECK(out.find(tag) == prefix.size());
            CHECK(injected_before_own_script(out));
        }
    }
    {
        // ⚠ THE CASE THE RULE EXISTS FOR: a `<script>` BEFORE the doctype. A "find the doctype
        // anywhere" implementation would splice after it and run package code FIRST — the exact
        // property-1 violation the header names. The doctype is not first, so we go to offset 0.
        const std::string body = "<script src=\"evil.js\"></script><!doctype html><html></html>";
        const std::string out = shell::ext_inject_port_bootstrap(html, body);
        CHECK(out.rfind(tag, 0) == 0);
        CHECK(injected_before_own_script(out));
    }
    {
        // A COMMENT before the doctype is the same shape (a comment can carry a conditional-comment
        // script on legacy engines), so it takes the same answer.
        const std::string out = shell::ext_inject_port_bootstrap(
            html, "<!-- hello --><!doctype html><script src=\"p.js\"></script>");
        CHECK(out.rfind(tag, 0) == 0);
        CHECK(injected_before_own_script(out));
    }
    {
        // No doctype at all — already quirks, so offset 0 costs nothing.
        const std::string out =
            shell::ext_inject_port_bootstrap(html, "<html><script src=\"p.js\"></script></html>");
        CHECK(out.rfind(tag, 0) == 0);
        CHECK(injected_before_own_script(out));
    }
    {
        // An UNTERMINATED doctype must not become an unbounded scan, and must still fail SAFE. The
        // padding is longer than the scan limit, so the '>' is beyond it.
        const std::string body =
            "<!doctype " + std::string(shell::kExtDoctypeScanLimit + 64, 'x') + ">";
        const std::string out = shell::ext_inject_port_bootstrap(html, body);
        CHECK(out.rfind(tag, 0) == 0);
    }
    {
        // A doctype whose '>' sits just INSIDE the limit is still honoured — the bound is a bound, not
        // an off-by-one that silently disables the standards-mode case for any long doctype.
        const std::string prefix =
            "<!doctype html " + std::string(shell::kExtDoctypeScanLimit - 32, 'a') + ">";
        const std::string out = shell::ext_inject_port_bootstrap(html, prefix + "<html></html>");
        CHECK(out.find(tag) == prefix.size());
    }
    {
        // AN EMPTY DOCUMENT still gets the bootstrap: a package whose entry is empty is broken, but it
        // must not be the one document that silently escapes the mechanism.
        CHECK(shell::ext_inject_port_bootstrap(html, "") == tag);
    }
    {
        // EVERY OTHER MEDIA TYPE PASSES THROUGH BYTE-IDENTICAL. Driven from the shared allowlist so a
        // media type added later cannot quietly start being rewritten — and asserted on a body that
        // LOOKS like HTML, because the decision must be the declared type's, never the bytes'.
        const std::string htmlish = "<!doctype html><html></html>";
        for (const auto& [extension, mime] : shell::asset_media_types())
        {
            const std::string out = shell::ext_inject_port_bootstrap(mime, htmlish);
            if (shell::split_media_type(mime).essence == "text/html")
            {
                CHECK(out != htmlish);
                continue;
            }
            if (out != htmlish)
            {
                std::fprintf(stderr, "  [ext-inject] %s (%s) was rewritten\n", extension.c_str(),
                             mime.c_str());
            }
            CHECK(out == htmlish);
        }
        // Including the bare essence with no charset, and an unlistable type.
        CHECK(shell::ext_inject_port_bootstrap("text/plain", htmlish) == htmlish);
        CHECK(shell::ext_inject_port_bootstrap("", htmlish) == htmlish);
        // ...while the bare HTML essence IS rewritten: the charset is a separate response field, so
        // classification must key off the essence alone.
        CHECK(shell::ext_inject_port_bootstrap("text/html", htmlish) != htmlish);
    }
    {
        // THE TAG IS AN ABSOLUTE PATH, which is what makes it resolve against the package ORIGIN
        // rather than the entry document's directory. A relative spelling 404s the bootstrap for every
        // package whose entry is not at the root.
        CHECK(shelltest::mentions(tag, "src=\"/"));
        // A CLASSIC external script — no `type="module"` and no `defer`/`async`, each of which would
        // defer execution past the package's own inline-order scripts and break property 1.
        CHECK(!shelltest::mentions(tag, "module"));
        CHECK(!shelltest::mentions(tag, "defer"));
        CHECK(!shelltest::mentions(tag, "async"));
    }
    {
        // NOTHING ELSE IN THE DOCUMENT MOVES: the output is the input with exactly the tag inserted,
        // asserted by reconstructing the input from the output.
        const std::string body = "<!doctype html><html><body>hello &amp; goodbye</body></html>";
        const std::string out = shell::ext_inject_port_bootstrap(html, body);
        const std::size_t at = out.find(tag);
        CHECK(at != std::string::npos);
        CHECK(out.substr(0, at) + out.substr(at + tag.size()) == body);
        CHECK(out.size() == body.size() + tag.size());
    }
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
    test_alternate_data_stream_refused();
    test_split_safe_path_segments_direct();
    test_path_contains_or_equals_direct();
    test_scheme_registration_options();
    test_scheme_constants();
    test_ext_csp_and_headers();
    test_app_policy_permits_framing_this_scheme();
    test_port_bootstrap_asset();
    test_port_bootstrap_script();
    test_port_bootstrap_injection();
    test_http_status_mapping();
    SHELL_TEST_MAIN_END();
}
