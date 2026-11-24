@echo off
rem Build the payload and the probe, assemble dist\RRTool, zip it.
rem Any argument is passed through: -SkipBuild, -NoZip.
setlocal
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_share.ps1" %*
exit /b %ERRORLEVEL%
