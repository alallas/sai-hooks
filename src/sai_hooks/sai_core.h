// sai_core.h — SAI2 核心层：偏移、锁、图层快照、COW 写回（全部经 P1 实证）
// 目标版本：SAI2 2024.02.22 x64（仅此版本）。结论见 docs/sai2-offsets.md §7。
#pragma once
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>

namespace saicore {

extern FILE* g_log; // 定义在 dllmain.cpp（头文件静态会导致每个 TU 一份）
static inline void logf(const char* fmt, ...) {
    if (!g_log) return;
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap); fflush(g_log);
}

// ---------- 偏移 ----------
#define RVA_APP        0x321118
#define OFF_CANVAS_W   0x250
#define OFF_CANVAS_H   0x254
#define OFF_SEL_LAYER  0x130
#define OFF_UNDO_MGR   0x168
#define OFF_LAYER_MIP0 0xf8
#define OFF_LAYER_ID   0x240
#define PL_VT          0x10
#define PL_CF_ROWS     0x38
#define PL_C_ROWS      0x40
#define PL_F_ROWS      0x68
#define PL_W_ROWS      0x70
#define PL_P_ROWS      0x78

#define TILE           32
#define FULL_SCALE     0x4000
#define MAX_LIQUEFY_LAYERS 8

// 多选标志位（2026-11-06 双状态 diff 实证）：
// 图层 +0x249 != 0 = 处于多选集合；单选时该字节为 0，用 app+0x130 兜底
#define OFF_LAYER_SEL    0x249
// 图层 +0x10 = 父对象（图层列表容器），父+0x00 = 兄弟链头（面板自上而下）
#define OFF_LAYER_PARENT 0x10
#define OFF_LAYER_NEXT   0x00

// 像素内存序（受控写回实证）：BGRA u16 预乘，满量程 0x4000
struct PixU16 { uint16_t b, g, r, a; };

static inline uint32_t rd32(uintptr_t p) { return *(volatile uint32_t*)p; }
static inline uint64_t rd64(uintptr_t p) { return *(volatile uint64_t*)p; }
static inline void     wr64(uintptr_t p, uint64_t v) { *(volatile uint64_t*)p = v; }
static inline int is_page(uint64_t p) {
    return p && (p % 0x2000 == 0) && p > 0x1000000 && p < 0x7FFF00000000ULL;
}

// 函数指针（sai2.exe + RVA）
struct SaiApi {
    uintptr_t base;
    uint64_t app;
    // 锁
    void (*lock)(void);        // 0xc900
    void (*unlock)(void);      // 0xc920
    // 撤销序列
    void     (*group)(uint64_t, uint32_t, int);          // 0x2e880
    uint64_t (*makeRecord)(uint64_t, uint64_t, uint32_t); // 0x2de30
    uint32_t (*cow)(uint64_t*, uint64_t*, uint32_t, char, uint64_t, uint64_t, uint32_t); // 0x74690
    uint32_t (*normalize)(uint32_t, uint64_t, uint32_t, uint64_t*); // 0x3bdc0
    void     (*regRect)(uint64_t, uint64_t, uint64_t, uint8_t*);    // 0x2ebd0
    void     (*capture)(uint64_t, uint64_t, uint64_t);              // 0x2eca0
    void     (*midCommit)(uint64_t);                                // 0x25ac0
    void     (*finishGroup)(uint64_t, uint32_t);                    // 0x36620
    // 重绘
    void (*redraw1)(uint64_t); // 0x20ed0
    void (*redraw2)(uint64_t); // 0x22920
    void (*redraw3)(uint64_t, uint32_t); // 0x22ab0，图层操作尾部固定组合第三件（FUN_14004d810 尾部实证）

    int init() {
        HMODULE m = GetModuleHandleW(L"sai2.exe");
        if (!m) return 0;
        base = (uintptr_t)m;
        app = rd64(base + RVA_APP);
        if (!app) return 0;
#define F(name, rva) name = (decltype(name))(base + (rva))
        F(lock, 0xc900);       F(unlock, 0xc920);
        F(group, 0x2e880);     F(makeRecord, 0x2de30);
        F(cow, 0x74690);       F(normalize, 0x3bdc0);
        F(regRect, 0x2ebd0);   F(capture, 0x2eca0);
        F(midCommit, 0x25ac0); F(finishGroup, 0x36620);
        F(redraw1, 0x20ed0);   F(redraw2, 0x22920);   F(redraw3, 0x22ab0);
#undef F
        return 1;
    }
    uint64_t selLayer() const { return rd64((uintptr_t)app + OFF_SEL_LAYER); }
    uint32_t canvasW() const { return rd32((uintptr_t)app + OFF_CANVAS_W); }
    uint32_t canvasH() const { return rd32((uintptr_t)app + OFF_CANVAS_H); }
    void redraw(HWND hwnd) const {
        redraw1(app); redraw2(app); redraw3(app, 0x10);
        if (hwnd) RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }
};

