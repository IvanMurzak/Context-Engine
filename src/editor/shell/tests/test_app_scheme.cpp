// ctest `editor-shell-test_app_scheme` — the `context-editor://app/` resolver + response policy
// (M9 e05c, design 04 §1 / 08 §2).
//
// The resolver decides WHICH BYTES ON DISK A RENDERER MAY READ, so the bulk of this suite is
// adversarial: traversal in every encoding the URL layer permits, escapes the textual pass cannot
// see, and media types that are not on the allowlist. It runs on all three default `build` legs
// because it is CEF-free by design (app_scheme.h explains why).

#include "context/editor/shell/app_scheme.h"

#include "shell_test.h"

#include <fstream>
#include <string>

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

// --------------------------------------------------------------------------- percent-decoding

void test_percent_decode()
{
    std::string out;

    CHECK(shell::percent_decode("editor-core.js", out));
    CHECK(out == "editor-core.js");

    // One pass, and only one: `%252e` decodes to the literal text `%2e`, NOT to `.`. A decoder that
    // looped would turn a double-encoded traversal into a real one.
    CHECK(shell::percent_decode("%252e%252e", out));
    CHECK(out == "%2e%2e");

    CHECK(shell::percent_decode("a%2Fb", out));
    CHECK(out == "a/b");

    // Malformed escapes are REFUSED, never passed through or stripped.
    CHECK(!shell::percent_decode("%", out));
    CHECK(!shell::percent_decode("%2", out));
    CHECK(!shell::percent_decode("%zz", out));
    CHECK(!shell::percent_decode("%2g", out));

    // NUL and control bytes, raw and encoded. A sanitizing decoder that dropped these is exactly
    // how a truncation trick survives its own check.
    CHECK(!shell::percent_decode("%00", out));
    CHECK(!shell::percent_decode("a%00b", out));
    CHECK(!shell::percent_decode("%0a", out));
    CHECK(!shell::percent_decode(std::string("a\0b", 3), out));
    CHECK(!shell::percent_decode("tab\there", out));
}

// -------------------------------------------------------------------------------- media types

void test_media_types()
{
    CHECK(shell::media_type_for_extension(".js") == "text/javascript; charset=utf-8");
    CHECK(shell::media_type_for_extension(".css") == "text/css; charset=utf-8");
    CHECK(shell::media_type_for_extension(".html") == "text/html; charset=utf-8");
    // Case-insensitive: the filesystem may hand back either.
    CHECK(shell::media_type_for_extension(".JS") == "text/javascript; charset=utf-8");
    // Deny-by-default — anything not on the allowlist has NO type, which the resolver turns into a
    // 403 regardless of whether the file exists.
    CHECK(shell::media_type_for_extension(".exe").empty());
    CHECK(shell::media_type_for_extension(".dll").empty());
    CHECK(shell::media_type_for_extension("").empty());
    CHECK(shell::media_type_for_extension(".ts").empty());
    CHECK(!shell::asset_media_types().empty());
}

// ----------------------------------------------------------------------------------- resolution

