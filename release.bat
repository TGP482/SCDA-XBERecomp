@echo off
cd /d "%~dp0"
set "OUT=out\build\release"
set "D3DCOMPILER=%ProgramFiles(x86)%\Windows Kits\10\Redist\D3D\x64\d3dcompiler_47.dll"
if not exist "%D3DCOMPILER%" (echo d3dcompiler_47.dll not found: install the Windows SDK from the Visual Studio Installer & exit /b 1)
rem the Visual C++ runtime beside the executables, so a fresh Windows install needs nothing more
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
for /d %%v in ("%VSDIR%\VC\Redist\MSVC\1*") do for /d %%c in ("%%v\x64\Microsoft.VC*.CRT") do set "VCRUNTIME=%%c"
if not exist "%VCRUNTIME%\vcruntime140.dll" (echo Visual C++ runtime not found: install Visual Studio with the C++ workload & exit /b 1)
if not exist "%OUT%\SCDA.exe" call build.bat || exit /b 1
rem One name for every release, so releases/latest/download/SCDA-XBERecomp.zip stays a working link
set "ZIP=SCDA-XBERecomp.zip"
del /q "%ZIP%" 2>nul
rem an empty assets folder beside the executables, where the game (files or .iso) goes
rd /s /q "%OUT%\release" 2>nul
mkdir "%OUT%\release\assets"
rem the game's shadercache\ is a link to the one at the top (not in git): deleting out loses no shaders
if not exist shadercache mkdir shadercache
dir /AL /B "%OUT%" 2>nul | findstr /X /I shadercache >nul || (
    if exist "%OUT%\shadercache" (
        copy /y "%OUT%\shadercache\*" shadercache\ >nul
        rd /s /q "%OUT%\shadercache"
    )
    mklink /J "%OUT%\shadercache" "%CD%\shadercache" >nul
)
rem only the runtime DLLs the executables import; xbe_recomp.exe needs its own copy, Windows only looks beside the
rem exe it starts
del /q "%OUT%\compiler\*.dll" 2>nul
for %%f in (msvcp140 vcruntime140) do copy /y "%VCRUNTIME%\%%f.dll" "%OUT%\compiler\" >nul
rem the compiler folder without its work folder: no game code goes in the zip, the first start builds it. The ps2
rem folder without the controller screens drawn from the game in its cache
7z a "%ZIP%" ".\%OUT%\SCDA.exe" ".\%OUT%\SCDA_Versus.exe" ".\%OUT%\release\assets" ".\%OUT%\compiler" ".\%OUT%\ps2" "%D3DCOMPILER%" "%VCRUNTIME%\msvcp140.dll" "%VCRUNTIME%\vcruntime140.dll" "%VCRUNTIME%\vcruntime140_1.dll" -xr!work -xr!cache
