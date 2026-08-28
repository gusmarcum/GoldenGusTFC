@echo off
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul 2>&1
if exist tfcbot.dll del tfcbot.dll >nul 2>&1
if exist tfcbot.dll (
  echo LOCKED: tfcbot.dll is still loaded in hl.exe. Press END in game to unload, then rebuild.
  exit /b 1
)
cl /nologo /LD /O2 /W3 /EHsc main.cpp /Fe:tfcbot.dll /link /SUBSYSTEM:WINDOWS user32.lib
if exist tfcbot.dll (echo BUILD OK) else (echo BUILD FAILED)
