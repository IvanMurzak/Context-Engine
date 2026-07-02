#include "renderer_d3d11.hpp"

#include <d3dcompiler.h>

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

const char kTriangleHlsl[] = R"(
cbuffer CB : register(b0) { float2 rot; float2 aspect; };
struct VSIn  { float2 pos : POSITION; float3 col : COLOR; };
struct VSOut { float4 pos : SV_Position; float3 col : COLOR; };
VSOut vsmain(VSIn i) {
    float2 p = float2(i.pos.x * rot.x - i.pos.y * rot.y,
                      i.pos.x * rot.y + i.pos.y * rot.x);
    p.x *= aspect.x;
    VSOut o; o.pos = float4(p, 0.0, 1.0); o.col = i.col; return o;
}
float4 psmain(VSOut i) : SV_Target { return float4(i.col, 1.0); }
)";

const char kUiHlsl[] = R"(
cbuffer CB : register(b0) { float2 uvScale; float2 pad; }; // visible_rect / coded_size
Texture2D uiTex : register(t0);
SamplerState smp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vsmain(uint id : SV_VertexID) {
    // Fullscreen triangle.
    float2 uv = float2((id << 1) & 2, id & 2);
    VSOut o; o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o;
}
float4 psmain(VSOut i) : SV_Target { return uiTex.Sample(smp, i.uv * uvScale); } // premultiplied
)";

double qpcUs(LONGLONG t0, LONGLONG t1) {
    static LONGLONG freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    return static_cast<double>(t1 - t0) * 1e6 / static_cast<double>(freq);
}

LONGLONG qpcNow() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

bool compile(const char* src, const char* entry, const char* target,
             ID3DBlob** blob, std::string* err) {
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob, &errors);
    if (FAILED(hr)) {
        if (err) {
            *err = errors ? static_cast<const char*>(errors->GetBufferPointer())
                          : "D3DCompile failed";
        }
        return false;
    }
    return true;
}

} // namespace

bool D3D11Renderer::init(HWND hwnd, int width, int height, std::string* err) {
    hwnd_ = hwnd;
    width_ = width;
    height_ = height;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                   2, D3D11_SDK_VERSION, &device_, &got, &ctx_);
    if (FAILED(hr)) {
        if (err) *err = "D3D11CreateDevice failed";
        return false;
    }
    if (FAILED(device_.As(&device1_))) {
        if (err) *err = "ID3D11Device1 unavailable (needed for OpenSharedResource1)";
        return false;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    device_.As(&dxgiDevice);
    ComPtr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(&adapter);
    DXGI_ADAPTER_DESC ad{};
    adapter->GetDesc(&ad);
    char desc[256]{};
    WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, desc, sizeof(desc) - 1, nullptr, nullptr);
    adapterDesc_ = desc;

    ComPtr<IDXGIFactory2> factory;
    adapter->GetParent(IID_PPV_ARGS(&factory));

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = static_cast<UINT>(width);
    sd.Height = static_cast<UINT>(height);
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    hr = factory->CreateSwapChainForHwnd(device_.Get(), hwnd, &sd, nullptr, nullptr, &swapchain_);
    if (FAILED(hr)) {
        if (err) *err = "CreateSwapChainForHwnd failed";
        return false;
    }

    ComPtr<ID3D11Texture2D> back;
    swapchain_->GetBuffer(0, IID_PPV_ARGS(&back));
    if (FAILED(device_->CreateRenderTargetView(back.Get(), nullptr, &rtv_))) {
        if (err) *err = "CreateRenderTargetView failed";
        return false;
    }

    if (!compileShaders(err)) return false;

    struct Vtx {
        float x, y, r, g, b;
    };
    const Vtx verts[] = {
        {0.0f, 0.62f, 1.0f, 0.25f, 0.25f},
        {0.54f, -0.31f, 0.25f, 1.0f, 0.25f},
        {-0.54f, -0.31f, 0.25f, 0.45f, 1.0f},
    };
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(verts);
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init{verts, 0, 0};
    device_->CreateBuffer(&bd, &init, &triVb_);

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = 16;
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device_->CreateBuffer(&cbd, nullptr, &triCb_);
    device_->CreateBuffer(&cbd, nullptr, &uiCb_);

    D3D11_SAMPLER_DESC smp{};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    device_->CreateSamplerState(&smp, &uiSampler_);

    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; // CEF output is premultiplied
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device_->CreateBlendState(&blend, &uiBlend_);

    return true;
}

