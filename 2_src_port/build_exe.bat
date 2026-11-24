@echo off
rem build_exe.bat - rebuild RRTool.exe, the double-click entry in the package.
rem Needs the same Visual Studio install build_int.bat uses; /MT keeps the exe
rem free of the VC runtime so the friend build runs on a stock Windows.
setlocal
cd /d "%~dp0"

set VCVARS=
for %%p in (
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
) do if not defined VCVARS if exist %%p set VCVARS=%%p
if not defined VCVARS (
    echo NO_VCVARS: vcvars64.bat not found
    exit /b 1
)

set OUT=..\1_run\RRTool.exe
if exist "%OUT%" del /q "%OUT%"

cmd /c "call %VCVARS% >nul 2>&1 && cl /nologo /W4 /permissive- /O2 /MT /DUNICODE /D_UNICODE rrtool_exe.c /link /SUBSYSTEM:CONSOLE /OUT:rrtool_exe.exe advapi32.lib shell32.lib user32.lib"
if errorlevel 1 (
    echo BUILD_FAILED
    exit /b 1
)

copy /y rrtool_exe.exe "%OUT%" >nul
del /q rrtool_exe.exe rrtool_exe.obj >nul 2>&1
for %%F in ("%OUT%") do echo BUILD_OK  -^>  %%~fF  ^(%%~zF bytes^)
endlocal
