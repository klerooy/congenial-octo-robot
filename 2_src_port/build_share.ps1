# build_share.ps1 - rebuild the payload and assemble the folder that goes to
# another machine, then zip it.
#
# What ships and why:
#   rr_probe.exe          the console that installs nothing and does everything:
#                         kernel channel proof, manual map, diagnostics
#   rr_exec4.sys          the channel driver (OP_PROTECT) - verdict 301 with
#                         Easy Anti-Cheat, the image the byovd stage starts
#   payload\kernel-mode.sys  the second driver the stage starts alongside it
#   payload\rr_int.dll    the in-process build: ESP, aim, anti-aim, loot, radar
#   byovd\RTCore64.sys.b64   borrowed signed driver, base64 so no AV flag on a
#                         .sys extension at rest
#   byovd\rr_byovd.ps1    the stage that turns DSE off, starts the pair, puts
#                         the stock flags back and unloads the borrowed driver
#   RRTool.exe            the double-click entry: elevates, then runs the script
#   RRTool.ps1 / .bat     the chain itself, paths relative to its own folder
#   play.ps1              Steam handoff plus the anti-cheat gate check
#   README.txt            what to do on the other machine
#
# Nothing is copied from a build output that was not just produced here: the
# script rebuilds rr_int.dll and rr_probe.exe first, so the zip cannot carry a
# stale payload.
#
# Usage:  powershell -NoProfile -ExecutionPolicy Bypass -File build_share.ps1
#         ... -SkipBuild     package what is on disk
#         ... -NoZip         leave dist\RRTool unpacked only

param(
    [switch]$SkipBuild,
    [switch]$NoZip
)

$ErrorActionPreference = 'Stop'
$src  = $PSScriptRoot
$root = Split-Path -Parent $src
$dist = Join-Path $root 'dist'
$stage = Join-Path $dist 'RRTool'
$zip  = Join-Path $dist 'RRTool.zip'

function Step([string]$text) { Write-Host "`n== $text" -ForegroundColor Cyan }
function Die([string]$text) { Write-Host "FAIL: $text" -ForegroundColor Red; exit 1 }

if (-not $SkipBuild) {
    Step 'build payload'
    & cmd.exe /c "`"$src\build_int.bat`"" | Write-Host
    if ($LASTEXITCODE -ne 0) { Die 'build_int.bat failed' }

    Step 'build probe'
    & cmd.exe /c "`"$src\build_probe.bat`"" | Write-Host
    if ($LASTEXITCODE -ne 0) { Die 'build_probe.bat failed' }
}

$files = [ordered]@{
    'rr_probe.exe'        = Join-Path $src  'rr_probe.exe'
    'rr_exec4.sys'        = Join-Path $root '3_src_driver\rr_exec4.sys'
    'payload\rr_int.dll'  = Join-Path $root '1_run\rr_int.dll'
    'payload\kernel-mode.sys' = Join-Path $root '1_run\payload\kernel-mode.sys'
    'byovd\RTCore64.sys.b64'  = Join-Path $root '1_run\byovd\RTCore64.sys.b64'
    'byovd\rr_byovd.ps1'      = Join-Path $root '1_run\byovd\rr_byovd.ps1'
    'RRTool.ps1'          = Join-Path $root '1_run\RRTool.ps1'
    'RRTool.exe'          = Join-Path $root '1_run\RRTool.exe'
    'RRTool.bat'          = Join-Path $root '1_run\RRTool.bat'
    'play.ps1'            = Join-Path $root '1_run\play.ps1'
}

