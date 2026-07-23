// The native OS network seam for the e14d update check (M9 e14d): one HTTPS GET, and "open this URL
// in the user's browser". Isolated in its own TU for the same reason folder_picker.cpp is — the
// Windows implementation pulls <windows.h> + <winhttp.h> + <shellapi.h>, which must not reach a
// wgpu-native / CEF header (the `near`/`far` macro landmine, conventions.md). Nothing here is a
// header, and no identifier is named `near`/`far`, so the include is contained.
//
// ⚠ THIS FILE ADDS NOTHING TO THE REQUEST. It is the OTHER half of banners.h's privacy argument
// (property (c)): the request VALUE is built once, in banners.cpp, and this file transcribes it —
// method, URL, and exactly `request.headers`, in order. Concretely, three things that a naive HTTPS
// client would smuggle in are switched OFF here, each deliberately:
//
//   * NO DEFAULT USER AGENT. `WinHttpOpen` is called with a NULL agent string, so WinHTTP contributes
//     no `User-Agent` of its own; the only one on the wire is the constant from the request value.
//   * NO ACCEPT-TYPE DEFAULT. `WinHttpOpenRequest` is called with a NULL accept-types list
//     (`WINHTTP_DEFAULT_ACCEPT_TYPES`), so no `Accept: */*` is synthesised alongside ours.
//   * NO AMBIENT CREDENTIALS. Cookies and authentication are disabled outright, and the autologon
//     policy is set to HIGH so Windows never attaches the logged-in user's credentials to an
//     anonymous version query. Those credentials ARE an identifier; a default-configured client
//     would attach them on a 401 without anyone writing a line of code.
//
// `tools/check_release_request.py` (ctest `editor-shell-release-request`) reads this file and refuses
// a re-introduced header literal or a re-enabled ambient-credential path, so the three bullets above
// are gates rather than comments.
//
// THE UN-WIRED PLATFORMS ARE HONEST, NOT SILENT. macOS/Linux have no Shell yet (e12); rather than
// pretend, `native_https_get` returns `ran:false` with a legible reason and the banner reports "not
// checked" — the same honest-gap discipline `native_pick_folder()` and the window backend take.
//
// D10 BOUNDARY-CLEAN: pure OS-SDK calls, no engine-internal link edge.

#include "context/editor/shell/banners.h"

#if defined(_WIN32)
// NOMINMAX: <windows.h> otherwise macro-defines min/max and mangles std::min/std::max at every later
// include (mirrors win32_window.cpp / folder_picker.cpp). WIN32_LEAN_AND_MEAN drops headers this file
// has no use for; winhttp/shellapi are included explicitly below.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// clang-format off
#include <windows.h>
#include <winhttp.h>    // the OS HTTPS client — platform SDK, NOT a third-party dependency
#include <shellapi.h>   // ShellExecuteW, for the downloads-page click-through
// clang-format on
#include <string>
#include <vector>
#endif

namespace context::editor::shell
{

#if defined(_WIN32)

namespace
{

// UTF-8 -> UTF-16. Returns an empty string for empty input (and for anything the OS refuses to
// convert), which every caller below treats as a hard failure rather than as an empty header.
[[nodiscard]] std::wstring widen(const std::string& text)
{
    if (text.empty())
    {
        return {};
    }
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                             static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0)
    {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    const int written = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                              static_cast<int>(text.size()), wide.data(), needed);
    if (written <= 0)
    {
        return {};
    }
    return wide;
}

// A tiny RAII wrapper so every early return closes its handles. WinHTTP handles are not HANDLEs and
// need WinHttpCloseHandle, so std::unique_ptr with a custom deleter would be noisier than this.
class WinHttpHandle
{
public:
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET handle) noexcept : handle_(handle) {}
    ~WinHttpHandle()
    {
        if (handle_ != nullptr)
        {
            (void)::WinHttpCloseHandle(handle_);
        }
    }

    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    WinHttpHandle(WinHttpHandle&&) = delete;
    WinHttpHandle& operator=(WinHttpHandle&&) = delete;

    [[nodiscard]] HINTERNET get() const noexcept { return handle_; }
    [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }

private:
    HINTERNET handle_ = nullptr;
};

[[nodiscard]] HttpResponse failure(std::string reason)
{
    HttpResponse response;
    response.ran = false;
    response.error = std::move(reason);
    return response;
}

} // namespace

