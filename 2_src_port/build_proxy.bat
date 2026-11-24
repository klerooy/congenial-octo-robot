@echo off
setlocal
rem Build the version.dll proxy that side-loads rr_int.dll into ROB RUN.
rem Output: proxy\version.dll (x64), copied to ..\1_run\proxy\.
cd /d "%~dp0"

set "VS="
for %%P in (
    "C:\Program Files\Microsoft Visual Studio\18\Community"
    "C:\Program Files\Microsoft Visual Studio\18\Professional"
    "C:\Program Files\Microsoft Visual Studio\2022\Community"
    "C:\Program Files\Microsoft Visual Studio\2022\Professional"
) do if not defined VS if exist "%%~P\VC\Auxiliary\Build\vcvars64.bat" set "VS=%%~P"
if not defined VS ( echo [ERROR] vcvars64.bat not found & exit /b 1 )
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo VCVARS_FAIL & exit /b 1 )

if not exist proxy mkdir proxy
cl /nologo /O2 /MT /W3 /D_CRT_SECURE_NO_WARNINGS /LD version_proxy.c /Fe:proxy\version.dll /Fo:proxy\version_proxy.obj ^
  /link /MACHINE:X64 /OUT:proxy\version.dll kernel32.lib || goto fail

if not exist "..\1_run\proxy" mkdir "..\1_run\proxy"
copy /Y proxy\version.dll "..\1_run\proxy\version.dll" >nul
echo.
echo BUILD_OK  -^>  proxy\version.dll  (copied to ..\1_run\proxy)
exit /b 0

:fail
echo.
echo BUILD_FAIL
exit /b 1
