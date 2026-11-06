// liquefy.cpp — 液化会话实现（P2 骨架：快照 → 悬浮窗预览 → 提交/取消）
// P2-3 起在此扩展位移场/笔刷；P2-7 接 COW 写回。
#include "liquefy.h"
#include "sai_core.h"
#include "warp.h"
#include "imgui_dx11.h"
#include "../../third_party/imgui-2.0.0/imgui-cpp/imgui.h"
#include "../../third_party/imgui-2.0.0/imgui-cpp/backends/imgui_impl_win32.h"
#include "../../third_party/imgui-2.0.0/imgui-cpp/backends/imgui_impl_dx11.h"
#include <d3d11.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <tlhelp32.h>

// 1.82 后端头文件为免依赖 windows.h 注释掉了该声明，按官方说明自行前向声明
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

using namespace saicore;

namespace liquefy {

Config g_cfg = { MOD_CONTROL | MOD_SHIFT, 'X', 64, 80, 50, 50, 0 };

static const wchar_t* kWndClass = L"SAIHOOKS_LIQUEFY_WND";
static const wchar_t* kWndTitle = L"液化 — SAI Hooks";

// ---------- GPU 资源 ----------
struct Gpu {
    HWND hwnd;
    IDXGISwapChain* swap;
    ID3D11Device* device;
    ID3D11DeviceContext* ctx;
    ID3D11RenderTargetView* rtv;

