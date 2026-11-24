@echo off
setlocal
rem Build rr_int.dll - the in-process payload.
rem
rem Loaded by the import-table trampoline in rr_map.c (rr_probe mapdll ...).
rem It reuses the reader layer from the external overlay: rr_game.c and its
rem dependencies are compiled in unchanged, so the field offsets and the
rem resolver behave exactly as they do outside the game.
rem
rem Output: rr_int.dll in this folder, copied to ..\1_run.
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
set "CXXFLAGS=/nologo /Brepro /O2 /D_CRT_SECURE_NO_WARNINGS /W3 /MT /EHsc /std:c++17"

echo [1/3] reader layer
cl %CFLAGS% /c rr_game.c /Fo:int_game.obj || goto fail
cl %CFLAGS% /c rr_mem.c /Fo:int_mem.obj || goto fail
cl %CFLAGS% /c rr_unity.c /Fo:int_unity.obj || goto fail
cl %CFLAGS% /c rr_chain.c /Fo:int_chain.obj || goto fail
cl %CFLAGS% /c rr_nodescan.c /Fo:int_nodescan.obj || goto fail
cl %CFLAGS% /c rr_classscan.c /Fo:int_classscan.obj || goto fail
cl %CFLAGS% /c rr_find.c /Fo:int_find.obj || goto fail
cl %CFLAGS% /c rr_int_cfg.c /Fo:int_cfg.obj || goto fail
cl %CFLAGS% /c rr_aim.c /Fo:int_aim.obj || goto fail
cl %CFLAGS% /c rr_recoil.c /Fo:int_recoil.obj || goto fail
cl %CFLAGS% /c rr_antiaim.c /Fo:int_antiaim.obj || goto fail
cl %CFLAGS% /c rr_int_glue.c /Fo:int_glue.obj || goto fail
cl %CFLAGS% /c rr_input_gate.c /Fo:int_gate.obj || goto fail

echo [2/3] payload
cl %CXXFLAGS% /c rr_int.cpp /Fo:int_payload.obj || goto fail

echo [3/3] link
link /nologo /Brepro /DLL /MACHINE:X64 /OUT:rr_int.dll /IGNORE:4099 ^
  int_payload.obj int_game.obj int_mem.obj int_unity.obj int_chain.obj ^
  int_nodescan.obj int_classscan.obj int_find.obj int_cfg.obj ^
  int_aim.obj int_recoil.obj int_antiaim.obj int_glue.obj int_gate.obj ^
  kernel32.lib user32.lib gdi32.lib advapi32.lib ole32.lib oleaut32.lib uuid.lib ^
  winmm.lib d3d11.lib dxgi.lib d2d1.lib dwrite.lib || goto fail

copy /Y rr_int.dll "..\1_run\rr_int.dll" >nul
echo.
echo BUILD_OK  -^>  rr_int.dll  (copied to ..\1_run)
exit /b 0

:fail
echo.
echo BUILD_FAIL
exit /b 1