HttpResponse native_https_get(const HttpRequest& request)
{
    const std::wstring url = widen(request.url);
    if (url.empty())
    {
        return failure("the release-check URL could not be encoded");
    }

    // Split the URL. WinHttpCrackUrl writes pointers INTO `url`, so `url` must outlive `components`.
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (::WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &components) == FALSE)
    {
        return failure("the release-check URL could not be parsed");
    }
    if (components.nScheme != INTERNET_SCHEME_HTTPS)
    {
        // Fail closed: this seam exists to make ONE HTTPS request, and a plaintext downgrade would
        // put the (constant, identifier-free) query on the wire in the clear for no benefit.
        return failure("the release check refuses a non-HTTPS URL");
    }

    const std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength > 0)
    {
        path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    if (path.empty())
    {
        path = L"/";
    }

    // NULL agent => WinHTTP contributes NO User-Agent of its own (see the file header).
    WinHttpHandle session(::WinHttpOpen(nullptr, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.valid())
    {
        return failure("the OS HTTPS client could not be opened");
    }
    (void)::WinHttpSetTimeouts(session.get(), kReleaseCheckTimeoutMs, kReleaseCheckTimeoutMs,
                               kReleaseCheckTimeoutMs, kReleaseCheckTimeoutMs);

    WinHttpHandle connection(::WinHttpConnect(session.get(), host.c_str(),
                                              INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection.valid())
    {
        return failure("the release feed host could not be reached");
    }

    const std::wstring method = widen(request.method);
    // NULL accept types => no synthesised `Accept: */*` (see the file header).
    WinHttpHandle call(::WinHttpOpenRequest(connection.get(), method.c_str(), path.c_str(), nullptr,
                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            WINHTTP_FLAG_SECURE));
    if (!call.valid())
    {
        return failure("the release-check request could not be created");
    }

    // No cookies, no authentication, and never the logged-in user's credentials (see the header).
    DWORD disabled = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    (void)::WinHttpSetOption(call.get(), WINHTTP_OPTION_DISABLE_FEATURE, &disabled,
                             sizeof(disabled));
    DWORD autologon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    (void)::WinHttpSetOption(call.get(), WINHTTP_OPTION_AUTOLOGON_POLICY, &autologon,
                             sizeof(autologon));

    // EXACTLY the request's headers, in order — the only bytes this file contributes to the wire.
    std::wstring headers;
    for (const HttpHeader& header : request.headers)
    {
        headers += widen(header.name);
        headers += L": ";
        headers += widen(header.value);
        headers += L"\r\n";
    }
    if (!headers.empty())
    {
        if (::WinHttpAddRequestHeaders(call.get(), headers.c_str(),
                                       static_cast<DWORD>(headers.size()),
                                       WINHTTP_ADDREQ_FLAG_ADD) == FALSE)
        {
            return failure("the release-check headers were refused by the OS HTTPS client");
        }
    }

    if (::WinHttpSendRequest(call.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
                             0, 0, 0) == FALSE)
    {
        return failure("the release check could not be sent");
    }
    if (::WinHttpReceiveResponse(call.get(), nullptr) == FALSE)
    {
        return failure("the release feed did not answer");
    }

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (::WinHttpQueryHeaders(call.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                              WINHTTP_NO_HEADER_INDEX) == FALSE)
    {
        return failure("the release feed returned no status");
    }

    HttpResponse response;
    response.ran = true;
    response.status = static_cast<int>(status);

    // Bounded read: a release document is a few KB, and an unbounded body from a remote host is not
    // something a notify-only banner should ever pull into memory.
    std::vector<char> chunk(16u * 1024u);
    for (;;)
    {
        DWORD available = 0;
        if (::WinHttpQueryDataAvailable(call.get(), &available) == FALSE)
        {
            return failure("the release feed response could not be read");
        }
        if (available == 0)
        {
            break;
        }
        const DWORD want = available > static_cast<DWORD>(chunk.size())
                               ? static_cast<DWORD>(chunk.size())
                               : available;
        DWORD read = 0;
        if (::WinHttpReadData(call.get(), chunk.data(), want, &read) == FALSE)
        {
            return failure("the release feed response could not be read");
        }
        if (read == 0)
        {
            break;
        }
        if (response.body.size() + read > kMaxReleaseBodyBytes)
        {
            return failure("the release feed response was oversized");
        }
        response.body.append(chunk.data(), read);
    }
    return response;
}

bool native_open_url(const std::string& url)
{
    // Only ever the downloads page, and only ever https — a click-through that could be pointed at
    // an arbitrary scheme would be a launcher for whatever a compromised release feed named.
    if (url.rfind("https://", 0) != 0)
    {
        return false;
    }
    const std::wstring wide = widen(url);
    if (wide.empty())
    {
        return false;
    }
    // ShellExecuteW returns a value > 32 on success (a legacy HINSTANCE-shaped status).
    const HINSTANCE result = ::ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr,
                                             SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

#else

// The honest gap until e12 brings the macOS/Linux shells up — the same shape native_pick_folder()
// takes on these platforms. Reporting "not wired" is what lets the banner say "not checked" instead
// of silently claiming the build is current.
HttpResponse native_https_get(const HttpRequest&)
{
    HttpResponse response;
    response.ran = false;
    response.error = "the update check is not wired on this platform yet";
    return response;
}

bool native_open_url(const std::string&)
{
    return false;
}

#endif

} // namespace context::editor::shell
