// sai_hooks.dll — P2 液化插件入口
// 注入 SAI2 后：加载 ini → 子类化 SAI 主窗（IPC 通道保留）→ 注册热键 → 热键拉起液化会话
#include <windows.h>
#include <stdio.h>
#include "sai_core.h"
#include "liquefy.h"

using namespace saicore;

#ifndef MIP_LEVELS
#define MIP_LEVELS 6
#endif

FILE* saicore::g_log = NULL;
static void openLog(const char* tag) {
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    wchar_t p[MAX_PATH];
    wsprintfW(p, L"%ssai_hooks_log_%lu.txt", tmp, GetCurrentProcessId());
    g_log = _wfopen(p, L"a");
    logf("\n=== %s (pid=%lu) ===\n", tag, GetCurrentProcessId());
}

static UINT     g_ipcMsg = 0;
static WNDPROC  g_oldProc = NULL;
static HWND     g_saiWnd = NULL;
static HWND     g_msgWnd = NULL;      // 消息窗：收热键
static HANDLE   g_sessionThread = NULL;
static volatile LONG g_sessionRunning = 0;
static HMODULE  g_module = NULL;
static void placeLauncher(); // SAI 主窗子类化里调用，定义见启动按钮段

// 会话结束通知（自定义消息）
#define WM_SAIHOOKS_SESSION_END (WM_USER + 0x100)
#define WM_SAIHOOKS_SELFTEST    (WM_USER + 0x103) // 无头自检（不开窗）
// ---------- ini ----------
static void loadIni() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_module, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) slash[1] = 0;
    wcscat(path, L"sai_hooks.ini");

    liquefy::Config& c = liquefy::g_cfg;
    c.hotkeyModifiers = GetPrivateProfileIntW(L"hotkey", L"modifiers", MOD_CONTROL | MOD_SHIFT, path);
    c.hotkeyVk        = GetPrivateProfileIntW(L"hotkey", L"vk", 'X', path);
    c.brushSize       = GetPrivateProfileIntW(L"liquefy", L"brush_size", 64, path);
    c.brushStrength   = GetPrivateProfileIntW(L"liquefy", L"brush_strength", 80, path);
    c.brushDensity    = GetPrivateProfileIntW(L"liquefy", L"brush_density", 50, path);
    c.brushHardness   = GetPrivateProfileIntW(L"liquefy", L"brush_hardness", 50, path);
    logf("[ini] hotkey mods=%d vk=%d size=%d\n", c.hotkeyModifiers, c.hotkeyVk, c.brushSize);
}

// ---------- UI 线程代理（§4.3：SAI 内部函数必须在 SAI UI 线程执行）----------
// 会话线程用 SendMessage 把作业投递给 SAI 主窗，由子类化后的 SaiHookProc 在 UI 线程执行。
// ---------- 快照作业（UI 线程执行，P2-9 多图层版）----------
// buf = 选中图层自底向顶预乘合成的 R,G,B,A u16（0x4000 量程），供引擎预览；
// layerBufs/layers/nLayers = 各选中图层原始快照（自底向顶，调用方释放）
struct SnapshotJob {
    uint16_t* buf;
    uint16_t* layerBufs[MAX_LIQUEFY_LAYERS];
    uint64_t  layers[MAX_LIQUEFY_LAYERS];
    int nLayers;
    uint32_t W, H;
    int tiles;
};
#define WM_SAIHOOKS_SNAPSHOT (WM_USER + 0x101)
#define WM_SAIHOOKS_WRITEBACK (WM_USER + 0x102)

// 写回作业（UI 线程执行）：bufs = 各选中图层变形结果 R,G,B,A 16-bit（0xFFFF 量程）；
// origs = 各层原始快照（0x4000 量程），用于逐瓦片差异判定
struct WritebackJob {
    const uint16_t* bufs[MAX_LIQUEFY_LAYERS];
    const uint16_t* origs[MAX_LIQUEFY_LAYERS];
    uint64_t layers[MAX_LIQUEFY_LAYERS];
    int nLayers;
    uint32_t W, H;
    int result;   // >=0 写入瓦片总数，<0 错误码
};

