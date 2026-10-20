@echo off
setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [ERROR] Visual Studio Installer was not found.
    echo         Install Visual Studio 2022 or later with "Desktop development with C++".
    exit /b 1
)

set "VSPATH="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"

if not defined VSPATH (
    echo [ERROR] MSVC was not found.
    echo         Add "Desktop development with C++" in the Visual Studio Installer.
    exit /b 1
)

echo Visual Studio: %VSPATH%
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo [ERROR] Failed to run vcvars64.bat.
    exit /b 1
)

if not exist "build" mkdir "build"

echo Building...
pushd "build"
cl /nologo /std:c++20 /EHsc /O2 /utf-8 /W4 /permissive- /MT /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 /I"..\src" ..\src\*.cpp /Fe:AmazonMusicRPC.exe /link /SUBSYSTEM:WINDOWS windowsapp.lib runtimeobject.lib ole32.lib oleaut32.lib shell32.lib advapi32.lib winhttp.lib user32.lib comctl32.lib gdi32.lib
if errorlevel 1 (
    popd
    echo.
    echo [ERROR] Build failed.
    exit /b 1
)
popd

if not exist "dist" mkdir "dist"
copy /y "build\AmazonMusicRPC.exe" "dist\AmazonMusicRPC.exe" >nul
if errorlevel 1 (
    echo.
    echo [ERROR] Could not copy the executable to dist.
    echo         Close any running AmazonMusicRPC.exe and try again.
    exit /b 1
)

echo.
echo Build succeeded: dist\AmazonMusicRPC.exe
echo The first launch opens the settings window for your Discord Application ID.
