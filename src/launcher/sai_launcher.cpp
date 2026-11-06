// sai_launcher.exe — 静默启动器（图形子系统，无控制台窗口）
//
// 用法：把本文件和 sai_hooks.dll 一起放在 SAI 目录，双击本文件即可。
// 它会在自己所在目录找到 sai2.exe 并启动，等 SAI 主窗口出现后
// 自动把 sai_hooks.dll 注入进去，然后自己悄悄退出。
// 整个过程用户只看到 SAI 打开，和使用原版完全一样的体验。
//
// 为什么要这个文件：SAI 只依赖 12 个系统核心 DLL（都在 KnownDLLs 名单里，
// Windows 强制从 System32 加载，程序目录放同名文件无效），所以做不到
// "拷个 DLL 就永久生效"，只能用启动器代替用户手动双击注入 bat。
#include <windows.h>
#include <wchar.h>
#include <string.h>
#include <stdio.h>
#include <tlhelp32.h>

// 写一行日志到 %TEMP%\sai_launcher_log.txt（出问题排查用，正常无感）
static void Log(const wchar_t* msg) {
    wchar_t path[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, path)) return;
    lstrcatW(path, L"sai_launcher_log.txt");
    HANDLE f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    SetFilePointer(f, 0, NULL, FILE_END);
    char buf[1024];
    WideCharToMultiByte(CP_UTF8, 0, msg, -1, buf, sizeof(buf), NULL, NULL);
    DWORD n = 0;
    WriteFile(f, buf, (DWORD)strlen(buf), &n, NULL);
    WriteFile(f, "\r\n", 2, &n, NULL);
    CloseHandle(f);
}

// 找自己所在目录下的 sai2.exe 的 PID（可能刚启动，等它出现）
static DWORD WaitForSaiPid(const wchar_t* dir) {
    wchar_t expect[MAX_PATH];
    lstrcpyW(expect, dir);
    lstrcatW(expect, L"sai2.exe");
    // 简短路径比较：统一转成长路径再比
    wchar_t expectLong[MAX_PATH];
    if (!GetLongPathNameW(expect, expectLong, MAX_PATH)) lstrcpyW(expectLong, expect);

    for (int i = 0; i < 100; i++) { // 最多等 20 秒
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe;
            pe.dwSize = sizeof(pe);
            if (Process32FirstW(snap, &pe)) {
                do {
                    if (_wcsicmp(pe.szExeFile, L"sai2.exe") == 0) {
                        // 拿进程完整路径确认是我们启动的那个（防止注入到别的 SAI）
                        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
                        if (p) {
                            wchar_t path[MAX_PATH];
                            DWORD len = MAX_PATH;
                            if (QueryFullProcessImageNameW(p, 0, path, &len)) {
                                wchar_t long1[MAX_PATH], long2[MAX_PATH];
                                GetLongPathNameW(path, long1, MAX_PATH);
                                lstrcpyW(long2, expectLong);
                                _wcsupr(long1); _wcsupr(long2); // 大小写不敏感
                                if (wcsstr(long1, long2) != NULL ||
                                    wcsstr(long2, long1) != NULL) {
                                    CloseHandle(p);
                                    CloseHandle(snap);
                                    return pe.th32ProcessID;
                                }
                            }
                            CloseHandle(p);
                        }
                    }
                } while (Process32NextW(snap, &pe));
            }
            CloseHandle(snap);
        }
        Sleep(200);
    }
    return 0;
}

// 等 SAI 主窗口创建出来（注入太早 GUI 还没初始化，钩子会注册失败）
static BOOL WaitForMainWindow(DWORD pid) {
    for (int i = 0; i < 100; i++) { // 最多 20 秒
        HWND hwnd = GetTopWindow(NULL);
        while (hwnd) {
            DWORD wpid = 0;
            GetWindowThreadProcessId(hwnd, &wpid);
            if (wpid == pid && IsWindowVisible(hwnd) &&
                GetWindow(hwnd, GW_OWNER) == NULL) {
                wchar_t cls[256];
                GetClassNameW(hwnd, cls, 256);
                if (wcsstr(_wcsupr(cls), L"SAI") != NULL ||
                    wcsstr(_wcsupr(cls), L"SYSTEMAPI") != NULL ||
                    wcsstr(_wcsupr(cls), L"WX") != NULL) {
                    return TRUE;
                }
            }
            hwnd = GetNextWindow(hwnd, GW_HWNDNEXT);
        }
        Sleep(200);
    }
    // 窗口没等到也继续（宁可注入早，不能让用户等）
    return TRUE;
}

