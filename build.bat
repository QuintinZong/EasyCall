@echo off
rem ============================================================
rem  EasyCall 班级叫号系统 - 一键构建脚本
rem  产物输出: dist\EasyCall-Teacher.exe (教师端)
rem         dist\EasyCall-Board.exe  	  (教室端)
rem Gtihub构建：自动调用build.bat -nopause
rem  说明: 若 src\ 下存在 wintoastlib.h/.cpp, 教室端自动改用 MSVC 编译以支持 WinToast
rem ============================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"

set CXX=g++
set FLAGS=-std=c++17 -O2 -DUNICODE -D_UNICODE -DWINVER=0x0601 -D_WIN32_WINNT=0x0601 -D_WIN32_IE=0x0600 -municode -mwindows -static -pthread
set LIBS=-lwinhttp -lcomctl32 -lcomdlg32 -lws2_32 -lgdi32 -luser32 -lgdiplus -ladvapi32

if not exist build mkdir build
if not exist dist mkdir dist

echo.
echo ============================================
echo    EasyCall 构建开始...
echo ============================================
echo.

echo [1/3] 编译资源...
windres src\app.rc -O coff -o build\app.res
if errorlevel 1 goto :err

echo [2/3] 构建教师端...
g++ %FLAGS% src\teacher.cpp src\ec_net.cpp src\ec_xlsx.cpp build\app.res -o dist\EasyCall-Teacher.exe %LIBS%
if errorlevel 1 goto :err

echo [3/3] 构建班级大屏端...
if not exist src\wintoastlib.cpp goto :board_mingw
rem ---- 检测到 WinToast 库: 查找 Visual Studio(库依赖 MSVC 的 WRL) ----
set "MSVCDIR="
for /f "usebackq tokens=*" %%i in (`"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath 2^>nul`) do set "MSVCDIR=%%i"
if exist "%MSVCDIR%\VC\Auxiliary\Build\vcvars64.bat" goto :board_msvc
goto :board_mingw

:board_msvc
echo    使用 MSVC 构建(含 WinToast 通知)...
call "%MSVCDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /utf-8 /permissive- /std:c++17 /O2 /MT /EHsc /DUNICODE /D_UNICODE /DWINVER=0x0601 /D_WIN32_WINNT=0x0A00 /D_WIN32_IE=0x0600 /DHAVE_WINTOAST src\board.cpp src\ec_net.cpp src\wintoastlib.cpp build\app.res /Fe:dist\EasyCall-Board.exe /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib comctl32.lib comdlg32.lib ws2_32.lib winhttp.lib ole32.lib shell32.lib propsys.lib runtimeobject.lib advapi32.lib gdiplus.lib shlwapi.lib
if errorlevel 1 goto :err
goto :build_done

:board_mingw
echo    使用 MinGW 构建(未集成 WinToast)...
g++ %FLAGS% src\board.cpp src\ec_net.cpp build\app.res -o dist\EasyCall-Board.exe %LIBS%
if errorlevel 1 goto :err

:build_done
echo.
echo ============================================
echo   构建成功
echo   教师端:   dist\EasyCall-Teacher.exe
echo   教室端:   dist\EasyCall-Board.exe
echo ============================================
echo.
if /i "%~1" NEQ "-nopause" pause
exit /b 0

:err
echo.
echo ============================================
echo   构建失败, 请查看错误信息
echo ============================================
echo.
if /i "%~1" NEQ "-nopause" pause
exit /b 1