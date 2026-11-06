"""构建 sai_hooks 项目（MinGW-w64，免 Windows SDK 依赖）。

注：VC 路线暂时不可用——VS2026 未安装 Windows SDK（vcvars64 报语法错误、
cl 找不到 windows.h）。装好后可切回 MSVC 版（git 历史里有）。
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / 'bin'
BIN.mkdir(exist_ok=True)

CXX = r'D:\tools\mingw64\bin\x86_64-w64-mingw32-g++.exe'
IMGUI = ROOT / 'third_party' / 'imgui-2.0.0' / 'imgui-cpp'
BACKENDS = IMGUI / 'backends'
OBJ = ROOT / 'obj'
OBJ.mkdir(exist_ok=True)

HOOKS_SOURCES = [
    ROOT / 'src' / 'sai_hooks' / 'dllmain.cpp',
    ROOT / 'src' / 'sai_hooks' / 'liquefy.cpp',
    ROOT / 'src' / 'sai_hooks' / 'imgui_dx11.cpp',
    ROOT / 'src' / 'sai_hooks' / 'warp.cpp',
    IMGUI / 'imgui.cpp',
    IMGUI / 'imgui_draw.cpp',
    IMGUI / 'imgui_widgets.cpp',
    IMGUI / 'imgui_tables.cpp',
    BACKENDS / 'imgui_impl_dx11.cpp',
    BACKENDS / 'imgui_impl_win32.cpp',
]

# 项目头文件依赖：任一 src 头文件比 .o 新就重编项目源（imgui 第三方源不依赖我们的头）
PROJ_SOURCES = {'dllmain', 'liquefy', 'imgui_dx11', 'warp'}
_headers = list((ROOT / 'src').rglob('*.h'))
newest_hdr = max(h.stat().st_mtime for h in _headers)

def run(cmd) -> bool:
    print('+', ' '.join(cmd[:3]), '...')
    r = subprocess.run(cmd, capture_output=True, text=True, encoding='gbk', errors='replace')
    if r.returncode != 0:
        sys.stderr.write((r.stderr or '')[-4000:])
        sys.stdout.write((r.stdout or '')[-2000:])
    return r.returncode == 0

def build_hooks() -> bool:
    objs = []
    for src in HOOKS_SOURCES:
        obj = OBJ / (src.stem + '_' + src.parent.name + '.o')
        # imgui_draw 的 -O0 规避仅针对旧 1.65（stbtt 崩溃），1.82 已修复，统一 -O2
        opt = '-O2'
        stale = not obj.exists() or obj.stat().st_mtime < src.stat().st_mtime
        if src.stem in PROJ_SOURCES and obj.exists() and obj.stat().st_mtime < newest_hdr:
            stale = True
        if stale:
            if not run([CXX, opt, '-std=c++17', '-static', '-c',
                        '-DNDEBUG', '-DUNICODE', '-D_UNICODE',
                        f'-I{IMGUI}', f'-I{BACKENDS}',
                        '-o', str(obj), str(src)]):
                return False
        objs.append(str(obj))
    return run([CXX, '-shared', '-static', '-mwindows', '-o', str(BIN / 'sai_hooks.dll'), *objs,
                '-ld3d11', '-ldxgi', '-ld3dcompiler', '-limm32', '-ldwmapi',
                '-lgdi32', '-luser32', '-lshell32'])

ok = build_hooks()
print('构建成功' if ok else '构建失败')
sys.exit(0 if ok else 1)
