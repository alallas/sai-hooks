@echo off
chcp 65001 >nul
rem ===== SAI 液化插件 - 一键注入 =====
rem 使用方法：先启动 SAI 并打开画布，再双击本脚本

setlocal

rem 找 sai2.exe 的 PID
set SAI_PID=
for /f "tokens=2" %%p in ('tasklist /fi "IMAGENAME eq sai2.exe" /fo csv /nh 2^>nul') do set SAI_PID=%%~p

if not defined SAI_PID (
    echo [×] 没有找到正在运行的 SAI。
    echo     请先启动 PaintTool SAI，打开或新建一个画布，然后再双击本脚本。
    echo.
    pause
    exit /b 1
)

echo [*] 找到 SAI 进程 PID=%SAI_PID%
echo [*] 正在注入液化插件...
"%~dp0injector.exe" %SAI_PID% "%~dp0sai_hooks.dll"

if errorlevel 1 (
    echo.
    echo [×] 注入失败。最常见原因：杀毒软件拦截了 injector.exe。
    echo     请把本文件夹加入杀毒软件白名单后再试。
    echo.
    pause
    exit /b 1
)

echo.
echo [√] 注入成功！
echo.
echo  使用方法：
echo    1. 在 SAI 里画几笔
echo    2. 按 Ctrl+Shift+X（或点 SAI 窗口右下角的蓝色「液化」按钮）
echo    3. 在弹出的液化窗口里用鼠标推拉线条
echo    4. 满意后点「确定」，回到 SAI 看效果
echo.
echo  注意：关掉 SAI 重开后，需要重新双击本脚本注入。
echo.
pause
