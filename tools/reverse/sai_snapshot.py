"""SAI2 选中图层快照 -> PNG（P1-m2 解码器，格式已实证）

已实证结论（2026-11-03）：
- 画布尺寸: app+0x250 = 宽, app+0x254 = 高（当前 512x512）
- 像素格式: 8B/像素 = 4 x u16，**BGRA 顺序**（受控写回实验实证：写 c0=满值 渲染为蓝、写 c2=满值 渲染为红），**预乘 alpha**，满量程 0x4000
- 瓦片: 32x32 像素 x 8B = 0x2000B 页，8KB 对齐
- 图层 +0xf8[mip] -> 平面对象(0x200B)：
  - 平面+0x38 -> 行数组：行 y -> 0x40B 记录，前32字节=每列瓦片标志(0空/1有)
  - 平面+0x40 -> 行数组：行 y -> 0x80B 记录，16 个页指针
- 输出: 预乘合成到白底，8位 RGB PNG

用法: python tools/sai_snapshot.py [--out out.png] [--mip 0]
"""
import argparse
import struct
import sys
import zlib

sys.path.insert(0, __file__.rsplit('\\', 1)[0] if '\\' in __file__ else '.')
from sai_probe import Probe, find_sai_pid

TILE = 32
FULL = 0x4000          # u16 满量程（alpha 与颜色同量程，预乘）
PAGE_QWORDS = 16


def vp(p):
    return p is not None and p != 0 and p % 8 == 0 and p < 0x7FFF00000000


def is_page(p):
    return p and p % 0x2000 == 0 and 0x1000000 < p < 0x7FFF00000000


def write_png(path, w, h, rgb):
    raw = b''.join(b'\x00' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(typ, data):
        c = typ + data
        return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c))
    ihdr = struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)  # 8-bit RGB
    png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr)
           + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))
    with open(path, 'wb') as f:
        f.write(png)


def snapshot(probe, mip=0):
    app = probe.rq(probe.base + 0x321118)
    W = probe.rd(app + 0x250)
    H = probe.rd(app + 0x254)
    if not W or not H or W > 16384 or H > 16384:
        raise RuntimeError(f'画布尺寸异常: {W}x{H}')
    layer = probe.rq(app + 0x130)
    pl = probe.rq(layer + 0xf8 + mip * 8)
    if not vp(pl):
        raise RuntimeError(f'MIP{mip} 平面为空')

    flags_arr = probe.rq(pl + 0x38)
    pages_arr = probe.rq(pl + 0x40)
    if not vp(flags_arr) or not vp(pages_arr):
        raise RuntimeError('行数组缺失')

    tiles_y, tiles_x = (H + TILE - 1) // TILE, (W + TILE - 1) // TILE
    print(f'画布 {W}x{H} -> 瓦片网格 {tiles_x}x{tiles_y}')

    # 预乘合成到白底
    img = bytearray([255]) * (W * H * 3)
    n = 0
    for ty in range(tiles_y):
        fr = probe.rq(flags_arr + ty * 8)
        pr = probe.rq(pages_arr + ty * 8)
        if not vp(fr) or not vp(pr):
            continue
        flags = probe.read(fr, TILE)
        for tx in range(min(tiles_x, PAGE_QWORDS)):
            if not flags or tx >= len(flags) or not flags[tx]:
                continue
            page = probe.rq(pr + tx * 8)
            if not is_page(page):
                continue
            d = probe.read(page, TILE * TILE * 8)
            if not d or len(d) < TILE * TILE * 8:
                continue
            u16 = struct.unpack(f'<{TILE * TILE * 4}H', d)
            n += 1
            for py in range(TILE):
                gy = ty * TILE + py
                if gy >= H:
                    break
                row_base = gy * W * 3
                for px in range(TILE):
                    gx = tx * TILE + px
                    if gx >= W:
                        break
                    b16, g16, r16, a = u16[(py * TILE + px) * 4:(py * TILE + px) * 4 + 4]
                    # 预乘 -> 白底: out = c + (FULL - a)；内存序 BGRA -> PNG RGB
                    o = row_base + gx * 3
                    img[o] = min(255, (r16 + (FULL - a)) * 255 // FULL)
                    img[o + 1] = min(255, (g16 + (FULL - a)) * 255 // FULL)
                    img[o + 2] = min(255, (b16 + (FULL - a)) * 255 // FULL)
    return W, H, bytes(img), n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default='snapshot.png')
    ap.add_argument('--mip', type=int, default=0)
    ap.add_argument('--pid', type=int, default=None)
    args = ap.parse_args()

    probe = Probe(args.pid or find_sai_pid())
    W, H, rgb, n = snapshot(probe, args.mip)
    write_png(args.out, W, H, rgb)
    print(f'已写出 {args.out} ({W}x{H}, {n} 瓦片)')
    probe.close()


if __name__ == '__main__':
    main()
