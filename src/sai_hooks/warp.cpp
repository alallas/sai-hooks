// warp.cpp — 液化 GPU 引擎实现（DX11，无 Compute Shader，全部 PS 实现）
#include "warp.h"
#include "sai_core.h"
#include <d3dcompiler.h>
#include <stdio.h>
#include <math.h>

using saicore::logf;

namespace warp {

// ---------- 着色器 ----------

// 全屏三角形 VS：3 个顶点来自静态 VB（避免无输入布局的兼容性问题）
static const char* s_vsQuad = R"H(
struct In { float2 p : POSITION; };
struct Out { float4 pos : SV_POSITION; };
Out main(In i) {
    Out o;
    o.pos = float4(i.p, 0, 1);
    return o;
}
)H";

// 位移场笔刷：对当前场「读-改-写」单个 PS pass（replace 混合，冻结区输出原值）
static const char* s_psSplat = R"H(
cbuffer CBuf {
    float4 brush;    // x,y,radius,hardness
    float4 params;   // x:dx y:dy z:strength w:type(0push 1bloat 2pinch 3smooth 4restore)
    float2 imgSize;
};
Texture2D fieldTex : register(t0);
Texture2D freezeTex : register(t1);
SamplerState smp : register(s0);

float falloff(float d, float r, float hard) {
    float t = saturate(1.0 - d / max(r, 1e-3));
    float inner = hard;
    return smoothstep(0.0, 1.0, saturate((t - inner) / max(1.0 - inner, 1e-3))) * step(d, r);
}

float4 main(float4 pos : SV_POSITION) : SV_Target {
    float2 p = pos.xy;
    float2 old = fieldTex.SampleLevel(smp, p / imgSize, 0).xy;
    float frozen = freezeTex.SampleLevel(smp, p / imgSize, 0).r;
    float d = distance(p, brush.xy);
    float fall = falloff(d, brush.z, brush.w);
    float type = params.w;
    float2 nd = old;
    if (type < 0.5) {            // 推拉
        nd = old + params.xy * params.z * fall;
    } else if (type < 1.5) {     // 膨胀
        float2 dir = p - brush.xy;
        float len = length(dir);
        dir = len > 1e-3 ? dir / len : 0;
        nd = old + dir * params.z * fall * brush.z * 0.06;
    } else if (type < 2.5) {     // 收缩
        float2 dir = p - brush.xy;
        float len = length(dir);
        dir = len > 1e-3 ? dir / len : 0;
        nd = old - dir * params.z * fall * brush.z * 0.06;
    } else if (type < 3.5) {     // 平滑（盒式 4 邻域）
        float2 px = 1.0 / imgSize;
        float2 n = fieldTex.SampleLevel(smp, p / imgSize + float2(0, px.y), 0).xy
                 + fieldTex.SampleLevel(smp, p / imgSize - float2(0, px.y), 0).xy
                 + fieldTex.SampleLevel(smp, p / imgSize + float2(px.x, 0), 0).xy
                 + fieldTex.SampleLevel(smp, p / imgSize - float2(px.x, 0), 0).xy;
        nd = lerp(old, n * 0.25, params.z * fall);
    } else {                     // 重建：位移归零
        nd = old * (1.0 - params.z * fall);
    }
    float keep = (frozen > 0.5 && type < 4.5) ? 1.0 : 0.0; // 冻结区：变形笔刷无效；restore 也无效
    return float4(lerp(nd, old, keep), 0, 1);
}
)H";

// 冻结蒙版笔刷：写 1 或 0。冻结写绝对值；解冻需读旧蒙版（圆内置 0、圆外保留）
// —— 读与写必须分处两张纹理（ping-pong），同纹理 RT+SRV 绑定会让 Draw 被静默丢弃
static const char* s_psFreeze = R"H(
cbuffer CBuf {
    float4 brush;    // x,y,radius,hardness（硬度仅 0/1 语义）
    float4 params;   // w=5 冻结 / 6 擦除
    float2 imgSize;
};
Texture2D maskTex : register(t0);
SamplerState smp : register(s0);
float4 main(float4 pos : SV_POSITION) : SV_Target {
    float2 p = pos.xy;
    float d = distance(p, brush.xy);
    float inside = step(d, brush.z);
    float old = maskTex.SampleLevel(smp, p / imgSize, 0).r;
    // 冻结=叠加涂抹（保留旧遮罩），解冻=橡皮擦（只擦掉笔刷覆盖处）
    float v = (params.w < 5.5) ? max(old, inside) : old * (1.0 - inside);
    return float4(v, 0, 0, 1);
}
)H";

