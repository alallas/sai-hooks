// imgui_dx11.h — D3D11 设备/交换链辅助（渲染与输入用官方后端 imgui_impl_dx11/win32 1.82）
#pragma once
#include <windows.h>
#include <d3d11.h>

namespace imgui_dx11 {

HRESULT CreateSwapChain(HWND hwnd, int w, int h, IDXGISwapChain** outSwap,
                        ID3D11Device** outDevice, ID3D11DeviceContext** outCtx);
bool ResizeSwapChain(IDXGISwapChain* swap, ID3D11Device* device, int w, int h,
                     ID3D11RenderTargetView** outRtv);

} // namespace imgui_dx11
