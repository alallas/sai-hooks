// liquefy.h — 液化会话：入口由 dllmain 在热键时调用（独立线程阻塞运行）
#pragma once
#include <windows.h>
#include <stdint.h>

#ifndef MAX_LIQUEFY_LAYERS
#define MAX_LIQUEFY_LAYERS 8
#endif

namespace liquefy {

struct Config {
    int hotkeyModifiers; // MOD_CONTROL|MOD_SHIFT
    int hotkeyVk;        // 'X'
    int brushSize;      // px
    int brushStrength;  // 1-100
    int brushDensity;   // 1-100
    int brushHardness;  // 1-100
    int brushType;      // warp::Brush
};

// 会话结果码
enum Result { OK = 0, CANCELLED = 1, SNAPSHOT_FAIL = 2, INIT_FAIL = 3 };

extern Config g_cfg; // 由 dllmain 从 ini 装入

namespace sui {
// UI 线程代理：SAI 内部操作必须在 SAI UI 线程执行（§4.3），由 dllmain 实现
// 返回合成预览 buf（调用方 free）；各选中图层原始快照经 outLayers/outLayerBufs
// 返回（自底向顶，共 outN 层，调用方负责 free），供写回时逐层重放变形
bool snapshot(uint16_t** outBuf, uint32_t* outW, uint32_t* outH, int* outTiles,
              uint64_t* outLayers, uint16_t** outLayerBufs, int* outN);
// 写回：UI 线程执行 COW 序列；多层共用一个撤销分组
// bufs=变形结果(0xFFFF)，origs=各层原始快照(0x4000)，用于逐瓦片差异判定
int writeback(const uint16_t* const* bufs, const uint16_t* const* origs,
              const uint64_t* layers, int n, uint32_t W, uint32_t H);

// 写回故障跳车：WritebackLayer 进入时武装回跳点（setjmp），
// liquefy 的 VEH 捕获到异常时调用 wbBailJump 让进程活着回到写回入口。
bool wbBailArmed();
void wbBailJump(int code);
}

// 阻塞运行整个液化会话（快照 → UI → 提交/取消），返回 Result
int Run(HWND saiWnd);

// 无头自检：快照 → 引擎初始化 → 合成涂抹 → 数值断言，不开任何窗口。
// 结果写日志 [selftest] 行，五张关键帧 BMP 存 SAI 工作区（sai_test_*.bmp）。
int SelfTest();

} // namespace liquefy
