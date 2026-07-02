// spikes/cef-compositing — engine-side D3D11 renderer: spinning-triangle "viewport" plus a
// UI overlay texture fed either from CEF's accelerated-OSR shared handle (open + CopyResource)
// or from the software-OSR BGRA buffer (UpdateSubresource). THROWAWAY spike code.
#pragma once

#include <windows.h>

#include <d3d11_1.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <unordered_map>

struct UiUpdateStats {
    double cpu_us = 0.0;   // CPU cost of the update call (open+copy / upload)
    int width = 0;
    int height = 0;
    bool reopened = false; // accelerated path: shared handle was (re)opened this call
};

class D3D11Renderer {
public:
    bool init(HWND hwnd, int width, int height, std::string* err);
    void shutdown();

    // Swapchain resize (WM_SIZE). Returns false on device loss.
    bool resize(int width, int height);

    // Clear + spinning triangle + (optionally) UI overlay composite, then Present.
    // Returns false on device removal.
    bool renderAndPresent(float angleRad, bool drawUi, bool vsync);

    // Accelerated OSR: open CEF's shared NT handle (cached per handle) and CopyResource into
    // the private UI texture. Called on the CEF UI thread == main thread (single-pumped loop).
    // visW/visH = CEF's visible_rect (coded_size may be padded larger); <=0 means "full size".
    bool updateUiFromSharedHandle(HANDLE sharedHandle, int visW, int visH, UiUpdateStats* stats);

    // Software OSR: upload the BGRA buffer (full frame; dirty-rect granularity measured by
    // the caller via CEF's dirtyRects — the upload here takes the full-surface worst case
    // when rects cover everything, or per-rect boxes when they don't).
    struct DirtyRect { int x, y, w, h; };
    bool updateUiFromBuffer(const void* bgra, int width, int height,
                            const DirtyRect* rects, size_t rectCount, UiUpdateStats* stats);

    // Read one pixel back from the UI texture (BGRA byte order as 0xAARRGGBB value).
    bool sampleUiPixel(int x, int y, uint32_t* outArgb);

    // Dump the current backbuffer to a 32-bpp BMP (composite proof artifacts).
    bool dumpBackbufferBmp(const std::wstring& path);

    int uiWidth() const { return uiWidth_; }
    int uiHeight() const { return uiHeight_; }
    bool deviceRemoved() const { return deviceRemoved_; }
    std::string adapterDescription() const { return adapterDesc_; }

private:
    bool ensureUiTexture(int width, int height, bool asCopyDest);
    bool compileShaders(std::string* err);

    template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    HWND hwnd_ = nullptr;
    int width_ = 0, height_ = 0;
    bool deviceRemoved_ = false;
    std::string adapterDesc_;

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11Device1> device1_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGISwapChain1> swapchain_;
    ComPtr<ID3D11RenderTargetView> rtv_;

    // Triangle pipeline.
    ComPtr<ID3D11VertexShader> triVs_;
    ComPtr<ID3D11PixelShader> triPs_;
    ComPtr<ID3D11InputLayout> triLayout_;
    ComPtr<ID3D11Buffer> triVb_;
    ComPtr<ID3D11Buffer> triCb_;

    // UI composite pipeline (fullscreen triangle, premultiplied-alpha blend).
    ComPtr<ID3D11VertexShader> uiVs_;
    ComPtr<ID3D11PixelShader> uiPs_;
    ComPtr<ID3D11SamplerState> uiSampler_;
    ComPtr<ID3D11BlendState> uiBlend_;
    ComPtr<ID3D11Buffer> uiCb_; // uv scale: visible_rect / coded_size

    // Private UI texture the composite pass samples.
    ComPtr<ID3D11Texture2D> uiTex_;
    ComPtr<ID3D11ShaderResourceView> uiSrv_;
    int uiWidth_ = 0, uiHeight_ = 0;   // coded (texture) size
    int uiVisW_ = 0, uiVisH_ = 0;      // visible size

    // Accelerated path: cache of opened shared textures keyed by handle value (CEF rotates a
    // small pool of textures; reopening every frame would double the per-paint CPU cost).
    struct SharedEntry {
        ComPtr<ID3D11Texture2D> tex;
        int width = 0, height = 0;
    };
    std::unordered_map<HANDLE, SharedEntry> sharedCache_;

public:
    // Resize invalidates CEF's texture pool; drop stale cached handles.
    void clearSharedCache() { sharedCache_.clear(); }
};
