/* rr_dupe.c - Slot watcher/hold and input burst driver.
 *
 * Both halves of a quantity race are time-domain problems: the client has to
 * still believe it owns the stack when the second command is issued, and the
 * two commands have to reach the server close enough together that they are
 * validated against the same pre-state. This file instruments the first
 * (watch/hold) and drives the second (burst), and prints a clock that the two
 * logs can be lined up on.
 */
#include "rr_dupe.h"
#include "rr_mem.h"

#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <string.h>

/* timeBeginPeriod(1) makes Sleep(1) land near a millisecond instead of the
 * 15.6 ms scheduler default, which is the whole point of a race window. */
#pragma comment(lib, "winmm.lib")

static unsigned dupe_clampu(unsigned v, unsigned lo, unsigned hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ---- watch ------------------------------------------------------------- */

int rr_dupe_watch(const char* exe, const RR_DUPE_WATCH* cfg, RR_DUPE_LOG log) {
    if (!log) return 6;
    if (!cfg || cfg->addr < 0x10000) {
        log("[watch] 0x%llX is not a user-mode address\n",
            (unsigned long long)(cfg ? cfg->addr : 0));
        return 4;
    }
    if (!rr_attach(exe && *exe ? exe : "RobRun.exe")) {
        log("[watch] '%s' is not running\n", exe && *exe ? exe : "RobRun.exe");
        return 2;
    }
    if (!rr_drv_open()) { log("[watch] kernel backend unavailable\n"); return 3; }

    const uint64_t addr = cfg->addr;
    const unsigned interval = dupe_clampu(cfg->interval_ms, 1, 50);
    const unsigned duration = dupe_clampu(cfg->duration_ms, 100, 600000);
    const unsigned corr_ms  = dupe_clampu(cfg->correction_ms, 10, 2000);

    unsigned samples = 0, fails = 0, transitions = 0, ups = 0, downs = 0;
    unsigned corrections = 0, writes = 0, write_fails = 0, holds = 0;
    unsigned lat[16];
    unsigned nlat = 0;
    uint32_t first = 0, last = 0, vmin = 0, vmax = 0;
    int have = 0;
    uint32_t held = 0;
    ULONGLONG last_up_at = 0;
    int have_up = 0;

    log("[watch] addr=0x%llX interval=%ums duration=%ums hold=%d correction=%ums\n",
        (unsigned long long)addr, interval, duration, cfg->hold ? 1 : 0, corr_ms);

    timeBeginPeriod(1);
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG elapsed = now - start;
        if (elapsed >= duration) break;

        const uint32_t v = rr_u32(addr);
        if (!rr_ok()) {
            /* A failed read is not a zero: it must never be treated as a
             * transition, a hold source, or a write. */
            ++fails;
            Sleep(interval);
            continue;
        }
        ++samples;

        if (!have) {
            have = 1;
            first = last = vmin = vmax = v;
            held = v;
            log("[t=+%lums] base=0x%08X (%d)\n", (unsigned long)elapsed, v, (int)v);
        } else if (cfg->hold) {
            if (v != held) {
                ++holds;
                log("[t=+%lums] foreign=0x%08X kept=0x%08X (%+d)\n",
                    (unsigned long)elapsed, v, held, (int)v - (int)held);
                const uint32_t keep = held;
                if (rr_write(addr, &keep, sizeof keep)) ++writes;
                else ++write_fails;
            }
        } else if (v != last) {
            const int delta = (int)v - (int)last;
            ++transitions;
            if (delta > 0) {
                ++ups;
                last_up_at = now;
                have_up = 1;
            } else {
                ++downs;
                if (have_up) {
                    const ULONGLONG d = now - last_up_at;
                    if (d <= corr_ms) {
                        ++corrections;
                        if (nlat < 16) lat[nlat++] = (unsigned)d;
                        log("[t=+%lums] correction %+d after %lums\n",
                            (unsigned long)elapsed, delta, (unsigned long)d);
                    }
                    have_up = 0;
                }
            }
            log("[t=+%lums] 0x%08X -> 0x%08X (%+d)\n",
                (unsigned long)elapsed, last, v, delta);
            last = v;
            if (v > vmax) vmax = v;
            if (v < vmin) vmin = v;
        }
        Sleep(interval);
    }
    timeEndPeriod(1);

    if (!have) {
        log("[watch] no readable sample in %ums\n", duration);
        return 5;
    }

    log("[watch] samples=%u fails=%u transitions=%u up=%u down=%u corrections=%u\n",
        samples, fails, transitions, ups, downs, corrections);
    if (cfg->hold)
        log("[watch] held=0x%08X blocked=%u writes=%u write_fails=%u\n",
            held, holds, writes, write_fails);
    else
        log("[watch] first=0x%08X last=0x%08X net=%+d min=0x%08X max=0x%08X\n",
            first, last, (int)last - (int)first, vmin, vmax);
    if (nlat) {
        unsigned sum = 0;
        for (unsigned i = 0; i < nlat; ++i) sum += lat[i];
        log("[watch] correction latency:");
        for (unsigned i = 0; i < nlat; ++i) log(" %ums", lat[i]);
        log(" (mean %ums)\n", sum / nlat);
    }
    return 0;
}

/* ---- burst ------------------------------------------------------------- */

