@echo off
setlocal
rem Dragon Ball Z: Burst Limit Recompiled - Windows build (RelWithDebInfo).
rem Requires Visual Studio 2022 (C++ workload), LLVM clang, CMake, Ninja and Python 3.
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo Visual Studio 2022 with the C++ workload was not found.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

if not exist "game_data_root\default.xex" (
  echo game_data_root\default.xex not found. Copy your game files first - see README.md.
  exit /b 1
)

rem Python 3 patches the recompiled code for rollback (scripts\rollback\patch_generated.py).
set "PY="
python -c "import sys; sys.exit(sys.version_info[0] != 3)" >nul 2>nul && set "PY=python"
if not defined PY py -3 -c "pass" >nul 2>nul && set "PY=py -3"
if not defined PY (
  echo Python 3 was not found ^(python or py on PATH^). It is needed to patch the recompiled code for rollback.
  exit /b 1
)

call scripts\fix_sdk_symlinks.bat || exit /b 1

set "PRESET=win-amd64-relwithdebinfo"
if not exist "out\build\%PRESET%\build.ninja" (
  cmake --preset %PRESET% %* || exit /b 1
)
rem First pass generates the recompiled sources, second pass picks them up.
cmake --build "out\build\%PRESET%" --target burstlimit_codegen || exit /b 1
%PY% -I scripts\rollback\patch_generated.py generated\default || exit /b 1
cmake "out\build\%PRESET%" || exit /b 1
cmake --build "out\build\%PRESET%" --target burstlimit || exit /b 1
rem The build can run the recompiler again (manifest changed), which drops the patch: patch and build again.
findstr /c:"rb2 write tracking" generated\default\burstlimit_pch.h >nul || (
  %PY% -I scripts\rollback\patch_generated.py generated\default || exit /b 1
  cmake --build "out\build\%PRESET%" --target burstlimit || exit /b 1
)
findstr /c:"rb2 write tracking" generated\default\burstlimit_pch.h >nul || (
  echo The rollback patch is missing from generated\default - see README.md, Building.
  exit /b 1
)

echo.
echo Build finished: out\build\%PRESET%\burstlimit.exe
