// spikes/cef-compositing — L-41 fifth M0 spike (Windows leg).
//
// Engine-owned Win32 window + D3D11 spinning-triangle "viewport"; CEF runs in windowless
// (offscreen) mode with shared_texture_enabled, delivering its composited UI frames as D3D11
// NT shared handles via CefRenderHandler::OnAcceleratedPaint; the engine opens the handle,
// copies into a private texture, and alpha-composites UI-over-viewport every frame.
// --software switches to the classic software-OSR path (OnPaint BGRA buffer -> texture upload)
// so the fallback cost delta is measured, not estimated.
//
// The run is self-driving (autotest): synthetic WM_* input posted through the REAL WndProc
// forwarding path proves hover/click/key round-trips (verified by JS-set document.title events
// AND by pixel readback from the composited UI texture); two window resizes prove re-layout.
// Auto-closes after the script (~12 s) — never a long-lived window on the shared dev box.
//
// Threading model: multi_threaded_message_loop=false + CefDoMessageLoopWork() pumped from the
// render loop, so every CEF callback lands on the main thread (no locking). A production
// integration would likely run CEF's UI thread separately (multi_threaded_message_loop=true)
// and hand textures across with a mutex; that is an integration detail, not seam risk.
//
// THROWAWAY spike code. Results: ../FINDINGS.md.

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_render_handler.h"
#include "include/wrapper/cef_helpers.h"

#include "renderer_d3d11.hpp"

// windowsx.h's GET_X/Y_LPARAM equivalents — windowsx.h itself defines GetFirstChild/
// GetNextSibling macros that shred cef_dom.h, so it must not be included alongside CEF.
#define SPIKE_GET_X_LPARAM(lp) (static_cast<int>(static_cast<short>(LOWORD(lp))))
#define SPIKE_GET_Y_LPARAM(lp) (static_cast<int>(static_cast<short>(HIWORD(lp))))

namespace {

// ---------------------------------------------------------------------------------- utilities

LONGLONG qpcFreq() {
    static LONGLONG f = [] {
        LARGE_INTEGER v;
        QueryPerformanceFrequency(&v);
        return v.QuadPart;
    }();
    return f;
}

LONGLONG qpcNow() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

double msSince(LONGLONG t0) {
    return static_cast<double>(qpcNow() - t0) * 1000.0 / static_cast<double>(qpcFreq());
}

struct Percentiles {
    double avg = 0, p50 = 0, p95 = 0, p99 = 0, mx = 0;
};

Percentiles percentiles(std::vector<double> v) {
    Percentiles p;
    if (v.empty()) return p;
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double d : v) sum += d;
    p.avg = sum / static_cast<double>(v.size());
    auto at = [&](double q) { return v[static_cast<size_t>(q * (v.size() - 1))]; };
    p.p50 = at(0.50);
    p.p95 = at(0.95);
    p.p99 = at(0.99);
    p.mx = v.back();
    return p;
}

bool colorNear(uint32_t argb, int r, int g, int b, int tol = 16) {
    int pr = (argb >> 16) & 0xff, pg = (argb >> 8) & 0xff, pb = argb & 0xff;
    return std::abs(pr - r) <= tol && std::abs(pg - g) <= tol && std::abs(pb - b) <= tol;
}

std::wstring exeDir() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring s(buf);
    return s.substr(0, s.find_last_of(L"\\/"));
}

// ------------------------------------------------------------------------------- shared state

struct LatencyProbe {
    LONGLONG t0 = 0;
    double title_ms = -1; // input -> JS handler ran (document.title round-trip observed)
    double pixel_ms = -1; // input -> new pixels in the composited UI texture
};

struct AppState {
    HWND hwnd = nullptr;
    D3D11Renderer* renderer = nullptr;
    CefRefPtr<CefBrowser> browser;

