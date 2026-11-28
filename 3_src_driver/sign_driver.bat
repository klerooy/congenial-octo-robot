@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0"

net session >nul 2>&1
if errorlevel 1 ( echo [ERROR] Run elevated. & exit /b 1 )

set "SYS=%~dp0rr_exec.sys"
if not exist "%SYS%" ( echo [ERROR] Build first. & exit /b 1 )

echo [1/4] Creating self-signed code-signing certificate...
for /f "usebackq delims=" %%T in (`powershell -NoProfile -Command "$c = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=RR Driver Signer' -CertStoreLocation Cert:\LocalMachine\My -KeyUsage DigitalSignature -FriendlyName 'RR Driver Signing' -NotAfter (Get-Date).AddYears(5); $c.Thumbprint"`) do set "THUMB=%%T"
if "!THUMB!"=="" ( echo [ERROR] Cert creation failed. & exit /b 1 )
echo    Thumbprint: !THUMB!

echo [2/4] Finding signtool...
set "SIGNTOOL="
for /d %%D in ("C:\Program Files (x86)\Windows Kits\10\bin\10.*") do if exist "%%~D\x64\signtool.exe" set "SIGNTOOL=%%~D\x64\signtool.exe"
if not defined SIGNTOOL ( echo [ERROR] Install Windows SDK. & exit /b 1 )

echo [3/4] Signing rr_exec.sys...
"!SIGNTOOL!" sign /fd SHA256 /sm /sha1 !THUMB! /t http://timestamp.digicert.com "%SYS%"
if errorlevel 1 "!SIGNTOOL!" sign /fd SHA256 /sm /sha1 !THUMB! "%SYS%"
if errorlevel 1 ( echo [ERROR] Signing failed. & exit /b 1 )

echo [4/4] Verifying signature blob...
"!SIGNTOOL!" verify /ph "%SYS%" >nul 2>&1
if errorlevel 1 ( echo [ERROR] No signature hash. & exit /b 1 )
echo    Signature present.

echo.
echo  SIGN_OK — Thumbprint: !THUMB!
echo  For auto-sign on rebuild: set RR_SIGN_THUMB=!THUMB!
echo.
echo  Self-signed cert is NOT trusted by Windows CI.
echo  Mapper must bypass DSE at load time (g_CiOptions / BYOVD).
echo  EAC error 206 requires testsigning=OFF in BCD.
echo  Run fix_eac_clean.ps1 before launching the game.
endlocal