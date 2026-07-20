// The `context-editor://app/` resolver + response policy — see app_scheme.h for the model and why
// none of this lives inside the CEF resource handler.

#include "context/editor/shell/app_scheme.h"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace context::editor::shell
{
namespace
{

// Lower-case an ASCII string. Deliberately NOT std::tolower over the raw char: on a signed-char
// platform a byte >= 0x80 converts to a negative int, which is UB for the <cctype> functions.
std::string ascii_lower(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (char c : text)
    {
        const auto byte = static_cast<unsigned char>(c);
        out.push_back(static_cast<char>(std::tolower(byte)));
    }
    return out;
}

bool is_hex_digit(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int hex_value(unsigned char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    return c - 'A' + 10;
}

// Split a decoded relative path into segments on '/', rejecting every shape that could escape the
// root. This runs BEFORE the filesystem is touched, and its refusals are absolute — nothing here
// "cleans up" a suspicious path, because a cleaning pass is how `....//` becomes `../`.
bool split_safe_segments(std::string_view path, std::vector<std::string>& out, std::string& reason)
{
    out.clear();
    std::string current;
    // The loop runs one past the end so the final segment is flushed by the same branch as the
    // rest — a trailing flush after the loop is where an off-by-one lives.
    for (std::size_t i = 0; i <= path.size(); ++i)
    {
        const bool at_end = i == path.size();
        const char c = at_end ? '/' : path[i];

        if (!at_end)
        {
            // A backslash is a path separator on Windows, so `..\..\secret` is a traversal that a
            // '/'-only split would hand through as ONE innocent-looking segment.
            if (c == '\\')
            {
                reason = "backslash in path";
                return false;
            }
            // Belt and braces: percent_decode already refuses these, but this function is public
            // surface reachable from a future caller that decodes differently.
            if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f)
            {
                reason = "control character in path";
                return false;
            }
        }

        if (c != '/')
        {
            current.push_back(c);
            continue;
        }

        if (current.empty())
        {
            // An empty segment is a `//` (or a leading/trailing slash). Harmless to skip: it
            // cannot escape, and rejecting it would refuse the legitimate bare `app/` root.
            continue;
        }
        if (current == ".")
        {
            reason = "'.' segment";
            return false;
        }
        if (current == "..")
        {
            reason = "'..' traversal segment";
            return false;
        }
        // A drive-relative or UNC-ish segment (`C:`, `C:foo`). On Windows `root / "C:x"` does NOT
        // append — it re-roots onto that drive, silently leaving the asset root.
        if (current.size() >= 2 && current[1] == ':')
        {
            reason = "drive-qualified segment";
            return false;
        }
        out.push_back(current);
        current.clear();
    }
    return true;
}

} // namespace

int AssetResolution::http_status() const
{
    switch (status)
    {
    case AssetStatus::ok:
        return 200;
    case AssetStatus::bad_request:
        return 400;
    case AssetStatus::forbidden:
        return 403;
    case AssetStatus::not_found:
    default:
        return 404;
    }
}

const std::vector<std::pair<std::string, std::string>>& asset_media_types()
{
    // Function-local static, so the table is built once and there is no static-init-order question
    // between this and any other translation unit's globals.
    static const std::vector<std::pair<std::string, std::string>> kTypes = {
        {".html", "text/html; charset=utf-8"},
        {".js", "text/javascript; charset=utf-8"},
        {".mjs", "text/javascript; charset=utf-8"},
        {".css", "text/css; charset=utf-8"},
        {".json", "application/json; charset=utf-8"},
        {".map", "application/json; charset=utf-8"},
        {".svg", "image/svg+xml"},
        {".png", "image/png"},
        {".webp", "image/webp"},
        {".woff2", "font/woff2"},
    };
    return kTypes;
}

std::string media_type_for_extension(std::string_view extension)
{
    const std::string key = ascii_lower(extension);
    for (const auto& [ext, mime] : asset_media_types())
    {
        if (ext == key)
        {
            return mime;
        }
    }
    return {};
}

bool percent_decode(std::string_view input, std::string& out)
{
    out.clear();
    out.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i)
    {
        const char c = input[i];
        if (c != '%')
        {
            const auto byte = static_cast<unsigned char>(c);
            // Refuse rather than strip. A decoder that silently drops a NUL turns "a\0/../b" into
            // something whose textual form no longer matches what the filesystem will open.
            if (byte < 0x20 || byte == 0x7f)
            {
                return false;
            }
            out.push_back(c);
            continue;
        }
        if (i + 2 >= input.size())
        {
            return false;
        }
        const auto hi = static_cast<unsigned char>(input[i + 1]);
        const auto lo = static_cast<unsigned char>(input[i + 2]);
        if (!is_hex_digit(hi) || !is_hex_digit(lo))
        {
            return false;
        }
        const int value = hex_value(hi) * 16 + hex_value(lo);
        if (value < 0x20 || value == 0x7f)
        {
            return false;
        }
        out.push_back(static_cast<char>(value));
        i += 2;
    }
    return true;
}