    bool softwareMode = false;
    bool vsync = true;
    int windowlessFps = 60;
    std::wstring dumpDir;
    std::string mode() const { return softwareMode ? "software" : "accelerated"; }

    int clientW = 1280, clientH = 800;
    bool browserClosed = false;
    bool loadEnded = false;
    bool quit = false;

    // Measurements.
    std::vector<double> frameMs;       // present-to-present, measurement window only
    std::vector<double> uiUpdateUs;    // per-paint open+copy (accel) / upload (software) CPU us
    int uiPaintCount = 0;              // paints inside measurement window
    LONGLONG measureStart = 0, measureEnd = 0;
    int reopenCount = 0;               // accel: shared-handle pool (re)opens
    int paintCountTotal = 0;
    int acceleratedPaintCountTotal = 0;

    LatencyProbe hover, click, key, click2;
    uint32_t pxBaseline = 0, pxHover = 0, pxClicked = 0;
    bool pixelPass = false;

    // Resize checks.
    int resizePaintsToNewSize = -1;
    bool resizeConverged = false;
    bool resize2Converged = false;
    int pendingPaintW = 0, pendingPaintH = 0;

    std::vector<std::string> errors;
};

AppState g;

// Test-page geometry (ui.html anchors these top-left, so client coords == page coords).
constexpr int kBtnX = 140, kBtnY = 60;     // button center
constexpr int kStateX = 140, kStateY = 110; // state-strip center

// ------------------------------------------------------------------------------- CEF client

class OsrClient : public CefClient,
                  public CefRenderHandler,
                  public CefDisplayHandler,
                  public CefLifeSpanHandler,
                  public CefLoadHandler {
public:
    CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }

    void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override {
        rect.Set(0, 0, std::max(g.clientW, 1), std::max(g.clientH, 1));
    }

    bool GetScreenInfo(CefRefPtr<CefBrowser> browser, CefScreenInfo& info) override {
        CefRect view;
        GetViewRect(browser, view);
        info.device_scale_factor = 1.0f; // CSS px == device px == client px (spike simplicity)
        info.rect = view;
        info.available_rect = view;
        return true;
    }

    void OnAcceleratedPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList&,
                            const CefAcceleratedPaintInfo& info) override {
        if (type != PET_VIEW || !g.renderer) return;
        g.acceleratedPaintCountTotal++;
        UiUpdateStats stats;
        int visW = info.extra.visible_rect.width > 0 ? info.extra.visible_rect.width : 0;
        int visH = info.extra.visible_rect.height > 0 ? info.extra.visible_rect.height : 0;
        if (!g.renderer->updateUiFromSharedHandle(info.shared_texture_handle, visW, visH,
                                                  &stats)) {
            g.errors.push_back("updateUiFromSharedHandle failed");
            return;
        }
        if (stats.reopened) g.reopenCount++;
        notePaint(stats);
    }

    void OnPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList& dirtyRects,
                 const void* buffer, int width, int height) override {
        if (type != PET_VIEW || !g.renderer) return;
        // Accelerated mode never lands here; software mode always does.
        std::vector<D3D11Renderer::DirtyRect> rects;
        rects.reserve(dirtyRects.size());
        bool full = false;
        for (const auto& r : dirtyRects) {
            if (r.width >= width && r.height >= height) full = true;
            rects.push_back({r.x, r.y, r.width, r.height});
        }
        UiUpdateStats stats;
        if (!g.renderer->updateUiFromBuffer(buffer, width, height,
                                            full ? nullptr : rects.data(),
                                            full ? 0 : rects.size(), &stats)) {
            g.errors.push_back("updateUiFromBuffer failed");
            return;
        }
        notePaint(stats);
    }

    void OnTitleChange(CefRefPtr<CefBrowser>, const CefString& title) override {
        // Page protocol: document.title = "EV|<event>|<seq>"
        std::string t = title.ToString();
        if (t.rfind("EV|", 0) != 0) return;
        std::string ev = t.substr(3, t.find('|', 3) - 3);
        auto stamp = [](LatencyProbe& p) {
            if (p.t0 && p.title_ms < 0) p.title_ms = msSince(p.t0);
        };
        if (ev == "hover") stamp(g.hover);
        else if (ev == "click") { stamp(g.click); stamp(g.click2); }
        else if (ev == "key") stamp(g.key);
    }

    void OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame, int) override {
        if (frame->IsMain()) g.loadEnded = true;
    }

    void OnBeforeClose(CefRefPtr<CefBrowser>) override {
        g.browser = nullptr;
        g.browserClosed = true;
    }