// ---------- 写回实现（P2-7：整层 COW 写回 + MIP 重建 + 安全重绘）----------
// 0xFFFF 量程 -> SAI 0x4000 量程
static inline uint16_t toSai(uint32_t v) { return (uint16_t)((v * 0x4000u + 0x7FFFu) / 0xFFFFu); }

// 指针合理性：SAI 堆对象都在用户态高位地址、8 字节对齐；
// 层结构里未分配的 mip 槽位常是 0x55/小整数之类的垃圾，靠它过滤
static inline bool plausible(uint64_t p) {
    return p > 0x10000 && p < 0x7FFF00000000ULL && (p & 7) == 0;
}

// mip 级像素定位（sai_write P1-m3 验证过的 0x38/0x40 行布局）
static uint16_t* mipPx(uint64_t plane, uint32_t gx, uint32_t gy, int* ok) {
    uint64_t flagsArr = rd64((uintptr_t)plane + 0x38);
    uint64_t pagesArr = rd64((uintptr_t)plane + 0x40);
    uint32_t tx = gx / TILE, ty = gy / TILE;
    *ok = 0;
    if (!plausible(flagsArr) || !plausible(pagesArr) || tx >= 16) return NULL;
    uint64_t fr = rd64((uintptr_t)flagsArr + ty * 8);
    uint64_t pr = rd64((uintptr_t)pagesArr + ty * 8);
    if (!plausible(fr) || !plausible(pr) || ((uint8_t*)fr)[tx] == 0) return NULL;
    uint64_t page = rd64((uintptr_t)pr + tx * 8);
    if (!is_page(page)) return NULL;
    *ok = 1;
    return (uint16_t*)(uintptr_t)(page + ((gy % TILE) * TILE + (gx % TILE)) * 8);
}