// 预览：快照按位移场重采样，白底压平 + 冻结区淡蓝提示
// 注意：与笔刷共用同一个 48 字节常量缓冲（imgSize 在偏移 32），声明必须和 C++ 布局一致
static const char* s_psWarp = R"H(
cbuffer CBuf {
    float4 brush;
    float4 params;
    float2 imgSize;
};
Texture2D snapTex : register(t0);
Texture2D fieldTex : register(t1);
Texture2D freezeTex : register(t2);
SamplerState smp : register(s0);
float4 main(float4 pos : SV_POSITION) : SV_Target {
    float2 uv = pos.xy / imgSize;
    float2 disp = fieldTex.SampleLevel(smp, uv, 0).xy / imgSize;
    float4 c = snapTex.SampleLevel(smp, uv - disp, 0);
    c.rgb = c.rgb + (1.0 - c.a);            // 预乘 -> 白底压平
    float f = freezeTex.SampleLevel(smp, uv, 0).r;
    if (f > 0.5) c.rgb = lerp(c.rgb, float3(0.72, 0.84, 1.0), 0.42); // 冻结区：浅色标注
    return float4(c.rgb, 1.0);
}
)H";

// 写回：同位移重采样，但不压白底、不叠冻结提示（输出预乘 RGBA 供写回 SAI 图层）
static const char* s_psResult = R"H(
cbuffer CBuf {
    float4 brush;
    float4 params;
    float2 imgSize;
};
Texture2D snapTex : register(t0);
Texture2D fieldTex : register(t1);
SamplerState smp : register(s0);
float4 main(float4 pos : SV_POSITION) : SV_Target {
    float2 uv = pos.xy / imgSize;
    float2 disp = fieldTex.SampleLevel(smp, uv, 0).xy / imgSize;
    return snapTex.SampleLevel(smp, uv - disp, 0);
}
)H";

static bool Compile(const char* src, const char* target, const char* entry, ID3DBlob** blob) {
    ID3DBlob* err = NULL;
    HRESULT hr = D3DCompile(src, strlen(src), NULL, NULL, NULL, entry, target, 0, 0, blob, &err);
    if (hr != S_OK) {
        if (err) {
            logf("[warp] 着色器编译失败: %s\n", (char*)err->GetBufferPointer());
            err->Release();
        }
        return false;
    }
    return true;
}

bool Engine::compilePS(const char* src, ID3D11PixelShader** ps) {
    ID3DBlob* b = NULL;
    if (!Compile(src, "ps_4_0", "main", &b)) return false;
    bool ok = dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), NULL, ps) == S_OK;
    b->Release();
    return ok;
}

bool Engine::createFieldTex(ID3D11Texture2D** tex, ID3D11RenderTargetView** rtv,
                            ID3D11ShaderResourceView** srv, DXGI_FORMAT fmt) {
    D3D11_TEXTURE2D_DESC td;
    ZeroMemory(&td, sizeof(td));
    td.Width = W; td.Height = H;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (dev->CreateTexture2D(&td, NULL, tex) != S_OK) { logf("[warp] 纹理创建失败 fmt=%d\n", fmt); return false; }
    if (rtv && dev->CreateRenderTargetView(*tex, NULL, rtv) != S_OK) { logf("[warp] RTV 创建失败 fmt=%d\n", fmt); return false; }
    if (srv && dev->CreateShaderResourceView(*tex, NULL, srv) != S_OK) { logf("[warp] SRV 创建失败 fmt=%d\n", fmt); return false; }
    return true;
}