// ---------- 快照：指定图层 -> RGBA u16 预乘（内存序 R,G,B,A，便于 GPU 直传）----------
// outBuf = malloc(W*H*8)。返回瓦片数，失败返回 -1。
static inline int snapshotLayer(const SaiApi& sai, uint64_t layer,
                                uint16_t** outBuf, uint32_t* outW, uint32_t* outH) {
    uint32_t W = sai.canvasW(), H = sai.canvasH();
    if (!W || !H || W > 16384 || H > 16384) return -1;
    if (!layer) return -1;
    uint64_t plane = rd64((uintptr_t)layer + OFF_LAYER_MIP0);
    if (!plane) return -1;
    uint64_t flagsArr = rd64((uintptr_t)plane + PL_F_ROWS); // f 行（§7.6 行布局）
    uint64_t pagesArr = rd64((uintptr_t)plane + PL_W_ROWS); // w 行：页指针或内联纯色
    if (!flagsArr || !pagesArr) return -1;

    uint16_t* img = (uint16_t*)calloc((size_t)W * H, 8);
    if (!img) return -1;

    uint32_t tilesY = (H + TILE - 1) / TILE, tilesX = (W + TILE - 1) / TILE;
    int nTiles = 0;
    for (uint32_t ty = 0; ty < tilesY; ty++) {
        uint64_t fr = rd64((uintptr_t)flagsArr + ty * 8);
        uint64_t pr = rd64((uintptr_t)pagesArr + ty * 8);
        if (!fr || !pr) continue;
        for (uint32_t tx = 0; tx < tilesX && tx < 32; tx++) {
            uint8_t f = *(volatile uint8_t*)(uintptr_t)(fr + tx);
            if (f == 0) continue; // 空瓦片
            uint64_t page = rd64((uintptr_t)pr + tx * 8);
            if (!is_page(page)) {
                // 内联纯色描述符（03bdc0 折叠，§7.6）：前 8 字节即 BGRA u16 像素值，整瓦片同色
                if (page == 0) continue;
                PixU16 c = *(const PixU16*)&page;
                for (uint32_t py = 0; py < TILE; py++) {
                    uint32_t gy = ty * TILE + py;
                    if (gy >= H) break;
                    for (uint32_t pxI = 0; pxI < TILE; pxI++) {
                        uint32_t gx = tx * TILE + pxI;
                        if (gx >= W) break;
                        uint16_t* o = img + ((size_t)gy * W + gx) * 4;
                        o[0] = c.r; o[1] = c.g; o[2] = c.b; o[3] = c.a;
                    }
                }
                nTiles++;
                continue;
            }
            nTiles++;
            const PixU16* src = (const PixU16*)(uintptr_t)page;
            for (uint32_t py = 0; py < TILE; py++) {
                uint32_t gy = ty * TILE + py;
                if (gy >= H) break;
                for (uint32_t pxI = 0; pxI < TILE; pxI++) {
                    uint32_t gx = tx * TILE + pxI;
                    if (gx >= W) break;
                    PixU16 c = src[py * TILE + pxI];
                    uint16_t* o = img + ((size_t)gy * W + gx) * 4;
                    o[0] = c.r; o[1] = c.g; o[2] = c.b; o[3] = c.a;
                }
            }
        }
    }
    *outBuf = img; *outW = W; *outH = H;
    return nTiles;
}

// ---------- 多选图层枚举（2026-11-06 diff 实证，P2-9）----------
// 规则：沿父对象链头遍历兄弟链，收集 +0x249!=0 的图层；一个都没找到则
// 退回当前图层（单选行为与旧版一致）。返回自底向顶顺序（合成顺序）。
static inline int selectedLayers(const SaiApi& sai, uint64_t* out, int maxN) {
    uint64_t sel = sai.selLayer();
    if (!sel) return 0;
    uint64_t parent = rd64((uintptr_t)sel + OFF_LAYER_PARENT);
    uint64_t head = parent ? rd64((uintptr_t)parent + 0) : 0;
    if (!head) head = sel;
    int n = 0, nSeen = 0;
    uint64_t seen[64];
    for (uint64_t p = head; p && nSeen < 64; p = rd64((uintptr_t)p + OFF_LAYER_NEXT)) {
        int dup = 0;
        for (int i = 0; i < nSeen; i++) if (seen[i] == p) { dup = 1; break; }
        if (dup) break;
        seen[nSeen++] = p;
        if (*(volatile uint8_t*)(uintptr_t)(p + OFF_LAYER_SEL)) {
            if (n < maxN) out[n++] = p;
        }
    }
    if (n == 0) { out[0] = sel; n = 1; }
    for (int i = 0; i < n / 2; i++) { uint64_t t = out[i]; out[i] = out[n-1-i]; out[n-1-i] = t; }
    return n;
}

