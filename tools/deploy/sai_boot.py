# 启动 SAI -> 自动过恢复对话框 -> 注入 sai_hooks.dll
# 注意：SAI 对话框（sflRootWindow）完全自绘，PostMessage 点击无效，
# 必须用「附加输入队列 + SetForegroundWindow + 真实鼠标点击」（已实证）。
import ctypes, time, sys, subprocess
from ctypes import wintypes as wt

u = ctypes.windll.user32
k = ctypes.windll.kernel32
SAI = r"D:\Repos\sai-hooks\[Saier]SAI Ver.2\sai2.exe"
DLL = r"D:\Repos\sai-hooks\bin\sai_hooks.dll"

def enum_windows():
    out = []
    cb = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
    def f(h, lp):
        p = wt.DWORD()
        u.GetWindowThreadProcessId(h, ctypes.byref(p))
        t = ctypes.create_unicode_buffer(300)
        u.GetWindowTextW(h, t, 300)
        out.append((h, p.value, u.IsWindowVisible(h), t.value))
        return True
    u.EnumWindows(cb(f), 0)
    return out

def find_main(pid):
    for h, p, vis, t in enum_windows():
        if p == pid and vis and "PaintTool SAI" in t:
            return h
    return None

def dialogs(pid, main):
    return [(h, t) for h, p, vis, t in enum_windows()
            if p == pid and vis and h != main and t and "PaintTool" not in t]

def force_foreground(hwnd):
    fg = u.GetForegroundWindow()
    fg_tid = u.GetWindowThreadProcessId(fg, None)
    my_tid = k.GetCurrentThreadId()
    u.AttachThreadInput(my_tid, fg_tid, True)
    u.SetForegroundWindow(hwnd)
    time.sleep(0.3)
    u.SetWindowPos(hwnd, ctypes.c_void_p(-1), 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0040)
    u.AttachThreadInput(my_tid, fg_tid, False)
    time.sleep(0.4)

def real_click(hwnd, dx, dy):  # dx,dy 为相对窗口左上角的偏移
    r = wt.RECT()
    u.GetWindowRect(hwnd, ctypes.byref(r))
    u.SetCursorPos(r.left + dx, r.top + dy)
    time.sleep(0.15)
    u.mouse_event(0x0002, 0, 0, 0, 0)
    time.sleep(0.1)
    u.mouse_event(0x0004, 0, 0, 0, 0)
    time.sleep(0.8)

proc = subprocess.Popen([SAI])
pid = proc.pid
print("SAI pid =", pid)

main = None
for _ in range(40):
    main = find_main(pid)
    if main: break
    time.sleep(1)
if not main:
    print("主窗未出现"); sys.exit(1)
print("主窗 hwnd =", main)

time.sleep(3)  # 对话框晚于主窗出现，先等再轮询
deadline = time.time() + 45
clicked_yes = False
saw_dialog = False
while time.time() < deadline:
    ds = dialogs(pid, main)
    if not ds:
        if saw_dialog:  # 出现过又消失 = 处理完了
            break
        time.sleep(1.0)
        continue
    saw_dialog = True
    for h, t in ds:
        print(f"对话框 {t!r}")
        force_foreground(h)
        if "强制退出" in t:
            real_click(h, 200, 146)   # 是(Y)
            clicked_yes = True
        elif "恢复" in t:
            real_click(h, 230, 310)   # 恢复点列表项
            time.sleep(0.4)
            real_click(h, 600, 458)   # 恢复
    time.sleep(1.0)

print("点了「是」:", clicked_yes)
u.ShowWindow(main, 9)  # SW_RESTORE
time.sleep(1.5)

r = subprocess.run([r"D:\Repos\sai-hooks\bin\injector.exe", str(pid), DLL],
                   capture_output=True)
print("injector:", r.stdout.decode("gbk", "replace").strip(),
      r.stderr.decode("gbk", "replace").strip())
print("PID", pid)