// 单层写回：瓦片 COW + MIP 重建（分组/锁由调用方 WritebackLayersImpl 管理）
// buf = 变形结果（0xFFFF 量程），orig = 该层原始快照（0x4000 量程）。
// 写瓦片判定：变形后与原始不同才写——空瓦片被凸起内容覆盖时也会创建新瓦片
// （修「液化凸起到空白处被裁剪」：预览无此限制，写回必须一致）。
static int WritebackOneLayer(WriteSession& ws, const uint16_t* buf, const uint16_t* orig,
                             uint32_t W, uint32_t H) {
    int written = 0, skipped = 0;
    uint32_t tilesY = (H + TILE - 1) / TILE;
    uint32_t tilesX = (W + TILE - 1) / TILE; if (tilesX > 32) tilesX = 32;
    uint16_t* writtenTiles = (uint16_t*)malloc((size_t)tilesX * tilesY * 2 * sizeof(uint16_t));
    uint32_t nWritten = 0;
    PixU16 tile[TILE * TILE];
    for (uint32_t ty = 0; ty < tilesY; ty++) {
        uint64_t fa = rd64((uintptr_t)ws.plane + PL_F_ROWS);
        uint64_t fr = fa ? rd64((uintptr_t)fa + ty * 8) : 0;
        if (!fr) continue;
        for (uint32_t tx = 0; tx < tilesX; tx++) {
            // 逐像素判定本瓦片是否需要写（结果 vs 原始）
            bool needWrite = false;
            for (uint32_t py = 0; py < TILE && !needWrite; py++) {
                uint32_t gy = ty * TILE + py;
                if (gy >= H) break;
                for (uint32_t px = 0; px < TILE; px++) {
                    uint32_t gx = tx * TILE + px;
                    if (gx >= W) break;
                    size_t idx = ((size_t)gy * W + gx) * 4;
                    const uint16_t* s = buf + idx;   // R,G,B,A 0xFFFF
                    const uint16_t* o = orig + idx;  // R,G,B,A 0x4000
                    if (toSai(s[0]) != o[0] || toSai(s[1]) != o[1] ||
                        toSai(s[2]) != o[2] || toSai(s[3]) != o[3]) { needWrite = true; break; }
                }
            }
            if (!needWrite) continue; // 未变形区域：不触碰
            for (uint32_t py = 0; py < TILE; py++) {
                uint32_t gy = ty * TILE + py;
                for (uint32_t px = 0; px < TILE; px++) {
                    uint32_t gx = tx * TILE + px;
                    PixU16 c = {};
                    if (gx < W && gy < H) {
                        const uint16_t* s = buf + ((size_t)gy * W + gx) * 4; // R,G,B,A
                        c.r = toSai(s[0]); c.g = toSai(s[1]);
                        c.b = toSai(s[2]); c.a = toSai(s[3]);
                    }
                    tile[py * TILE + px] = c;
                }
            }
            if (ws.writeTile(tx, ty, tile)) {
                written++;
                uint32_t x1 = (tx + 1) * TILE < W ? (tx + 1) * TILE : W;
                uint32_t y1 = (ty + 1) * TILE < H ? (ty + 1) * TILE : H;
                ws.regDirtyRect(tx * TILE, ty * TILE, x1, y1);
                if (writtenTiles) {
                    writtenTiles[nWritten * 2] = (uint16_t)tx;
                    writtenTiles[nWritten * 2 + 1] = (uint16_t)ty;
                    nWritten++;
                }
            } else skipped++;
        }
    }

    // MIP1..5 重建：只对本次写过的瓦片足迹（P1-m3 验证过的有界遍历，
    // 全画布遍历会读到 SAI 懒分配之外的行数组 -> 野指针崩溃）
    uint64_t layer = ws.layer;
    for (int k = 1; k < MIP_LEVELS; k++) {
        uint64_t pl = rd64((uintptr_t)layer + OFF_LAYER_MIP0 + k * 8);
        uint64_t plPrev = rd64((uintptr_t)layer + OFF_LAYER_MIP0 + (k - 1) * 8);
        if (!pl || !plPrev) continue;
        uint32_t step = 1u << (k - 1);
        // 目标即 mipPx 的 +0x40 cache 行（§4.4：MIP1..5 的页就存于 cache 组）
        int rebuilt = 0;
        for (uint32_t t = 0; t < nWritten; t++) {
            uint32_t TX = writtenTiles[t * 2], TY = writtenTiles[t * 2 + 1];
            uint32_t x0 = (TX * TILE) >> k, x1 = ((TX * TILE) + TILE - 1) >> k;
            uint32_t y0 = (TY * TILE) >> k, y1 = ((TY * TILE) + TILE - 1) >> k;
            for (uint32_t gy = y0; gy <= y1; gy++)
                for (uint32_t gx = x0; gx <= x1; gx++) {
                    uint32_t sx = gx << k, sy = gy << k;
                    int o00, o10, o01, o11, okd;
                    uint16_t* p00 = mipPx(plPrev, sx, sy, &o00);
                    uint16_t* pA  = mipPx(plPrev, sx + step, sy, &o10);
                    uint16_t* pB  = mipPx(plPrev, sx, sy + step, &o01);
                    uint16_t* pC  = mipPx(plPrev, sx + step, sy + step, &o11);
                    uint16_t* d   = mipPx(pl, gx, gy, &okd);
                    if (!o00 || !o10 || !o01 || !o11 || !okd) continue;
                    for (int ch = 0; ch < 4; ch++)
                        d[ch] = (uint16_t)((p00[ch] + pA[ch] + pB[ch] + pC[ch] + 2) >> 2);
                    rebuilt++;
                }
        }
        logf("[writeback] mip%d 重建 %d 像素\n", k, rebuilt);
    }
    free(writtenTiles);
    return written;
}