bool D3D11Renderer::compileShaders(std::string* err) {
    ComPtr<ID3DBlob> vsb, psb;
    if (!compile(kTriangleHlsl, "vsmain", "vs_5_0", &vsb, err)) return false;
    if (!compile(kTriangleHlsl, "psmain", "ps_5_0", &psb, err)) return false;
    device_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &triVs_);
    device_->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &triPs_);

    D3D11_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    device_->CreateInputLayout(il, 2, vsb->GetBufferPointer(), vsb->GetBufferSize(), &triLayout_);

    ComPtr<ID3DBlob> uvsb, upsb;
    if (!compile(kUiHlsl, "vsmain", "vs_5_0", &uvsb, err)) return false;
    if (!compile(kUiHlsl, "psmain", "ps_5_0", &upsb, err)) return false;
    device_->CreateVertexShader(uvsb->GetBufferPointer(), uvsb->GetBufferSize(), nullptr, &uiVs_);
    device_->CreatePixelShader(upsb->GetBufferPointer(), upsb->GetBufferSize(), nullptr, &uiPs_);
    return true;
}

void D3D11Renderer::shutdown() {
    sharedCache_.clear();
    if (ctx_) ctx_->ClearState();
}

bool D3D11Renderer::resize(int width, int height) {
    if (!swapchain_ || width <= 0 || height <= 0) return true;
    width_ = width;
    height_ = height;
    rtv_.Reset();
    ctx_->ClearState();
    HRESULT hr = swapchain_->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height),
                                           DXGI_FORMAT_UNKNOWN, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        deviceRemoved_ = true;
        return false;
    }
    ComPtr<ID3D11Texture2D> back;
    swapchain_->GetBuffer(0, IID_PPV_ARGS(&back));
    device_->CreateRenderTargetView(back.Get(), nullptr, &rtv_);
    return true;
}

bool D3D11Renderer::renderAndPresent(float angleRad, bool drawUi, bool vsync) {
    if (!rtv_) return !deviceRemoved_;

    const float clear[4] = {0.05f, 0.07f, 0.12f, 1.0f};
    ctx_->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
    ctx_->ClearRenderTargetView(rtv_.Get(), clear);

    D3D11_VIEWPORT vp{0, 0, static_cast<float>(width_), static_cast<float>(height_), 0, 1};
    ctx_->RSSetViewports(1, &vp);

    // Triangle.
    D3D11_MAPPED_SUBRESOURCE map{};
    if (SUCCEEDED(ctx_->Map(triCb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        float* cb = static_cast<float*>(map.pData);
        cb[0] = std::cos(angleRad);
        cb[1] = std::sin(angleRad);
        cb[2] = width_ > 0 ? static_cast<float>(height_) / static_cast<float>(width_) : 1.0f;
        cb[3] = 0.0f;
        ctx_->Unmap(triCb_.Get(), 0);
    }
    UINT stride = 20, offset = 0;
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->IASetInputLayout(triLayout_.Get());
    ctx_->IASetVertexBuffers(0, 1, triVb_.GetAddressOf(), &stride, &offset);
    ctx_->VSSetShader(triVs_.Get(), nullptr, 0);
    ctx_->VSSetConstantBuffers(0, 1, triCb_.GetAddressOf());
    ctx_->PSSetShader(triPs_.Get(), nullptr, 0);
    ctx_->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ctx_->Draw(3, 0);

    // UI overlay.
    if (drawUi && uiSrv_) {
        D3D11_MAPPED_SUBRESOURCE umap{};
        if (SUCCEEDED(ctx_->Map(uiCb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &umap))) {
            float* cb = static_cast<float*>(umap.pData);
            cb[0] = (uiWidth_ > 0 && uiVisW_ > 0)
                        ? static_cast<float>(uiVisW_) / static_cast<float>(uiWidth_)
                        : 1.0f;
            cb[1] = (uiHeight_ > 0 && uiVisH_ > 0)
                        ? static_cast<float>(uiVisH_) / static_cast<float>(uiHeight_)
                        : 1.0f;
            cb[2] = cb[3] = 0.0f;
            ctx_->Unmap(uiCb_.Get(), 0);
        }
        ctx_->IASetInputLayout(nullptr);
        ctx_->VSSetShader(uiVs_.Get(), nullptr, 0);
        ctx_->PSSetShader(uiPs_.Get(), nullptr, 0);
        ctx_->PSSetConstantBuffers(0, 1, uiCb_.GetAddressOf());
        ctx_->PSSetShaderResources(0, 1, uiSrv_.GetAddressOf());
        ctx_->PSSetSamplers(0, 1, uiSampler_.GetAddressOf());
        ctx_->OMSetBlendState(uiBlend_.Get(), nullptr, 0xffffffff);
        ctx_->Draw(3, 0);
        ID3D11ShaderResourceView* nullSrv = nullptr;
        ctx_->PSSetShaderResources(0, 1, &nullSrv);
    }

    HRESULT hr = swapchain_->Present(vsync ? 1 : 0, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        deviceRemoved_ = true;
        return false;
    }
    return true;
}

bool D3D11Renderer::ensureUiTexture(int width, int height, bool /*asCopyDest*/) {
    if (uiTex_ && uiWidth_ == width && uiHeight_ == height) return true;
    uiSrv_.Reset();
    uiTex_.Reset();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(width);
    td.Height = static_cast<UINT>(height);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &uiTex_))) return false;
    if (FAILED(device_->CreateShaderResourceView(uiTex_.Get(), nullptr, &uiSrv_))) return false;
    uiWidth_ = width;
    uiHeight_ = height;
    return true;
}