private:
    void notePaint(const UiUpdateStats& stats) {
        g.paintCountTotal++;
        g.pendingPaintW = stats.width;
        g.pendingPaintH = stats.height;
        LONGLONG now = qpcNow();
        if (g.measureStart && (!g.measureEnd || now < g.measureEnd)) {
            g.uiPaintCount++;
            g.uiUpdateUs.push_back(stats.cpu_us);
        }
    }

    IMPLEMENT_REFCOUNTING(OsrClient);
};

// --------------------------------------------------------------------------- input forwarding

uint32_t mouseModifiers(WPARAM wParam) {
    uint32_t mod = 0;
    if (wParam & MK_LBUTTON) mod |= EVENTFLAG_LEFT_MOUSE_BUTTON;
    if (wParam & MK_RBUTTON) mod |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
    if (wParam & MK_MBUTTON) mod |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
    if (wParam & MK_CONTROL) mod |= EVENTFLAG_CONTROL_DOWN;
    if (wParam & MK_SHIFT) mod |= EVENTFLAG_SHIFT_DOWN;
    return mod;
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    CefRefPtr<CefBrowserHost> host =
        g.browser ? g.browser->GetHost() : CefRefPtr<CefBrowserHost>();
    switch (msg) {
        case WM_SIZE: {
            if (wParam == SIZE_MINIMIZED) return 0;
            int w = LOWORD(lParam), h = HIWORD(lParam);
            if (w == g.clientW && h == g.clientH) return 0;
            g.clientW = w;
            g.clientH = h;
            if (g.renderer && !g.renderer->resize(w, h)) {
                g.errors.push_back("swapchain resize -> device removed");
            }
            if (g.renderer) g.renderer->clearSharedCache(); // CEF reallocates its texture pool
            if (host) host->WasResized();
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (host) {
                CefMouseEvent e;
                e.x = SPIKE_GET_X_LPARAM(lParam);
                e.y = SPIKE_GET_Y_LPARAM(lParam);
                e.modifiers = mouseModifiers(wParam);
                host->SendMouseMoveEvent(e, false);
            }
            return 0;
        }
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP: {
            if (host) {
                CefMouseEvent e;
                e.x = SPIKE_GET_X_LPARAM(lParam);
                e.y = SPIKE_GET_Y_LPARAM(lParam);
                e.modifiers = mouseModifiers(wParam);
                bool up = (msg == WM_LBUTTONUP || msg == WM_RBUTTONUP);
                cef_mouse_button_type_t btn =
                    (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) ? MBT_LEFT : MBT_RIGHT;
                host->SendMouseClickEvent(e, btn, up, 1);
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            if (host) {
                POINT pt{SPIKE_GET_X_LPARAM(lParam), SPIKE_GET_Y_LPARAM(lParam)};
                ScreenToClient(hwnd, &pt);
                CefMouseEvent e;
                e.x = pt.x;
                e.y = pt.y;
                e.modifiers = mouseModifiers(GET_KEYSTATE_WPARAM(wParam));
                host->SendMouseWheelEvent(e, 0, GET_WHEEL_DELTA_WPARAM(wParam));
            }
            return 0;
        }
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_CHAR: {
            if (host) {
                CefKeyEvent e;
                e.windows_key_code = static_cast<int>(wParam);
                e.native_key_code = static_cast<int>(lParam);
                e.is_system_key = false;
                e.type = (msg == WM_KEYDOWN)  ? KEYEVENT_RAWKEYDOWN
                         : (msg == WM_KEYUP) ? KEYEVENT_KEYUP
                                             : KEYEVENT_CHAR;
                e.modifiers = 0;
                host->SendKeyEvent(e);
            }
            return 0;
        }
        case WM_CLOSE:
            g.quit = true;
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void pumpWin32() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) g.quit = true;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// -------------------------------------------------------------------------------- autotest

// Wall-clock scripted driver. Every input goes through SendMessage -> wndProc -> CEF, i.e.
// the exact path a user's mouse takes once the OS delivers the message.
class Autotest {
public:
    void run(double tSec, D3D11Renderer& r) {
        switch (stage_) {
            case 0: // settle, take baseline pixel
                if (tSec > 2.0) {
                    r.sampleUiPixel(kBtnX, kBtnY, &g.pxBaseline);
                    advance("baseline");
                }
                break;
            case 1: // hover
                if (tSec > 2.5) {
                    g.hover.t0 = qpcNow();
                    SendMessageW(g.hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(kBtnX, kBtnY));
                    advance("hover-sent");
                }
                break;
            case 2: { // poll for hover pixel (button #2266CC -> #FF8800)
                uint32_t px = 0;
                if (r.sampleUiPixel(kBtnX, kBtnY, &px) && colorNear(px, 0xFF, 0x88, 0x00)) {
                    g.pxHover = px;
                    g.hover.pixel_ms = msSince(g.hover.t0);
                    advance("hover-pixel");
                } else if (msSince(g.hover.t0) > 2500) {
                    g.pxHover = px;
                    g.errors.push_back("hover pixel never changed");
                    advance("hover-timeout");
                }
                break;
            }
            case 3: // click
                if (tSec > 4.5) {
                    g.click.t0 = qpcNow();
                    SendMessageW(g.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(kBtnX, kBtnY));
                    SendMessageW(g.hwnd, WM_LBUTTONUP, 0, MAKELPARAM(kBtnX, kBtnY));
                    advance("click-sent");
                }
                break;
            case 4: { // poll for clicked state strip (#444444 -> #22CC44)
                uint32_t px = 0;
                if (r.sampleUiPixel(kStateX, kStateY, &px) && colorNear(px, 0x22, 0xCC, 0x44)) {
                    g.pxClicked = px;
                    g.click.pixel_ms = msSince(g.click.t0);
                    advance("click-pixel");
                } else if (msSince(g.click.t0) > 2500) {
                    g.pxClicked = px;
                    g.errors.push_back("click pixel never changed");
                    advance("click-timeout");
                }
                break;
            }
            case 5: // key
                if (tSec > 6.0) {
                    g.key.t0 = qpcNow();
                    SendMessageW(g.hwnd, WM_KEYDOWN, 'K', 0);
                    SendMessageW(g.hwnd, WM_CHAR, 'k', 0);
                    SendMessageW(g.hwnd, WM_KEYUP, 'K', 0);
                    advance("key-sent");
                }
                break;
            case 6: // dump composite proof, end clean-measurement window, grow-resize
                if (tSec > 7.0) {
                    g.measureEnd = qpcNow();
                    if (!g.dumpDir.empty()) {
                        r.dumpBackbufferBmp(g.dumpDir + L"\\composite-" +
                                            (g.softwareMode ? L"sw" : L"acc") + L".bmp");
                    }
                    resizeT0_ = qpcNow();
                    paintsAtResize_ = g.paintCountTotal;
                    SetWindowPos(g.hwnd, nullptr, 0, 0, 1620, 940,
                                 SWP_NOMOVE | SWP_NOZORDER); // ~1600x900 client
                    advance("resize1-sent");
                }
                break;
            case 7: // wait for CEF paints at the new size
                if (g.pendingPaintW == g.clientW && g.pendingPaintH == g.clientH &&
                    g.clientW > 1500) {
                    g.resizeConverged = true;
                    g.resizePaintsToNewSize = g.paintCountTotal - paintsAtResize_;
                    advance("resize1-converged");
                } else if (msSince(resizeT0_) > 3000) {
                    g.errors.push_back("resize1 did not converge in 3s");
                    advance("resize1-timeout");
                }
                break;
            case 8: // second click, after resize (UI must still be interactive)
                if (tSec > 8.5) {
                    g.click2.t0 = qpcNow();
                    SendMessageW(g.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(kBtnX, kBtnY));
                    SendMessageW(g.hwnd, WM_LBUTTONUP, 0, MAKELPARAM(kBtnX, kBtnY));
                    advance("click2-sent");
                }
                break;
            case 9: // shrink-resize
                if (tSec > 9.5) {
                    resizeT0_ = qpcNow();
                    SetWindowPos(g.hwnd, nullptr, 0, 0, 1020, 740,
                                 SWP_NOMOVE | SWP_NOZORDER); // ~1000x700 client
                    advance("resize2-sent");
                }
                break;
            case 10: // wait for shrink convergence
                if ((g.pendingPaintW == g.clientW && g.pendingPaintH == g.clientH &&
                     g.clientW < 1100) ||
                    msSince(resizeT0_) > 3000) {
                    g.resize2Converged =
                        (g.pendingPaintW == g.clientW && g.pendingPaintH == g.clientH);
                    settleT0_ = qpcNow();
                    advance("resize2-converged");
                }
                break;
            case 11: // let layout settle past Chromium's resize transient, then dump + quit
                if (msSince(settleT0_) > 500) {
                    if (!g.dumpDir.empty()) {
                        r.dumpBackbufferBmp(g.dumpDir + L"\\resized-" +
                                            (g.softwareMode ? L"sw" : L"acc") + L".bmp");
                    }
                    advance("done");
                    g.quit = true;
                }
                break;
            default:
                break;
        }
    }

private:
    void advance(const char* what) {
        std::fprintf(stderr, "[autotest] %s (t=%.2fs)\n", what,
                     g.measureStart ? msSince(g.measureStart) / 1000.0 : 0.0);
        stage_++;
    }
    int stage_ = 0;
    LONGLONG resizeT0_ = 0;
    LONGLONG settleT0_ = 0;
    int paintsAtResize_ = 0;
};

// ---------------------------------------------------------------------------------- reporting

void printResult() {
    double wallS = g.measureStart && g.measureEnd
                       ? static_cast<double>(g.measureEnd - g.measureStart) /
                             static_cast<double>(qpcFreq())
                       : 0.0;
    Percentiles fr = percentiles(g.frameMs);
    Percentiles up = percentiles(g.uiUpdateUs);
    double fps = (wallS > 0 && !g.frameMs.empty())
                     ? static_cast<double>(g.frameMs.size()) / wallS
                     : 0.0;
    double uiHz = wallS > 0 ? static_cast<double>(g.uiPaintCount) / wallS : 0.0;

    g.pixelPass = colorNear(g.pxBaseline, 0x22, 0x66, 0xCC) &&
                  colorNear(g.pxHover, 0xFF, 0x88, 0x00) &&
                  colorNear(g.pxClicked, 0x22, 0xCC, 0x44);

    std::string errs;
    for (size_t i = 0; i < g.errors.size(); ++i) {
        errs += (i ? "\",\"" : "\"") + g.errors[i];
    }
    if (!errs.empty()) errs += "\"";

    std::printf(
        "RESULT: {\"mode\":\"%s\",\"vsync\":%s,\"adapter\":\"%s\","
        "\"measure_s\":%.2f,\"frames\":%zu,\"fps\":%.1f,"
        "\"frame_ms\":{\"avg\":%.3f,\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f,\"max\":%.3f},"
        "\"ui_paint_hz\":%.1f,\"ui_paints\":%d,\"paints_total\":%d,\"accel_paints_total\":%d,"
        "\"ui_update_us\":{\"avg\":%.1f,\"p50\":%.1f,\"p95\":%.1f,\"p99\":%.1f,\"max\":%.1f},"
        "\"shared_handle_reopens\":%d,"
        "\"latency_ms\":{\"hover_title\":%.1f,\"hover_pixel\":%.1f,\"click_title\":%.1f,"
        "\"click_pixel\":%.1f,\"key_title\":%.1f,\"click2_title\":%.1f},"
        "\"pixel_checks\":{\"baseline\":\"%06x\",\"hover\":\"%06x\",\"clicked\":\"%06x\","
        "\"pass\":%s},"
        "\"resize\":{\"grow_converged\":%s,\"paints_to_new_size\":%d,\"shrink_converged\":%s,"
        "\"device_removed\":%s},"
        "\"errors\":[%s]}\n",
        g.mode().c_str(), g.vsync ? "true" : "false",
        g.renderer ? g.renderer->adapterDescription().c_str() : "?", wallS, g.frameMs.size(),
        fps, fr.avg, fr.p50, fr.p95, fr.p99, fr.mx, uiHz, g.uiPaintCount, g.paintCountTotal,
        g.acceleratedPaintCountTotal, up.avg, up.p50, up.p95, up.p99, up.mx, g.reopenCount,
        g.hover.title_ms, g.hover.pixel_ms, g.click.title_ms, g.click.pixel_ms, g.key.title_ms,
        g.click2.title_ms, g.pxBaseline & 0xffffff, g.pxHover & 0xffffff,
        g.pxClicked & 0xffffff, g.pixelPass ? "true" : "false",
        g.resizeConverged ? "true" : "false", g.resizePaintsToNewSize,
        g.resize2Converged ? "true" : "false",
        (g.renderer && g.renderer->deviceRemoved()) ? "true" : "false", errs.c_str());
    std::fflush(stdout);
}

} // namespace

// -------------------------------------------------------------------------------------- main

int main(int argc, char** argv) {
    CefMainArgs mainArgs(GetModuleHandleW(nullptr));
    // Subprocess re-entry (renderer/GPU/utility processes reuse this exe).
    int exitCode = CefExecuteProcess(mainArgs, nullptr, nullptr);
    if (exitCode >= 0) return exitCode;

    std::string url;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--software") g.softwareMode = true;
        else if (a == "--novsync") g.vsync = false;
        else if (a == "--url" && i + 1 < argc) url = argv[++i];
        else if (a == "--windowless-fps" && i + 1 < argc) g.windowlessFps = atoi(argv[++i]);
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    std::wstring dir = exeDir();
    g.dumpDir = dir;
    if (url.empty()) {
        char dirUtf8[MAX_PATH * 3]{};
        WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, dirUtf8, sizeof(dirUtf8) - 1, nullptr,
                            nullptr);
        std::string d(dirUtf8);
        for (auto& c : d) {
            if (c == '\\') c = '/';
        }
        url = "file:///" + d + "/ui.html";
    }

    // Window with an exact client size.
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"CtxCefSpike";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    RECT wr{0, 0, g.clientW, g.clientH};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
    g.hwnd = CreateWindowW(wc.lpszClassName,
                           g.softwareMode ? L"ctx cef spike (software OSR)"
                                          : L"ctx cef spike (accelerated OSR)",
                           WS_OVERLAPPEDWINDOW, 60, 60, wr.right - wr.left, wr.bottom - wr.top,
                           nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(g.hwnd, SW_SHOWNORMAL);
    RECT cr{};
    GetClientRect(g.hwnd, &cr);
    g.clientW = cr.right;
    g.clientH = cr.bottom;

    D3D11Renderer renderer;
    std::string err;
    if (!renderer.init(g.hwnd, g.clientW, g.clientH, &err)) {
        std::fprintf(stderr, "renderer init failed: %s\n", err.c_str());
        return 2;
    }
    g.renderer = &renderer;
    std::fprintf(stderr, "[spike] adapter: %s\n", renderer.adapterDescription().c_str());

    // CEF init: single-threaded pump; windowless + shared textures unless --software.
    CefSettings settings;
    settings.no_sandbox = 1;
    settings.windowless_rendering_enabled = 1;
    settings.multi_threaded_message_loop = 0;
    settings.background_color = 0; // fully transparent page background
    settings.log_severity = LOGSEVERITY_WARNING;
    CefString(&settings.root_cache_path) = dir + L"\\cef-cache";
    CefString(&settings.cache_path) = dir + L"\\cef-cache";
    CefString(&settings.log_file) = dir + L"\\cef.log";
    if (!CefInitialize(mainArgs, settings, nullptr, nullptr)) {
        std::fprintf(stderr, "CefInitialize failed\n");
        return 2;
    }

    CefWindowInfo wi;
    wi.SetAsWindowless(g.hwnd);
    wi.shared_texture_enabled = g.softwareMode ? 0 : 1;
    CefBrowserSettings bs;
    bs.windowless_frame_rate = g.windowlessFps;
    bs.background_color = 0;
    CefRefPtr<OsrClient> client = new OsrClient();
    g.browser = CefBrowserHost::CreateBrowserSync(wi, client.get(), url, bs, nullptr, nullptr);
    if (!g.browser) {
        std::fprintf(stderr, "CreateBrowserSync failed\n");
        CefShutdown();
        return 2;
    }
    g.browser->GetHost()->SetFocus(true);

    // ------------------------------------------------------------------------- main loop
    Autotest autotest;
    LONGLONG t0 = qpcNow();
    LONGLONG lastPresent = 0;
    const double hardStopS = 30.0; // safety: never outlive 30 s on the shared dev box

    while (!g.quit) {
        pumpWin32();
        CefDoMessageLoopWork();

        double tSec = static_cast<double>(qpcNow() - t0) / static_cast<double>(qpcFreq());
        if (tSec > hardStopS) {
            g.errors.push_back("hard 30s stop hit");
            break;
        }
        if (g.loadEnded && !g.measureStart && tSec > 1.0) {
            g.measureStart = qpcNow(); // measurement window opens after page load settles
        }

        float angle = static_cast<float>(tSec * 1.2);
        if (!renderer.renderAndPresent(angle, true, g.vsync)) {
            g.errors.push_back("device removed during present");
            break;
        }
        LONGLONG now = qpcNow();
        if (lastPresent && g.measureStart && (!g.measureEnd || now < g.measureEnd)) {
            g.frameMs.push_back(static_cast<double>(now - lastPresent) * 1000.0 /
                                static_cast<double>(qpcFreq()));
        }
        lastPresent = now;

        if (g.measureStart) {
            autotest.run(tSec, renderer);
        }
        if (!g.vsync) {
            // Uncapped throughput run: don't melt the shared box; yield the tiniest slice.
            Sleep(0);
        }
    }
    if (!g.measureEnd) g.measureEnd = qpcNow();

    // Orderly CEF teardown: close browser, pump until OnBeforeClose, then shutdown.
    if (g.browser) g.browser->GetHost()->CloseBrowser(true);
    LONGLONG closeT0 = qpcNow();
    while (!g.browserClosed && msSince(closeT0) < 5000.0) {
        pumpWin32();
        CefDoMessageLoopWork();
        Sleep(1);
    }
    renderer.shutdown();
    CefShutdown();

    printResult();
    return g.pixelPass && g.errors.empty() ? 0 : 1;
}
