"""SAI2 像素内存 diff 工具（只读）

原理：对可疑内存区做稀疏指纹（每 64KB 采样 256B），画笔画完后重采样对比，
定位变化的页，再全量读变化窗口找出具体字节。
用法:
  python tools/sai_diff.py record   # 画之前
  python tools/sai_diff.py diff     # 画之后
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import hashlib
import json
import sys

sys.path.insert(0, __file__.rsplit('\\', 1)[0])
from sai_probe import Probe, find_sai_pid  # noqa: E402

FP_PATH = __file__.rsplit('\\', 1)[0] + '\\sai_fingerprint.json'

kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)

MEM_COMMIT = 0x1000
PAGE_NOACCESS = 0x01
PAGE_GUARD = 0x100


class MEMORY_BASIC_INFORMATION64(ctypes.Structure):
    _fields_ = [('BaseAddress', ctypes.c_ulonglong), ('AllocationBase', ctypes.c_ulonglong),
                ('AllocationProtect', wt.DWORD), ('__align', wt.DWORD),
                ('RegionSize', ctypes.c_ulonglong), ('State', wt.DWORD),
                ('Protect', wt.DWORD), ('Type', wt.DWORD), ('__align2', wt.DWORD)]


def committed_regions(probe, start=0x1000000, end=0x7FFF0000):
    """遍历进程已提交内存区"""
    addr = start
    while addr < end:
        mbi = MEMORY_BASIC_INFORMATION64()
        r = kernel32.VirtualQueryEx(probe.proc, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi))
        if not r:
            break
        if mbi.State == MEM_COMMIT and not (mbi.Protect & PAGE_GUARD) and mbi.Protect != PAGE_NOACCESS:
            yield mbi.BaseAddress, mbi.RegionSize, mbi.Protect
        addr = mbi.BaseAddress + max(mbi.RegionSize, 0x1000)


def sparse_fingerprint(probe, sample_len=256, step=0x10000):
    """稀疏指纹所有已提交区：每 64KB 采样 256B"""
    fp = {}
    total = 0
    for base, size, prot in committed_regions(probe):
        # 跳过纯只读镜像区（模块映射），只看可写区（堆/像素池）
        if not (prot & 0xCC):  # PAGE_READWRITE/WRITECOPY/EXECUTE_READWRITE/EXECUTE_WRITECOPY
            continue
        n = 0
        pos = base
        while pos < base + size:
            d = probe.read(pos, sample_len)
            if d and len(d) == sample_len:
                fp[hex(pos)] = hashlib.md5(d).hexdigest()
                n += 1
            pos += step
        total += n
    return fp, total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd', choices=['record', 'diff'])
    args = ap.parse_args()

    pid = find_sai_pid()
    if not pid:
        print('未找到 sai2.exe')
        sys.exit(1)
    probe = Probe(pid)
    print(f'PID={pid} base={probe.base:#x}')

    fp, total = sparse_fingerprint(probe)
    print(f'指纹采样点: {total}')

    if args.cmd == 'record':
        with open(FP_PATH, 'w') as f:
            json.dump(fp, f)
        print(f'基线已保存: {FP_PATH}')
    else:
        old = json.load(open(FP_PATH))
        changed = [int(k, 16) for k, v in fp.items() if old.get(k) != v]
        print(f'变化采样点: {len(changed)}')
        # 聚合成 64KB 窗口
        windows = {}
        for addr in changed:
            w = addr & ~0xFFFF
            windows[w] = windows.get(w, 0) + 1
        for w in sorted(windows)[:40]:
            cnt = windows[w]
            print(f'  窗口 {w:#010x}  变化采样 {cnt}')
        if changed:
            print(f'\n最小变化地址: {min(changed):#x}  最大: {max(changed):#x}')
    probe.close()


if __name__ == '__main__':
    main()