    void release() {
        if (rtv) rtv->Release();
        if (swap) swap->Release();
        if (ctx) ctx->Release();
        if (device) device->Release();
        ZeroMemory(this, sizeof(*this));
    }
};

static Gpu g;
static bool g_wantClose = false;
static int g_result = CANCELLED;
static int g_resizeW = 0, g_resizeH = 0;
static const char* g_closeReason = "?"; // 退出原因追踪

// 异常诊断：记录崩溃代码、访问地址、所属模块（排查期临时，稳定后可移除）
static LONG CALLBACK VehHandler(EXCEPTION_POINTERS* ex) {
    void* addr = ex->ExceptionRecord->ExceptionAddress;
    // 写回跳车：回跳点有效时直接 longjmp 回 WritebackLayer，保进程
    if (sui::wbBailArmed() &&
        (ex->ExceptionRecord->ExceptionCode & 0xF0000000) == 0xC0000000) {
        logf("[veh] 写回段故障，跳车 addr=%p\n", addr);
        sui::wbBailJump((int)ex->ExceptionRecord->ExceptionCode);
    }
    logf("[veh] code=%08lx addr=%p op=%s va=%p\n",
         ex->ExceptionRecord->ExceptionCode, addr,
         ex->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
         (void*)ex->ExceptionRecord->ExceptionInformation[1]);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me; me.dwSize = sizeof(me);
        if (Module32FirstW(snap, &me)) {
            do {
                uintptr_t base = (uintptr_t)me.modBaseAddr;
                if ((uintptr_t)addr >= base && (uintptr_t)addr < base + me.modBaseSize)
                    logf("[veh] 模块=%ls 偏移=+%llx\n", me.szModule,
                         (unsigned long long)((uintptr_t)addr - base));
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// 视图状态（缩放/平移）
static float g_zoom = 1.0f;
static float g_panX = 0, g_panY = 0;

static LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return 0;
    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) { g_resizeW = LOWORD(lParam); g_resizeH = HIWORD(lParam); }
        return 0;
    case WM_DESTROY:
        g_wantClose = true;
        g_result = CANCELLED;
        g_closeReason = "WM_DESTROY";
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_CLOSE) { g_wantClose = true; g_result = CANCELLED; g_closeReason = "SC_CLOSE"; return 0; }
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------- 主会话 ----------
int Run(HWND saiWnd) {
    logf("[liquefy] Run 开始\n");
    void* veh = AddVectoredExceptionHandler(1, VehHandler);
    g_result = CANCELLED;
    g_wantClose = false;
    g_zoom = 1.0f; g_panX = g_panY = 0;

    // 1. 快照（经 UI 线程代理，§4.3；P2-9 多图层：合成预览 + 各层原始缓冲）
    uint16_t* snap = NULL;
    uint32_t W = 0, H = 0;
    int nTiles = 0;
    uint64_t layers[MAX_LIQUEFY_LAYERS] = {};
    uint16_t* layerBufs[MAX_LIQUEFY_LAYERS] = {};
    int nL = 0;
    logf("[liquefy] 请求快照\n");
    if (!sui::snapshot(&snap, &W, &H, &nTiles, layers, layerBufs, &nL) || !snap) {
        logf("[liquefy] 快照失败 tiles=%d\n", nTiles);
        // 无画布时 SAI 未建应用单例（sai.init 失败）；给用户明确提示而非静默退出
        MessageBoxW(saiWnd, L"液化功能需要至少一个打开的画布。\n请先新建或打开画布，再启动液化。",
                    L"SAI Hooks 液化", MB_OK | MB_ICONINFORMATION);
        return SNAPSHOT_FAIL;
    }
    logf("[liquefy] 快照 %ux%u 瓦片=%d 选中层数=%d\n", W, H, nTiles, nL);

    // 2. 建窗 + D3D11（P2-10：内嵌为 SAI 主窗的子窗口，跟随移动、无独立任务栏条目）
    logf("[liquefy] 建窗\n");
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = kWndClass;
    RegisterClassExW(&wc);

    RECT saiRc; GetClientRect(saiWnd, &saiRc);
    int winW = (int)W + 480, winH = (int)H + 60;
    if (winW < 700) winW = 700;
    if (winH < 500) winH = 500;
    // SAI 客户区处于过渡态（尺寸退化）时不采纳其约束，避免负值/超小窗
    if (saiRc.right > 320 && winW > saiRc.right - 16) winW = saiRc.right - 16;
    if (saiRc.bottom > 240 && winH > saiRc.bottom - 16) winH = saiRc.bottom - 16;
    logf("[liquefy] SAI客户区=%dx%d 液化窗=%dx%d\n", saiRc.right, saiRc.bottom, winW, winH);
    DWORD wstyle = WS_CHILD | WS_CLIPSIBLINGS | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME;
    RECT rc = { 0, 0, winW, winH };
    AdjustWindowRect(&rc, wstyle, FALSE);
    g.hwnd = CreateWindowExW(0, kWndClass, kWndTitle, wstyle,
        (saiRc.right - (rc.right - rc.left)) / 2, (saiRc.bottom - (rc.bottom - rc.top)) / 2,
        rc.right - rc.left, rc.bottom - rc.top,
        saiWnd, NULL, inst, NULL);
    if (!g.hwnd) { free(snap); for (int i = 0; i < nL; i++) free(layerBufs[i]); return INIT_FAIL; }

    logf("[liquefy] 窗口已建 hwnd=%p\n", (void*)g.hwnd);
    HRESULT hrSc = imgui_dx11::CreateSwapChain(g.hwnd, winW, winH, &g.swap, &g.device, &g.ctx);
    if (hrSc != S_OK) {
        logf("[liquefy] CreateSwapChain 失败 hr=%08lx winW=%d winH=%d\n", (unsigned long)hrSc, winW, winH);
        DestroyWindow(g.hwnd); free(snap); for (int i = 0; i < nL; i++) free(layerBufs[i]); return INIT_FAIL;
    }
    imgui_dx11::ResizeSwapChain(g.swap, g.device, winW, winH, &g.rtv);
    // warp 引擎：接管快照上传 + 位移场/预览渲染（内部做 0x4000->0xFFFF 量程归一）
    warp::Engine eng;
    if (!eng.init(g.device, g.ctx, snap, W, H)) {
        logf("[liquefy] warp 引擎初始化失败\n");
        g.release(); DestroyWindow(g.hwnd); free(snap);
        for (int i = 0; i < nL; i++) free(layerBufs[i]);
        return INIT_FAIL;
    }
    free(snap);

    // 3. ImGui（官方 1.82 后端：win32 输入 + dx11 渲染）
    logf("[liquefy] swapchain+rtv ok\n");
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    ImGui::StyleColorsLight(); // 整体浅色主题（PRD：往亮色调）
    // 中文字体：默认 Proggy Clean 无 CJK 字形。逐个尝试系统字体（纯 ASCII 路径），失败退回默认。
    {
        static const char* candidates[] = {
            "C:\\Windows\\Fonts\\msyh.ttc",    // 微软雅黑
            "C:\\Windows\\Fonts\\msyhbd.ttc",  // 微软雅黑 Bold
            "C:\\Windows\\Fonts\\simhei.ttf",  // 黑体
            "C:\\Windows\\Fonts\\simsun.ttc",  // 宋体
        };
        ImFont* font = NULL;
        for (int i = 0; i < 4 && !font; i++) {
            FILE* test = fopen(candidates[i], "rb");
            if (!test) continue;
            fclose(test);
            font = io.Fonts->AddFontFromFileTTF(candidates[i], 18.0f, NULL,
                                                io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        }
        logf("[liquefy] 中文字体: %s\n", font ? "ok" : "未找到，退回默认（中文显示为??）");
    }
    ImGui_ImplWin32_Init(g.hwnd);
    ImGui_ImplDX11_Init(g.device, g.ctx);
    logf("[liquefy] imgui 后端 ok\n");

    ShowWindow(g.hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g.hwnd);
    SetFocus(g.hwnd); // 子窗直接拿键盘焦点（画布热键在 SAI 全局仍可用）
    // SAI 面板布局会把新子窗压到 Z 序底部，强制置顶液化窗
    SetWindowPos(g.hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    logf("[liquefy] 进入主循环\n");
    // 4. 主循环
    LARGE_INTEGER freq; QueryPerformanceFrequency(&freq);
    LARGE_INTEGER last; QueryPerformanceCounter(&last);
    MSG msg;

    while (!g_wantClose) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_wantClose = true; g_closeReason = "WM_QUIT"; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_resizeW && g_resizeH) {
            imgui_dx11::ResizeSwapChain(g.swap, g.device, g_resizeW, g_resizeH, &g.rtv);
            g_resizeW = g_resizeH = 0;
        }

        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        float dt = (float)(now.QuadPart - last.QuadPart) / (float)freq.QuadPart;
        last = now;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // ---- UI ----
        // 画布：变形预览 + 缩放平移 + 笔刷涂抹（先提交，液化面板置顶在上）
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2((float)W + 40, (float)H + 60), ImGuiCond_Always);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        ImGui::Begin("画布", NULL, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        {
            static ImVec2 prevMouse = ImVec2(0, 0);
            static bool stroking = false;
            static float lastImgX = 0, lastImgY = 0;
            ImVec2 avail = ImGui::GetContentRegionAvail();
            ImVec2 imgSize((float)W * g_zoom, (float)H * g_zoom);
            ImGui::SetCursorPos(ImVec2((avail.x - imgSize.x) * 0.5f + g_panX,
                                       (avail.y - imgSize.y) * 0.5f + g_panY));
            ImGui::Image((ImTextureID)eng.previewSrv(), imgSize);
            ImVec2 imgOrigin = ImGui::GetItemRectMin();
            // 在同一位置铺隐形按钮接笔刷输入
            ImGui::SetCursorPos(ImVec2((avail.x - imgSize.x) * 0.5f + g_panX,
                                       (avail.y - imgSize.y) * 0.5f + g_panY));
            ImGui::InvisibleButton("canvas", imgSize);
            bool canvasHovered = ImGui::IsItemHovered();

            // img<->屏 坐标换算
            float imgX = (io.MousePos.x - imgOrigin.x) / g_zoom;
            float imgY = (io.MousePos.y - imgOrigin.y) / g_zoom;

            // 滚轮缩放（以鼠标位置为中心）
            if (canvasHovered && io.MouseWheel != 0) {
                float z0 = g_zoom;
                g_zoom *= (io.MouseWheel > 0) ? 1.1f : 0.9f;
                if (g_zoom < 0.05f) g_zoom = 0.05f;
                if (g_zoom > 40.f) g_zoom = 40.f;
                // 屏幕位置 s = base + pan + m*z（m 为图像坐标），保持 s 不动：pan -= m*(z1-z0)
                g_panX -= imgX * (g_zoom - z0);
                g_panY -= imgY * (g_zoom - z0);
            }
            // 右键拖动平移
            ImVec2 mouseDelta = ImVec2(io.MousePos.x - prevMouse.x, io.MousePos.y - prevMouse.y);
            if (ImGui::IsWindowHovered() && ImGui::IsMouseDragging(1)) {
                g_panX += mouseDelta.x; g_panY += mouseDelta.y;
            }
            prevMouse = io.MousePos;

            // ---- 笔刷输入 ----
            bool deformBrush = g_cfg.brushType <= warp::BRUSH_RESTORE;
            if (ImGui::IsItemActive() && ImGui::IsMouseClicked(0)) {
                eng.beginStroke();
                stroking = true;
                lastImgX = imgX; lastImgY = imgY;
            }
            if (stroking) {
                if (!ImGui::IsMouseDown(0)) {
                    stroking = false;
                } else if (imgX >= -g_cfg.brushSize && imgY >= -g_cfg.brushSize &&
                           imgX <= (float)W + g_cfg.brushSize && imgY <= (float)H + g_cfg.brushSize) {
                    // 最小间距节流（图像像素），避免同一位置重复 dab
                    float dx = imgX - lastImgX, dy = imgY - lastImgY;
                    float dist = sqrtf(dx * dx + dy * dy);
                    if (dist >= 1.0f) {
                        if (deformBrush) {
                            eng.dab((warp::Brush)g_cfg.brushType, imgX, imgY, dx, dy,
                                    (float)g_cfg.brushSize,
                                    (float)g_cfg.brushStrength / 100.0f,
                                    (float)g_cfg.brushHardness / 100.0f);
                        } else {
                            // 冻结/解冻：每次 dab 一步到位
                            eng.dab((warp::Brush)g_cfg.brushType, imgX, imgY, 0, 0,
                                    (float)g_cfg.brushSize, 1.0f, 0.0f);
                        }
                        lastImgX = imgX; lastImgY = imgY;
                    }
                }
            }
        }
        ImGui::End();
        ImGui::PopStyleColor();

        // 液化面板（后提交 = 置顶覆盖画布窗口），放画布右侧；窗口被压窄时回收到可视区内
        float panelX = (float)W + 60;
        RECT cliRc; GetClientRect(g.hwnd, &cliRc);
        float availW = (float)(cliRc.right - cliRc.left);
        if (panelX + 260 > availW) panelX = availW - 260 > 4 ? availW - 260 : 4;
        ImGui::SetNextWindowPos(ImVec2(panelX, 10), ImGuiCond_Always);
        ImGui::SetNextWindowSizeConstraints(ImVec2(250, 0), ImVec2(360, 10000));
        ImGui::Begin("液化", NULL, ImGuiWindowFlags_AlwaysAutoResize);
        // 笔刷选择（P2-4 手感调参在此迭代）
        static const char* kBrushNames[warp::BRUSH_COUNT] = {
            "推拉", "膨胀", "收缩", "平滑", "重建", "冻结", "解冻" };
        ImGui::Text("笔刷:");
        for (int i = 0; i < warp::BRUSH_COUNT; i++) {
            char lbl[32];
            snprintf(lbl, sizeof(lbl), "%s##b%d", kBrushNames[i], i);
            ImGui::RadioButton(lbl, &g_cfg.brushType, i);
            if (i % 4 != 3 && i != warp::BRUSH_COUNT - 1) ImGui::SameLine();
            if (i % 4 == 3) ImGui::NewLine();
        }
        ImGui::NewLine();
        const char* kParamNames[4] = { "大小(笔刷直径):", "力度(变形强度):",
                                       "浓度(中心浓度):", "硬度(边缘软硬):" };
        int* kParamVars[4] = { &g_cfg.brushSize, &g_cfg.brushStrength,
                               &g_cfg.brushDensity, &g_cfg.brushHardness };
        const int kParamMax[4] = { 500, 100, 100, 100 };
        for (int i = 0; i < 4; i++) {
            ImGui::Text("%s", kParamNames[i]);
            ImGui::SameLine();
            char id[16]; snprintf(id, sizeof(id), "##p%d", i);
            ImGui::SliderInt(id, kParamVars[i], 1, kParamMax[i]);
        }
        ImGui::Separator();
        // 撤销/重做（按笔，P2-6 升级为多级栈）
        bool canUndo = eng.canUndo(), canRedo = eng.canRedo();
        if (canUndo && ImGui::Button("撤销 (Ctrl+Z)", ImVec2(110, 26))) eng.undoStroke();
        ImGui::SameLine();
        if (canRedo && ImGui::Button("重做 (Ctrl+Y)", ImVec2(110, 26))) eng.redoStroke();
        ImGui::SameLine();
        if (ImGui::Button("重置", ImVec2(80, 26))) eng.reset();
        ImGui::Separator();
        if (ImGui::Button("确定 (Enter)", ImVec2(120, 30))) { g_result = OK; g_wantClose = true; g_closeReason = "确定按钮"; }
        ImGui::SameLine();
        if (ImGui::Button("取消 (Esc)", ImVec2(120, 30))) { g_result = CANCELLED; g_wantClose = true; g_closeReason = "取消按钮"; }
        ImGui::End();

        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { g_result = CANCELLED; g_wantClose = true; g_closeReason = "Esc键"; }
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false)) { g_result = OK; g_wantClose = true; g_closeReason = "Enter键"; }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) eng.undoStroke();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) eng.redoStroke();
        // [ ] 调笔刷大小（按住连调）
        int step = (int)(300 * dt); if (step < 1) step = 1;
        if (io.KeysDown[VK_OEM_4]) g_cfg.brushSize -= step;
        if (io.KeysDown[VK_OEM_6]) g_cfg.brushSize += step;
        if (g_cfg.brushSize < 1) g_cfg.brushSize = 1;
        if (g_cfg.brushSize > 500) g_cfg.brushSize = 500;

        // ---- 渲染 ----
        eng.renderPreview(); // ImGui 后端在 RenderDrawData 里自行重绑 RTV，此处设备空闲
        ImGui::Render();
        const float clear[4] = { 0.94f, 0.94f, 0.96f, 1.f }; // 浅色主题底
        g.ctx->OMSetRenderTargets(1, &g.rtv, NULL);
        g.ctx->ClearRenderTargetView(g.rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g.swap->Present(1, 0);
    }

    // 5. 收尾
    // P2-7：确定 → result pass 重采样 → COW 写回；P2-9：逐层换源重放同一变形，
    // 多层共用一个撤销分组；取消 → 直接丢弃
    if (g_result == OK) {
        logf("[liquefy] 确定：写回 SAI（%d 层）\n", nL);
        const uint16_t* outs[MAX_LIQUEFY_LAYERS] = {};
        bool readOk = true;
        for (int i = 0; i < nL; i++) {
            if (!eng.uploadSource(layerBufs[i])) { readOk = false; break; }
            eng.renderResult();
            outs[i] = eng.readResult16();
            if (!outs[i]) { readOk = false; break; }
        }
        if (readOk) {
            int r = sui::writeback(outs, (const uint16_t* const*)layerBufs, layers, nL, W, H);
            logf("[liquefy] 写回结果=%d（瓦片总数，<0 失败）\n", r);
        } else {
            logf("[liquefy] 写回结果回读失败\n");
            g_result = INIT_FAIL;
        }
        for (int i = 0; i < nL; i++) free((void*)outs[i]);
    } else {
        logf("[liquefy] 取消：丢弃变形\n");
    }
    for (int i = 0; i < nL; i++) free(layerBufs[i]);
    eng.shutdown();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g.release();
    DestroyWindow(g.hwnd);
    UnregisterClassW(kWndClass, inst);
    if (veh) RemoveVectoredExceptionHandler(veh);
    logf("[liquefy] 会话结束 result=%d 原因=%s\n", g_result, g_closeReason);
    return g_result;
}