// 多图层写回编排（P2-9）：一次锁、一个 0x47 撤销分组覆盖所有选中图层，
// SAI 里按一次 Ctrl+Z 即同时撤销全部图层的液化。
static int WritebackLayersImpl(const uint16_t* const* bufs, const uint16_t* const* origs,
                               const uint64_t* layers,
                               int nLayers, uint32_t W, uint32_t H) {
    SaiApi sai;
    if (!sai.init()) return -1;
    if (nLayers <= 0) return 0;
    sai.lock();
    uint64_t O = rd64((uintptr_t)sai.app + OFF_UNDO_MGR);
    uint64_t anchor = sai.selLayer();
    uint32_t anchorId = anchor ? rd32((uintptr_t)anchor + OFF_LAYER_ID) : 0;
    if (anchor && rd64((uintptr_t)O + 0xa0) != 0) sai.finishGroup(sai.app, anchorId);
    sai.group(sai.app, 0x47, 1);
    int total = 0;
    for (int i = 0; i < nLayers; i++) {
        WriteSession ws{sai};
        if (!ws.begin(layers[i], false)) {
            logf("[writeback] 层 %d begin 失败，跳过\n", i);
            continue;
        }
        int r = WritebackOneLayer(ws, bufs[i], origs[i], W, H);
        ws.commit(i == nLayers - 1); // 仅最后一层关闭分组并出锁
        if (r > 0) total += r;
        logf("[writeback] 层 %d 瓦片=%d cSync=%d cMiss=%d\n", i, r, ws.cSync, ws.cMiss);
    }
    sai.midCommit(sai.app);
    if (anchor) sai.finishGroup(sai.app, anchorId);
    sai.unlock();
    sai.redraw(g_saiWnd);
    // redraw1/2/3 只刷新内部缓存；画布视图是子窗口，需 RDW_ALLCHILDREN
    // 连子窗一起失效并同步重绘，否则用户要手动缩放才看到液化结果。
    if (g_saiWnd) RedrawWindow(g_saiWnd, NULL, NULL,
                               RDW_INVALIDATE | RDW_ALLCHILDREN |
                               RDW_ERASE | RDW_UPDATENOW);
    return total;
}

// MinGW 不支持 __try/__except；用「VEH + setjmp」实现故障跳车：
// 进入写回前 setjmp 设好回跳点并登记到 TLS；VehHandler 发现回跳点
// 有效就 longjmp 回来——即使崩在 SAI 内部 API（writeTile/commit），
// 也只让本次写回失败，不让 SAI 进程死掉。
#include <setjmp.h>
static jmp_buf g_wbBail;
static volatile LONG g_wbBailActive = 0;

// 跳车外壳：即使故障发生在 SAI 内部 API（writeTile/commit），
// 也只让本次写回失败，不让 SAI 进程死掉
static int WritebackLayers(const uint16_t* const* bufs, const uint16_t* const* origs,
                           const uint64_t* layers,
                           int nLayers, uint32_t W, uint32_t H) {
    int rc;
    InterlockedExchange(&g_wbBailActive, 1);
    if ((rc = setjmp(g_wbBail)) == 0) {
        rc = WritebackLayersImpl(bufs, origs, layers, nLayers, W, H);
    } else {
        logf("[writeback] 故障跳车 rc=%d\n", rc);
        rc = -3;
    }
    InterlockedExchange(&g_wbBailActive, 0);
    return rc;
}

namespace liquefy { namespace sui {
bool wbBailArmed() { return g_wbBailActive != 0; }
void wbBailJump(int code) {
    logf("[writeback] 跳车触发 code=%08lx\n", (long)code);
    longjmp(g_wbBail, -3);
}
}}

static bool Sui_Snapshot(uint16_t** outBuf, uint32_t* outW, uint32_t* outH, int* outTiles,
                         uint64_t* outLayers, uint16_t** outLayerBufs, int* outN) {
    if (!g_saiWnd) return false;
    SnapshotJob job = {};
    SendMessageW(g_saiWnd, WM_SAIHOOKS_SNAPSHOT, 0, (LPARAM)&job);
    *outBuf = job.buf; *outW = job.W; *outH = job.H; *outTiles = job.tiles;
    *outN = job.nLayers;
    for (int i = 0; i < job.nLayers; i++) {
        outLayers[i] = job.layers[i];
        outLayerBufs[i] = job.layerBufs[i];
    }
    return job.tiles >= 0;
}

