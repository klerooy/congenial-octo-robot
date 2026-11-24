/* rr_dupe.h - Duplication instrumentation: slot watcher and input burst driver.
 *
 * Slot contents live in a packed InventorySlot int (InventorySlotStackModel's
 * inline _data at +0x10). Nothing here writes that int except the hold mode,
 * and a held value is a client-side prediction only - the next replicated
 * snapshot carries the server's copy. What the two modes deliver is evidence:
 *
 *   watch  - every transition of one packed slot value, timestamped, with a
 *            correction counter (a rise followed by a fall inside a short
 *            window is the server overwriting a client prediction).
 *   hold   - the same poll, rewriting the first good value back into the slot
 *            so the client keeps predicting a non-empty stack while the
 *            server-side copy has already moved. This is what lets a second
 *            take command be issued at all: the client refuses to draw from an
 *            empty predicted stack.
 *   burst  - a bounded click/move replay for the input-timed side of a race
 *            (pickup double-click, split confirm, drag-drop), with the cycle
 *            spacing logged so it can be matched against the watcher's clock.
 *
 * Both modes write nothing into the game's message layer: this overlay has no
 * send-path address yet, so the only levers available are the predicted state
 * and the input timeline.
 */
#ifndef RR_DUPE_H
#define RR_DUPE_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*RR_DUPE_LOG)(const char* fmt, ...);

enum { RR_DUPE_PATTERN_DOUBLE = 0, RR_DUPE_PATTERN_SPLIT = 1, RR_DUPE_PATTERN_DRAG = 2 };

typedef struct {
    uint64_t addr;          /* packed slot int (InventorySlotStackModel + 0x10) */
    unsigned duration_ms;   /* clamp 100..600000                          */
    unsigned interval_ms;   /* poll period, clamp 1..50                   */
    int      hold;          /* 1 = rewrite the first good value back      */
    unsigned correction_ms; /* up->down inside this window = correction   */
} RR_DUPE_WATCH;

typedef struct {
    int pattern;            /* RR_DUPE_PATTERN_*                          */
    int x, y;               /* primary screen point                       */
    int x2, y2;             /* secondary point (split/drag destination)   */
    int repeat;             /* cycles, clamp 1..5000                      */
    int step_ms;            /* delay inside a cycle, clamp 0..1000        */
    int cycle_ms;           /* delay between cycles, clamp 0..5000        */
} RR_DUPE_BURST;

/* Attach read/write access and poll cfg->addr for cfg->duration_ms.
 * Return 0 on completion, 2 no process, 3 no driver, 4 bad address, 6 bad log. */
int rr_dupe_watch(const char* exe, const RR_DUPE_WATCH* cfg, RR_DUPE_LOG log);

/* Replay cfg->pattern cfg->repeat times through SendInput.
 * Return 0 on completion, 2 no process, 4 nothing to do, 6 bad log. */
int rr_dupe_burst(const char* exe, const RR_DUPE_BURST* cfg, RR_DUPE_LOG log);

#ifdef __cplusplus
}
#endif

#endif /* RR_DUPE_H */