// ---------- 无头自检（不开窗：快照→引擎→合成涂抹→数值断言）----------
namespace {

bool SaveBmp24(const char* path, const uint8_t* rgba, uint32_t W, uint32_t H) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    uint32_t rowBytes = (W * 3 + 3) & ~3u;
    uint8_t hdr[54];
    ZeroMemory(hdr, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    *(uint32_t*)(hdr + 2) = 54 + rowBytes * H;
    *(uint32_t*)(hdr + 10) = 54;
    *(uint32_t*)(hdr + 14) = 40;
    *(uint32_t*)(hdr + 18) = W;
    *(int32_t*)(hdr + 22) = -(int32_t)H; // 顶向下
    *(uint16_t*)(hdr + 26) = 1;
    *(uint16_t*)(hdr + 28) = 24;
    *(uint32_t*)(hdr + 34) = rowBytes * H;
    fwrite(hdr, 1, 54, f);
    uint8_t* row = (uint8_t*)malloc(rowBytes);
    for (uint32_t y = 0; y < H; y++) {
        const uint8_t* s = rgba + (size_t)y * W * 4;
        for (uint32_t x = 0; x < W; x++) {
            row[x * 3 + 0] = s[x * 4 + 2]; // B
            row[x * 3 + 1] = s[x * 4 + 1]; // G
            row[x * 3 + 2] = s[x * 4 + 0]; // R
        }
        memset(row + W * 3, 0, rowBytes - W * 3);
        fwrite(row, 1, rowBytes, f);
    }
    free(row);
    fclose(f);
    return true;
}

// 统计两帧差异 > 8 的像素数；可限定圆区（cx,cy,r 内，r<=0 表示全图）
uint32_t DiffCount(const uint8_t* a, const uint8_t* b, uint32_t W, uint32_t H,
                   float cx, float cy, float r) {
    uint32_t n = 0;
    for (uint32_t y = 0; y < H; y++) {
        for (uint32_t x = 0; x < W; x++) {
            if (r > 0) {
                float dx = (float)x - cx, dy = (float)y - cy;
                if (dx * dx + dy * dy > r * r) continue;
            }
            const uint8_t* pa = a + ((size_t)y * W + x) * 4;
            const uint8_t* pb = b + ((size_t)y * W + x) * 4;
            if ((uint32_t)abs(pa[0] - pb[0]) > 8 || (uint32_t)abs(pa[1] - pb[1]) > 8 ||
                (uint32_t)abs(pa[2] - pb[2]) > 8) n++;
        }
    }
    return n;
}

} // namespace

