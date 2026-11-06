"""SAI2 图层瓦片结构探测（P1-m2 像素格式实证）

链: app -> +0x130 选中图层 -> +0xf8[i] MIP 平面 -> 平面 +0x68/+0x70 行数组 -> 瓦片对象(0x200B)

只读，不影响 SAI 进程。
用法: python tools/sai_tiles.py [--pid PID] [--dump-tile N] [--full]
"""
import argparse
import ctypes
import sys

sys.path.insert(0, __file__.rsplit('\\', 1)[0] if '\\' in __file__ else '.')
from sai_probe import Probe, find_sai_pid

def heap_ptr(p):
    return p is not None and p != 0 and p % 8 == 0 and p < 0x7FFF00000000

MIP_COUNT = 6  # +0xf8[i] 六个 MIP 平面


def hx(b, maxn=32):
    return ' '.join(f'{x:02x}' for x in b[:maxn])


def dump_plane(probe, p, name):
    print(f'\n== {name} @ {p:#x} ==')
    head = probe.read(p, 0x80)
    if not head:
        print('  读取失败')
        return
    print(f'  头部0x80: {hx(head, 64)}')
    for off in (0x68, 0x70):
        arr = probe.rq(p + off)
        print(f'  +{off:#x} = {arr:#x}' if arr else f'  +{off:#x} = 0', end='')
        if heap_ptr(arr):
            # 读数组前 4 个条目
            entries = []
            for i in range(4):
                e = probe.rq(arr + i * 8)
                entries.append(f'{e:#x}' if e else '0')
            print(f'  条目[0..3]: {entries}')
        else:
            print()
    # 常见尺寸字段猜测：在头部找小的正整数（宽/高/瓦片数）
    import struct
    dwords = struct.unpack_from('<32I', head, 0)
    smalls = [(i * 4, v) for i, v in enumerate(dwords) if 0 < v <= 0x10000]
    print(f'  头部小整数字段(<=0x10000): {[(hex(o), v) for o, v in smalls]}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pid', type=int, default=None)
    ap.add_argument('--dump-tile', type=int, default=None, help='dump 指定 mip 层第一个非空瓦片的 0x200 字节')
    args = ap.parse_args()

    pid = args.pid or find_sai_pid()
    probe = Probe(pid)
    print(f'base={probe.base:#x}')

    app = probe.rq(probe.base + 0x321118)
    print(f'app={app:#x}')
    layer = probe.rq(app + 0x130)
    print(f'选中图层={layer:#x} type={probe.rd(layer + 0x244)} id={probe.rd(layer + 0x240):#x}')

    # 图层尺寸相关字段（FUN_1400471e0 显示 +0x254 处拷 0x14 字节 rect）
    import struct
    lb = probe.read(layer, 0x2c0)
    dims = struct.unpack_from('<28I', lb, 0x250)
    print(f'图层 +0x250..0x2bc dwords: {[hex(d) for d in dims]}')

    mips = []
    for i in range(MIP_COUNT):
        p = probe.rq(layer + 0xf8 + i * 8)
        mips.append(p)
        print(f'MIP[{i}] (+{0xf8 + i * 8:#x}) = {p:#x}' if p else f'MIP[{i}] = 0')

    for i, p in enumerate(mips):
        if heap_ptr(p):
            dump_plane(probe, p, f'MIP[{i}]')

    # dump 瓦片
    mi = args.dump_tile if args.dump_tile is not None else next(
        (i for i, p in enumerate(mips) if heap_ptr(p)), None)
    if mi is not None and heap_ptr(mips[mi]):
        p = mips[mi]
        tile = None
        for off in (0x68, 0x70):
            arr = probe.rq(p + off)
            if heap_ptr(arr):
                row = probe.rq(arr)
                if heap_ptr(row):
                    t = probe.rq(row)
                    if heap_ptr(t):
                        tile = t
                        print(f'\n瓦片路径: MIP[{mi}]+{off:#x} -> {arr:#x} -> {row:#x} -> 瓦片 {t:#x}')
                        break
        if tile:
            data = probe.read(tile, 0x200)
            print(f'瓦片前0x40: {hx(data, 64)}')
            # 非零统计
            nz = sum(1 for x in data if x != 0)
            print(f'瓦片非零字节: {nz}/512')
            if nz:
                # 按 16-bit little endian 打印前 64 个像素值
                px = struct.unpack_from('<256H', data, 0)
                print(f'前64像素(u16): {[hex(v) for v in px[:64]]}')
        else:
            print('未找到瓦片（行数组为空？可能该 mip 无像素）')

    probe.close()


if __name__ == '__main__':
    main()