Step 'stage'
foreach ($rel in $files.Keys) {
    if (-not (Test-Path -LiteralPath $files[$rel] -PathType Leaf)) { Die "missing input: $($files[$rel])" }
}
# Both kernel images ship signed with the project certificate. The anti-cheat
# only requires that a signature blob exists (verdict 206 fires on a blob-less
# image), so the blob has to travel with the file: signing on the receiving
# machine needs signtool and the private key, which friends do not have.
foreach ($rel in @('rr_exec4.sys', 'payload\kernel-mode.sys')) {
    $st = (Get-AuthenticodeSignature -LiteralPath $files[$rel]).Status
    if ($st -eq 'NotSigned') { Die "$rel carries no signature blob; sign it before packaging" }
    Write-Host ("  {0,-26} signed: {1}" -f $rel, $st)
}
if (Test-Path -LiteralPath $stage) {
    # A file held by a running RRTool.exe, an Explorer window or a scanner
    # blocks the recursive delete. Clear what can be cleared, wait, retry; a
    # stale folder left under the standard name is worse than no folder at all,
    # because that is the one a double-click finds and it has no payload in it.
    for ($try = 0; $try -lt 3 -and (Test-Path -LiteralPath $stage); ++$try) {
        try { Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction Stop }
        catch {
            Get-ChildItem -LiteralPath $stage -Force -ErrorAction SilentlyContinue |
                ForEach-Object { Remove-Item -LiteralPath $_.FullName -Recurse -Force -ErrorAction SilentlyContinue }
            Start-Sleep -Milliseconds 400
        }
    }
    if (Test-Path -LiteralPath $stage) {
        # Rename succeeds where delete does not: move the locked folder aside so
        # the new stage can still take the standard name.
        $aside = "RRTool-old-" + (Get-Date -Format 'MMdd-HHmmss')
        try {
            Rename-Item -LiteralPath $stage -NewName $aside -ErrorAction Stop
            Write-Host ("  locked stage moved to dist\{0}" -f $aside) -ForegroundColor Yellow
            Remove-Item -LiteralPath (Join-Path $dist $aside) -Recurse -Force -ErrorAction SilentlyContinue
        } catch {
            $stage = Join-Path $dist ("RRTool-" + (Get-Date -Format 'MMdd-HHmm'))
            Write-Host ("  dist\RRTool is locked; staging under {0}" -f (Split-Path -Leaf $stage)) -ForegroundColor Yellow
            Write-Host '  dist\RRTool is stale now: close RRTool.exe and delete it, or the next double-click finds the old one' -ForegroundColor Yellow
        }
    }
}
foreach ($rel in $files.Keys) {
    $target = Join-Path $stage $rel
    New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
    Copy-Item -LiteralPath $files[$rel] -Destination $target -Force
    Write-Host ("  {0,-26} {1,10:N0} bytes" -f $rel, (Get-Item -LiteralPath $target).Length)
}

# The stage is what a double-click runs, so a stage without the payload is a
# package that fails on the receiving machine. Check, do not assume.
foreach ($rel in @('payload\rr_int.dll', 'byovd\rr_byovd.ps1', 'byovd\RTCore64.sys.b64',
                   'RRTool.exe', 'RRTool.ps1', 'rr_probe.exe', 'rr_exec4.sys',
                   'payload\kernel-mode.sys', 'play.ps1')) {
    if (-not (Test-Path -LiteralPath (Join-Path $stage $rel) -PathType Leaf)) {
        Die "stage is incomplete: $rel missing under $stage"
    }
}

Step 'readme'
$readme = @'
RRTool - in-process build for ROB RUN

WHAT THIS IS
  An overlay payload that runs inside the game: player and loot ESP, aim with
  a closed-loop mouse lock, anti-aim, radar and auto-loot. The payload owns no
  thread of its own; everything runs inside the game's present hook, because
  Easy Anti-Cheat terminates created threads.

BEFORE YOU START
  * Windows 10/11 x64, the game installed through Steam (app id 4262460)
  * run from a folder whose path is plain ASCII: C:\RRTool is right,
    C:\Users\<name>\<cyrillic folder>\ is not - the mapper narrows paths
    with the ANSI code page
  * an administrator prompt appears once. The kernel services need it.
  * on the first run after a reboot close the game if it is open: the
    anti-cheat refuses driver loads while it runs. Later runs reuse the loaded
    drivers and do not care.

