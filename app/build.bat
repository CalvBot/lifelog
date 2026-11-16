@echo off
rem Builds build\lifelog.exe: one cl.exe call on lifelog.cpp (a unity build). SQLite and Dear ImGui
rem are compiled once into build\vendor\*.obj and reused. "build release" for an optimised build.
setlocal
cd /d "%~dp0"

where cl >nul 2>nul
if not errorlevel 1 goto have_cl
rem Kept out of a parenthesised block: the ")" in ProgramFiles(x86) would end it.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VS=%%i"
if not defined VS set "VS=%ProgramFiles%\Microsoft Visual Studio\2022\Community"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
:have_cl

if not exist build\vendor mkdir build\vendor
if not exist build\assets mkdir build\assets

set OPT=/Od /Zi
if "%1"=="release" set OPT=/O2 /Zi
set VENDOR_OPT=/O2 /nologo /c /MT

if not exist build\vendor\sqlite3.obj (
    cl %VENDOR_OPT% /TC /DSQLITE_ENABLE_FTS5 /DSQLITE_THREADSAFE=2 /DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 vendor\sqlite\sqlite3.c /Fobuild\vendor\sqlite3.obj || exit /b 1
)
for %%f in (imgui imgui_draw imgui_tables imgui_widgets imgui_impl_win32 imgui_impl_dx11) do (
    if not exist build\vendor\%%f.obj (
        cl %VENDOR_OPT% /std:c++20 /EHsc vendor\imgui\%%f.cpp /Fobuild\vendor\%%f.obj || exit /b 1
    )
)

copy /y ..\schema.sql build\ >nul
copy /y ..\assets\lucide.ttf build\assets\ >nul

cl /nologo /std:c++20 /MT /W4 /wd4100 /wd4201 /wd4505 /GR- /EHa- /utf-8 %OPT% /DUNICODE /D_UNICODE ^
    lifelog.cpp build\vendor\*.obj /Fobuild\ /Fdbuild\ /Febuild\lifelog.exe ^
    /link /INCREMENTAL:NO /SUBSYSTEM:WINDOWS ^
    user32.lib gdi32.lib shell32.lib ole32.lib d3d11.lib dxgi.lib dwmapi.lib ^
    || exit /b 1
echo built build\lifelog.exe
