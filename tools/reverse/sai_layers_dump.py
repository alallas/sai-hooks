"""多选状态 diff 实验：沿图层 +0x00 兄弟链枚举并 dump 图层对象字节
用法: python tools/sai_layers_dump.py <out.txt>
配合：状态A（单选一个图层）dump A；状态B（Ctrl加选第二个）dump B；diff 两文件。
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sai_probe import Probe, RVA_APP_SINGLETON, find_sai_pid
import ctypes

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else 'layers_dump.txt'
    pid = find_sai_pid()
    r = Probe(pid)
    app = r.rq(r.base + RVA_APP_SINGLETON)
    sel = r.rq(app + 0x130)
    lines = [f'pid={pid} app={app:#x} selLayer={sel:#x}', '']
    seen = set()
    p = sel
    while p and p not in seen and len(seen) < 64:
        seen.add(p)
        data = r.read(p, 0x640)
        if not data:
            lines.append(f'{p:#x} <读取失败>')
            break
        lid = int.from_bytes(data[0x240:0x244], 'little')
        typ = int.from_bytes(data[0x244:0x248], 'little')
        lines.append(f'== {p:#x} id={lid:#x} type={typ}')
        for off in range(0, 0x640, 16):
            chunk = data[off:off+16]
            hexs = ' '.join(f'{b:02x}' for b in chunk)
            lines.append(f'  +{off:03x}: {hexs}')
        lines.append('')
        p = int.from_bytes(data[0x00:0x08], 'little')  # +0x00 兄弟链
    open(out, 'w', encoding='utf-8').write('\n'.join(lines))
    print(f'已写出 {out}（{len(seen)} 个图层）')

if __name__ == '__main__':
    main()
