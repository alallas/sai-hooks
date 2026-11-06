// injector.exe — 开发用 DLL 注入器
// 用法: injector.exe <PID> <dll 完整路径>
#include <windows.h>
#include <stdio.h>

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) {
        fwprintf(stderr, L"用法: injector.exe <PID> <dll路径>\n");
        return 1;
    }
    DWORD pid = (DWORD)_wtoi(argv[1]);
    const wchar_t* dllPath = argv[2];

    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                  PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                              FALSE, pid);
    if (!proc) { fwprintf(stderr, L"OpenProcess 失败: %lu\n", GetLastError()); return 1; }

    SIZE_T pathBytes = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    LPVOID remotePath = VirtualAllocEx(proc, NULL, pathBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) { fwprintf(stderr, L"VirtualAllocEx 失败: %lu\n", GetLastError()); return 1; }

    if (!WriteProcessMemory(proc, remotePath, dllPath, pathBytes, NULL)) {
        fwprintf(stderr, L"WriteProcessMemory 失败: %lu\n", GetLastError()); return 1;
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    LPTHREAD_START_ROUTINE loadLibrary =
        (LPTHREAD_START_ROUTINE)GetProcAddress(kernel32, "LoadLibraryW");

    HANDLE thread = CreateRemoteThread(proc, NULL, 0, loadLibrary, remotePath, 0, NULL);
    if (!thread) { fwprintf(stderr, L"CreateRemoteThread 失败: %lu\n", GetLastError()); return 1; }

    WaitForSingleObject(thread, 10000);
    DWORD exitCode = 0;
    GetExitCodeThread(thread, &exitCode);
    wprintf(L"注入完成: LoadLibrary 返回 0x%p（0 表示失败）\n", (void*)(ULONG_PTR)exitCode);

    VirtualFreeEx(proc, remotePath, 0, MEM_RELEASE);
    CloseHandle(thread);
    CloseHandle(proc);
    return exitCode ? 0 : 2;
}
