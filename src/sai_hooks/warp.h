// warp.h — 液化 GPU 引擎：位移场 / 冻结蒙版 / 笔刷涂抹 / 变形预览
#pragma once
#include <windows.h>
#include <d3d11.h>
#include <stdint.h>

namespace warp {

enum Brush {
    BRUSH_PUSH = 0,   // 推拉：沿笔刷移动方向带动像素
    BRUSH_BLOAT,      // 膨胀：向外推
    BRUSH_PINCH,      // 收缩：向内聚
    BRUSH_SMOOTH,     // 平滑：柔化位移
    BRUSH_RESTORE,    // 重建：恢复原始图像
    BRUSH_FREEZE,     // 冻结蒙版：标记保护
    BRUSH_UNFREEZE,   // 擦除冻结
    BRUSH_COUNT
};

class Engine {
public:
    bool init(ID3D11Device* device, ID3D11DeviceContext* ctx,
              const uint16_t* rgba16, uint32_t W, uint32_t H); // rgba16 为 0x4000 量程
    // 多图层写回：把另一层快照换进快照纹理（位移场不变，重放同一变形）
    bool uploadSource(const uint16_t* rgba16);
    void shutdown();

    uint32_t width() const { return W; }
    uint32_t height() const { return H; }

    // 笔刷：x,y=图像像素坐标，dx,dy=本次笔刷位移（仅推拉用），radius/strength=像素/0..1
    void dab(Brush type, float x, float y, float dx, float dy,
             float radius, float strength, float hardness);
    void beginStroke(); // 当前位移场压栈（按笔撤销用，深度 16）
    void undoStroke();
    void redoStroke();
    bool canUndo() const { return undoCount > 0; }
    bool canRedo() const { return redoCount > 0; }
    void reset();       // 位移场与冻结蒙版清零

    bool previewDirty() const { return dirty; }
    void renderPreview();              // 位移场重采样 -> 预览纹理（白底压平+冻结提示，UI 用）
    void renderResult();               // 同上色器但不压平/不提示（写回 SAI 用）
    ID3D11ShaderResourceView* previewSrv() { return previewSRV; }

    // 回读预览为 32-bit RGBA（malloc，调用方 free；失败返回 NULL）
    uint8_t* readPreview();
    // 回读写回结果为 16-bit RGBA（0..0xFFFF 量程，malloc，调用方 free）
    uint16_t* readResult16();

private:
    bool createFieldTex(ID3D11Texture2D** tex, ID3D11RenderTargetView** rtv,
                        ID3D11ShaderResourceView** srv, DXGI_FORMAT fmt);
    bool compilePS(const char* src, ID3D11PixelShader** ps);
    bool createQuadVB();

    ID3D11Device* dev = NULL;
    ID3D11DeviceContext* ctx = NULL;
    uint32_t W = 0, H = 0;

    // 快照
    ID3D11Texture2D* snap = NULL;
    ID3D11ShaderResourceView* snapSRV = NULL;
    // 位移场 ping-pong（A=当前态）
    ID3D11Texture2D *fieldA = NULL, *fieldB = NULL;
    ID3D11RenderTargetView *fieldARTV = NULL, *fieldBRTV = NULL;
    ID3D11ShaderResourceView *fieldASRV = NULL, *fieldBSRV = NULL;
    // 按笔撤销/重做：双 16 级环形栈（快照为「该笔之前」的位移场）
    static const int kStackDepth = 16;
    ID3D11Texture2D* undoStack[kStackDepth];
    ID3D11Texture2D* redoStack[kStackDepth];
    int undoStart = 0, undoCount = 0;
    int redoStart = 0, redoCount = 0;
    // 冻结蒙版（R8，ping-pong：RT 与 SRV 不能同纹理，否则 Draw 被静默丢弃）
    ID3D11Texture2D *freezeA = NULL, *freezeB = NULL;
    ID3D11RenderTargetView *freezeARTV = NULL, *freezeBRTV = NULL;
    ID3D11ShaderResourceView *freezeASRV = NULL, *freezeBSRV = NULL;
    // 预览
    ID3D11Texture2D* preview = NULL;
    ID3D11RenderTargetView* previewRTV = NULL;
    ID3D11ShaderResourceView* previewSRV = NULL;
    // 写回结果（与预览同尺寸，预乘 RGBA，不压白底）
    ID3D11Texture2D* result = NULL;
    ID3D11RenderTargetView* resultRTV = NULL;

    ID3D11VertexShader* vsQuad = NULL;      // 全屏三角形
    ID3D11InputLayout* ilEmpty = NULL;      // 无输入布局（VS 只用 SV_VertexID；NULL 布局会导致 Draw 被丢弃）
    ID3D11PixelShader *psSplat = NULL, *psFreeze = NULL, *psWarp = NULL, *psResult = NULL;
    ID3D11Buffer* quadVB = NULL;            // 笔刷包围盒动态四边形
    ID3D11Buffer* constBuf = NULL;          // 笔刷参数
    ID3D11SamplerState* smp = NULL;
    ID3D11BlendState* bsReplace = NULL;
    ID3D11RasterizerState* rsNoCull = NULL; // 全屏三角形为 CCW 绕向，默认背面剔除会剔掉
    bool dirty = true;
};

} // namespace warp
