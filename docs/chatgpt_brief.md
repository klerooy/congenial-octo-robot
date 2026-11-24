# Review brief: in-process instrumentation overlay for a Unity / D3D11 game client

I am building a Windows research overlay that runs inside a shipped Unity game
process and measures two things I cannot measure from outside the process: how
much latency there is between my input and the rendered frame, and how accurately
a 3D world point projects onto the swapchain the game actually presents. To get
those numbers I had to solve a handful of systems problems that I would like an
outside review of, because each of them is a place where my implementation works
but I am not certain it is *correct*.

The overlay is local-only: it attaches to one game process I am running, reads
that process, draws over it, and injects synthetic pointer input into the same
process. It has no installer, no persistence, no network code, no propagation.
Everything below is the internals of that tool.

I am asking for **review and design advice, not for a rewrite**. If a section's
approach is wrong, say what the correct approach is and what invariant it
protects.

---

## 1. Environment

| item | value |
|---|---|
| OS | Windows 11 x64, HVCI off, test signing off |
| compiler | MSVC 19.4x, `/W4 /permissive- /MT` for the launcher, `/MT` for the payload |
| graphics | the client renders through a Unity D3D11 backend, flip-model swapchain |
| overlay API | D2D1 + DWrite, drawn inside the client's own present call |
| input | `SendInput` with `MOUSEEVENTF_MOVE` relative counts |
| host process | the game is 64-bit, Unity 2021-era runtime, Mono |
| module loading | manual map (no `LoadLibrary` in the target) |
| module format | a PE DLL with a static CRT, no C++ exceptions on hot paths, SEH guarded |

Two constraints shape almost every design decision:

1. **The client refuses threads created inside it.** Any thread the payload
   creates is terminated by the client's own integrity component within
   milliseconds. So the payload cannot run a loop of its own: all work is done
   inside the present hook the game already calls, once per frame.
2. **The module is mapped by hand, not loaded.** Nothing registers the image
   with the loader, so there is no `DllMain` contract, no TLS directory
   processing, no unwind tables unless I add them myself, and no CRT per-thread
   state for the thread that arrives at the entry point.

The second constraint has already cost me real bugs (see P6 and P7), and I
suspect there are more in the same family.

---

## 2. Component map

| file | role |
|---|---|
| `rr_mmap.c` | manual PE mapper: section copy, relocations, imports, entry call |
| `rr_mem.c` | memory access layer: two transports, batched reads |
| `rr_chain.c` | pointer-chain resolver (`module+off -> [..+off] -> ...`) |
| `rr_unity.c` | runtime class/field lookup through the Mono API |
| `rr_esp.c` | entity enumeration, projection, screen-space label layout |
| `rr_aim.c` | closed-loop cursor servo: gain/direction estimation, correction |
| `rr_antiaim.c` | view offset modulator used for input-characterisation runs |
| `rr_recoil.c` | pattern compensation for the client's own view kick |
| `rr_los.c` | ray/occlusion test against colliders |
| `rr_int.cpp` | the payload: hook install, D2D overlay, menu, telemetry |
| `rr_byovd.ps1` | staging helper that loads two signed test modules (see P7) |
| `RRTool.ps1` / `RRTool.exe` | launcher chain and elevation entry |

---

## 3. Open problems

### P1 — Hooking a swapchain whose method pointers are cached by the engine

The engine caches the swapchain's vtable slots when it creates the device, so
replacing the pointers in the live vtable alone does not take effect on every
frame. I patch both the vtable and, in one pass, every aligned 8-byte word in the
process that still equals the original `Present`/`Present1` address.

