// spikes/dockview-cef — M9 spike s1: Dockview v7 ratified inside a REAL CEF 149 host under a
// strict no-inline-script CSP served from a CUSTOM scheme (context-editor://, not file://), with
// sandboxed-iframe panel content. THROWAWAY proof code — the ratified DECISIONS + ../FINDINGS.md
// are the deliverable, not this host.
//
// This host reuses spikes/cef-compositing's CEF-149 lifecycle idioms (CefMainArgs +
// CefExecuteProcess subprocess re-entry, no_sandbox, single-threaded CefDoMessageLoopWork pump,
// orderly CloseBrowser->OnBeforeClose->CefShutdown teardown, MSVC-only gating). It adds the two
// genuinely CEF-149-specific measurements the headless-Chromium browser driver (tools/run_probes.py)
// CANNOT make:
//   * probe 1 residual — serve the harness from the pinned custom scheme context-editor://
//     (STANDARD|SECURE|CORS_ENABLED, registered in ALL processes) with the CSP as a real response
//     header, proving Dockview runs under a custom-scheme secure origin, not just http://;
//   * probe 5 — the OS renderer-process count for N distinct-origin (context-ext://<id>) sandboxed
//     iframes, i.e. Chromium's IsolateSandboxedIframes behavior under CEF 149 (a feature DEFAULT,
//     not a CEF contract — we record what actually happens).
// The page (web/index.html + probes.js) is loaded windowless (OSR, no GPU needed), runs the 6-probe
// matrix, and reports its verdict on the console as "__PROBE_RESULT__ <json>", captured here via
// OnConsoleMessage. Exit code mirrors the harness verdict.
//
// LOCAL BUILD (Windows/MSVC only, Release — the minimal CEF distro has no Debug libs). Like
// cef-compositing this is NEVER built in CI (the ~162 MB CEF download is doubly-gated); the CI
// bench job configure-exercises the CMake early-return only:
//   cmake -S src -B src/build/dvspike -DCONTEXT_BUILD_SPIKE_DOCKVIEW=ON
//   cmake --build src/build/dvspike --config Release --target dockview-cef-spike
//   src/build/dvspike/spikes/dockview-cef/Release/dockview-cef-spike.exe

#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_parser.h"
#include "include/cef_scheme.h"
#include "include/wrapper/cef_helpers.h"

namespace {

constexpr char kScheme[] = "context-editor";
constexpr char kHost[] = "app";

// Strict no-inline-SCRIPT CSP — the security-critical directive (design 08). Sent as a REAL
// response header for the harness documents (the production editor-core path), so the probe
// exercises header-delivered CSP from a custom-scheme secure origin, not just the <meta> fallback.
constexpr char kCsp[] =
    "default-src 'none'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; "
    "font-src 'self'; connect-src 'self'; frame-src 'self'; child-src 'self'; base-uri 'none'; "
    "form-action 'none'";

std::atomic<int> g_exit{-1};    // set from the __PROBE_RESULT__ console sentinel
std::string g_verdictJson;      // the raw verdict JSON (written to findings-verdict.json)
bool g_loadError = false;

std::wstring exeDir() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring s(buf);
    return s.substr(0, s.find_last_of(L"\\/"));
}

std::string webDirUtf8() {
    std::wstring dir = exeDir() + L"\\web";
    char utf8[MAX_PATH * 3]{};
    WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
    return std::string(utf8);
}

// Count OUR renderer subprocesses (CefExecuteProcess re-entry spawns child copies of this exe for
// renderer/GPU/utility roles). Distinct-origin sandboxed iframes under IsolateSandboxedIframes get
// their own renderer process, so this count is the observable for probe 5. A raw child-process
// count is a proxy (it also counts GPU/utility); FINDINGS interprets it against a control run.
int childProcessCount() {
    DWORD self = GetCurrentProcessId();
    wchar_t selfImg[MAX_PATH]{};
    GetModuleFileNameW(nullptr, selfImg, MAX_PATH);
    std::wstring base(selfImg);
    base = base.substr(base.find_last_of(L"\\/") + 1);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return -1;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    int count = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ParentProcessID == self && base == pe.szExeFile) count++;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return count;
}

