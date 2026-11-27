# fix_eac_clean.ps1 - restore a clean boot / DSE state and remove leftover drivers.
#
# Run from an elevated PowerShell:
#   powershell -NoProfile -ExecutionPolicy Bypass -File "C:\Users\adm\Downloads\RobRunCheat-main\2_src_port\fix_eac_clean.ps1"
#
# What it does:
#   * turns testsigning / nointegritychecks off and removes disableelamdrivers
#   * removes DISABLE_INTEGRITY_CHECKS from loadoptions, if present
#   * restores the stock g_CiOptions value (0x16) when the channel is up
#   * stops/deletes rr_exec, kernel-mode, WmPort and the old WmPort key
#   * moves the RobRunESP runtime artifacts out of the game folder
#
# It does NOT reboot. Reboot manually after it finishes.

$ErrorActionPreference = 'Continue'
$log = 'C:\Users\Public\rr_fix.log'

function Log([string]$m) {
    $line = '[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $m
    Write-Host $line
    Add-Content -LiteralPath $log -Value $line -Encoding UTF8
}

$principal = New-Object Security.Principal.WindowsPrincipal(
    [Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host 'Run this from an elevated PowerShell.' -ForegroundColor Red
    exit 1
}

Log '=== BCD before ==='
Log ((& bcdedit /enum '{current}' 2>&1 | Out-String).Trim())

# The two flags EAC documents for error 206.
foreach ($pair in @(@('testsigning', 'off'), @('nointegritychecks', 'off'))) {
    $out = & bcdedit /set $pair[0] $pair[1] 2>&1 | Out-String
    Log ('bcdedit /set {0} {1}: {2}' -f $pair[0], $pair[1], $out.Trim())
}

# Optional ELAM flag that also makes the boot non-standard.
$enum = & bcdedit /enum '{current}' 2>&1 | Out-String
if ($enum -match '(?im)^\s*disableelamdrivers\s+Yes') {
    $out = & bcdedit /deletevalue disableelamdrivers 2>&1 | Out-String
    Log ('bcdedit /deletevalue disableelamdrivers: {0}' -f $out.Trim())
}

# F7 / "Disable driver signature enforcement" leaves this token in loadoptions.
$enum = & bcdedit /enum '{current}' 2>&1 | Out-String
$m = [regex]::Match($enum, '(?im)^\s*loadoptions\s+(.+)$')
if ($m.Success) {
    $opts = $m.Groups[1].Value.Trim()
    Log ('loadoptions: {0}' -f $opts)
    if ($opts -match '(?i)DISABLE_INTEGRITY_CHECKS') {
        $new = (($opts -split '\s+') | Where-Object {
            $_ -and $_ -notmatch '^(?i)DISABLE_INTEGRITY_CHECKS$'
        }) -join ' '
        if ($new) {
            $out = & bcdedit /set loadoptions $new 2>&1 | Out-String
        } else {
            $out = & bcdedit /deletevalue loadoptions 2>&1 | Out-String
        }
        Log ('loadoptions cleaned: {0}' -f $out.Trim())
    }
}

# Stock value on this machine is 0x16 (22). A reboot resets ci.dll anyway,
# but restoring it now keeps a live session consistent if the user tests
# before rebooting.
$probe = 'C:\Users\adm\Desktop\123\rr_probe.exe'
if (Test-Path -LiteralPath $probe) {
    $out = & $probe dse 22 2>&1 | Out-String
    Log ('rr_probe dse 22: {0}' -f $out.Trim())
} else {
    Log ('rr_probe not found at {0}' -f $probe)
}

# Remove the old WmPort key and the rr_exec services.
$cleanup = 'C:\Users\adm\Downloads\RobRunCheat-main\2_src_port\remove_wmport.ps1'
if (Test-Path -LiteralPath $cleanup) {
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File $cleanup 2>&1 | Out-String
    Log ('cleanup: {0}' -f $out.Trim())
}

foreach ($svc in @('kernel-mode', 'rr_exec', 'rr_exec4', 'rr_exec5', 'WmPort')) {
    $s = Get-Service -Name $svc -ErrorAction SilentlyContinue
    if ($s) {
        Log ('service {0}: {1}' -f $svc, $s.Status)
        & sc.exe stop $svc 2>&1 | Out-Null
        & sc.exe delete $svc 2>&1 | Out-Null
    }
}

# Keep the cheat's scan logs, but out of the EAC-protected game folder.
$game = 'D:\SteamLibrary\steamapps\common\Rob Run'
$dest = 'C:\Users\adm\Desktop\123\logs'
if (-not (Test-Path -LiteralPath $dest)) {
    New-Item -ItemType Directory -Path $dest -Force | Out-Null
}
foreach ($f in @('robrun_runtime.txt', 'discovery.log')) {
    $src = Join-Path $game $f
    if (Test-Path -LiteralPath $src) {
        Move-Item -LiteralPath $src -Destination (Join-Path $dest $f) -Force
        Log ('moved {0}' -f $f)
    }
}

Log '=== BCD after ==='
Log ((& bcdedit /enum '{current}' 2>&1 | Out-String).Trim())
Log 'DONE. Reboot now.'
Write-Host ''
Write-Host 'Reboot now. After the reboot do not run RRTool, LOAD.bat, or rr_probe load.' -ForegroundColor Yellow