bool Engine::createQuadVB() {
    D3D11_BUFFER_DESC bd;
    ZeroMemory(&bd, sizeof(bd));
    bd.ByteWidth = 16 * 4;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return dev->CreateBuffer(&bd, NULL, &quadVB) == S_OK;
}

bool Engine::init(ID3D11Device* device, ID3D11DeviceContext* context,
                  const uint16_t* rgba16, uint32_t w, uint32_t h) {
    dev = device; ctx = context; W = w; H = h;

    // 快照上传（0x4000 -> 0xFFFF 量程归一）
    uint16_t* norm = (uint16_t*)malloc((size_t)W * H * 8);
    if (!norm) return false;
    size_t n = (size_t)W * H * 4;
    for (size_t i = 0; i < n; i++) {
        uint32_t v = rgba16[i];
        norm[i] = (uint16_t)(v >= 0x4000 ? 0xFFFF : v * 4);
    }
    logf("[warp] 引擎就绪 %ux%u\n", W, H);
    D3D11_TEXTURE2D_DESC td;
    ZeroMemory(&td, sizeof(td));
    td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; // P2-9：多图层写回需 UpdateSubresource 换源
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = { norm, W * 8, 0 };
    bool ok = dev->CreateTexture2D(&td, &sd, &snap) == S_OK;
    free(norm);
    if (!ok) { logf("[warp] 快照纹理创建失败\n"); return false; }
    if (dev->CreateShaderResourceView(snap, NULL, &snapSRV) != S_OK) { logf("[warp] 快照 SRV 失败\n"); return false; }

    if (!createFieldTex(&fieldA, &fieldARTV, &fieldASRV, DXGI_FORMAT_R16G16_FLOAT)) return false;
    if (!createFieldTex(&fieldB, &fieldBRTV, &fieldBSRV, DXGI_FORMAT_R16G16_FLOAT)) return false;
    for (int i = 0; i < kStackDepth; i++) {
        if (!createFieldTex(&undoStack[i], NULL, NULL, DXGI_FORMAT_R16G16_FLOAT)) return false;
        if (!createFieldTex(&redoStack[i], NULL, NULL, DXGI_FORMAT_R16G16_FLOAT)) return false;
    }
    if (!createFieldTex(&freezeA, &freezeARTV, &freezeASRV, DXGI_FORMAT_R8_UNORM)) return false;
    if (!createFieldTex(&freezeB, &freezeBRTV, &freezeBSRV, DXGI_FORMAT_R8_UNORM)) return false;
    if (!createFieldTex(&preview, &previewRTV, &previewSRV, DXGI_FORMAT_R16G16B16A16_UNORM)) return false;
    if (!createFieldTex(&result, &resultRTV, NULL, DXGI_FORMAT_R16G16B16A16_UNORM)) return false;
    if (!createQuadVB()) { logf("[warp] quadVB 失败\n"); return false; }

    // 着色器
    ID3DBlob* b = NULL;
    if (!Compile(s_vsQuad, "vs_4_0", "main", &b)) { logf("[warp] vsQuad 编译失败\n"); return false; }
    ok = dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), NULL, &vsQuad) == S_OK;
    D3D11_INPUT_ELEMENT_DESC ie = { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
                                    D3D11_INPUT_PER_VERTEX_DATA, 0 };
    HRESULT hrIL = dev->CreateInputLayout(&ie, 1, b->GetBufferPointer(), b->GetBufferSize(), &ilEmpty);
    if (ok) ok = hrIL == S_OK;
    b->Release();
    if (!ok) { logf("[warp] vsQuad/输入布局创建失败 hr=%08lx vsOK=%d\n", (long)hrIL, (int)(vsQuad != NULL)); return false; }
    // 全屏三角形顶点写入 quadVB（(-1,-1)(3,-1)(-1,3)，覆盖整个视口）
    {
        D3D11_MAPPED_SUBRESOURCE ms;
        if (ctx->Map(quadVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms) != S_OK) return false;
        float tri[6] = { -1, -1,  3, -1,  -1, 3 };
        memcpy(ms.pData, tri, sizeof(tri));
        ctx->Unmap(quadVB, 0);
    }
    b = NULL;
    // 常量缓冲
    D3D11_BUFFER_DESC cbd;
    ZeroMemory(&cbd, sizeof(cbd));
    cbd.ByteWidth = 48; // brush16 + params16 + imgSize8 (+pad)
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (dev->CreateBuffer(&cbd, NULL, &constBuf) != S_OK) return false;

    if (!compilePS(s_psSplat, &psSplat)) { logf("[warp] psSplat 失败\n"); return false; }
    if (!compilePS(s_psFreeze, &psFreeze)) { logf("[warp] psFreeze 失败\n"); return false; }
    if (!compilePS(s_psWarp, &psWarp)) { logf("[warp] psWarp 失败\n"); return false; }
    if (!compilePS(s_psResult, &psResult)) { logf("[warp] psResult 失败\n"); return false; }

    D3D11_SAMPLER_DESC sdesc;
    ZeroMemory(&sdesc, sizeof(sdesc));
    sdesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sdesc.AddressU = sdesc.AddressV = sdesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (dev->CreateSamplerState(&sdesc, &smp) != S_OK) { logf("[warp] 采样器失败\n"); return false; }

    D3D11_BLEND_DESC bdesc;
    ZeroMemory(&bdesc, sizeof(bdesc));
    bdesc.RenderTarget[0].BlendEnable = FALSE;
    bdesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (dev->CreateBlendState(&bdesc, &bsReplace) != S_OK) { logf("[warp] 混合状态失败\n"); return false; }

    D3D11_RASTERIZER_DESC rdesc;
    ZeroMemory(&rdesc, sizeof(rdesc));
    rdesc.FillMode = D3D11_FILL_SOLID;
    rdesc.CullMode = D3D11_CULL_NONE;
    if (dev->CreateRasterizerState(&rdesc, &rsNoCull) != S_OK) { logf("[warp] 光栅化状态失败\n"); return false; }

    // 清零位移场、冻结与预览（预览不清则初始为未定义内容）
    const float zero[4] = { 0, 0, 0, 0 };
    ctx->ClearRenderTargetView(fieldARTV, zero);
    ctx->ClearRenderTargetView(fieldBRTV, zero);
    ctx->ClearRenderTargetView(freezeARTV, zero);
    ctx->ClearRenderTargetView(freezeBRTV, zero);
    const float white[4] = { 1, 1, 1, 1 };
    ctx->ClearRenderTargetView(previewRTV, white);
    dirty = true;
    logf("[warp] 引擎就绪 %ux%u\n", W, H);
    return true;
}