// ---------- COW 写回序列（P1-m4 终验，§7.6）----------
// 锁外勿调。groupActive=本 DLL 已开过 0x47 分组。
struct WriteSession {
    const SaiApi& sai;
    uint64_t layer, plane, vt, O;
    uint64_t rec;
    uint32_t layerId;
    bool open;
    int cSync = 0, cMiss = 0; // 诊断：视图缓存同步命中/未命中

    // openGroup=true：进锁 + 关闭残留分组 + 开 0x47 分组（多图层写回时仅首层开）
    bool begin(uint64_t layerIn, bool openGroup) {
        layer = layerIn ? layerIn : sai.selLayer();
        if (!layer) return false;
        plane = rd64((uintptr_t)layer + OFF_LAYER_MIP0);
        if (!plane) return false;
        vt = rd64((uintptr_t)plane + PL_VT);
        O = rd64((uintptr_t)sai.app + OFF_UNDO_MGR);
        layerId = rd32((uintptr_t)layer + OFF_LAYER_ID);
        if (openGroup) {
            sai.lock();
            if (rd64((uintptr_t)O + 0xa0) != 0) sai.finishGroup(sai.app, layerId);
            sai.group(sai.app, 0x47, 1);
        }
        rec = sai.makeRecord(sai.app, layer, 2);
        if (!rec) { if (openGroup) sai.unlock(); return false; }
        open = true;
        return true;
    }
    // 单瓦片 COW + 写像素 + 规范化
    bool writeTile(uint32_t tx, uint32_t ty, const PixU16* pixels) {
        uint64_t fa = rd64((uintptr_t)plane + PL_F_ROWS);
        uint64_t wa = rd64((uintptr_t)plane + PL_W_ROWS);
        uint64_t pa = rd64((uintptr_t)plane + PL_P_ROWS);
        uint64_t fr = rd64((uintptr_t)fa + ty * 8);
        uint64_t wr = rd64((uintptr_t)wa + ty * 8);
        uint64_t pr = rd64((uintptr_t)pa + ty * 8);
        if (!fr || !wr || !pr) return false;
        uint8_t flag = *(volatile uint8_t*)(uintptr_t)(fr + tx);
        uint32_t nf = sai.cow((uint64_t*)(uintptr_t)(wr + tx * 8),
                              (uint64_t*)(uintptr_t)(pr + tx * 8), flag, 1, 0, vt, 0);
        *(volatile uint8_t*)(uintptr_t)(fr + tx) = (uint8_t)nf;
        uint64_t wcopy = rd64((uintptr_t)wr + tx * 8);
        if (!is_page(wcopy)) return false;
        memcpy((void*)(uintptr_t)wcopy, pixels, TILE * TILE * 8);
        uint32_t nf2 = sai.normalize(0, vt, nf, (uint64_t*)(uintptr_t)(wr + tx * 8));
        *(volatile uint8_t*)(uintptr_t)(fr + tx) = (uint8_t)nf2;
        // 视图缓存就地同步（§4.4/§7.6）：屏幕显示读 +0x40 c 行。
        // 不改指针、不改标志（撤销执行器不维护 c 别名，改指针会破坏撤销）；
        // c 条目有页就原样 memcpy 新像素，两路（master/c）内容一致，
        // 撤销由 SAI 自己的显示刷新路径从 master 重建，互不干扰。
        uint64_t cArr  = rd64((uintptr_t)plane + PL_C_ROWS);
        uint64_t cr    = cArr ? rd64((uintptr_t)cArr + ty * 8) : 0;
        uint64_t cpage = cr ? rd64((uintptr_t)cr + tx * 8) : 0;
        if (is_page(cpage)) {
            memcpy((void*)(uintptr_t)cpage, pixels, TILE * TILE * 8);
            cSync++;
            if (cSync <= 3) // 诊断样本：看第二次写回时缓存页状态
                logf("[writeback] c同步 t=(%u,%u) f=%02x w=%p c=%p\n",
                     tx, ty, (unsigned)nf2, (void*)(uintptr_t)wcopy, (void*)(uintptr_t)cpage);
        } else cMiss++;
        return true;
    }
    void regDirtyRect(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
        uint8_t rectbuf[0xc0];
        memset(rectbuf, 0, sizeof(rectbuf));
        *(int*)(rectbuf + 0) = (int)x0; *(int*)(rectbuf + 4) = (int)y0;
        *(int*)(rectbuf + 8) = (int)x1; *(int*)(rectbuf + 12) = (int)y1;
        sai.regRect(rec, sai.app, layer, rectbuf);
    }
    bool commit(bool closeGroup) {
        if (!open) return false;
        sai.capture(rec, sai.app, layer);
        if (closeGroup) {
            sai.midCommit(sai.app);
            sai.finishGroup(sai.app, layerId);
            sai.unlock();
        }
        open = false;
        return true;
    }
    void abort(bool closeGroup) {
        if (!open) return;
        if (closeGroup) sai.unlock();
        open = false;
    }
};

} // namespace saicore
