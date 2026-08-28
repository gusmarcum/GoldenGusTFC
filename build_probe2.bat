@echo off
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul 2>&1
set S=C:\Users\gmarc\Downloads\halflife-master\halflife-master
cl /nologo /W0 probe2.c /I"%S%\common" /I"%S%\engine" /I"%S%\pm_shared" /I"%S%\public" /Fe:probe2.exe