void test_resolution()
{
    const std::filesystem::path root = shelltest::make_temp_project("ctx-app-scheme", "resolve");
    const std::filesystem::path assets = root / "app";
    write_file(assets / "index.html", "<!DOCTYPE html>");
    write_file(assets / "editor-core.js", "export const x = 1;");
    write_file(assets / "dockview.css", ".dv {}");
    write_file(assets / "sub" / "nested.js", "1;");
    // The file a traversal would be reaching FOR: a sibling of the asset root, outside it.
    write_file(root / "secret.js", "TOKEN");

    const shell::AppAssetResolver resolver(assets);
    CHECK(resolver.root_exists());

    // --- the happy paths ---
    {
        const shell::AssetResolution r = resolver.resolve("context-editor://app/editor-core.js");
        CHECK(r.ok());
        CHECK(r.http_status() == 200);
        CHECK(r.mime_type == "text/javascript; charset=utf-8");
        CHECK(r.file.filename() == "editor-core.js");
    }
    {
        // A bare host resolves to the default document, with and without the trailing slash.
        CHECK(resolver.resolve("context-editor://app/").ok());
        CHECK(resolver.resolve("context-editor://app").ok());
        CHECK(resolver.resolve("context-editor://app/").file.filename() == "index.html");
    }
    {
        CHECK(resolver.resolve("context-editor://app/sub/nested.js").ok());
        // Query and fragment belong to the URL, not the filename.
        CHECK(resolver.resolve("context-editor://app/editor-core.js?v=2").ok());
        CHECK(resolver.resolve("context-editor://app/editor-core.js#top").ok());
        // A percent-encoded ordinary character still resolves.
        CHECK(resolver.resolve("context-editor://app/editor%2Dcore.js").ok());
    }

    // --- wrong scheme / host / shape => bad_request ---
    for (const char* url : {"file:///etc/passwd", "https://example.com/editor-core.js",
                            "context-editor://ipc", "context-editor://other/editor-core.js",
                            "context-ext://pkg/panel.js", "", "app/editor-core.js"})
    {
        const shell::AssetResolution r = resolver.resolve(url);
        CHECK(r.status == shell::AssetStatus::bad_request);
        CHECK(r.http_status() == 400);
    }

    // --- traversal, in every shape the URL layer permits => forbidden ---
    for (const char* url : {
             "context-editor://app/../secret.js",
             "context-editor://app/sub/../../secret.js",
             "context-editor://app/%2e%2e/secret.js",
             "context-editor://app/%2E%2E/secret.js",
             "context-editor://app/..%2fsecret.js",
             "context-editor://app/sub/%2e%2e/%2e%2e/secret.js",
             "context-editor://app/./editor-core.js",
             // A backslash is a separator on Windows, so this is a traversal a '/'-only split would
             // hand through as one innocent segment.
             "context-editor://app/..\\secret.js",
             "context-editor://app/%5c..%5csecret.js",
             // Drive-qualified: on Windows `root / "C:x"` re-roots onto that drive instead of
             // appending.
             "context-editor://app/C:/Windows/System32/drivers/etc/hosts",
             "context-editor://app/C:secret.js",
         })
    {
        const shell::AssetResolution r = resolver.resolve(url);
        CHECK(r.status == shell::AssetStatus::forbidden);
        CHECK(r.http_status() == 403);
        CHECK(r.file.empty());
    }

    // --- media type not on the allowlist => forbidden, whether or not it exists ---
    {
        write_file(assets / "payload.exe", "MZ");
        const shell::AssetResolution present = resolver.resolve("context-editor://app/payload.exe");
        CHECK(present.status == shell::AssetStatus::forbidden);
        const shell::AssetResolution absent = resolver.resolve("context-editor://app/nothing.exe");
        CHECK(absent.status == shell::AssetStatus::forbidden);
        // The SAME status for present and absent is the point: the response cannot be used to probe
        // for files by their status code.
        CHECK(present.status == absent.status);
    }

    // --- well-formed, permitted, but absent => not_found ---
    {
        const shell::AssetResolution r = resolver.resolve("context-editor://app/missing.js");
        CHECK(r.status == shell::AssetStatus::not_found);
        CHECK(r.http_status() == 404);
    }
    {
        // A directory is not a regular file.
        const shell::AssetResolution r = resolver.resolve("context-editor://app/sub");
        CHECK(!r.ok());
    }

    // --- malformed encoding => bad_request ---
    {
        CHECK(resolver.resolve("context-editor://app/%zz.js").status ==
              shell::AssetStatus::bad_request);
        CHECK(resolver.resolve("context-editor://app/a%00.js").status ==
              shell::AssetStatus::bad_request);
    }

    shelltest::cleanup(root);
}

void test_missing_root_degrades()
{
    // A resolver over a root that does not exist must report not_found, never throw and never
    // claim success — the Shell reports the gap once at boot rather than crashing per request.
    const shell::AppAssetResolver resolver(
        std::filesystem::path("this") / "root" / "does" / "not" / "exist");
    CHECK(!resolver.root_exists());
    const shell::AssetResolution r = resolver.resolve("context-editor://app/editor-core.js");
    CHECK(r.status == shell::AssetStatus::not_found);
    // The type allowlist is still applied first, so a forbidden type is still forbidden.
    CHECK(resolver.resolve("context-editor://app/x.exe").status == shell::AssetStatus::forbidden);
}

// ------------------------------------------------------------------------------- response policy