static int dupe_move_abs(int x, int y) {
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw < 2) vw = 2;
    if (vh < 2) vh = 2;
    /* SendInput wants 0..65535 across the desktop, not pixels. Clamp inside
     * the virtual screen so a stale coordinate cannot wrap to the far edge. */
    if (x < vx) x = vx;
    if (y < vy) y = vy;
    if (x > vx + vw - 1) x = vx + vw - 1;
    if (y > vy + vh - 1) y = vy + vh - 1;

    INPUT in;
    memset(&in, 0, sizeof in);
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    in.mi.dx = (LONG)(((long long)(x - vx) * 65535LL) / (long long)(vw - 1));
    in.mi.dy = (LONG)(((long long)(y - vy) * 65535LL) / (long long)(vh - 1));
    return SendInput(1, &in, sizeof in) == 1;
}

static int dupe_button(int down) {
    INPUT in;
    memset(&in, 0, sizeof in);
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    return SendInput(1, &in, sizeof in) == 1;
}

static int dupe_click(void) {
    INPUT click[2];
    memset(click, 0, sizeof click);
    click[0].type = click[1].type = INPUT_MOUSE;
    click[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    click[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    /* A partial insertion must not leave the synthetic button held. */
    if (SendInput(2, click, sizeof(INPUT)) == 1) SendInput(1, &click[1], sizeof(INPUT));
    return 1;
}

int rr_dupe_burst(const char* exe, const RR_DUPE_BURST* cfg, RR_DUPE_LOG log) {
    static const char* const k_name[] = { "double", "split", "drag" };
    if (!log) return 6;
    if (!cfg || cfg->repeat <= 0 || (cfg->x <= 0 && cfg->y <= 0)) {
        log("[burst] nothing to do: <double|split|drag> <x> <y> [x2 y2] [repeat] [step_ms] [cycle_ms]\n");
        return 4;
    }
    if (exe && *exe && !rr_attach(exe)) {
        log("[burst] '%s' is not running\n", exe);
        return 2;
    }

    const int pattern = cfg->pattern >= 0 && cfg->pattern <= RR_DUPE_PATTERN_DRAG
                      ? cfg->pattern : RR_DUPE_PATTERN_DOUBLE;
    const int repeat = cfg->repeat > 5000 ? 5000 : cfg->repeat;
    const unsigned step  = dupe_clampu((unsigned)(cfg->step_ms < 0 ? 0 : cfg->step_ms), 0, 1000);
    const unsigned cycle = dupe_clampu((unsigned)(cfg->cycle_ms < 0 ? 0 : cfg->cycle_ms), 0, 5000);

    log("[burst] pattern=%s from=(%d,%d) to=(%d,%d) repeat=%d step=%ums cycle=%ums\n",
        k_name[pattern], cfg->x, cfg->y, cfg->x2, cfg->y2, repeat, step, cycle);

    unsigned sends_failed = 0, cycles = 0, gaps = 0;
    unsigned gap_min = 0, gap_max = 0;
    unsigned long long gap_sum = 0;
    const ULONGLONG start = GetTickCount64();
    ULONGLONG prev = 0;

    for (int i = 0; i < repeat; ++i) {
        const ULONGLONG t0 = GetTickCount64();
        if (prev) {
            const unsigned g = (unsigned)(t0 - prev);
            gap_sum += g;
            if (!gaps || g < gap_min) gap_min = g;
            if (g > gap_max) gap_max = g;
            ++gaps;
        }
        prev = t0;
        log("[burst] cycle %d at +%lums\n", i + 1, (unsigned long)(t0 - start));

        switch (pattern) {
        case RR_DUPE_PATTERN_SPLIT:
            /* Select the source stack, then confirm the destination. Two
             * clicks per cycle is the shape of a split dialog confirm. */
            if (!dupe_move_abs(cfg->x, cfg->y)) ++sends_failed;
            Sleep(step);
            if (!dupe_click()) ++sends_failed;
            Sleep(step);
            if (!dupe_move_abs(cfg->x2, cfg->y2)) ++sends_failed;
            Sleep(step);
            if (!dupe_click()) ++sends_failed;
            break;
        case RR_DUPE_PATTERN_DRAG:
            /* Press on the source, travel in interpolated steps so the UI's
             * drag threshold is crossed by real motion, release on target. */
            if (!dupe_move_abs(cfg->x, cfg->y)) ++sends_failed;
            Sleep(step);
            if (!dupe_button(1)) ++sends_failed;
            for (int k = 1; k <= 6; ++k) {
                const int mx = cfg->x + (cfg->x2 - cfg->x) * k / 6;
                const int my = cfg->y + (cfg->y2 - cfg->y) * k / 6;
                if (!dupe_move_abs(mx, my)) ++sends_failed;
                Sleep(step ? step / 6 + 1 : 1);
            }
            if (!dupe_button(0)) ++sends_failed;
            break;
        default:
            /* Two clicks on the same point: pickup double-click, or a
             * repeated confirm on one button. */
            if (!dupe_move_abs(cfg->x, cfg->y)) ++sends_failed;
            Sleep(step);
            if (!dupe_click()) ++sends_failed;
            Sleep(step);
            if (!dupe_click()) ++sends_failed;
            break;
        }
        ++cycles;
        if (cycle) Sleep(cycle);
    }

    /* A failed drag release leaves the desktop holding the left button. */
    dupe_button(0);

    log("[burst] cycles=%u elapsed=%lums send_fails=%u\n",
        cycles, (unsigned long)(GetTickCount64() - start), sends_failed);
    if (gaps)
        log("[burst] cycle gap mean=%llums min=%ums max=%ums\n",
            gap_sum / gaps, gap_min, gap_max);
    return 0;
}