```cpp
/* sweep: patch every pointer that still equals the real Present/Present1.
 * Reads go through ReadProcessMemory, never a dereference: regions are
 * decommitted under a live game and a fault inside a hand-mapped image
 * used to take the whole process with it. */
while (VirtualQuery(cursor, &mbi, sizeof mbi) == sizeof mbi) {
    unsigned char* base = (unsigned char*)mbi.BaseAddress;
    unsigned char* next = base + mbi.RegionSize;
    if (next <= base) break;
    if (mbi.State == MEM_COMMIT &&
        (mbi.Type == MEM_IMAGE || mbi.Type == MEM_PRIVATE) &&
        (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY)) &&
        !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
        !(self && base <= self && self < next)) {
        for (off = 0; off + sizeof(uint64_t) <= mbi.RegionSize; off += CHUNK) {
            size_t chunk = mbi.RegionSize - off;
            if (chunk > CHUNK) chunk = CHUNK;
            if (!ReadProcessMemory(GetCurrentProcess(), base + off, buf, chunk, &got) && !got) {
                if (!off) log("region %p unreadable, skipped", base);
                break;
            }
            n = got / sizeof(uint64_t);
            for (i = 0; i < n; ++i) {
                uint64_t cand = ((const uint64_t*)buf)[i];
                if (!cand) continue;
                for (k = 0; k < 2; ++k) {
                    if (cand != want[k]) continue;
                    q = (uint64_t*)(base + off + i * sizeof(uint64_t));
                    if (!VirtualProtect(q, sizeof(uint64_t), PAGE_READWRITE, &oldp)) continue;
                    if (WriteProcessMemory(GetCurrentProcess(), q, &repl[k], 8, &wrote) && wrote == 8)
                        ++patched;
                    VirtualProtect(q, sizeof(uint64_t), oldp, &oldp);
                }
            }
            if (got < chunk) break;
        }
    }
    cursor = next;
}
```

Questions:

1. The sweep writes 8 bytes and restores the old protection per candidate. Is
   there a window where a concurrent thread reads a half-written pointer, and
   does it matter for a function pointer that is only ever called through the
   vtable? What is the correct way to make this atomic — aligned 8-byte stores
   are atomic on x64, but the *protection* change is not; is the `VirtualProtect`
   pair itself the hazard?
2. `MEM_PRIVATE` regions include the heap. Patching a value that happens to
   equal the Present address in heap memory corrupts a data structure. Is there
   a cheap discriminator beyond "the region is an image section and the offset
   lies in a `PAGE_EXECUTE_READ` sibling", and would that miss the case I am
   trying to catch?
3. Flip-model engines usually call `Present1`. My detection is
   `in_module(ptr, GetModuleHandleW(L"dxgi.dll"))` — i.e. "the slot points into
   dxgi, so it is a real implementation and not a wrapper". Is this a sound test
   across Windows 10/11 dxgi versions, including the `d3d11.dll` indirection?
4. The sweep runs once at map time. If the engine recreates the swapchain
   (device reset, resolution change, alt-tab to exclusive fullscreen) the new
   vtable is fresh and unpatched — my `ResizeBuffers` hook re-patches the vtable
   but does **not** re-sweep. Should the sweep be re-run on
   `ResizeBuffers`/`SetFullscreenState`, and is there a reliable "new swapchain
   object" signal inside the hook itself?

### P2 — Two transports behind one read API

The payload can read memory either through a kernel-mode channel driver (one
IOCTL per batch) or, when that driver is absent, through `ReadProcessMemory`.
Callers must not know which is in use, and a failure must be distinguishable
from "read returned zeros".

```c
int rr_read_pid(uint32_t pid, uint64_t addr, void* out, size_t n) {
    if (!out || n == 0 || n > 0x10000u) return 0;
    if (g_dev && g_pdioc) {
        DRVCMD c; /* key, size, pid, pad, address */
        NTSTATUS st = g_pdioc(g_dev, NULL, NULL, NULL, &ios, IOCTL_READ,
                              &c, sizeof c, out, (ULONG)n);
        if (st >= 0 && ios.Information == n) { (*ok_cell()) = 1; return 1; }
        return 0;
    }
    if (!user_open(pid)) return 0;
    if (!ReadProcessMemory(g_uproc, (LPCVOID)(ULONG_PTR)addr, out, n, &got) || got != n)
        return 0;
    (*ok_cell()) = 1;
    return 1;
}
```

The batch path splits on a reply budget and recurses:

