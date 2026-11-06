// imgui_dx11.cpp — 交换链创建/重建辅助（渲染与输入见官方后端 imgui_impl_dx11/win32）
#include "imgui_dx11.h"

namespace imgui_dx11 {

HRESULT CreateSwapChain(HWND hwnd, int w, int h, IDXGISwapChain** outSwap,
                        ID3D11Device** outDevice, ID3D11DeviceContext** outCtx) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = w; sd.BufferDesc.Height = h;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60; sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1; sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    return D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
        levels, 2, D3D11_SDK_VERSION, &sd, outSwap, outDevice, NULL, outCtx);
}

bool ResizeSwapChain(IDXGISwapChain* swap, ID3D11Device* device, int w, int h,
                     ID3D11RenderTargetView** outRtv) {
    if (*outRtv) { (*outRtv)->Release(); *outRtv = NULL; }
    if (swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0) != S_OK) return false;
    ID3D11Texture2D* backBuffer = NULL;
    if (swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer) != S_OK) return false;
    device->CreateRenderTargetView(backBuffer, NULL, outRtv);
    backBuffer->Release();
    return *outRtv != NULL;
}

} // namespace imgui_dx11
