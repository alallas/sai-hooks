# -*- coding: utf-8 -*-
"""PrintWindow 直接抓液化子窗内容（不受遮挡影响，需窗口仍存活）。"""
import ctypes, ctypes.wintypes as wt, sys, time
ct = ctypes
u32 = ctypes.windll.user32
g32 = ctypes.windll.gdi32

wins = []
cb = ctypes.WINFUNCTYPE(ct.c_bool, wt.HWND, wt.LPARAM)
def proc(h, l):
    t = ctypes.create_unicode_buffer(256)
    u32.GetWindowTextW(h, t, 256)
    if 'SAI' in t.value:
        wins.append(h)
    return True
u32.EnumWindows(cb(proc), 0)
if not wins:
    print('no SAI main'); sys.exit(0)
main = wins[0]

found = []
cb2 = ctypes.WINFUNCTYPE(ct.c_bool, wt.HWND, wt.LPARAM)
def proc2(h, l):
    t = ctypes.create_unicode_buffer(256)
    u32.GetWindowTextW(h, t, 256)
    if '液化' in t.value:
        found.append(h)
    return True
u32.EnumChildWindows(main, cb2(proc2), 0)
if not found:
    print('no liquefy child'); sys.exit(0)
h = found[0]
r = wt.RECT()
u32.GetWindowRect(h, ctypes.byref(r))
W, H = r.right - r.left, r.bottom - r.top
print('hwnd=%x %dx%d' % (h, W, H))

hdc = u32.GetWindowDC(h)
mdc = g32.CreateCompatibleDC(hdc)
bmp = g32.CreateCompatibleBitmap(hdc, W, H)
g32.SelectObject(mdc, bmp)
# PW_RENDERFULLCONTENT=2
ok = u32.PrintWindow(h, mdc, 2)
if not ok:
    ok = u32.PrintWindow(h, mdc, 0)
print('PrintWindow ok=', ok)

class BITMAPINFOHEADER(ct.Structure):
    _fields_ = [('biSize', ct.c_uint), ('biWidth', ct.c_long), ('biHeight', ct.c_long),
                ('biPlanes', ct.c_short), ('biBitCount', ct.c_short), ('biCompression', ct.c_uint),
                ('biSizeImage', ct.c_uint), ('biXPelsPerMeter', ct.c_long), ('biYPelsPerMeter', ct.c_long),
                ('biClrUsed', ct.c_uint), ('biClrImportant', ct.c_uint)]
class BITMAPINFO(ct.Structure):
    _fields_ = [('bmiHeader', BITMAPINFOHEADER), ('bmiColors', ct.c_uint * 3)]
bi = BITMAPINFO()
bi.bmiHeader.biSize = ct.sizeof(BITMAPINFOHEADER)
bi.bmiHeader.biWidth = W
bi.bmiHeader.biHeight = -H
bi.bmiHeader.biPlanes = 1
bi.bmiHeader.biBitCount = 32
bi.bmiHeader.biCompression = 0
buf = (ct.c_ubyte * (W * H * 4))()
g32.GetDIBits(mdc, bmp, 0, H, buf, ct.byref(bi), 0)
import numpy as np
from PIL import Image
a = np.frombuffer(buf, dtype=np.uint8).reshape(H, W, 4)[:, :, :3][:, :, ::-1]
out = sys.argv[1] if len(sys.argv) > 1 else 'liquefy_pw.png'
Image.fromarray(a).save(out)
print('已截图 ->', out)
