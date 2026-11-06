# 不停 SAI 迭代：把当前 bin/sai_hooks.dll 复制为带序号的新文件注入。
# 新实例子类化会覆盖旧实例的窗口过程并拦截 IPC，因此只有新 DLL 响应触发。
import ctypes, shutil, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / 'bin' / 'sai_hooks.dll'

def find_sai_pid():
    r = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq sai2.exe'],
                       capture_output=True, text=True, encoding='gbk', errors='replace')
    for line in r.stdout.splitlines():
        if 'sai2.exe' in line:
            parts = line.split()
            if parts[1].isdigit():
                return int(parts[1])
    return None

def main():
    pid = int(sys.argv[1]) if len(sys.argv) > 1 else find_sai_pid()
    if not pid:
        print('未找到 SAI 进程'); sys.exit(1)
    dst_dir = ROOT / 'bin' / 'try'
    dst_dir.mkdir(exist_ok=True)
    n = len(list(dst_dir.glob('sai_hooks_*.dll'))) + 1
    dst = dst_dir / f'sai_hooks_{n:03d}.dll'
    shutil.copy2(SRC, dst)
    print(f'注入 {dst.name} -> pid {pid}')
    r = subprocess.run([str(ROOT / 'bin' / 'injector.exe'), str(pid), str(dst)],
                       capture_output=True, text=True, encoding='gbk', errors='replace')
    print(r.stdout.strip() or r.stderr.strip())
    time.sleep(1.5)
    sys.path.insert(0, str(ROOT / 'tools' / 'deploy'))
    from sai_send import send_cmd
    send_cmd(pid, 99)

if __name__ == '__main__':
    main()