// ---------- SAI 主窗子类化（保留 KIMI_SAIHOOKS_CMD IPC 通道）----------
static LRESULT CALLBACK SaiHookProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_SAIHOOKS_SNAPSHOT) {
        SnapshotJob* job = (SnapshotJob*)l;
        SaiApi sai;
        if (!sai.init()) { job->tiles = -1; logf("[ui] sai.init 失败\n"); return TRUE; }
        sai.lock();
        // P2-9：枚举多选图层（自底向顶），逐层快照后预乘合成预览
        uint64_t ls[MAX_LIQUEFY_LAYERS];
        int n = selectedLayers(sai, ls, MAX_LIQUEFY_LAYERS);
        uint32_t W = sai.canvasW(), H = sai.canvasH();
        job->nLayers = 0; job->buf = NULL; job->W = W; job->H = H; job->tiles = 0;
        if (W && H && n > 0) {
            uint16_t* comp = (uint16_t*)calloc((size_t)W * H, 8);
            for (int i = 0; i < n; i++) {
                uint16_t* lb = NULL; uint32_t lw = 0, lh = 0;
                int t = snapshotLayer(sai, ls[i], &lb, &lw, &lh);
                if (t < 0 || !lb) { free(lb); continue; }
                job->layers[job->nLayers] = ls[i];
                job->layerBufs[job->nLayers] = lb;
                job->nLayers++;
                job->tiles += t;
                // 预乘 over 合成（0x4000 量程）：comp = lb over comp
                size_t np = (size_t)W * H;
                for (size_t px = 0; px < np; px++) {
                    uint16_t* s = lb + px * 4; uint16_t* d = comp + px * 4;
                    uint32_t a = s[3], inv = 0x4000 - a;
                    for (int ch = 0; ch < 3; ch++)
                        d[ch] = (uint16_t)(s[ch] + ((d[ch] * inv) >> 14));
                    d[3] = (uint16_t)(a + ((d[3] * inv) >> 14));
                }
            }
            job->buf = comp;
        }
        sai.unlock();
        logf("[ui] 快照完成 层数=%d 瓦片=%d buf=%p\n",
             job->nLayers, job->tiles, (void*)job->buf);
        if (job->nLayers == 0) { free(job->buf); job->buf = NULL; job->tiles = -1; }
        return TRUE;
    }
    if (m == WM_SAIHOOKS_WRITEBACK) {
        logf("[ui] WRITEBACK 作业收到\n");
        WritebackJob* job = (WritebackJob*)l;
        job->result = WritebackLayers(job->bufs, job->origs, job->layers, job->nLayers, job->W, job->H);
        logf("[ui] 写回完成 result=%d\n", job->result);
        return TRUE;
    }
    if (m == g_ipcMsg) {
        logf("[IPC] mode=%d\n", (int)l);
        if (l == 99) { // 自动化测试：拉起液化会话（等同热键）
            logf("[IPC] 触发会话\n");
            PostMessageW(g_msgWnd, WM_HOTKEY, 0, 0);
        } else if (l == 98) { // 无头自检：不开窗，数值断言写日志
            logf("[IPC] 触发无头自检\n");
            PostMessageW(g_msgWnd, WM_SAIHOOKS_SELFTEST, 0, 0);
        }
        return 0;
    }
    if (m == WM_SIZE) placeLauncher(); // 主窗缩放时重定位启动按钮
    return CallWindowProcW(g_oldProc, h, m, w, l);
}