```c
if (total > 0xF000u) {
    while (head < n && acc + items[head].size <= 0xF000u) acc += items[head].size, head++;
    if (head == 0) return 0;                 /* one item is over budget */
    if (head == n) return 0;                 /* recursion guard: no progress */
    a = rr_read_batch(items, head);
    b = rr_read_batch(items + head, n - head);
    if (a < 0 && b < 0) return -1;
    return (a > 0 ? a : 0) + (b > 0 ? b : 0);
}
/* reply layout: total bytes of data, then one status byte per item */
if (st < 0 || ios.Information != off + (ULONG)n) { free(buf); return -1; }
```

Questions:

1. `NT_SUCCESS` is `st >= 0`; I also require `Information == n` before calling
   the read successful. Is there a driver-visible case where a partially
   completed IOCTL is still useful, and does the strict check throw away good
   data?
2. The reply layout is "data then status bytes" in one buffer. If the driver
   ever returns fewer status bytes than items, my check fails the whole batch
   and the caller falls back to individual reads — but an individual read on a
   dead page is also a failure, so the fallback is `n` round trips for nothing.
   Is a per-item first-failure split (binary search on the item range) a better
   policy than whole-batch rejection?
3. The recursion bound: with `BATCH_MAX` items each of up to 64 KiB, the worst
   case recursion depth is `n` and each level copies the whole request header.
   Is there a formulation that keeps the "no progress" guard but bounds depth
   by `log(n)`?
4. `(*ok_cell())` is a one-shot "this frame had at least one good read" flag
   consumed elsewhere. That is a side effect on a success path; it makes the
   read path non-reentrant. Would you keep it or replace it with an out-param?

### P3 — A cursor servo that estimates the plant instead of assuming it

