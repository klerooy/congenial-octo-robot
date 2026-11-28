@echo off
setlocal
cd /d "%~dp0"
if not defined OUT set "OUT=rr_exec.sys"

set "VS="
for %%P in (
    "C:\Program Files\Microsoft Visual Studio\18\Insiders"
    "C:\Program Files\Microsoft Visual Studio\18\Community"
    "C:\Program Files\Microsoft Visual Studio\18\Professional"
    "C:\Program Files\Microsoft Visual Studio\2022\Community"
    "C:\Program Files\Microsoft Visual Studio\2022\Professional"
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise"
    "C:\Program Files\Microsoft Visual Studio\2022\BuildTools"
) do if not defined VS if exist "%%~P\VC\Auxiliary\Build\vcvars64.bat" set "VS=%%~P"
if not defined VS ( echo [ERROR] vcvars64.bat not found & exit /b 1 )
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo VCVARS_FAIL & exit /b 1 )

echo [1/6] kernel exports
dumpbin /nologo /exports "%SystemRoot%\System32\ntoskrnl.exe" > nt_exports.txt || goto fail
python make_importlib.py nt_exports.txt ntoskrnl.def || goto fail
lib /nologo /def:ntoskrnl.def /machine:x64 /out:ntoskrnl.lib || goto fail

echo [2/6] compile C source
cl /nologo /c /O2 /GS- /Gm- /Zl /W3 /D_AMD64_ /DAMD64 ^
   /Fo:rr_exec.obj rr_exec.c || goto fail

echo [3/6] assemble callout stub
ml64 /nologo /c /Cx /Focallout.obj callout.asm || goto fail

echo [4/6] link
link /nologo /SUBSYSTEM:NATIVE /MACHINE:X64 /ENTRY:DriverEntry ^
     /NODEFAULTLIB /RELEASE /FIXED:NO /OPT:ICF ^
     /OUT:%OUT% rr_exec.obj callout.obj ntoskrnl.lib || goto fail

echo [5/6] resolve gadgets for mapper
python resolve_gadgets.py "%SystemRoot%\System32\ntoskrnl.exe" > gadgets.cfg || goto fail

echo [6/6] sign (optional)
if not defined RR_SIGN_THUMB ( echo SKIP_SIGN & goto built )
set "SIGNTOOL="
for /d %%D in ("C:\Program Files (x86)\Windows Kits\10\bin\10.*") do if exist "%%~D\x64\signtool.exe" set "SIGNTOOL=%%~D\x64\signtool.exe"
if not defined SIGNTOOL ( echo SIGNTOOL_MISSING & goto fail )
"%SIGNTOOL%" sign /fd SHA256 /sm /sha1 %RR_SIGN_THUMB% %OUT% || goto fail

:built
echo BUILD_OK ^> %OUT%
exit /b 0
:fail
echo BUILD_FAIL
exit /b 1