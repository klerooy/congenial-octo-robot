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
| `tools/harvest_il2cpp.py` | static metadata reader used to produce the tables above |

## Build

Windows x64, MSVC (VS 2022/2026 detection is in the script), no package manager:

```bat
2_src_port\build_int.bat
:: BUILD_OK -> rr_int.dll (copied to ..\1_run)
```

`/Brepro` is set on the compiler and the linker, so the same tree produces the
same bytes and a published SHA-256 means something.

## Tests

```bat
tests\run_checks.bat
:: SELFTEST OK (0 failures)
:: ALL OFFLINE CHECKS PASSED
```

`tests\verify_changes.py` compiles the aim suites against mocks (`SendInput`,
`GetAsyncKeyState`, `rr_read`, `rr_write`, the clock) and runs closed-loop plants.
Nothing in the suite touches a game process.

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