bool Engine::uploadSource(const uint16_t* rgba16) {
    if (!snap || !rgba16) return false;
    uint16_t* norm = (uint16_t*)malloc((size_t)W * H * 8);
    if (!norm) return false;
    for (size_t i = 0, n = (size_t)W * H * 4; i < n; i++) {
        uint32_t v = rgba16[i];
        norm[i] = (uint16_t)(v >= 0x4000 ? 0xFFFF : v * 4);
    }
    ctx->UpdateSubresource(snap, 0, NULL, norm, W * 8, 0);
    free(norm);
    dirty = true;
    return true;
}

void Engine::shutdown() {
#define R(p) if (p) { p->Release(); p = NULL; }
    R(snap); R(snapSRV);
    R(fieldA); R(fieldB); R(fieldARTV); R(fieldBRTV); R(fieldASRV); R(fieldBSRV);
    for (int i = 0; i < kStackDepth; i++) { R(undoStack[i]); R(redoStack[i]); }
    R(freezeA); R(freezeB); R(freezeARTV); R(freezeBRTV); R(freezeASRV); R(freezeBSRV);
    R(preview); R(previewRTV); R(previewSRV);
    R(result); R(resultRTV);
    R(vsQuad); R(ilEmpty); R(psSplat); R(psFreeze); R(psWarp); R(psResult); R(quadVB); R(constBuf);
    R(smp); R(bsReplace); R(rsNoCull);
#undef R
}

