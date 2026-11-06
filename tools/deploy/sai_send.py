"""向 SAI 主窗发送 KIMI_SAIHOOKS_CMD（触发 UI 线程执行注入序列）"""
import ctypes, ctypes.wintypes as wt, sys, time

u32 = ctypes.windll.user32

def find_sai_hwnd(pid):
    res = []
    cb = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
    def proc(h, l):
        p = wt.DWORD()
        u32.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid and u32.IsWindowVisible(h):
            t = ctypes.create_unicode_buffer(256)
            u32.GetWindowTextW(h, t, 256)
            if 'PaintTool SAI' in t.value:
                res.append(h)
        return True
    u32.EnumWindows(cb(proc), 0)
    return res[0] if res else None

def send_cmd(pid, lparam=0, timeout_ms=15000):
    hwnd = find_sai_hwnd(pid)
    if not hwnd:
        print('未找到 SAI 主窗'); return None
    msg = u32.RegisterWindowMessageW('KIMI_SAIHOOKS_CMD')
    print(f'hwnd={hwnd} msg={msg} lparam={lparam}')
    r = wt.DWORD(0)
    ok = u32.SendMessageTimeoutW(hwnd, msg, 0, lparam, 0x0000, timeout_ms, ctypes.byref(r))
    if not ok:
        print(f'SendMessageTimeout 失败 err={ctypes.get_last_error()}')
        return None
    print(f'返回 {r.value}')
    return r.value

if __name__ == '__main__':
    pid = int(sys.argv[1])
    lparam = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    # 等 READY（日志出现 READY 行）
    import os
    log = os.path.join(os.environ['TEMP'], 'sai_undo_log.txt')
    for _ in range(30):
        try:
            if 'READY' in open(log, encoding='gbk', errors='replace').read()[-400:]:
                break
        except FileNotFoundError:
            pass
        time.sleep(0.5)
    else:
        print('等待 READY 超时')
    send_cmd(pid, lparam)
