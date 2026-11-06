"""SAI2 撤销栈观察工具 v2（P1-m4）

4 个候选撤销栈容器（均为 {head,tail,count} 侵入式链表，节点 +0x58 = type, +0x54 = layer id）：
  A=*( *(app+0x168)+0x10 )  B=*( *(app+0x168)+0x18 )  C=*( *(app+0x168)+0x20 )  D=*( *(app+0x168)+0x30 )
用法: python tools/sai_undo.py [baseline|after]   # 输出便于 diff
"""
import struct
import sys

sys.path.insert(0, __file__.rsplit('\\', 1)[0] if '\\' in __file__ else '.')
from sai_probe import Probe, find_sai_pid


def vp(p):
    return p is not None and p != 0 and p % 8 == 0 and p < 0x7FFF00000000


def dump_list(probe, tag, lst, maxn=6):
    d = probe.read(lst, 0x68)
    if not d:
        print(f'{tag}: 读取失败')
        return
    h, t = struct.unpack_from('<QQ', d, 0x50)
    c = struct.unpack_from('<I', d, 0x60)[0]
    print(f'{tag}: list@{lst:#x} head={h:#x} tail={t:#x} count={c}')
    node, i = t, 0
    while vp(node) and i < maxn:
        ty = probe.rd(node + 0x58)
        lid = probe.rd(node + 0x54)
        older = probe.rq(node + 0x08)
        body = probe.read(node, 0x180) or b''
        pages = []
        for off in range(0x10, 0x180, 8):
            p = struct.unpack_from('<Q', body, off)[0]
            if p and p % 0x2000 == 0 and 0x1000000 < p < 0x7FFF00000000:
                pages.append(f'+{off:#x}:{p:#x}')
        rect = struct.unpack_from('<4i', body, 0x10)
        extra = ''
        if len(body) >= 0xb8:
            a0, a4 = struct.unpack_from('<II', body, 0xa0)
            extra = f' +0xa0={a0:#x} +0xa4={a4:#x}'
        print(f'  [{i}] rec={node:#x} type={ty:#x} layer={lid:#x} rect(+0x10)={rect}{extra}')
        if pages:
            print(f'       页指针: {pages}')
        node = older
        i += 1


def main():
    probe = Probe(find_sai_pid())
    app = probe.rq(probe.base + 0x321118)
    o = probe.rq(app + 0x168)
    print(f'app={app:#x} O=*(app+0x168)={o:#x}')
    for off, tag in ((0x10, 'A'), (0x18, 'B'), (0x20, 'C'), (0x30, 'D')):
        lst = probe.rq(o + off)
        if vp(lst):
            dump_list(probe, tag, lst)
        else:
            print(f'{tag}: 空')
    probe.close()


if __name__ == '__main__':
    main()