void Engine::dab(Brush type, float x, float y, float dx, float dy,
                 float radius, float strength, float hardness) {
    if (radius < 1) radius = 1;
    // 笔刷包围盒
    float x0 = floorf(x - radius - 2), y0 = floorf(y - radius - 2);
    float x1 = ceilf(x + radius + 2), y1 = ceilf(y + radius + 2);
    x0 = x0 < 0 ? 0 : x0; y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > (float)W ? (float)W : x1; y1 = y1 > (float)H ? (float)H : y1;
    if (x1 <= x0 || y1 <= y0) return;

    struct { float brush[4]; float params[4]; float imgSize[2]; float pad[2]; } cbuf = {
        { x, y, radius, hardness },
        { dx, dy, strength, (float)type },
        { (float)W, (float)H }, { 0, 0 }
    };
    D3D11_MAPPED_SUBRESOURCE ms;
    if (ctx->Map(constBuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms) != S_OK) return;
    memcpy(ms.pData, &cbuf, sizeof(cbuf));
    ctx->Unmap(constBuf, 0);

    UINT stride = 8, offset = 0;
    ctx->IASetVertexBuffers(0, 1, &quadVB, &stride, &offset);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->VSSetShader(vsQuad, NULL, 0);
    ctx->IASetInputLayout(ilEmpty);
    ctx->PSSetConstantBuffers(0, 1, &constBuf);

    const float blendFactor[4] = { 0, 0, 0, 0 };
    if (type == BRUSH_FREEZE || type == BRUSH_UNFREEZE) {
        ctx->PSSetShader(psFreeze, NULL, 0);
        ID3D11ShaderResourceView* srvs[1] = { freezeASRV }; // 读 A
        ctx->PSSetShaderResources(0, 1, srvs);
        ctx->PSSetSamplers(0, 1, &smp);
        ctx->OMSetRenderTargets(1, &freezeBRTV, NULL);      // 写 B
        ctx->OMSetBlendState(bsReplace, blendFactor, 0xffffffff);
        ctx->RSSetState(rsNoCull);
        D3D11_VIEWPORT vp = { 0, 0, (float)W, (float)H, 0, 1 };
        ctx->RSSetViewports(1, &vp);
        ctx->Draw(3, 0);
        // 解绑后交换
        ID3D11ShaderResourceView* nullSrv[1] = { NULL };
        ctx->PSSetShaderResources(0, 1, nullSrv);
        ID3D11RenderTargetView* nullRtv = NULL;
        ctx->OMSetRenderTargets(1, &nullRtv, NULL);
        ID3D11Texture2D* t = freezeA; freezeA = freezeB; freezeB = t;
        ID3D11RenderTargetView* r = freezeARTV; freezeARTV = freezeBRTV; freezeBRTV = r;
        ID3D11ShaderResourceView* s = freezeASRV; freezeASRV = freezeBSRV; freezeBSRV = s;
    } else {
        // fieldA -> fieldB，然后交换
        ctx->PSSetShader(psSplat, NULL, 0);
        ID3D11ShaderResourceView* srvs[2] = { fieldASRV, freezeASRV };
        ctx->PSSetShaderResources(0, 2, srvs);
        ctx->PSSetSamplers(0, 1, &smp);
        ctx->OMSetRenderTargets(1, &fieldBRTV, NULL);
        ctx->OMSetBlendState(bsReplace, blendFactor, 0xffffffff);
        ctx->RSSetState(rsNoCull);
        D3D11_VIEWPORT vp = { 0, 0, (float)W, (float)H, 0, 1 };
        ctx->RSSetViewports(1, &vp);
        ctx->Draw(3, 0);
        // 解绑后交换
        ID3D11ShaderResourceView* nullSrv[2] = { NULL, NULL };
        ctx->PSSetShaderResources(0, 2, nullSrv);
        ID3D11RenderTargetView* nullRtv = NULL;
        ctx->OMSetRenderTargets(1, &nullRtv, NULL);
        ID3D11Texture2D* t = fieldA; fieldA = fieldB; fieldB = t;
        ID3D11RenderTargetView* r = fieldARTV; fieldARTV = fieldBRTV; fieldBRTV = r;
        ID3D11ShaderResourceView* s = fieldASRV; fieldASRV = fieldBSRV; fieldBSRV = s;
    }
    dirty = true;
}