AppAssetResolver::AppAssetResolver(std::filesystem::path asset_root)
{
    std::error_code ec;
    // weakly_canonical rather than canonical: a root that does not exist yet must not throw, and
    // the `root_exists_` flag below is what reports that honestly.
    asset_root_ = std::filesystem::weakly_canonical(asset_root, ec);
    if (ec)
    {
        asset_root_ = std::move(asset_root);
    }
    root_exists_ = std::filesystem::is_directory(asset_root_, ec);
}

AssetResolution AppAssetResolver::resolve(std::string_view url) const
{
    AssetResolution result;

    const std::string_view prefix{kAppUrlPrefix};
    // `context-editor://app` with NO trailing slash is the origin, not a request path; CEF
    // normalizes it to `.../app/` before it reaches a handler, but a direct caller may not, so both
    // are accepted and mean the default document.
    const std::string_view origin{kAppOrigin};
    std::string_view rest;
    if (url.size() >= prefix.size() && url.substr(0, prefix.size()) == prefix)
    {
        rest = url.substr(prefix.size());
    }
    else if (url == origin)
    {
        rest = {};
    }
    else
    {
        result.status = AssetStatus::bad_request;
        result.reason = "not a " + std::string(kAppUrlPrefix) + " URL";
        return result;
    }

    // Strip the query and fragment BEFORE decoding: they are not part of the path, and a `?` or `#`
    // left in would become a literal filename character.
    if (const std::size_t cut = rest.find_first_of("?#"); cut != std::string_view::npos)
    {
        rest = rest.substr(0, cut);
    }

    std::string decoded;
    if (!percent_decode(rest, decoded))
    {
        result.status = AssetStatus::bad_request;
        result.reason = "malformed percent-encoding";
        return result;
    }

    std::vector<std::string> segments;
    std::string reason;
    if (!split_safe_segments(decoded, segments, reason))
    {
        // FORBIDDEN, not bad_request: the path parsed fine, it just asked for something outside the
        // asset set. Keeping the two apart is what makes the traversal count in the tests real.
        result.status = AssetStatus::forbidden;
        result.reason = reason;
        return result;
    }

    if (segments.empty())
    {
        segments.emplace_back(kDefaultDocument);
    }

    std::filesystem::path candidate = asset_root_;
    for (const std::string& segment : segments)
    {
        candidate /= segment;
    }

    // Media type FIRST, before the filesystem is touched: a request for an extension that is not on
    // the allowlist is refused whether or not the file happens to exist, so the response cannot be
    // used to probe for files (`.env`, `.git/config`) by their status code.
    result.mime_type = media_type_for_extension(candidate.extension().string());
    if (result.mime_type.empty())
    {
        result.status = AssetStatus::forbidden;
        result.reason = "media type not on the asset allowlist";
        return result;
    }

    if (!root_exists_)
    {
        result.status = AssetStatus::not_found;
        result.reason = "asset root does not exist";
        return result;
    }

    // The SECOND, independent containment check (see the header). weakly_canonical resolves `..`
    // that survived, and — the case the textual pass genuinely cannot see — follows symlinks, so a
    // link inside the asset dir pointing at the user's home directory is caught here.
    std::error_code ec;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(candidate, ec);
    if (ec)
    {
        result.status = AssetStatus::not_found;
        result.reason = "path could not be canonicalized";
        return result;
    }

    // lexically_relative yields a path starting with ".." exactly when `canonical` is outside the
    // root, which is the containment predicate — and it is a pure path operation, so it cannot be
    // defeated by a race between the check and the open.
    const std::filesystem::path relative = canonical.lexically_relative(asset_root_);
    if (relative.empty() || *relative.begin() == "..")
    {
        result.status = AssetStatus::forbidden;
        result.reason = "resolved outside the asset root";
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

const char* app_csp_header()
{
    // One string, spelled once — see the header for why each directive is what it is.
    return "default-src 'none'; "
           "script-src 'self'; "
           "style-src 'self'; "
           "img-src 'self' data:; "
           "font-src 'self'; "
           "connect-src 'none'; "
           "frame-src 'none'; "
           "object-src 'none'; "
           "base-uri 'none'; "
           "form-action 'none'; "
           "frame-ancestors 'none'";
}

std::vector<std::pair<std::string, std::string>>
app_response_headers(const std::string& mime_type)
{
    return {
        {"Content-Type", mime_type},
        {"Content-Security-Policy", app_csp_header()},
        // Without nosniff a served asset whose bytes look like HTML can be re-interpreted as a
        // document regardless of the Content-Type the allowlist chose.
        {"X-Content-Type-Options", "nosniff"},
        // The editor is never framed and never frames; belt-and-braces alongside the CSP
        // frame-ancestors directive for anything that honours only the legacy header.
        {"X-Frame-Options", "DENY"},
        // No document should leak a URL to anywhere, and with connect-src 'none' there is nowhere
        // to leak it to anyway — stated so the posture does not depend on that coincidence.
        {"Referrer-Policy", "no-referrer"},
        // These assets are rebuilt with the app; a cached copy across versions would serve a stale
        // bundle against a fresh contract surface.
        {"Cache-Control", "no-store"},
    };
}

MediaType split_media_type(std::string_view media_type)
{
    const auto trim = [](std::string_view text) {
        const auto is_space = [](unsigned char c) { return c == ' ' || (c >= 0x09 && c <= 0x0d); };
        std::size_t begin = 0;
        std::size_t end = text.size();
        while (begin < end && is_space(static_cast<unsigned char>(text[begin])))
        {
            ++begin;
        }
        while (end > begin && is_space(static_cast<unsigned char>(text[end - 1])))
        {
            --end;
        }
        return text.substr(begin, end - begin);
    };

    MediaType out;
    const std::size_t semicolon = media_type.find(';');
    out.essence = std::string(trim(media_type.substr(0, semicolon)));
    if (semicolon == std::string_view::npos)
    {
        return out;
    }

    // Walk the parameters rather than assuming charset is the first (or only) one.
    std::string_view rest = media_type.substr(semicolon + 1);
    while (!rest.empty())
    {
        const std::size_t next = rest.find(';');
        const std::string_view parameter = trim(rest.substr(0, next));
        const std::size_t equals = parameter.find('=');
        if (equals != std::string_view::npos)
        {
            const std::string_view name = trim(parameter.substr(0, equals));
            if (name.size() == 7 && ascii_lower(name) == "charset")
            {
                std::string_view value = trim(parameter.substr(equals + 1));
                // A quoted parameter value is legal per RFC 9110; unwrap it so the charset field
                // carries the value, not the quotes.
                if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
                {
                    value = value.substr(1, value.size() - 2);
                }
                out.charset = std::string(value);
                return out;
            }
        }
        if (next == std::string_view::npos)
        {
            break;
        }
        rest = rest.substr(next + 1);
    }
    return out;
}

} // namespace context::editor::shell
