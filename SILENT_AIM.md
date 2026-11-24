# Silent aim - task brief

Goal: a shot lands on the target's head **regardless of where the crosshair is
pointing**, and the view never moves. Visible aim (what exists) steers the mouse;
silent aim rewrites the shot itself.

Nothing below is speculation about a game I cannot run. Each section names the
file, the address walk or the offset that makes it checkable. Anything that needs
a live process says so.

---

## 1. What the payload already has

| capability | where |
|---|---|
| per-frame worker with the camera and the roster | `rr_aim.c` -> `rr_aim_update(players, count, camera, w, h)`, called from `rr_int.cpp` (frame thread, after the snapshot) |
| target and joint selection | `rr_aim_pick()`, `rr_trigger_pick()`, `rr_aim_point(player, group, camera, w, h, &world)` |
| camera basis and projection | `rr_game.h` -> `RR_CAMERA { pos, rot, fwd, right, up, fov }`, `game_w2s()` |
| raw memory | `rr_mem.h` -> `rr_read`, `rr_write`, `rr_read_batch`, `rr_read_pid`, `rr_write_pid` (64 KiB per request) |
| managed-object traversal | `rr_unity.h` -> `u_klass`, `u_class_name`, `u_is`, `u_static_fields`, `u_string`, list/dict helpers |
| class and object scans | `rr_classscan.c`, `rr_nodescan.c`, `rr_chain.c`, `rr_find.c` |
| resolved IL2CPP exports | `il2cpp_class_from_name`, `il2cpp_image_get_class`, `il2cpp_domain_get`, `il2cpp_domain_get_assemblies`, `il2cpp_runtime_invoke`, `il2cpp_object_new`, `il2cpp_thread_attach/detach` |
| managed field offsets | `robrun_offsets.h`, `robrun_sdk_offsets.h` |
| settings plumbing | `menu_bridge.h` (`BR_SETTINGS` macro), `rr_int.cpp` (`INT_CFG`, `cfg_apply`, ini, menu rows) |

The camera used by every projection is the same one the game renders with
(`camera=model`, resolved in `rr_game.c`), so a direction computed from
`camera->pos` and a joint point is in the same space as the game's own shot.

## 2. The address walk that has to be used first

`docs/il2cpp_signatures.md`, section **Method addresses**, has the layout for this
exact build:

```
Il2CppClass.methods   -> MethodInfo* [method_count]
MethodInfo +0x00        native code pointer (inside GameAssembly.dll)
MethodInfo +0x10        method name (C string)
MethodInfo +0x18        klass back-pointer (null on a generic stub)
```

and a working finder: `--methoda <Class> [name-filter]` walks `Il2CppClass` from
`+0x80` to `+0x1A0`, validates every pointer as landing inside the loaded
`GameAssembly.dll` image, and prints each method name with `GameAssembly+0xRVA`.
`--sigscan <pattern>` goes the other way (byte pattern with `??` wildcards).

That is the whole entry point for this task: **find the method that turns a
crosshair position / camera ray into a shot, and get its address.**

## 3. Where the shot is likely to be decided

Ordered by how cheaply they can be checked, not by how likely they are:

1. **A "shoot" method on the weapon or character model.** Class names that already
   appear in the tree: `BattleCharacterPrivateModel`, `PlayerClientModel`,
   `BattleCharacterModel`, `PlayerEntityModel`, `WeaponModel`, `BattleModel`.
   Method names to filter on: `Shoot`, `Fire`, `Shot`, `Attack`, `Hit`, `Damage`,
   `Projectile`, `Raycast`, `Trace`, `Ray`, `Aim`, `Target`, `Direction`.
   ```bat
   rr_probe.exe --methoda BattleCharacterPrivateModel Shoot
   rr_probe.exe --methoda PlayerClientModel Fire
   ```
2. **A direction getter** the shoot method calls, e.g. something like
   `GetShootDirection`, `GetAimPoint`, `Camera.ScreenPointToRay`. Hooking the
   *getter* is easier than hooking the shot: the return value is the thing to
   rewrite, and every caller inherits it.
3. **A field the shot reads.** If the fire path reads a world point or a direction
   out of the client model, writing that field each frame is silent aim with no
   code patch at all - the same trick nospread uses on the cone. Look for float
   triples near the fields already mapped in `robrun_offsets.h`, and check them by
   freezing the value and watching where bullets land.
4. **The camera transform itself.** Last resort and not silent: it moves the view.

## 4. How to hook, given this payload