void Engine::beginStroke() {
    if (!fieldA || !undoStack[0]) return;
    if (undoCount == kStackDepth) { undoStart = (undoStart + 1) % kStackDepth; undoCount--; } // 丢最旧
    int slot = (undoStart + undoCount) % kStackDepth;
    ctx->CopyResource(undoStack[slot], fieldA);
    undoCount++;
    redoCount = 0; // 新笔使重做失效
}
void Engine::undoStroke() {
    if (!undoCount) return;
    int slot = (undoStart + undoCount - 1) % kStackDepth;
    if (redoCount == kStackDepth) { redoStart = (redoStart + 1) % kStackDepth; redoCount--; }
    int rslot = (redoStart + redoCount) % kStackDepth;
    ctx->CopyResource(redoStack[rslot], fieldA);
    ctx->CopyResource(fieldA, undoStack[slot]);
    undoCount--;
    redoCount++;
    dirty = true;
}
void Engine::redoStroke() {
    if (!redoCount) return;
    int slot = (redoStart + redoCount - 1) % kStackDepth;
    if (undoCount == kStackDepth) { undoStart = (undoStart + 1) % kStackDepth; undoCount--; }
    int uslot = (undoStart + undoCount) % kStackDepth;
    ctx->CopyResource(undoStack[uslot], fieldA);
    ctx->CopyResource(fieldA, redoStack[slot]);
    redoCount--;
    undoCount++;
    dirty = true;
}
void Engine::reset() {
    const float zero[4] = { 0, 0, 0, 0 };
    ctx->ClearRenderTargetView(fieldARTV, zero);
    ctx->ClearRenderTargetView(freezeARTV, zero);
    ctx->ClearRenderTargetView(freezeBRTV, zero);
    undoStart = undoCount = redoStart = redoCount = 0;
    dirty = true;
}

