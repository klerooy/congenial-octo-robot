# RobRunCheat

Source tree for an in-process overlay on a Unity IL2CPP title (Unity 6000.3.18f1,
metadata v39). Source only: no binaries, no driver blob, no game files.

The payload is a manually mapped DLL (`rr_int.dll`) that hooks `IDXGISwapChain::Present`
for rendering and runs a per-frame worker for the aim, trigger, recoil, anti-aim and
nospread. Game memory is reached through a read/write transport (`rr_mem.c`) and the
managed runtime is traversed directly (`rr_unity.c`, `rr_classscan.c`, `rr_nodescan.c`,
`rr_chain.c`).

## Layout

| path | contents |
|---|---|
| `2_src_port/` | every translation unit: payload, reader layer, aim/trigger/recoil/anti-aim, IL2CPP helpers, menu, HTML panel, diagnostics tool |
| `2_src_port/robrun_offsets.h` | managed field offsets for this game build |
| `2_src_port/robrun_sdk_offsets.h` | SDK-side offsets |
| `2_src_port/menu_bridge.h` | one schema macro; it defines `BR_Config`, the defaults, the clamps and the shared-memory layout |
| `tests/` | offline regression checks (`verify_changes.py` compiles and runs the aim suites) |
| `docs/il2cpp_signatures.md` | static harvest of the installed build: metadata layout, method-address walk, invocation transport, live status |
| `docs/chatgpt_brief.md` | architecture review brief: 8 problems, evidence, backlog |
| `docs/driverless_launch.md` | the WmPort-free launch path, cleanup steps and the remaining file/HWID notes |
| `tools/harvest_il2cpp.py` | static metadata reader used to produce the tables above |

## Build

Windows x64, MSVC (VS 2022/2026 detection is in the script), no package manager:

```bat
2_src_port\build_int.bat
:: BUILD_OK -> rr_int.dll (copied to ..\1_run)

2_src_port\build_safe.bat
:: BUILD_OK -> RRSafe.exe (copied to ..\1_run)
```

`/Brepro` is set on the compiler and the linker, so the same tree produces the
same bytes and a published SHA-256 means something.

## Driverless internal launch

The internal payload no longer opens `\Device\WmPort`, reads
`HKLM\SYSTEM\WmPort` or sends IOCTLs: `rr_mem.c` is compiled with
`RR_SELF_ONLY` and every read/write is a guarded copy in the game process
(`transport: self`). `RRSafe.exe` starts the game directly, maps `rr_int.dll`
with `rr_safemap.c` during early startup, and lets the game continue. No driver
is mapped and no file is placed in the game folder.

```bat
1_run\RRSafe.exe --find     :: print the discovered game path
1_run\RRSafe.exe
:: or: RRSafe.exe --game "D:\SteamLibrary\steamapps\common\Rob Run\ROB RUN.exe"
```

Starting `ROB RUN.exe` directly bypasses the `start_protected_game.exe` EAC
wrapper. If the game refuses to run or cannot go online without EAC, read the
tradeoff section in `docs/driverless_launch.md` before choosing another path.

See `docs/driverless_launch.md` for switches, the old-driver cleanup and the
remaining file/HWID notes. The old `RRTool` / `kd2` / `kernel-mode.sys` path
still works for the external overlay, but it must not be used with this internal
build if the driver ban vector is to be removed.

## Tests

```bat
tests\run_checks.bat
:: ALL OFFLINE CHECKS PASSED
```

`tests\verify_changes.py` compiles the aim suites against mocks (`SendInput`,
`GetAsyncKeyState`, `rr_read`, `rr_write`, the clock) and runs closed-loop plants.
The external overlay's `--selftest` link runs when its objects and the WebView2
SDK are present; otherwise that step is reported as skipped. Nothing in the
suite touches a game process.

## What exists

- **ESP / markers / radar** - boxes, names, health, skeleton, distances, loot, vehicles, owners.
- **Aim** - crosshair-relative pick, instant-lock snap path (bounded probe, per-axis
  gain measurement and sign), smooth fallback path, optional LOS.
- **Trigger** - screen-space joint test, delay, head-only or any joint, strict fire LOS.
- **RAGE** - preset over a copy of the config: key-free, whole-screen ring, head bone,
  autofire, nospread, anti-aim off.
- **Nospread** - zeroes the two local cone carriers (`WeaponSpread` FloatData and
  `PlayerFakeSpreadModel.Spread/Air/Move`) on the frame the worker runs.
- **Anti-aim** - bounded view perturbation with offset tracking, walks back to zero.
- **Norecoil** - pattern or camera-climb compensation.

## What does not exist

**Silent aim.** Every shot today leaves from the camera, so the view has to move.
See `SILENT_AIM.md` for the task, the evidence already gathered and the constraints.
