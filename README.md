# sai-hooks

PaintTool SAI Ver.2 插件系统 —— 以 DLL 注入方式为 SAI2 补充液化等画布编辑能力。一期交付**液化工具**（仿 CSP 手感），已完成验收。

- 目标软件：PaintTool SAI Ver.2（64bit）Preview 2024.02.22（仅此一个版本）
- 技术形态：C++ DLL 注入 + D3D11 自绘窗口（ImGui 1.82）
- 详细需求见 [docs/product/PRD.md](docs/product/PRD.md)，**架构说明见 [docs/architecture.md](docs/architecture.md)，踩坑记录见 [docs/pitfalls.md](docs/pitfalls.md)**，逆向偏移档案见 [docs/sai2-offsets.md](docs/sai2-offsets.md)

## 目录架构

```
sai-hooks/
├── src/
│   ├── sai_hooks/          # 【生产代码】主插件 DLL
│   │   ├── dllmain.cpp     #   DllMain / IPC / 快照 / 写回编排 / 启动按钮
│   │   ├── liquefy.cpp     #   液化会话：窗口、ImGui UI、笔刷输入、跳车保险
│   │   ├── warp.cpp/.h     #   warp 引擎：D3D11 位移场 ping-pong、冻结、预览、写回重采样
│   │   ├── imgui_dx11.cpp  #   swapchain/RTV 辅助
│   │   └── sai_core.h      #   SAI 内部结构偏移（已验证的 offsets）
│   ├── injector/           # 【生产代码】DLL 注入器（独立 exe，备用方案用）
│   └── launcher/           # 【生产代码】静默启动器 sai_launcher.exe（推荐方案：
│                           #   双击它开 SAI，自动注入插件，详见 dist/使用说明.txt）
├── tools/                  # 构建与自动化脚本（Python），按职责分子目录
│   ├── build/build.py      # 【主构建】MinGW-w64 编译 sai_hooks.dll
│   ├── deploy/             # 启动注入与 IPC：sai_boot（启动+注入+过对话框）、
│   │                       #   sai_send（IPC 控制）、sai_try（不停机热迭代）、sai_new_canvas（自动验收）
│   ├── reverse/            # 逆向观察（只读，不影响 SAI）：sai_probe（基础库）、
│   │                       #   sai_snapshot / sai_tiles / sai_diff / sai_undo / sai_layers_dump
│   └── shot/grab_pw.py     # 液化窗截图（PrintWindow，不受遮挡）
├── third_party/
│   └── imgui-2.0.0/        # ImGui 源码（唯一参与构建的第三方库）
├── docs/
│   ├── product/              # 需求文档：PRD.md、需求.md
│   ├── architecture.md       # 【先读这个】架构说明（通俗版）：全貌、数据流、模块划分、关键决定
│   ├── pitfalls.md           # 【再读这个】踩坑记录：21 条已踩过的坑 + 验收方法
│   ├── sai2-offsets.md       # SAI2 逆向偏移档案（结构、调用链、结论）
│   └── dev-plan.md           # 开发计划与里程碑（历史过程文档）
├── bin/                    # 构建产物（git 忽略）：sai_hooks.dll / injector.exe / sai_hooks.ini
├── obj/                    # 中间产物（git 忽略）
├── artifacts/              # 过程产物（git 忽略，仅本地保留）
│   ├── rea/                #   REA 逆向分析缓存
│   ├── shots/              #   调试截图
│   ├── test-data/          #   快照/日志/dump
│   └── bin-history/        #   历史 DLL 版本
└── [Saier]SAI Ver.2/       # 目标 SAI 程序目录（git 忽略）
```

**分类原则**：`src/` 只放生产代码；探索期一次性代码完成使命即删，有价值的结论沉淀到 `docs/sai2-offsets.md` 和 `docs/pitfalls.md`；`tools/` 按 `build/deploy/reverse/shot` 职责分目录放；过程产物一律进 `artifacts/`（不进 git）。

## 构建

主构建链（MinGW-w64，无需 Windows SDK）：

```bash
# 链接 bin/sai_hooks.dll 失败属正常（文件被运行中的 SAI 占用时用 try 链）
rm -f obj/*.o && python tools/build/build.py

# 生成带版本号的 DLL（freshN 递增）
D:/tools/mingw64/bin/x86_64-w64-mingw32-g++.exe -shared -static -mwindows \
  -o bin/try/sai_hooks_freshN.dll obj/*.o \
  -ld3d11 -ldxgi -ld3dcompiler -limm32 -ldwmapi -lgdi32 -luser32 -lshell32

# 注入器（杀软可能误删 bin/injector.exe，缺失时重编）
D:/tools/mingw64/bin/x86_64-w64-mingw32-g++.exe -O2 -static -std=c++11 -municode \
  -o bin/injector.exe src/injector/injector.cpp

# 静默启动器（用户分发用，图形子系统无控制台窗口）
D:/tools/mingw64/bin/x86_64-w64-mingw32-g++.exe -O2 -static -mwindows -municode \
  -o bin/sai_launcher.exe src/launcher/sai_launcher.cpp
```

配置文件为 `bin/sai_hooks.ini`（热键等）。编译器路径硬编码在 `tools/build/build.py` 头部，换机器时改 `CXX` 常量。

## 部署与调试

```bash
# 1. 杀掉旧 SAI（Git Bash 里 taskkill 需 //PID //F）
tasklist | grep -i "sai2.exe" | awk '{print $2}' | while read p; do taskkill //PID $p //F; done
sleep 4
# 2. 替换 DLL 并启动+注入（自动处理「强制退出的工程」「恢复文件」对话框）
cp bin/try/sai_hooks_freshN.dll bin/sai_hooks.dll && python tools/deploy/sai_boot.py
# 3. 看日志确认「热键已注册」（%TEMP%\sai_hooks_log_<pid>.txt）
# 4. 无头自检：python tools/deploy/sai_send.py <pid> 98
```

验收方式：SAI 画布画几笔 → 点右下角「液化」按钮或 `Ctrl+Shift+X` → 推拉变形 → 确定 → 画布即时更新 + Ctrl+Z/Y 撤销重做。

## 运行时架构要点

- 快照/写回必须经 `SendMessage` 到子类化的 SAI 主窗，在 SAI UI 线程执行（外部线程直接调必崩）。
- 崩溃保险两道：指针 `plausible()` 校验 + VEH 跳车（写回包在 setjmp/longjmp 外壳里，保 SAI 进程不死）。
- 视图刷新三件套：`redraw1/redraw2/redraw3(app, 0x10)` + `RedrawWindow(RDW_ALLCHILDREN|UPDATENOW)`，缺一不可。
- 撤销集成：写回走 SAI 撤销记录 type=0x47 分组，多图层一次 Ctrl+Z 全撤。

## 已验证关键 offsets（详见 docs/sai2-offsets.md）

| 偏移 | 含义 |
|---|---|
| `app+0x130` | 选中图层对象（单选） |
| `layer+0x10` → `+0x00` | 选中集链头（多选遍历，自底向顶） |
| `layer+0x249` | 多选标志（!=0 在选中集中） |
| `layer+0x244` | 图层类型（3=文件夹；8/9/10=特殊层） |
| `undo管理器 app+0x168` | `+0xa0` = 当前开放撤销分组容器 |
