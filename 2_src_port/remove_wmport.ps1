# remove_wmport.ps1 - remove the old WmPort driver artifacts.
#
# Run from an elevated PowerShell:
#   powershell -ExecutionPolicy Bypass -File remove_wmport.ps1
#
# The driver is mapped into the kernel by kd2.exe; deleting the registry key
# does not unload a mapped image. A reboot is required before the machine is
# clean. This script touches only the WmPort service/key and the rr_exec
# services this project may have registered. It does not touch the game,
# Steam, or account data.

$ErrorActionPreference = 'SilentlyContinue'

$principal = New-Object Security.Principal.WindowsPrincipal(
    [Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host 'This script must run from an elevated PowerShell.' -ForegroundColor Red
    exit 1
}

Write-Host 'Stopping and deleting known service entries...'
foreach ($name in @('WmPort', 'rr_exec', 'rr_exec4', 'rr_exec5')) {
    $svc = Get-Service -Name $name -ErrorAction SilentlyContinue
    if ($svc) {
        Write-Host ("  {0}: {1}" -f $name, $svc.Status)
        Stop-Service -Name $name -Force -ErrorAction SilentlyContinue
        & sc.exe delete $name | Out-Null
    }
}

Write-Host 'Removing HKLM\SYSTEM\WmPort...'
if (Test-Path 'HKLM:\SYSTEM\WmPort') {
    Remove-Item -Path 'HKLM:\SYSTEM\WmPort' -Recurse -Force
    Write-Host '  removed'
} else {
    Write-Host '  not present'
}

Write-Host ''
Write-Host 'Reboot now. A mapped driver image stays resident until the machine restarts.' -ForegroundColor Yellow
Write-Host 'After the reboot, do not run RRTool.exe, kd2.exe or kernel-mode.sys again.' -ForegroundColor Yellow