// 远程线程注入（逻辑同 injector.exe）
static BOOL InjectDll(DWORD pid, const wchar_t* dllPath) {
    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                  PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                              FALSE, pid);
    if (!proc) { Log(L"OpenProcess 失败"); return FALSE; }

    SIZE_T pathBytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    LPVOID remotePath = VirtualAllocEx(proc, NULL, pathBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) { Log(L"VirtualAllocEx 失败"); CloseHandle(proc); return FALSE; }

    BOOL ok = FALSE;
    if (WriteProcessMemory(proc, remotePath, dllPath, pathBytes, NULL)) {
        HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        LPTHREAD_START_ROUTINE loadLibrary =
            (LPTHREAD_START_ROUTINE)GetProcAddress(kernel32, "LoadLibraryW");
        HANDLE thread = CreateRemoteThread(proc, NULL, 0, loadLibrary, remotePath, 0, NULL);
        if (thread) {
            WaitForSingleObject(thread, 10000);
            DWORD exitCode = 0;
            GetExitCodeThread(thread, &exitCode);
            ok = (exitCode != 0);
            CloseHandle(thread);
        } else {
            Log(L"CreateRemoteThread 失败");
        }
    } else {
        Log(L"WriteProcessMemory 失败");
    }
    VirtualFreeEx(proc, remotePath, 0, MEM_RELEASE);
    CloseHandle(proc);
    return ok;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // 1. 自己所在目录
    wchar_t dir[MAX_PATH];
    if (!GetModuleFileNameW(NULL, dir, MAX_PATH)) return 1;
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (!slash) return 1;
    slash[1] = 0;

    wchar_t saiExe[MAX_PATH], hookDll[MAX_PATH];
    lstrcpyW(saiExe, dir);  lstrcatW(saiExe, L"sai2.exe");
    lstrcpyW(hookDll, dir); lstrcatW(hookDll, L"sai_hooks.dll");

    if (GetFileAttributesW(saiExe) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(NULL, L"没找到 sai2.exe\n请把本启动器放在 SAI 安装目录里。",
                    L"SAI 液化启动器", MB_ICONERROR);
        return 1;
    }
    if (GetFileAttributesW(hookDll) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(NULL, L"没找到 sai_hooks.dll\n请把它和启动器放在同一目录。",
                    L"SAI 液化启动器", MB_ICONERROR);
        return 1;
    }

    // 2. 如果 SAI 已经在运行，直接注入，不再开第二个
    DWORD existing = WaitForSaiPid(dir);
    if (existing) {
        Log(L"SAI 已在运行，直接注入");
        WaitForMainWindow(existing);
        if (InjectDll(existing, hookDll)) Log(L"注入成功(已有实例)");
        return 0;
    }

    // 3. 启动 SAI
    Log(L"启动 SAI");
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    if (!CreateProcessW(saiExe, NULL, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        MessageBoxW(NULL, L"sai2.exe 启动失败。", L"SAI 液化启动器", MB_ICONERROR);
        return 1;
    }
    CloseHandle(pi.hThread);

    // 4. 等窗口出来后注入
    WaitForMainWindow(pi.dwProcessId);
    Sleep(500); // 再等半秒让 SAI 完成初始化（注册热键在 DllMain 线程里，留点余量）
    if (InjectDll(pi.dwProcessId, hookDll)) {
        Log(L"注入成功");
    } else {
        Log(L"注入失败");
        MessageBoxW(NULL, L"sai_hooks.dll 注入失败。\n"
                         L"可能是杀毒软件拦截，请把本目录加入杀软白名单。",
                    L"SAI 液化启动器", MB_ICONWARNING);
    }
    CloseHandle(pi.hProcess);
    return 0;
}