void Engine::renderPreview() {
    if (!dirty) return;
    // 同一常量缓冲被笔刷 pass 复用，预览 pass 前重写 imgSize（psWarp 只读前 8 字节）
    struct { float brush[4]; float params[4]; float imgSize[2]; float pad[2]; } cbuf = {
        { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { (float)W, (float)H }, { 0, 0 }
    };
    D3D11_MAPPED_SUBRESOURCE ms;
    HRESULT mhr = ctx->Map(constBuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    if (mhr != S_OK) { logf("[warp] constBuf Map 失败 hr=%08lx\n", (long)mhr); return; }
    memcpy(ms.pData, &cbuf, sizeof(cbuf));
    ctx->Unmap(constBuf, 0);

    ID3D11ShaderResourceView* srvs[3] = { snapSRV, fieldASRV, freezeASRV };
    ctx->PSSetShaderResources(0, 3, srvs);
    ctx->PSSetSamplers(0, 1, &smp);
    ctx->VSSetShader(vsQuad, NULL, 0);
    ctx->IASetInputLayout(ilEmpty);
    {
        UINT stride = 8, offset = 0;
        ctx->IASetVertexBuffers(0, 1, &quadVB, &stride, &offset);
    }
    ctx->PSSetShader(psWarp, NULL, 0);
    ctx->PSSetConstantBuffers(0, 1, &constBuf);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); // 首帧 ImGui 尚未渲染，拓扑默认 UNDEFINED 会导致 Draw 空跑
    ctx->OMSetRenderTargets(1, &previewRTV, NULL);
    const float blendFactor[4] = { 0, 0, 0, 0 };
    ctx->OMSetBlendState(bsReplace, blendFactor, 0xffffffff);
    ctx->RSSetState(rsNoCull);
    D3D11_VIEWPORT vp = { 0, 0, (float)W, (float)H, 0, 1 };
    ctx->RSSetViewports(1, &vp);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nullSrv[3] = { NULL, NULL, NULL };
    ctx->PSSetShaderResources(0, 3, nullSrv);
    dirty = false;
}

// 写回 pass：快照按位移场重采样（不压白底、不叠提示），每次确定都全量重画
void Engine::renderResult() {
    struct { float brush[4]; float params[4]; float imgSize[2]; float pad[2]; } cbuf = {
        { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { (float)W, (float)H }, { 0, 0 }
    };
    D3D11_MAPPED_SUBRESOURCE ms;
    if (ctx->Map(constBuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms) != S_OK) return;
    memcpy(ms.pData, &cbuf, sizeof(cbuf));
    ctx->Unmap(constBuf, 0);

    ID3D11ShaderResourceView* srvs[2] = { snapSRV, fieldASRV };
    ctx->PSSetShaderResources(0, 2, srvs);
    ctx->PSSetSamplers(0, 1, &smp);
    ctx->VSSetShader(vsQuad, NULL, 0);
    ctx->IASetInputLayout(ilEmpty);
    {
        UINT stride = 8, offset = 0;
        ctx->IASetVertexBuffers(0, 1, &quadVB, &stride, &offset);
    }
    ctx->PSSetShader(psResult, NULL, 0);
    ctx->PSSetConstantBuffers(0, 1, &constBuf);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->OMSetRenderTargets(1, &resultRTV, NULL);
    const float blendFactor[4] = { 0, 0, 0, 0 };
    ctx->OMSetBlendState(bsReplace, blendFactor, 0xffffffff);
    ctx->RSSetState(rsNoCull);
    D3D11_VIEWPORT vp = { 0, 0, (float)W, (float)H, 0, 1 };
    ctx->RSSetViewports(1, &vp);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nullSrv[2] = { NULL, NULL };
    ctx->PSSetShaderResources(0, 2, nullSrv);
    ID3D11RenderTargetView* nullRtv = NULL;
    ctx->OMSetRenderTargets(1, &nullRtv, NULL);
}

uint16_t* Engine::readResult16() {
    D3D11_TEXTURE2D_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.Width = W; sd.Height = H; sd.MipLevels = 1; sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* stg = NULL;
    if (dev->CreateTexture2D(&sd, NULL, &stg) != S_OK) return NULL;
    ctx->CopyResource(stg, result);
    uint16_t* out = (uint16_t*)malloc((size_t)W * H * 8);
    D3D11_MAPPED_SUBRESOURCE ms;
    if (!out || ctx->Map(stg, 0, D3D11_MAP_READ, 0, &ms) != S_OK) {
        if (out) free(out);
        stg->Release();
        return NULL;
    }
    for (uint32_t y = 0; y < H; y++)
        memcpy((char*)out + (size_t)y * W * 8, (const char*)ms.pData + y * ms.RowPitch, (size_t)W * 8);
    ctx->Unmap(stg, 0);
    stg->Release();
    return out;
}

uint8_t* Engine::readPreview() {    D3D11_TEXTURE2D_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.Width = W; sd.Height = H; sd.MipLevels = 1; sd.ArraySize = 1;
    sd.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* stg = NULL;
    if (dev->CreateTexture2D(&sd, NULL, &stg) != S_OK) return NULL;
    ctx->CopyResource(stg, preview);
    uint8_t* out = (uint8_t*)malloc((size_t)W * H * 4);
    D3D11_MAPPED_SUBRESOURCE ms;
    if (!out || ctx->Map(stg, 0, D3D11_MAP_READ, 0, &ms) != S_OK) {
        if (out) free(out);
        stg->Release();
        return NULL;
    }
    for (uint32_t y = 0; y < H; y++) {
        const uint16_t* src = (const uint16_t*)((const char*)ms.pData + y * ms.RowPitch);
        uint8_t* dst = out + (size_t)y * W * 4;
        for (uint32_t x = 0; x < W; x++) {
            dst[x * 4 + 0] = (uint8_t)(src[x * 4 + 0] >> 8);
            dst[x * 4 + 1] = (uint8_t)(src[x * 4 + 1] >> 8);
            dst[x * 4 + 2] = (uint8_t)(src[x * 4 + 2] >> 8);
            dst[x * 4 + 3] = 255;
        }
    }
    ctx->Unmap(stg, 0);
    stg->Release();
    return out;
}

} // namespace warp