// ---------- SAI 内嵌「液化」启动按钮（P2-10：不依赖快捷键）----------
static HWND g_launcherBtn = NULL;
static void placeLauncher() {
    if (!g_launcherBtn || !g_saiWnd) return;
    if (IsIconic(g_saiWnd)) return; // 最小化时客户区无意义，别乱动
    RECT rc; GetClientRect(g_saiWnd, &rc);
    if (rc.right < 200 || rc.bottom < 100) return; // 过渡态尺寸，等正常 WM_SIZE
    MoveWindow(g_launcherBtn, rc.right - 76, rc.bottom - 30, 70, 24, TRUE);
    // SAI 的面板布局会把新子窗压到 Z 序底部，强制置顶按钮
    SetWindowPos(g_launcherBtn, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}
static LRESULT CALLBACK LauncherProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HBRUSH br = CreateSolidBrush(RGB(0x3a, 0x6e, 0xa5));
        FillRect(dc, &rc, br); DeleteObject(br);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
        HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HDC old = (HDC)SelectObject(dc, f);
        DrawTextW(dc, L"液化", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, old);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONUP:
        logf("[launcher] 点击启动液化\n");
        PostMessageW(g_msgWnd, WM_HOTKEY, 0, 0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

struct EnumCtx { DWORD pid; HWND* wins; int* nw; };
static BOOL CALLBACK EnumSaiWnd(HWND h, LPARAM lp) {
    auto c = (EnumCtx*)lp;
    DWORD p = 0; GetWindowThreadProcessId(h, &p);
    if (p != c->pid || !IsWindowVisible(h)) return TRUE;
    wchar_t t[256]; GetWindowTextW(h, t, 256);
    if (wcsstr(t, L"PaintTool SAI") && *c->nw < 8) c->wins[(*c->nw)++] = h;
    return TRUE;
}

// ---------- 会话线程 ----------
static DWORD WINAPI SessionThread(LPVOID) {
    int r = liquefy::Run(g_saiWnd);
    logf("[session] 结束 result=%d\n", r);
    InterlockedExchange(&g_sessionRunning, 0);
    if (g_msgWnd) PostMessageW(g_msgWnd, WM_SAIHOOKS_SESSION_END, (WPARAM)r, 0);
    return 0;
}

// ---------- 消息窗 ----------
static LRESULT CALLBACK MsgWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_TIMER:
        if (w == 1) { // 热键被占用的重试（对方退出后自动注册成功）
            if (!RegisterHotKey(h, 1, liquefy::g_cfg.hotkeyModifiers, liquefy::g_cfg.hotkeyVk)) {
                logf("热键重试仍失败 err=%lu\n", GetLastError());
            } else {
                KillTimer(h, 1);
                logf("热键注册成功(重试): mods=%d vk=%d\n",
                     liquefy::g_cfg.hotkeyModifiers, liquefy::g_cfg.hotkeyVk);
            }
        } else if (w == 2) { // 启动按钮周期性归位（恢复/最大化时序不可控，轮询兜底）
            placeLauncher();
        }
        return 0;
    case WM_HOTKEY: {
        placeLauncher(); // 兜底：启动时序错过的布局在此补位
        if (InterlockedCompareExchange(&g_sessionRunning, 1, 0) == 0) {
            logf("[hotkey] 拉起液化会话\n");
            g_sessionThread = CreateThread(NULL, 0, SessionThread, NULL, 0, NULL);
            if (!g_sessionThread) InterlockedExchange(&g_sessionRunning, 0);
        } else {
            logf("[hotkey] 会话进行中，忽略\n");
        }
        return 0;
    }
    case WM_SAIHOOKS_SESSION_END:
        if (g_sessionThread) { CloseHandle(g_sessionThread); g_sessionThread = NULL; }
        return 0;
    case WM_SAIHOOKS_SELFTEST: {
        logf("[selftest] 线程启动\n");
        HANDLE t = CreateThread(NULL, 0, [](LPVOID) -> DWORD {
            int r = liquefy::SelfTest();
            logf("[selftest] 完成 r=%d\n", r);
            return 0;
        }, NULL, 0, NULL);
        if (t) CloseHandle(t);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------- Setup ----------
static DWORD WINAPI SetupThread(LPVOID) {
    openLog("sai_hooks P2");
    loadIni();

    DWORD pid = GetCurrentProcessId();
    HWND wins[8]; int nw = 0;
    EnumCtx ctx = { pid, wins, &nw };
    EnumWindows(EnumSaiWnd, (LPARAM)&ctx);
    if (nw == 0) { logf("未找到 SAI 主窗\n"); return 0; }
    g_saiWnd = wins[0];
    g_ipcMsg = RegisterWindowMessageW(L"KIMI_SAIHOOKS_CMD");
    g_oldProc = (WNDPROC)GetWindowLongPtrW(g_saiWnd, GWLP_WNDPROC);
    SetWindowLongPtrW(g_saiWnd, GWLP_WNDPROC, (LONG_PTR)SaiHookProc);
    logf("SAI 主窗=%p IPC msg=%#x\n", (void*)g_saiWnd, g_ipcMsg);

    // 消息窗（隐藏）+ 热键
    static const wchar_t kClass[] = L"SAIHOOKS_MSG_WND";
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = MsgWndProc;
    wc.hInstance = g_module;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    g_msgWnd = CreateWindowExW(0, kClass, L"SAIHOOKS_MSG", 0, 0, 0, 0, 0,
        HWND_MESSAGE, NULL, g_module, NULL);
    if (!g_msgWnd) { logf("消息窗创建失败\n"); return 0; }

    // SAI 内嵌「液化」启动按钮（右下角，替代快捷键入口）
    WNDCLASSW lwc = { 0 };
    lwc.lpfnWndProc = LauncherProc;
    lwc.hInstance = g_module;
    lwc.hCursor = LoadCursor(NULL, IDC_HAND);
    lwc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    lwc.lpszClassName = L"SAIHOOKS_LAUNCHER";
    RegisterClassW(&lwc);
    g_launcherBtn = CreateWindowExW(0, L"SAIHOOKS_LAUNCHER", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0, 0, 70, 24, g_saiWnd, NULL, g_module, NULL);
    placeLauncher();
    SetTimer(g_msgWnd, 2, 2000, NULL); // 按钮周期性归位（恢复/最大化时序兜底）
    logf("启动按钮 hwnd=%p\n", (void*)g_launcherBtn);

    if (!RegisterHotKey(g_msgWnd, 1, liquefy::g_cfg.hotkeyModifiers, liquefy::g_cfg.hotkeyVk)) {
        logf("RegisterHotKey 失败 err=%lu，启动 3 秒重试\n", GetLastError());
        SetTimer(g_msgWnd, 1, 3000, NULL);
    } else
        logf("热键已注册: mods=%d vk=%d\n", liquefy::g_cfg.hotkeyModifiers, liquefy::g_cfg.hotkeyVk);

    // 消息循环（热键 + 会话通知都走这里）
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

namespace liquefy {
namespace sui {
bool snapshot(uint16_t** outBuf, uint32_t* outW, uint32_t* outH, int* outTiles,
              uint64_t* outLayers, uint16_t** outLayerBufs, int* outN) {
    return Sui_Snapshot(outBuf, outW, outH, outTiles, outLayers, outLayerBufs, outN);
}
// 写回：UI 线程执行 COW 序列；多层共用一个撤销分组
int writeback(const uint16_t* const* bufs, const uint16_t* const* origs,
              const uint64_t* layers, int n, uint32_t W, uint32_t H) {
    if (!g_saiWnd) return -1;
    WritebackJob job = {};
    job.nLayers = n; job.W = W; job.H = H;
    for (int i = 0; i < n && i < MAX_LIQUEFY_LAYERS; i++) {
        job.bufs[i] = bufs[i]; job.origs[i] = origs[i]; job.layers[i] = layers[i];
    }
    SendMessageW(g_saiWnd, WM_SAIHOOKS_WRITEBACK, 0, (LPARAM)&job);
    return job.result;
}
}
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
        g_module = mod;
        HANDLE t = CreateThread(NULL, 0, SetupThread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
