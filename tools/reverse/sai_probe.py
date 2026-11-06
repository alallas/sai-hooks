"""SAI2 只读内存观察工具（P0.3 运行时验证）

纯 ctypes 实现，只使用 PROCESS_VM_READ，对 SAI 进程零写入。
验证链：sai2.exe 基址(ASLR) -> DAT_140321118 应用单例 -> +0x180 图层树 / +0x1d8 图层数组
用法: python tools/sai_probe.py [--pid PID] [--watch]
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import sys
import time

kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
psapi = ctypes.WinDLL('psapi', use_last_error=True)

PROCESS_VM_READ = 0x0010
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000

# ---- 逆向结论中的 RVA（相对 0x140000000）----
RVA_APP_SINGLETON = 0x321118          # DAT_140321118 应用单例
OFF_DOC_LAYER_ROOT  = 0x180           # 文档 +0x180 图层树根
OFF_DOC_LAYER_COUNT = 0x198           # 文档 +0x198 图层数
OFF_DOC_LAYER_ARRAY = 0x1D8           # 文档 +0x1d8 图层指针数组（NULL 结尾）
# 图层对象字段
OFF_LAYER_CHILD     = 0x00            # 第一个子图层
OFF_LAYER_TYPE      = 0x244           # 类型（3=文件夹, 8/9/10=特殊）
OFF_LAYER_DEPTH     = 0x248           # 嵌套深度
OFF_LAYER_ID        = 0x240           # 图层 ID

STR_TYPES = {0: '?', 1: 'Col', 2: 'Ink', 3: 'Folder', 8: 'Vp1', 9: 'Grid2', 10: 'Symmetric'}


def find_sai_pid():
    snap = kernel32.CreateToolhelp32Snapshot(0x2, 0)  # TH32CS_SNAPPROCESS
    if snap == -1:
        return None
    pe = ctypes.create_string_buffer(304)
    entry = ctypes.cast(pe, ctypes.POINTER(ctypes.c_byte))
    # PROCESSENTRY32W
    class PROCESSENTRY32W(ctypes.Structure):
        _fields_ = [('dwSize', wt.DWORD), ('cntUsage', wt.DWORD), ('th32ProcessID', wt.DWORD),
                    ('th32DefaultHeapID', ctypes.POINTER(ctypes.c_ulong)), ('th32ModuleID', wt.DWORD),
                    ('cntThreads', wt.DWORD), ('th32ParentProcessID', wt.DWORD),
                    ('pcPriClassBase', ctypes.c_long), ('dwFlags', wt.DWORD),
                    ('szExeFile', ctypes.c_wchar * 260)]
    e = PROCESSENTRY32W()
    e.dwSize = ctypes.sizeof(e)
    if not kernel32.Process32FirstW(snap, ctypes.byref(e)):
        kernel32.CloseHandle(snap)
        return None
    pid = None
    while True:
        if e.szExeFile.lower() == 'sai2.exe':
            pid = e.th32ProcessID
            break
        if not kernel32.Process32NextW(snap, ctypes.byref(e)):
            break
    kernel32.CloseHandle(snap)
    return pid


def get_module_base(proc, name='sai2.exe'):
    arr = (ctypes.c_void_p * 1024)()
    needed = wt.DWORD()
    if not psapi.EnumProcessModules(proc, arr, ctypes.sizeof(arr), ctypes.byref(needed)):
        return None
    for i in range(needed.value // 8):
        mod = ctypes.c_void_p(arr[i])
        buf = ctypes.create_unicode_buffer(260)
        psapi.GetModuleBaseNameW(proc, mod, buf, 260)
        if buf.value.lower() == name:
            info = ctypes.create_string_buffer(24)
            if psapi.GetModuleInformation(proc, mod, info, 24):
                base = struct_unpack_module(info)
                return base
    return None


def struct_unpack_module(buf):
    import struct
    # MODULEINFO: lpBaseOfDll(void*), SizeOfImage(ulong), EntryPoint(void*)
    return struct.unpack_from('<Q', buf, 0)[0]


class Probe:
    def __init__(self, pid):
        self.pid = pid
        self.proc = kernel32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
        if not self.proc:
            raise RuntimeError(f'OpenProcess 失败: {ctypes.get_last_error()}')
        self.base = get_module_base(self.proc)
        if not self.base:
            raise RuntimeError('找不到 sai2.exe 模块')

    def read(self, addr, size):
        buf = ctypes.create_string_buffer(size)
        n = ctypes.c_size_t()
        if not kernel32.ReadProcessMemory(self.proc, ctypes.c_void_p(addr), buf, size, ctypes.byref(n)):
            return None
        return buf.raw[:n.value]

    def rq(self, addr):
        d = self.read(addr, 8)
        return int.from_bytes(d, 'little') if d and len(d) == 8 else None

    def rd(self, addr):
        d = self.read(addr, 4)
        return int.from_bytes(d, 'little') if d and len(d) == 4 else None

    def in_module(self, p):
        return p and self.base <= p < self.base + 0x600000

    @staticmethod
    def heap_ptr(p):
        """堆对象指针：非空、8 对齐、落在合理用户地址区"""
        return p is not None and p != 0 and p % 8 == 0 and p < 0x7FFF00000000

    def snapshot(self):
        app = self.rq(self.base + RVA_APP_SINGLETON)
        out = {'base': hex(self.base), 'app': hex(app) if app else None}
        if not self.heap_ptr(app):
            out['error'] = 'app singleton 无效（SAI 可能未完成初始化）'
            return out
        # 应用单例 -> 文档：*(*(?)+...) 直接按 FUN_14015f5d0 模式: *(app+0x28) 是文档
        doc = self.rq(app + 0x28)
        out['doc'] = hex(doc) if doc else None
        if self.heap_ptr(doc):
            out['layer_count'] = self.rd(doc + OFF_DOC_LAYER_COUNT)
            root = self.rq(doc + OFF_DOC_LAYER_ROOT)
            arr = self.rq(doc + OFF_DOC_LAYER_ARRAY)
            out['layer_root'] = hex(root) if root else None
            out['layer_array'] = hex(arr) if arr else None
            layers = []
            if self.heap_ptr(arr):
                for i in range(64):  # 防御上限
                    p = self.rq(arr + i * 8)
                    if not p:
                        break
                    if not self.heap_ptr(p):
                        layers.append(f'[{i}] <无效 {p:#x}>')
                        break
                    t = self.rd(p + OFF_LAYER_TYPE)
                    depth = self.rd(p + OFF_LAYER_DEPTH) & 0xFF if self.rd(p + OFF_LAYER_DEPTH) is not None else None
                    lid = self.rd(p + OFF_LAYER_ID)
                    layers.append(f'[{i}] {p:#x} type={t}({STR_TYPES.get(t, "?")}) depth={depth} id={lid:#x}')
            out['layers'] = layers
        return out

    def close(self):
        if self.proc:
            kernel32.CloseHandle(self.proc)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pid', type=int, default=None)
    ap.add_argument('--watch', action='store_true', help='每 3 秒刷新（Ctrl+C 退出）')
    args = ap.parse_args()

    pid = args.pid or find_sai_pid()
    if not pid:
        print('未找到 sai2.exe 进程')
        sys.exit(1)
    print(f'sai2.exe PID={pid}')

    probe = Probe(pid)
    print(f'模块基址: {probe.base:#x}')

    try:
        while True:
            snap = probe.snapshot()
            print('\n----', time.strftime('%H:%M:%S'), '----')
            for k, v in snap.items():
                if k == 'layers':
                    print(f'  图层数组内容 ({len(v)} 项):')
                    for line in v:
                        print('   ', line)
                else:
                    print(f'  {k}: {v}')
            if not args.watch:
                break
            time.sleep(3)
    except KeyboardInterrupt:
        pass
    finally:
        probe.close()


if __name__ == '__main__':
    main()
