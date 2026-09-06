@echo off
rem ============================================================
rem  EasyCall 班级叫号系统 - 一键构建脚本 (MinGW-w64)
rem  产物输出: dist\EasyCall-Teacher.exe (教师端)
rem         dist\EasyCall-Board.exe  	  (教室端)
rem Gtihub构建：自动调用build.bat -nopause
rem ============================================================
setlocal
cd /d "%~dp0"

set CXX=g++
set FLAGS=-std=c++17 -O2 -DUNICODE -D_UNICODE -DWINVER=0x0601 -D_WIN32_WINNT=0x0601 -D_WIN32_IE=0x0600 -municode -mwindows -static -pthread
set LIBS=-lwinhttp -lcomctl32 -lcomdlg32 -lws2_32 -lgdi32 -luser32 -lgdiplus

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
g++ %FLAGS% src\board.cpp src\ec_net.cpp build\app.res -o dist\EasyCall-Board.exe %LIBS%
if errorlevel 1 goto :err

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