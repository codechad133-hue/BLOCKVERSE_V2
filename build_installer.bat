@echo off
setlocal
cd /d "%~dp0"
echo [1/3] Configuring...
cmake -S . -B build -A x64 || goto :fail
echo [2/3] Building Release...
cmake --build build --config Release || goto :fail
echo [3/3] Creating installer...
set ISCC="%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not exist %ISCC% set ISCC="%ProgramFiles%\Inno Setup 6\ISCC.exe"
if not exist %ISCC% (
  echo Inno Setup 6 not found. Install it from https://jrsoftware.org/isdl.php ^(or: winget install JRSoftware.InnoSetup^)
  echo Fallback: portable ZIP via "cpack -C Release --config build\CPackConfig.cmake"
  goto :fail
)
%ISCC% installer\BLOCKVERSE.iss || goto :fail
echo Done: dist\BLOCKVERSE-Setup-2.0.0.exe
exit /b 0
:fail
echo Build failed.
exit /b 1