int SelfTest() {
    logf("[selftest] === 无头自检开始 ===\n");
    uint16_t* snap = NULL;
    uint32_t W = 0, H = 0;
    int tiles = 0;
    uint64_t stLayers[MAX_LIQUEFY_LAYERS] = {};
    uint16_t* stBufs[MAX_LIQUEFY_LAYERS] = {};
    int stN = 0;
    if (!sui::snapshot(&snap, &W, &H, &tiles, stLayers, stBufs, &stN) || !snap) {
        logf("[selftest] FAIL 快照失败 tiles=%d\n", tiles);
        return 1;
    }
    for (int i = 0; i < stN; i++) free(stBufs[i]);
    logf("[selftest] 快照 %ux%u tiles=%d\n", W, H, tiles);

    ID3D11Device* dev = NULL;
    ID3D11DeviceContext* ctx = NULL;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
                                   D3D11_SDK_VERSION, &dev, NULL, &ctx);
    if (hr != S_OK)
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0,
                               D3D11_SDK_VERSION, &dev, NULL, &ctx);
    if (hr != S_OK) { logf("[selftest] FAIL D3D11 设备创建 hr=%08lx\n", (long)hr); free(snap); return 1; }

    warp::Engine eng;
    if (!eng.init(dev, ctx, snap, W, H)) {
        logf("[selftest] FAIL 引擎初始化\n");
        dev->Release(); ctx->Release(); free(snap);
        return 1;
    }
    free(snap);

    eng.renderPreview();
    uint8_t* base = eng.readPreview();
    if (base) SaveBmp24("sai_test_1_baseline.bmp", base, W, H);

    // 1) 推拉：30 dab 沿中线从左到右
    eng.beginStroke();
    float y0 = (float)H / 2, x0 = (float)W * 0.2f, x1 = (float)W * 0.8f;
    float step = (x1 - x0) / 30.0f;
    for (int i = 0; i < 30; i++)
        eng.dab(warp::BRUSH_PUSH, x0 + i * step, y0, step, 0, 64, 0.8f, 0.5f);
    eng.renderPreview();
    uint8_t* pushed = eng.readPreview();
    if (pushed) SaveBmp24("sai_test_2_pushed.bmp", pushed, W, H);

    // 2) 撤销 → 应回到基线
    eng.undoStroke();
    eng.renderPreview();
    uint8_t* undone = eng.readPreview();
    if (undone) SaveBmp24("sai_test_3_undone.bmp", undone, W, H);

    // 3) 重做 → 应回到变形后
    eng.redoStroke();
    eng.renderPreview();
    uint8_t* redone = eng.readPreview();
    if (redone) SaveBmp24("sai_test_4_redone.bmp", redone, W, H);

    // 4) 冻结 (0.65W, H/2) r=60，在同一区域推拉，然后解冻
    float fx = (float)W * 0.65f, fy = (float)H / 2;
    eng.dab(warp::BRUSH_FREEZE, fx, fy, 0, 0, 60, 1.0f, 0.0f);
    eng.beginStroke();
    for (int i = 0; i < 10; i++)
        eng.dab(warp::BRUSH_PUSH, fx - 30 + i * 6, fy, 6, 0, 64, 0.8f, 0.5f);
    eng.dab(warp::BRUSH_UNFREEZE, fx, fy, 0, 0, 60, 1.0f, 0.0f); // 解冻消除蓝色提示再比较
    eng.renderPreview();
    uint8_t* frozen = eng.readPreview();
    if (frozen) SaveBmp24("sai_test_5_frozen.bmp", frozen, W, H);

    // 对照：在未冻结区（0.30W, H/2）做同样 10 dab 推拉，应产生变形
    float gx = (float)W * 0.30f, gy = (float)H / 2;
    eng.beginStroke();
    for (int i = 0; i < 10; i++)
        eng.dab(warp::BRUSH_PUSH, gx - 30 + i * 6, gy, 6, 0, 64, 0.8f, 0.5f);
    eng.renderPreview();
    uint8_t* ctrl = eng.readPreview();
    if (ctrl) SaveBmp24("sai_test_6_control.bmp", ctrl, W, H);

    // ---- 数值断言 ----
    int pass = 0, fail = 0;
    auto check = [&](const char* name, bool ok, uint32_t v) {
        logf("[selftest] %s: %s (%u)\n", name, ok ? "PASS" : "FAIL", v);
        ok ? pass++ : fail++;
    };
    if (base && pushed)
        check("变形生效(基线vs推拉)", DiffCount(base, pushed, W, H, 0, 0, 0) > 500,
              DiffCount(base, pushed, W, H, 0, 0, 0));
    if (base && undone)
        check("撤销还原(基线vs撤销)", DiffCount(base, undone, W, H, 0, 0, 0) <= 10,
              DiffCount(base, undone, W, H, 0, 0, 0));
    if (pushed && redone)
        check("重做还原(推拉vs重做)", DiffCount(pushed, redone, W, H, 0, 0, 0) <= 10,
              DiffCount(pushed, redone, W, H, 0, 0, 0));
    // 冻结保护：解冻后圆内（远离蒙版边缘）应与「仅第一次推拉」一致；
    // 对照组在未冻结区做同样涂抹，应产生明显变形
    if (pushed && frozen)
        check("冻结保护(圆内不变)", DiffCount(pushed, frozen, W, H, fx, fy, 55) <= 10,
              DiffCount(pushed, frozen, W, H, fx, fy, 55));
    if (redone && ctrl)
        check("未冻结对照(同样涂抹变形)", DiffCount(redone, ctrl, W, H, gx, gy, 55) > 100,
              DiffCount(redone, ctrl, W, H, gx, gy, 55));

    // ---- P2-6: 16 级撤销栈 ----
    eng.renderPreview();
    uint8_t* s0 = eng.readPreview();
    for (int s = 0; s < 16; s++) { // 连画 16 笔，每笔不同高度
        eng.beginStroke();
        float sy = (float)H * (0.15f + 0.7f * s / 15.0f);
        for (int i = 0; i < 6; i++)
            eng.dab(warp::BRUSH_PUSH, (float)W * 0.3f + i * 8, sy, 8, 0, 48, 0.6f, 0.5f);
    }
    eng.renderPreview();
    uint8_t* s16 = eng.readPreview();
    if (s16) SaveBmp24("sai_test_7_stack16.bmp", s16, W, H);
    if (s0 && s16)
        check("16笔后画面已变", DiffCount(s0, s16, W, H, 0, 0, 0) > 500,
              DiffCount(s0, s16, W, H, 0, 0, 0));
    for (int i = 0; i < 16; i++) eng.undoStroke();
    check("撤销栈耗尽(canUndo=false)", !eng.canUndo(), (uint32_t)eng.canUndo());
    eng.renderPreview();
    uint8_t* sU = eng.readPreview();
    if (s0 && sU)
        check("16级撤销回基线", DiffCount(s0, sU, W, H, 0, 0, 0) <= 10,
              DiffCount(s0, sU, W, H, 0, 0, 0));
    for (int i = 0; i < 16; i++) eng.redoStroke();
    check("重做栈耗尽(canRedo=false)", !eng.canRedo(), (uint32_t)eng.canRedo());
    eng.renderPreview();
    uint8_t* sR = eng.readPreview();
    if (s16 && sR)
        check("16级重做还原", DiffCount(s16, sR, W, H, 0, 0, 0) <= 10,
              DiffCount(s16, sR, W, H, 0, 0, 0));
    // 撤销后再画新笔，重做应失效
    eng.undoStroke();
    eng.beginStroke();
    eng.dab(warp::BRUSH_PUSH, (float)W * 0.5f, (float)H * 0.5f, 8, 0, 48, 0.6f, 0.5f);
    check("新笔使重做失效(canRedo=false)", !eng.canRedo(), (uint32_t)eng.canRedo());
    free(s0); free(s16); free(sU); free(sR);

    // ---- P2-7: 写回 result pass（引擎侧数值验证，不触碰 SAI 画布）----
    eng.reset();
    eng.renderResult();
    uint16_t* r0 = eng.readResult16();
    {
        size_t nz = 0;
        if (r0) for (size_t i = 0; i < (size_t)W * H * 4; i++) if (r0[i]) nz++;
        logf("[selftest] result r0 非零分量=%zu\n", nz);
        eng.renderPreview();
        uint8_t* pv = eng.readPreview();
        if (pv) {
            uint32_t np = 0;
            for (size_t i = 0; i < (size_t)W * H; i++)
                if (pv[i * 4] < 0xC0 || pv[i * 4 + 1] < 0xC0 || pv[i * 4 + 2] < 0xC0) np++;
            logf("[selftest] 同刻 preview 非白像素=%u\n", np);
            free(pv);
        }
        if (r0) {
            uint8_t* vis = (uint8_t*)malloc((size_t)W * H * 4);
            for (size_t i = 0; i < (size_t)W * H * 4; i++) vis[i] = (uint8_t)(r0[i] >> 8);
            SaveBmp24("sai_test_8_result0.bmp", vis, W, H);
            free(vis);
        }
    }
    eng.beginStroke();
    for (int i = 0; i < 10; i++)
        eng.dab(warp::BRUSH_PUSH, (float)W * 0.25f + i * 8, (float)H * 0.5f, 8, 0, 64, 0.8f, 0.5f);
    eng.renderResult();
    uint16_t* r1 = eng.readResult16();
    // 质心按 alpha 加权（预乘图黑笔画 rgb=0，亮度阈值会把透明背景也算进来）
    auto comXBand = [&](const uint16_t* img, uint32_t y0, uint32_t y1) -> double {
        double sum = 0, cnt = 0;
        for (uint32_t y = y0; y < y1; y++)
            for (uint32_t x = 0; x < W; x++) {
                const uint16_t* p = img + ((size_t)y * W + x) * 4;
                double w = (double)p[3];
                if (w > 0x1000) { sum += x * w; cnt += w; }
            }
        return cnt ? sum / cnt : -1;
    };
    if (r0 && r1) {
        uint32_t d = 0; // 16-bit 差异计数
        for (size_t i = 0; i < (size_t)W * H * 4; i++)
            if ((uint32_t)abs((int)r0[i] - (int)r1[i]) > 0x800) d++;
        check("写回pass变形(差异数)", d > 500, d);
    } else check("写回pass回读非空", false, 0);
    free(r0); free(r1);

    // ---- P2-7b: 合成图像确定性验证（方向/幅度/撤销，不依赖用户画布内容）----
    {
        const uint32_t SW = 256, SH = 256;
        uint16_t* syn = (uint16_t*)calloc((size_t)SW * SH, 8);
        for (uint32_t y = 0; y < SH; y++)
            for (uint32_t x = 0; x < SW; x++) {
                uint16_t* p = syn + ((size_t)y * SW + x) * 4;
                uint16_t v = (y >= 120 && y < 136) ? 0x0000 : 0x4000; // 水平黑条 : 白底
                p[0] = v; p[1] = v; p[2] = v; p[3] = 0x4000;
            }
        warp::Engine e2;
        if (!e2.init(dev, ctx, syn, SW, SH)) {
            check("合成:引擎初始化", false, 0);
        } else {
            // 水平黑条 + 垂直向下推：条带应整体下移（沿 push 方向测质心）
            auto barComY = [&](const uint16_t* img, uint32_t x0, uint32_t x1) -> double {
                double sum = 0, cnt = 0;
                for (uint32_t y = 0; y < SH; y++)
                    for (uint32_t x = x0; x < x1; x++) {
                        const uint16_t* p = img + ((size_t)y * SW + x) * 4;
                        if (p[3] > 0x2000 && p[0] < 0x2000 && p[1] < 0x2000 && p[2] < 0x2000) { sum += y; cnt++; }
                    }
                return cnt ? sum / cnt : -1;
            };
            e2.renderResult();
            uint16_t* b0 = e2.readResult16();
            double c0 = barComY(b0, 100, 200);
            e2.beginStroke();
            for (int i = 0; i < 5; i++)
                e2.dab(warp::BRUSH_PUSH, 128 + i * 10, 128, 0, 10, 48, 1.0f, 0.9f);
            e2.renderResult();
            uint16_t* b1 = e2.readResult16();
            // 涂抹影响：差异应存在且全部落在笔刷作用域内（dab 路径 ±64 的包围盒）
            uint32_t nDiff = 0, outBox = 0;
            for (uint32_t y = 0; y < SH; y++)
                for (uint32_t x = 0; x < SW; x++) {
                    const uint16_t* pa = b0 + ((size_t)y * SW + x) * 4;
                    const uint16_t* pb = b1 + ((size_t)y * SW + x) * 4;
                    bool diff = false;
                    for (int ch = 0; ch < 4; ch++)
                        if ((uint32_t)abs((int)pa[ch] - (int)pb[ch]) > 0x800) { diff = true; break; }
                    if (!diff) continue;
                    nDiff++;
                    if ((int)x < 128 - 64 || (int)x > 168 + 64 || (int)y < 128 - 64 || (int)y > 128 + 64) outBox++;
                }
            check("合成:横条存在(质心y 126-130)", c0 > 124 && c0 < 132, (uint32_t)c0);
            check("合成:涂抹产生局部差异(>20且不出框)", nDiff > 20 && outBox == 0, nDiff * 1000000 + outBox);
            logf("[selftest] 合成差异=%u 出框=%u\n", nDiff, outBox);
            e2.undoStroke();
            e2.renderResult();
            uint16_t* b2 = e2.readResult16();
            double c2 = barComY(b2, 100, 200);
            check("合成:撤销还原(<1px)", fabs(c2 - c0) < 1, (uint32_t)fabs(c2 - c0));
            free(b0); free(b1); free(b2);
            e2.shutdown();
        }
        free(syn);
    }
    check("回读非空", base && pushed && undone && redone && frozen && ctrl, 0);

    logf("[selftest] === 结束: %d PASS / %d FAIL ===\n", pass, fail);
    free(base); free(pushed); free(undone); free(redone); free(frozen); free(ctrl);
    eng.shutdown();
    dev->Release();
    ctx->Release();
    return fail ? 1 : 0;
}

} // namespace liquefy
