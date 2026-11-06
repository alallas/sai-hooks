# -*- coding: utf-8 -*-
"""自动验收辅助：关 SAI 上的弹窗，Ctrl+N 新建画布并回车确认，再 IPC 拉起液化窗。"""
import ctypes, ctypes.wintypes as wt, sys, time
ct = ctypes
u32 = ctypes.windll.user32

def enum_sai_windows():
    wins = []
    cb = ctypes.WINFUNCTYPE(ct.c_bool, wt.HWND, wt.LPARAM)
    def proc(h, l):
        pid = wt.DWORD()
        u32.GetWindowThreadProcessId(h, ctypes.byref(pid))
        if pid.value == SAI_PID:
            t = ctypes.create_unicode_buffer(256)
            u32.GetWindowTextW(h, t, 256)
            wins.append((h, t.value))
        return True
    u32.EnumWindows(cb(proc), 0)
    return wins

SAI_PID = int(sys.argv[1])

# 1. 只关提示弹窗（标题含「液化」或「SAI Hooks」），主窗不动
for h, t in enum_sai_windows():
    if u32.IsWindowVisible(h) and ('液化' in t or 'SAI Hooks' in t):
        print('关闭弹窗 hwnd=%x title=%s' % (h, t))
        u32.PostMessageW(h, 0x0010, 0, 0)  # WM_CLOSE
        time.sleep(0.3)

# 2. 激活 SAI 主窗，Ctrl+N
wins = [(h, t) for h, t in enum_sai_windows() if 'PaintTool' in t]
main = wins[0][0]
u32.ShowWindow(main, 9)
u32.SetForegroundWindow(main)
time.sleep(0.5)

def key(vk, delay=0.05):
    u32.keybd_event(vk, 0, 0, 0)
    time.sleep(delay)
    u32.keybd_event(vk, 0, 2, 0)  # KEYEVENTF_KEYUP
    time.sleep(delay)

CTRL = 0x11; N = 0x4E; ENTER = 0x0D
u32.keybd_event(CTRL, 0, 0, 0)
key(N)
u32.keybd_event(CTRL, 0, 2, 0)
print('已发 Ctrl+N')
time.sleep(1.5)
key(ENTER)  # 确认新建画布默认设置
time.sleep(1.0)
key(ENTER)  # 可能的二次确认
print('已回车确认')