bool D3D11Renderer::updateUiFromSharedHandle(HANDLE sharedHandle, int visW, int visH,
                                             UiUpdateStats* stats) {
    LONGLONG t0 = qpcNow();
    bool reopened = false;

    auto it = sharedCache_.find(sharedHandle);
    if (it == sharedCache_.end()) {
        ComPtr<ID3D11Texture2D> tex;
        HRESULT hr = device1_->OpenSharedResource1(sharedHandle, IID_PPV_ARGS(&tex));
        if (FAILED(hr)) {
            std::fprintf(stderr, "[spike] OpenSharedResource1 failed hr=0x%08lx\n",
                         static_cast<unsigned long>(hr));
            return false;
        }
        D3D11_TEXTURE2D_DESC desc{};
        tex->GetDesc(&desc);
        SharedEntry entry{tex, static_cast<int>(desc.Width), static_cast<int>(desc.Height)};
        it = sharedCache_.emplace(sharedHandle, entry).first;
        reopened = true;
    }

    const SharedEntry& e = it->second;
    if (!ensureUiTexture(e.width, e.height, true)) return false;
    uiVisW_ = visW > 0 ? visW : e.width;
    uiVisH_ = visH > 0 ? visH : e.height;
    // CEF 149 shared textures are created WITHOUT a keyed mutex; the documented-safe pattern
    // is to copy out of the shared texture inside the OnAcceleratedPaint callback (the frame
    // returns to CEF's ~11-deep capture pool when the callback returns).
    ctx_->CopyResource(uiTex_.Get(), e.tex.Get());

    if (stats) {
        stats->cpu_us = qpcUs(t0, qpcNow());
        stats->width = uiVisW_;
        stats->height = uiVisH_;
        stats->reopened = reopened;
    }
    return true;
}

bool D3D11Renderer::updateUiFromBuffer(const void* bgra, int width, int height,
                                       const DirtyRect* rects, size_t rectCount,
                                       UiUpdateStats* stats) {
    LONGLONG t0 = qpcNow();
    if (!ensureUiTexture(width, height, false)) return false;
    uiVisW_ = width;
    uiVisH_ = height;

    const auto* src = static_cast<const uint8_t*>(bgra);
    const UINT pitch = static_cast<UINT>(width) * 4;
    if (rectCount == 0) {
        ctx_->UpdateSubresource(uiTex_.Get(), 0, nullptr, src, pitch, 0);
    } else {
        for (size_t i = 0; i < rectCount; ++i) {
            const DirtyRect& r = rects[i];
            D3D11_BOX box{static_cast<UINT>(r.x), static_cast<UINT>(r.y), 0,
                          static_cast<UINT>(r.x + r.w), static_cast<UINT>(r.y + r.h), 1};
            ctx_->UpdateSubresource(uiTex_.Get(), 0, &box,
                                    src + (static_cast<size_t>(r.y) * width + r.x) * 4, pitch, 0);
        }
    }

    if (stats) {
        stats->cpu_us = qpcUs(t0, qpcNow());
        stats->width = width;
        stats->height = height;
        stats->reopened = false;
    }
    return true;
}

bool D3D11Renderer::sampleUiPixel(int x, int y, uint32_t* outArgb) {
    if (!uiTex_ || x < 0 || y < 0 || x >= uiWidth_ || y >= uiHeight_) return false;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 1;
    td.Height = 1;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &staging))) return false;
    D3D11_BOX box{static_cast<UINT>(x), static_cast<UINT>(y), 0,
                  static_cast<UINT>(x + 1), static_cast<UINT>(y + 1), 1};
    ctx_->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, uiTex_.Get(), 0, &box);
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(ctx_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return false;
    *outArgb = *static_cast<const uint32_t*>(map.pData);
    ctx_->Unmap(staging.Get(), 0);
    return true;
}

bool D3D11Renderer::dumpBackbufferBmp(const std::wstring& path) {
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(swapchain_->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    D3D11_TEXTURE2D_DESC desc{};
    back->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &staging))) return false;
    ctx_->CopyResource(staging.Get(), back.Get());
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(ctx_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return false;

    const int w = static_cast<int>(desc.Width), h = static_cast<int>(desc.Height);
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + static_cast<DWORD>(w) * h * 4;
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = -h; // top-down
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;

    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    bool ok = false;
    if (f) {
        std::fwrite(&fh, sizeof(fh), 1, f);
        std::fwrite(&ih, sizeof(ih), 1, f);
        const auto* rows = static_cast<const uint8_t*>(map.pData);
        for (int y = 0; y < h; ++y) {
            std::fwrite(rows + static_cast<size_t>(y) * map.RowPitch, 4, static_cast<size_t>(w), f);
        }
        std::fclose(f);
        ok = true;
    }
    ctx_->Unmap(staging.Get(), 0);
    return ok;
}