std::string mimeFor(const std::string& path) {
    if (path.ends_with(".html")) return "text/html";
    if (path.ends_with(".js")) return "text/javascript";
    if (path.ends_with(".css")) return "text/css";
    if (path.ends_with(".json")) return "application/json";
    return "application/octet-stream";
}

// ------------------------------------------------------------ custom-scheme resource handler

class WebResourceHandler : public CefResourceHandler {
public:
    bool Open(CefRefPtr<CefRequest> request, bool& handle_self, CefRefPtr<CefCallback>) override {
        handle_self = true;
        CefURLParts parts;
        CefParseURL(request->GetURL(), parts);
        std::string path = CefString(&parts.path).ToString(); // e.g. "/index.html"
        if (path.empty() || path == "/") path = "/index.html";
        if (path.find("..") != std::string::npos) { ok_ = false; return true; } // path-traversal guard
        std::string full = webDirUtf8() + path;
        for (auto& c : full) if (c == '/') c = '\\';
        std::ifstream f(full, std::ios::binary);
        if (!f) { ok_ = false; return true; }
        std::ostringstream ss;
        ss << f.rdbuf();
        data_ = ss.str();
        mime_ = mimeFor(path);
        isHtml_ = (mime_ == "text/html");
        ok_ = true;
        return true;
    }

    void GetResponseHeaders(CefRefPtr<CefResponse> response, int64_t& length, CefString&) override {
        if (!ok_) { response->SetStatus(404); response->SetStatusText("Not Found"); length = 0; return; }
        response->SetStatus(200);
        response->SetMimeType(mime_);
        CefResponse::HeaderMap headers;
        response->GetHeaderMap(headers);
        // The security-critical part: deliver the strict CSP as a real response header for docs.
        if (isHtml_) headers.insert(std::make_pair("Content-Security-Policy", kCsp));
        headers.insert(std::make_pair("Cache-Control", "no-store"));
        response->SetHeaderMap(headers);
        length = static_cast<int64_t>(data_.size());
    }

    bool Read(void* out, int bytes, int& read, CefRefPtr<CefResourceReadCallback>) override {
        if (offset_ >= data_.size()) { read = 0; return false; }
        int n = static_cast<int>((std::min)(static_cast<size_t>(bytes), data_.size() - offset_));
        memcpy(out, data_.data() + offset_, static_cast<size_t>(n));
        offset_ += static_cast<size_t>(n);
        read = n;
        return true;
    }

    void Cancel() override {}

private:
    std::string data_, mime_;
    size_t offset_ = 0;
    bool ok_ = false;
    bool isHtml_ = false;
    IMPLEMENT_REFCOUNTING(WebResourceHandler);
};

class WebSchemeFactory : public CefSchemeHandlerFactory {
public:
    CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                                         const CefString&, CefRefPtr<CefRequest>) override {
        return new WebResourceHandler();
    }
    IMPLEMENT_REFCOUNTING(WebSchemeFactory);
};

// ------------------------------------------------------------------------- client / app

class SpikeClient : public CefClient,
                    public CefLifeSpanHandler,
                    public CefLoadHandler,
                    public CefDisplayHandler,
                    public CefRenderHandler {
public:
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }

    // Windowless: a fixed viewport, no painting (we only need JS to run).
    void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override { rect.Set(0, 0, 1280, 800); }
    void OnPaint(CefRefPtr<CefBrowser>, PaintElementType, const RectList&, const void*, int,
                 int) override {}

    bool OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t, const CefString& message,
                          const CefString&, int) override {
        std::string m = message.ToString();
        const std::string tag = "__PROBE_RESULT__ ";
        auto pos = m.find(tag);
        if (pos != std::string::npos) {
            g_verdictJson = m.substr(pos + tag.size());
            // exit code is meta.exit in the JSON; a cheap scan avoids a JSON dep in the spike.
            auto ep = g_verdictJson.find("\"exit\":");
            g_exit = (ep != std::string::npos) ? atoi(g_verdictJson.c_str() + ep + 7) : 1;
        }
        return false;
    }

    void OnLoadError(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, ErrorCode code,
                     const CefString&, const CefString&) override {
        if (frame->IsMain() && code != ERR_ABORTED) g_loadError = true;
    }

    void OnBeforeClose(CefRefPtr<CefBrowser>) override { closed_ = true; }
    bool closed() const { return closed_; }

