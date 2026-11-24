@echo off
setlocal
rem Build rr_probe.exe - the read-only live diagnostic.
rem
rem The overlay needs elevation because of its manifest; this console does not.
rem It links rr_mem.c (driver transport) and rr_il.c (export walk) directly and
rem answers read-path questions against a running game:
rem
rem   rr_probe module
rem   rr_probe exports il2cpp_
rem   rr_probe probe
rem   rr_probe peek 7FF6... 64
rem   rr_probe regions 12
rem   rr_probe handle
rem
rem Output: rr_probe.exe in this folder. No manifest, no privilege request.
cd /d "%~dp0"

set "VS="
for %%P in (
    "C:\Program Files\Microsoft Visual Studio\18\Community"
    "C:\Program Files\Microsoft Visual Studio\18\Professional"
    "C:\Program Files\Microsoft Visual Studio\2022\Community"
    "C:\Program Files\Microsoft Visual Studio\2022\Professional"
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise"
    "C:\Program Files\Microsoft Visual Studio\2022\BuildTools"
) do if not defined VS if exist "%%~P\VC\Auxiliary\Build\vcvars64.bat" set "VS=%%~P"
if not defined VS (
    echo [ERROR] vcvars64.bat not found - edit the VS detection above.
    exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo VCVARS_FAIL & exit /b 1 )

set "CFLAGS=/nologo /O2 /W3 /MT /D_CRT_SECURE_NO_WARNINGS"

echo [1/2] compile
cl %CFLAGS% /c rr_probe.c /Fo:rr_probe_main.obj || goto fail
cl %CFLAGS% /c rr_mem.c /Fo:rr_probe_mem.obj || goto fail
cl %CFLAGS% /c rr_il.c /Fo:rr_probe_il.obj || goto fail
cl %CFLAGS% /c rr_exec.c /Fo:rr_probe_exec.obj || goto fail
cl %CFLAGS% /c rr_map.c /Fo:rr_probe_map.obj || goto fail
cl %CFLAGS% /c rr_mmap.c /Fo:rr_probe_mmap.obj || goto fail
echo [2/2] link
link /nologo /SUBSYSTEM:CONSOLE /MACHINE:X64 /OUT:rr_probe.exe ^
  rr_probe_main.obj rr_probe_mem.obj rr_probe_il.obj rr_probe_exec.obj rr_probe_map.obj rr_probe_mmap.obj ^
  kernel32.lib advapi32.lib user32.lib ntdll.lib || goto fail

echo.
echo BUILD_OK  -^>  rr_probe.exe
exit /b 0

:fail
echo BUILD_FAIL
exit /b 1