- The payload has **no unload path**: the mapped image stays until the process
  exits. So the relay must live in memory that survives (`VirtualAlloc`), never in
  a code cave that could be reused, and the hook must be **idempotent** - patched
  once, disarmed by a flag, never re-patched.
- A 12-byte absolute `jmp [rip+0]` + 8-byte target is the simplest patch at an
  x64 method entry; `GameAssembly`'s code pages are mapped RX, so flip with
  `VirtualProtect` for the 12 bytes, write, restore protection. Save the
  overwritten prologue bytes and execute them from the relay before jumping to
  `entry+12`.
- Anything that takes a by-value `Vector3`/`Ray` argument has its argument in
  registers (`xmm0-2`, or `rcx/rdx` for a reference). Rewriting a by-value struct
  in a hook means saving and rebuilding the register file - doable but brittle.
  A getter that *returns* a `Vector3` (xmm0) or fills an `sret` pointer (`rcx`) is
  the cheaper target, and it is the same address walk.
- `MethodInfo +0x00` is the address to patch only when the method is not shared
  generic code. For a shared/generic entry (name present, klass back-pointer
  null) patch the concrete caller instead.
- Keep every write bounded: one patch, size 12, target validated to be inside the
  `GameAssembly` image (`rr_mem` can read the module range through the PEB walk in
  `rr_mem.c`).

## 5. The selection side is already solved

`rr_aim_point(player, RR_AIM_HEAD, camera, w, h, &point)` returns the head in world
space for any player in the snapshot, and `snapshot_read` already filters dead
bodies, hidden bodies, vehicles and loot. Silent aim therefore needs no new
targeting code: pick with `rr_aim_pick` as RAGE does today, take the
`RR_AIM_PICK` it returns (`.player`, `.point`), and hand `.point` to the shot
redirect.

For a shot that leaves from a muzzle rather than the camera, the direction is
`normalize(point - origin)`; the origin comes from the weapon transform, which the
payload can read but does not map yet.

## 6. Switch and plumbing

Follow the nospread pattern, which is the newest switch in the tree:

1. `menu_bridge.h`: `X(silent_aim, 0, 0, 1) \` and bump `BR_VERSION`.
2. `rr_int.cpp`: field in `INT_CFG`, one line in the positional initialiser, a line
   in `cfg_apply`, a line in `cfg_save`/`cfg_load`, an enum row next to `M_NOSPREAD`,
   one entry in `row_tab`, one case in `menu_text` and one in `menu_set`.
3. `rr_aim.c`: call the redirect from `rr_aim_update` after the pick, before the
   trigger, and gate it on `config->silent_aim`.
4. RAGE preset: `rage.silent_aim = 1;` if the mode should imply it.

## 7. Tests (offline, no game)

`tests/aim_checks.c` and `tests/aim_full_checks.c` compile the real `rr_aim.c`
against mocks and already cover the picker, the snap loop and the trigger. Add:

- **direction math**: for a known camera and a known head point, the produced
  direction normalises to the expected values, is `NaN`-safe, and is invariant
  under resolution (it must not depend on `w`/`h`);
- **relay bytes**: the trampoline builder emits the exact 12-byte patch + 8-byte
  target for a known entry address, and refuses an address outside the module
  range;
- **fail closed**: a null or unreadable entry address, a dead player, or a point
  behind the camera leaves the hook disarmed and the shot untouched;
- **arm/disarm**: disarming restores the original bytes and a second arm does not
  stack a second patch.

`tests/run_checks.bat` must stay green: `SELFTEST OK (0 failures)` and
`ALL OFFLINE CHECKS PASSED`.

## 8. Constraints

- MSVC x64, `/MT`, C11 and C++17 as the existing files use them. No new
  dependencies.
- Memory access only through `rr_mem.h`; no `ReadProcessMemory` on the game.
- Bounded everything: fixed patch size, validated module range, no unbounded scan
  per frame.
- The reader layer stays read-only where tests assert it:
  `tests/verify_changes.py` asserts zero writes inside `snapshot_read_spread`; keep
  new writes out of readers.
- Comments say why, not what; no debug prints per frame except a throttled
  `ilog("silent: ...")` line like the existing `aim:` and `nospread:` lines.

## 9. What is unknown and must be measured

- Which method actually builds the shot direction (see section 3).
- Whether the shot origin is the camera or the muzzle.
- Whether the client computes the direction or the server does. If the server
  derives the shot from the replicated view direction, no client hook is silent -
  the view has to move, and that is the answer, not a workaround.
- Whether `VirtualProtect` on the `GameAssembly` code section succeeds under the
  anti-cheat's page protection (try it; if it fails, the field-write route in
  section 3.3 is the fallback).