private:
    bool closed_ = false;
    IMPLEMENT_REFCOUNTING(SpikeClient);
};

class SpikeApp : public CefApp, public CefBrowserProcessHandler {
public:
    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }

    // Registered in EVERY process (browser + renderer + others) — required for a working scheme.
    void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override {
        // STANDARD (origin semantics) | SECURE (treated as a trustworthy origin — strict-origin
        // CSP + secure-context features) | CORS_ENABLED — the design 04 §5 / 08 pin.
        registrar->AddCustomScheme(kScheme, CEF_SCHEME_OPTION_STANDARD | CEF_SCHEME_OPTION_SECURE |
                                                CEF_SCHEME_OPTION_CORS_ENABLED);
    }

    void OnContextInitialized() override {
        CefRegisterSchemeHandlerFactory(kScheme, kHost, new WebSchemeFactory());
    }

    IMPLEMENT_REFCOUNTING(SpikeApp);
};

} // namespace

int main(int, char**) {
    CefMainArgs mainArgs(GetModuleHandleW(nullptr));
    CefRefPtr<SpikeApp> app(new SpikeApp());

    // Subprocess re-entry (renderer/GPU/utility reuse this exe). MUST pass `app` so every process
    // registers the custom scheme.
    int code = CefExecuteProcess(mainArgs, app.get(), nullptr);
    if (code >= 0) return code;

    CefSettings settings;
    settings.no_sandbox = 1;
    settings.windowless_rendering_enabled = 1;
    settings.multi_threaded_message_loop = 0;
    settings.log_severity = LOGSEVERITY_WARNING;
    std::wstring dir = exeDir();
    CefString(&settings.root_cache_path) = dir + L"\\cef-cache";
    CefString(&settings.log_file) = dir + L"\\cef.log";
    if (!CefInitialize(mainArgs, settings, app.get(), nullptr)) {
        std::fprintf(stderr, "CefInitialize failed\n");
        return 2;
    }

    CefWindowInfo wi;
    wi.SetAsWindowless(nullptr);
    CefBrowserSettings bs;
    bs.windowless_frame_rate = 30;
    CefRefPtr<SpikeClient> client(new SpikeClient());
    std::string url = std::string(kScheme) + "://" + kHost + "/index.html";
    CefRefPtr<CefBrowser> browser =
        CefBrowserHost::CreateBrowserSync(wi, client.get(), url, bs, nullptr, nullptr);
    if (!browser) {
        std::fprintf(stderr, "CreateBrowserSync failed\n");
        CefShutdown();
        return 2;
    }

    // Pump until the harness reports (g_exit set) or a hard 45s safety stop.
    ULONGLONG t0 = GetTickCount64();
    while (g_exit.load() < 0 && !g_loadError && (GetTickCount64() - t0) < 45000ULL) {
        CefDoMessageLoopWork();
        Sleep(4);
    }
    int childrenAtVerdict = childProcessCount(); // probe 5 observable at verdict time

    // Persist the verdict + process-count evidence next to the exe for FINDINGS.
    {
        std::ofstream out(std::string(webDirUtf8()) + "\\..\\findings-verdict.json", std::ios::binary);
        out << "{\"renderer_child_processes\":" << childrenAtVerdict << ",\"load_error\":"
            << (g_loadError ? "true" : "false") << ",\"verdict\":"
            << (g_verdictJson.empty() ? "null" : g_verdictJson) << "}\n";
    }
    std::fprintf(stderr, "[dockview-cef-spike] child_processes=%d load_error=%d exit=%d\n",
                 childrenAtVerdict, g_loadError ? 1 : 0, g_exit.load());

    browser->GetHost()->CloseBrowser(true);
    ULONGLONG c0 = GetTickCount64();
    while (!client->closed() && (GetTickCount64() - c0) < 5000ULL) {
        CefDoMessageLoopWork();
        Sleep(2);
    }
    CefShutdown();

    if (g_loadError) return 2;
    int e = g_exit.load();
    return e < 0 ? 1 : e; // no verdict = fail
}