void test_csp_and_headers()
{
    const std::string csp = shell::app_csp_header();

    // The three properties the DoD names, asserted rather than eyeballed.
    CHECK(shelltest::mentions(csp, "default-src 'none'"));
    CHECK(shelltest::mentions(csp, "script-src 'self'"));
    // NO INLINE SCRIPT and NO eval: the backstop behind the render_html escaping contract (C-F6).
    CHECK(!shelltest::mentions(csp, "unsafe-inline"));
    CHECK(!shelltest::mentions(csp, "unsafe-eval"));
    // NO NETWORK — the 08 §2 "token leakage via the web layer" control.
    CHECK(shelltest::mentions(csp, "connect-src 'none'"));
    CHECK(shelltest::mentions(csp, "object-src 'none'"));
    CHECK(shelltest::mentions(csp, "base-uri 'none'"));
    CHECK(shelltest::mentions(csp, "frame-ancestors 'none'"));
    // Third-party panels are not framed yet; when 04 §5 lands, THIS is the line that widens.
    CHECK(shelltest::mentions(csp, "frame-src 'none'"));
    CHECK(!shelltest::mentions(csp, "http://"));
    CHECK(!shelltest::mentions(csp, "https://"));
    CHECK(!shelltest::mentions(csp, "*"));

    const auto headers = shell::app_response_headers("text/javascript; charset=utf-8");
    bool has_type = false;
    bool has_csp = false;
    bool has_nosniff = false;
    for (const auto& [name, value] : headers)
    {
        if (name == "Content-Type")
        {
            has_type = value == "text/javascript; charset=utf-8";
        }
        else if (name == "Content-Security-Policy")
        {
            has_csp = value == csp;
        }
        else if (name == "X-Content-Type-Options")
        {
            has_nosniff = value == "nosniff";
        }
    }
    CHECK(has_type);
    CHECK(has_csp);
    CHECK(has_nosniff);
}

void test_scheme_constants()
{
    // The one place the scheme vocabulary is spelled — pinned so a rename desyncs loudly instead of
    // leaving the TS client, the binding and the resolver disagreeing about the origin.
    CHECK(std::string(shell::kAppScheme) == "context-editor");
    CHECK(std::string(shell::kAppOrigin) == "context-editor://app");
    CHECK(std::string(shell::kAppUrlPrefix) == "context-editor://app/");
    CHECK(std::string(shell::kIpcEndpoint) == "context-editor://ipc");
    CHECK(std::string(shell::kAppEntryUrl) == "context-editor://app/index.html");
    // NO file:// fallback exists anywhere in the vocabulary (a DoD line).
    CHECK(!shelltest::mentions(std::string(shell::kAppEntryUrl), "file:"));
}

void test_split_media_type()
{
    // The CEF response carries the essence and the charset in SEPARATE fields (CefResponse has
    // distinct SetMimeType/SetCharset accessors, and CEF's own resource manager fills the first from
    // CefGetMimeType(), which returns an essence). Handing the full value to the essence field makes
    // Chromium's by-essence comparison fail, and under this response's own nosniff that silently
    // refuses the stylesheet and the ES module. Only the live CEF smoke can see it, so the SPLIT is
    // pinned here, where all three default build legs run it.
    const shell::MediaType html = shell::split_media_type("text/html; charset=utf-8");
    CHECK(html.essence == "text/html");
    CHECK(html.charset == "utf-8");

    // EVERY type the allowlist can return must split to a bare essence — this is the assertion that
    // would have caught the original break, because it walks the real table rather than one example.
    struct ExpectedEssence
    {
        const char* extension;
        const char* essence;
    };
    static const ExpectedEssence kExpected[] = {
        {".html", "text/html"},       {".js", "text/javascript"},
        {".mjs", "text/javascript"},  {".css", "text/css"},
        {".json", "application/json"},
    };
    for (const ExpectedEssence& expected : kExpected)
    {
        const shell::MediaType split =
            shell::split_media_type(shell::media_type_for_extension(expected.extension));
        CHECK(split.essence == expected.essence);
        CHECK(split.charset == "utf-8");
        // The essence must never carry a parameter — that is the whole failure mode.
        CHECK(!shelltest::mentions(split.essence, ";"));
        CHECK(!shelltest::mentions(split.essence, "charset"));
    }

    // Shapes the splitter must survive.
    CHECK(shell::split_media_type("text/plain").essence == "text/plain");
    CHECK(shell::split_media_type("text/plain").charset.empty());
    CHECK(shell::split_media_type("  text/css  ;  charset = utf-8  ").essence == "text/css");
    CHECK(shell::split_media_type("  text/css  ;  charset = utf-8  ").charset == "utf-8");
    // Case-insensitive parameter NAME (the value is passed through as written).
    CHECK(shell::split_media_type("text/css; CHARSET=UTF-8").charset == "UTF-8");
    // A quoted value is legal per RFC 9110 and must arrive unquoted.
    CHECK(shell::split_media_type("text/css; charset=\"utf-8\"").charset == "utf-8");
    // charset need not be the first parameter.
    CHECK(shell::split_media_type("text/css; boundary=x; charset=utf-8").charset == "utf-8");
    // A parameter that is not charset must not be mistaken for one.
    CHECK(shell::split_media_type("text/css; boundary=charsetish").charset.empty());
    CHECK(shell::split_media_type("").essence.empty());
}

} // namespace

int main()
{
    test_percent_decode();
    test_media_types();
    test_resolution();
    test_missing_root_degrades();
    test_csp_and_headers();
    test_scheme_constants();
    test_split_media_type();
    SHELL_TEST_MAIN_END();
}