The overlay drives the crosshair with relative mouse counts. Pixels-per-count
and axis direction are not knowable in advance (sensitivity slider, invert-Y,
DPI, the client's own smoothing), so the servo measures both from what its own
moves actually did.

```c
/* one axis: learn gain and direction from the last move, then ask for the
 * counts the remaining error needs */
static int snap_axis(SNAP_AXIS* a, float err, unsigned dt_ms) {
    const float s = (float)a->sign;
    if (isfinite(a->err)) {
        float moved  = a->err - err;
        float landed = moved * s / a->px;
        if (a->pend > 0 && landed > 0) a->pend = fmaxf(0, a->pend - landed);
        else if (a->pend < 0 && landed < 0) a->pend = fminf(0, a->pend - landed);
        if (fabsf(a->pend * a->px) > 1.0f && fabsf(moved) < 0.1f)
            { if (++a->starve > STARVE) { a->pend = 0; a->starve = 0; } }
        else a->starve = 0;

        if (a->sent && fabsf((float)a->sent) >= MIN_SENT) {
            float eff = moved * s * (float)a->sent;      /* > 0: went where told */
            if (eff > 0.0f && fabsf(moved) > 0.4f) {
                float gain = fabsf(moved) / fabsf((float)a->sent);
                if (gain > 0.01f && gain < 16.0f) {
                    if (!a->valid) { a->px = gain; a->valid = 1; }   /* first sample is truth */
                    else a->px += alpha(dt_ms) * (gain - a->px);
                }
            } else if (eff < 0.0f) {
                a->pend = 0;
                if (!a->valid && a->err * err > 0.0f && fabsf(err) > fabsf(a->err)) {
                    a->wrong_px += fabsf(err) - fabsf(a->err);
                    if (++a->wrong >= 2 && (a->wrong_px > 6.0f || (a->wrong >= 3 && a->wrong_px > 1.0f))) {
                        a->sign = -a->sign; a->wrong = 0; a->wrong_px = 0;
                        a->pend = 0; a->sent = 0; a->err = 0;
                    }
                }
            } else if (isfinite(expect) && fabsf(expect) >= 3.0f &&
                       fabsf(err) > fabsf(a->err) - 0.5f && a->err * err > 0.0f) {
                a->px *= 0.5f;                    /* nothing arrived: seed too optimistic */
            }
        }
    }
    if (!isfinite(a->px)) { a->px = 0.35f; a->valid = 0; }
    if (a->px < PX_MIN) a->px = PX_MIN;
    if (a->px > 16.0f) a->px = 16.0f;

    float want = err - a->pend * a->px * s;        /* correct what will still be wrong */
    float deadzone = fmaxf(a->px * 0.75f, 0.6f);
    if (fabsf(want) < deadzone) return 0;
    float counts = want / a->px * s;
    if (!a->valid) {                               /* bounded probe before the gain is known */
        float probe = fabsf(want) / PX_MAX * PROBE;
        if (fabsf(counts) > probe) counts = counts > 0 ? probe : -probe;
        if (fabsf(counts) < 1.0f) counts = counts > 0 ? 1.0f : -1.0f;
    }
    if (counts > MAX) counts = MAX;
    if (counts < -MAX) counts = -MAX;
    return (int)(counts < 0 ? ceilf(counts) : floorf(counts));
}
```

Measured behaviour (`aimtest`, five plant gains 0.02 / 0.05 / 0.35 / 1.5 / 4.0
px per count, 120 px start error, normal and inverted axis):

| axis | gains | direction flip | frames to settle | final gain |
|---|---|---|---|---|
| normal | all five | never | within 2 | within the plant value |
| inverted | all five | frame 2..3 | within 3 | within the plant value |

Questions:

1. This is a *pure proportional* controller with a gain estimator and a deadband.
   The plant has delay: counts are applied by the game one or two frames after
   `SendInput`, and the client smooths camera rotation. I compensate with `pend`
   (counts in flight, converted back to pixels). Is that compensation sound, or
   is it double-counting the delay once the gain estimate is accurate?
2. The gain estimator writes the first clean sample directly into `px` and then
   exponentially smooths subsequent samples. That means one noisy first sample
   sets the operating point. Would a two-parameter estimator (bounded Kalman on
   `[gain]` with a known input `sent` and output `moved`) be measurably better,
   and would it survive the case where `moved` saturates (camera clamped at the
   view limit)?
3. Direction detection fires only while `!valid`, on two consecutive
   wrong-direction moves totalling > 6 px. Is there a class of game where the
   first *correct* sample arrives before the second wrong one and the axis stays
   inverted forever? What cheap statistic distinguishes "inverted axis" from
   "overshoot past the target"?
4. The deadband is `max(px*0.75, 0.6)` px, i.e. under one count. At low
   sensitivity (`px = 4.0`) that is 3 px of standing error, which is visible.
   Is there a standard formulation that keeps sub-count convergence (dithering
   the remainder across frames) without turning the servo into an oscillator?
5. `counts` is truncated toward zero. Is that the right quantization, or should
   the remainder be carried in `pend` so the error integral is preserved?

### P4 — Two actuators on the same input axis

The view offset modulator (used to characterise how long the client takes to
apply a look delta) and the servo (P3) both write to the same two mouse axes.
Only one may drive a frame.

```c
/* offset, not velocity: `applied` is what is currently on the view, `want` is
 * where the mode is heading; a frame sends only the difference */
static int antiaim_move(int* applied, int want, int limit) {
    int delta = want - *applied;
    if (delta > limit) delta = limit;
    if (delta < -limit) delta = -limit;
    *applied += delta;
    return delta;
}

void rr_antiaim_tick(RR_ANTIAIM_STATE* st, int mode, int speed, int range,
                     unsigned now, int* mx, int* my) {
    /* mode -> want_x/want_y; spin is a bounded sweep that reverses at +-range */
    const int limit = range > 2 ? range : 2;
    *mx = antiaim_move(&st->applied_x, want_x, limit);
    *my = antiaim_move(&st->applied_y, want_y, limit);
}

/* when the mode stops, walk the offset back instead of abandoning it */
int rr_antiaim_return(RR_ANTIAIM_STATE* st, int limit, int* mx, int* my) {
    if (!st->applied_x && !st->applied_y) return 0;
    *mx = antiaim_move(&st->applied_x, 0, limit);
    *my = antiaim_move(&st->applied_y, 0, limit);
    st->phase = 0; st->next_at = 0; st->last_at = 0;
    return *mx || *my;
}
```

The arbitration in the frame loop is: if the servo has a target and is steering,
run the servo; otherwise if the modulator is enabled, tick it; otherwise, if
`applied != 0`, run the return ramp. The return ramp is skipped on frames where
the fire gate is open.

Questions:

1. The return ramp is a linear walk of up to `range` counts. If the user toggles
   the modulator during the walk, the next tick computes `want - applied`, so the
   two can fight. Is a small state machine (`idle -> engage -> hold -> release ->
   idle`) with a single writer the right structure, or is the offset abstraction
   already sufficient?
2. Because both actuators write the same axis, servo telemetry (P3's `moved`
   sample) is contaminated on frames where the modulator moved the view. Today
   the servo simply does not run on those frames, which loses samples. Should the
   servo be told "the plant was disturbed by `d` counts, subtract it" instead of
   being paused?
3. Is a bounded sweep the right primitive at all for latency characterisation, or
   should it be a pseudo-random binary sequence on the offset so the impulse
   response can be recovered by correlation?

### P5 — D2D overlay inside someone else's present call

Everything is drawn inside `Present`/`Present1`, before the real call, using the
swapchain's own back buffer. The swapchain can be recreated at any time.

```cpp
static void on_present(IDXGISwapChain* swap) {
    __try {
        g_swap = swap;
        draw_frame(swap);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log("draw: exception 0x%08lX, overlay disabled", GetExceptionCode());
        InterlockedExchange(&g_draw_on, 0);
    }
    InterlockedIncrement(&g_frames);   /* heartbeat even when the draw bailed */
    maint_tick();
}
static HRESULT STDMETHODCALLTYPE present1_hook(IDXGISwapChain* swap, UINT sync, UINT flags,
                                               const DXGI_PRESENT_PARAMETERS* params) {
    on_present(swap);
    return g_present1_orig(swap, sync, flags, params);
}
static HRESULT STDMETHODCALLTYPE resize_hook(IDXGISwapChain* swap, UINT n, UINT w, UINT h,
                                             DXGI_FORMAT f, UINT flags) {
    drop_target();
    HRESULT hr = g_resize_orig(swap, n, w, h, f, flags);
    make_target();
    return hr;
}
```

Questions:

1. The device and the D2D render target are cached and rebuilt only when the
   back-buffer pointer changes. Is comparing the back-buffer pointer a complete
   device-lost detector for D3D11 flip-model, or must `GetDeviceRemovedReason`
   be consulted as well?
2. If the game presents to a different swapchain (multi-monitor, a second
   window), `g_swap` changes and the cached target is wrong for one frame. Is
   there a cheaper correct guard than "target belongs to this swapchain pointer"?
3. Drawing before the real `Present` means the overlay is inside the frame's
   command buffer. Is there an ordering hazard with `Present1`'s dirty-rect
   parameters when the overlay covers the whole viewport but the game only
   passed a partial dirty region?
4. The `__try/__except` around the whole draw is a blunt instrument: it disables
   the overlay permanently on the first fault. For a debugging tool that is what
   I want, but is there a way to keep the overlay alive for a fault that is
   known-benign (a bad projection from a partially-updated entity) without
   masking a real bug?

### P6 — Manual mapping: what the loader would have done for me

The module is mapped by hand: parse headers, copy sections, apply relocations,
resolve imports, set protections, call the entry point. Nothing else about the
PE is applied. That is where the bugs live.

Already found and fixed (both were silent, not crashes):

- **Exception directory.** The module contains `__try/__except` and C++ destructors.
  Without a function table registered for the mapped base, every handler is dead
  and every exception escapes into the game. Fix: `RtlAddFunctionTable` with the
  mapped base, called before any guarded code runs.
- **CRT per-thread state.** The thread that reaches the entry point was not
  created by the loader, so it has no `_tiddata`. The first stdio call faults.
  Fix: call `__vcrt_thread_attach()` and `__acrt_thread_attach()` at the top of
  the entry point.

What I am unsure about:

1. TLS: the image has no `IMAGE_DIRECTORY_ENTRY_TLS` by construction (I stripped
   it). If a future dependency adds thread-local storage, what is the minimal
   correct initialisation, given that no `DLL_THREAD_ATTACH` will ever arrive for
   existing threads?
2. Load-config / security cookie: with no loader the `/GS` cookie is whatever is
   in the image's `.data`. `__security_init_cookie` exists but calling it from a
   hand-mapped image is itself a stack cookie user. Is there a supported order:
   cookie, then CRT, then code with buffers?
3. Imports: I resolve by name against the target's loaded modules. For
   delay-loaded imports and for `api-set` forwards, is a name-only walk over
   `InLoadOrderModuleList` sufficient, or do I need the `api-ms-win-*` host
   mapping?
4. `DllMain` is called manually with `DLL_PROCESS_ATTACH`. Is calling
   `DisableThreadLibraryCalls` first — as the loader would — the correct way to
   keep it from registering an attach it will never receive?
5. Section protections: I set them from the section characteristics before
   relocations and imports, then restore after. Is there a case (a `.text`
   section that must be writable while relocating) where the final protection
   should differ from the header's?

### P7 — A staging script that must never leave the machine altered

Two of the modules are self-signed and the machine's CI policy refuses them. The
staging script borrows a signed vendor driver that can write 4 bytes anywhere in
kernel space, turns policy enforcement off, starts the pair, writes the original
flags back byte for byte, then proves enforcement is back and unloads.

```powershell
function Restore-CiOptions {
    if (-not $script:byovd_dirty -or -not $script:byovd_open) { return }
    [int]$e = 0
    $null = [RtcIo]::Write($script:byovd_handle, $script:byovd_addr, $script:byovd_stock, [ref]$e)
    $script:byovd_dirty = $false
}

function Clear-Byovd {
    # reverse order of creation; a cleanup failure must not hide the real error
    try { Restore-CiOptions } catch {}
    if ($script:byovd_open) { try { [RtcIo]::Close($script:byovd_handle) } catch {} ; $script:byovd_open = $false }
    try { Invoke-Sc 'stop' $ServiceName | Out-Null; Invoke-Sc 'delete' $ServiceName | Out-Null } catch {}
    if ($script:byovd_sys) { try { Remove-Item -LiteralPath $script:byovd_sys -Force -ErrorAction Stop } catch {} ; $script:byovd_sys = $null }
}

function Fail([int]$code, [string]$msg) { Write-Host "[byovd] FAIL($code): $msg"; Clear-Byovd; exit $code }
```

The window I care about is between "flags zeroed" and "flags restored": if the
process is killed there (or the machine loses power), the machine is left with
signature enforcement off, which is a security state I would not want to leave
behind even on my own box.

Questions:

1. Beyond a journal file plus a `--restore` mode on next run, is there a
   structure that makes the window provably short — e.g. do the whole thing in
   one IOCTL? Is there a documented way to reload CI policy from the registry
   without a reboot?
2. `Restore-CiOptions` guards on `dirty`/`open`, and `Clear-Byovd` is idempotent.
   Is there a crash-consistency hole between "write(-ci)" and "dirty = false" —
   i.e. write succeeded but the assignment did not run? Which order is safer?
3. Proving enforcement came back: I try to start a known-unsigned image and
   require failure with 577. Is that the right proof, or is reading `g_CiOptions`
   back and comparing to the saved value stronger?
4. The stock value is read, not assumed (`0x16` on a stock boot; a test-signing
   boot has `0x100001E`). Is there a case where the *live* value legitimately
   differs from the value that should be restored (a third-party driver that
   already toggled a bit), and if so, what should be preserved?

### P8 — Menu input while the game holds the cursor

The game clips and hides the cursor during play. To make the overlay panel
clickable I release the clip each frame while the panel is open and read the OS
cursor position, then convert to client coordinates.

```cpp
if (key_rep(g_cfg.menu_key, &k_menu)) {
    g_menu_open = !g_menu_open;
    g_menu_rebind = 0;
    g_menu_hover = -1;
    /* the click that opened the panel must not also click a row */
    g_menu_lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
}
if (!g_menu_open) return;
ClipCursor(NULL);
/* tab bar first, then rows: a click on a tab must not fall through to the row
 * under the pointer */
```

Questions:

1. `ClipCursor(NULL)` every frame races with the game's own clip call. Is there a
   correct pattern (clip to the client rect instead of releasing, or re-entering
   the clip on close) that does not flicker the cursor position while the panel
   is open?
2. Key rebinding waits for the first `GetAsyncKeyState(vk) & 1` and ignores
   mouse buttons for 300 ms after the initiating click. Is a time window the right
   guard, or should the bind state hold the physical button mask it started with?
3. The panel is drawn with the same D2D context as the overlay. If the game's own
   UI is under the panel and the panel is semi-transparent, is there a
   compositing approach (draw to an offscreen target once per layout change) that
   cuts per-frame text layout cost without adding another device?

---

## 4. Functional backlog (things that do not work yet)

Not questions — just so you know what is already known to be broken:

- **Entity enumeration cost.** Per frame, the projection layer walks the full
  player list and rebinds class fields by name once per session. On a hub scene
  with many actors this is the frame's dominant cost. I have not profiled which
  part (name lookup vs. transform read) dominates.
- **Label layout determinism.** Screen-space labels are placed by a greedy pass
  with a hash-keyed slot cache. Two identical scenes at different frame rates
  produce different placements because the cache is keyed by a string hash and
  evicts on collision. Deterministic ordering needs a stable key.
- **Occlusion test cost.** Ray tests are per-entity per-frame against colliders;
  a cached last-known-visibility with a re-test interval would cut it.
- **Container interaction automation** exists for pickup but not for the sell
  flow: the client's sell entry points resolve to null in the class metadata I
  read, so the call cannot be constructed safely.
- **Recoil compensation** is a fixed pattern applied at a fixed rate. It is not
  conditioned on weapon state, so it overshoots on weapons with a different
  kick curve.
- **First-run cost.** The staging path resolves a kernel symbol by scanning for a
  signature; on some boots it needs two attempts. No retry policy beyond one.

---

## 5. What I am asking for

For each problem:

1. **Is the design sound?** If not, name the invariant it violates.
2. **What is the correct minimal structure?** A named pattern, an ordering, or
   the specific API contract I am missing.
3. **How would you test it** without a debugger attached to the target process
   (the client terminates debuggers)? I test offline with a simulated plant; a
   better test design is welcome.
4. **What is the failure mode that will bite me next** in this component?

I would rather read a short, specific list of "here is the correct ordering and
why" than a general survey. Code examples in C or C++ are welcome; pseudocode is
fine.

---

## 6. Ordered list of the questions, if you answer in one pass

| # | component | the single question |
|---|---|---|
| 1 | P1 | Is a per-candidate `VirtualProtect` + 8-byte store still a race, and what should the sweep skip? |
| 2 | P1 | Does `in_module(ptr, dxgi)` correctly identify a real `Present1` on Win10/11? |
| 3 | P1 | Must the pointer sweep re-run on swapchain recreation, and what is the signal? |
| 4 | P2 | Whole-batch rejection or binary split on the first failed item? |
| 5 | P2 | Bounded-depth batch split with the same no-progress guard? |
| 6 | P3 | Is the in-flight (`pend`) compensation sound with a known delay, or double-counting? |
| 7 | P3 | Kalman on gain vs. first-sample-plus-EMA — measurable difference? |
| 8 | P3 | Sub-count convergence without oscillation: carry the remainder or dither? |
| 9 | P4 | Single-writer state machine vs. offset arithmetic for two actuators on one axis? |
| 10 | P4 | Feed servo disturbances forward instead of pausing the servo? |
| 11 | P5 | Complete device-lost detection for flip-model from inside `Present`? |
| 12 | P5 | Dirty-rect hazard when the overlay fills the viewport? |
| 13 | P6 | Correct initialisation order for a hand-mapped image: cookie, CRT, TLS, then code? |
| 14 | P6 | Minimal `DllMain` contract when the loader never runs? |
| 15 | P7 | How to make the "policy disabled" window as short as possible, and crash-consistent? |
| 16 | P7 | What is the strongest proof that the policy is back? |
| 17 | P8 | Correct cursor-clip pattern for a clickable overlay over a game that clips it? |
| 18 | P8 | Offscreen compositing for the panel: worth it, and where does it break? |
