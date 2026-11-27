@echo off
setlocal
rem Build RRSafe.exe - the driverless launcher for rr_int.dll.
rem
rem It starts ROB RUN.exe suspended, maps rr_int.dll with rr_safemap.c before
rem the game's entry point runs, then resumes the game. No WmPort device, no
rem driver image, no LoadLibrary inside the target and no file in the game
rem folder.
rem
rem Output: RRSafe.exe in this folder, copied to ..\1_run.
cd /d "%~dp0"

set "VS="
for %%P in (
    "C:\Program Files\Microsoft Visual Studio\18\Insiders"
    "C:\Program Files\Microsoft Visual Studio\18\Professional"
    "C:\Program Files\Microsoft Visual Studio\2022\Community"
    "C:\Program Files\Microsoft Visual Studio\2022\Professional"
) do if not defined VS if exist "%%~P\VC\Auxiliary\Build\vcvars64.bat" set "VS=%%~P"
if not defined VS (
    echo [ERROR] vcvars64.bat not found - edit the VS detection above.
    pause
    exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo VCVARS_FAIL & exit /b 1 )

set "CFLAGS=/nologo /Brepro /O2 /D_CRT_SECURE_NO_WARNINGS /W3 /MT"

echo [1/2] mapper
cl %CFLAGS% /c rr_safemap.c /Fo:safe_map.obj || goto fail

echo [2/2] launcher
cl %CFLAGS% /c rr_safe_launch.c /Fo:safe_launch.obj || goto fail
link /nologo /Brepro /MACHINE:X64 /OUT:RRSafe.exe /SUBSYSTEM:CONSOLE ^
  safe_launch.obj safe_map.obj ^
  kernel32.lib user32.lib advapi32.lib shell32.lib || goto fail

copy /Y RRSafe.exe "..\1_run\RRSafe.exe" >nul
echo.
echo BUILD_OK  -^>  RRSafe.exe  (copied to ..\1_run)
exit /b 0

:fail
echo.
echo BUILD_FAIL
exit /b 1
