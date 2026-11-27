@echo off
cd /d "%~dp0"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
dumpbin /imports rr_exec2.sys > imports_exec2.txt 2>&1
echo done