RUN
  1. double-click RRTool.exe
  2. it elevates itself, registers rr_exec + kernel-mode, stops the anti-cheat
     service, loads the pair through the borrowed driver and proves the
     channel. Each of those steps is skipped on a later run, when it is
     already up.
  3. it launches the game through Steam, or takes the window that is already
     open, waits for a hub scene instead of a fixed delay, maps the payload
     once and reports whether frames are moving
  4. running it again is harmless: a process that already carries the payload
     is detected and left alone. A second map would hook Present twice.
  5. in game: F1 opens the panel, INSERT closes it. The panel is mouse driven:
     left-click the left half of a row to decrease, the right half to increase,
     click the key column to rebind. Settings are saved to
     C:\Users\Public\rr_int.ini and survive restarts.

COMMANDS
  RRTool.exe                the whole chain, nothing to type
  RRTool.exe -NoPause       same, but close the window instead of waiting
  RRTool.exe -MapOnly       map into a game that is already at the menu
  RRTool.exe -NoGame        drivers only; start the game yourself, then run
                            RRTool.exe -MapOnly
  RRTool.exe -Install       register the services and stop
  RRTool.exe -Uninstall     stop and delete every service this tool created
  RRTool.exe -Check         show what the folder resolves to, change nothing
  RRTool.exe -Force         map even when this process already carries a payload
  RRTool.exe -AppId 12345   a different Steam app id
  RRTool.bat                the same chain as a script, same switches

WHAT CAN STOP IT (all three are machine settings, not bugs)
  * vulnerable-driver blocklist - Windows 11 ships it on, and it refuses the
    borrowed driver by name, so the stage fails before it does anything.
    Run: RRTool.bat -DisableBlocklist  then reboot if the stage still refuses.
  * memory integrity (HVCI) - Windows Security > Device security > Core
    isolation > Memory integrity. While it is on the borrowed driver cannot
    load at all. Turn it off, reboot.
  * Windows Defender can quarantine RTCore64.sys out of the extracted folder.
    The stage already adds an exclusion for C:\ProgramData\rr\byovd; if the
    file goes missing from the archive, restore protection after the run.

FILES
  RRTool.exe                double-click entry: elevation + RRTool.ps1 -Auto
  rr_probe.exe              console: channel proof, manual map, diagnostics
  rr_exec4.sys              channel driver (OP_PROTECT)
  payload\rr_int.dll        the payload
  payload\kernel-mode.sys   companion driver
  byovd\                    borrowed signed driver (base64) + the stage script
  RRTool.ps1 / RRTool.bat   the chain itself
  play.ps1                  Steam handoff + anti-cheat gate check
  SHA256SUMS.txt            checksums of everything above

IF THE MAP FAILS
  The payload logs to C:\Users\Public\rr_int.log. If that file grew while the
  map printed no answer, the image is already in - the tool treats that as
  loaded and will refuse to map a second time on the next run. Restart the game
  to start clean, then run RRTool.exe again.
'@
Set-Content -LiteralPath (Join-Path $stage 'README.txt') -Value $readme -Encoding ASCII

Step 'checksums'
$sums = foreach ($f in Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName) {
    $rel = $f.FullName.Substring($stage.Length + 1)
    '{0}  {1}' -f (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash.ToLower(), $rel
}
Set-Content -LiteralPath (Join-Path $stage 'SHA256SUMS.txt') -Value $sums -Encoding ASCII
Write-Host ("  {0} entries" -f @($sums).Count)

if (-not $NoZip) {
    Step 'zip'
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal
    Write-Host ("  {0}  {1:N0} KB" -f $zip, ((Get-Item -LiteralPath $zip).Length / 1KB))
}

Step 'done'
Write-Host ("  folder: {0}" -f $stage)
if (-not $NoZip) { Write-Host ("  archive: {0}  - send this" -f $zip) }
exit 0
