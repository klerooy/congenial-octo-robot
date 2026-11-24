/* rr_antiaim.h - View perturbation for the input worker.
 *
 * This is a client-side view scrambler, not a server-side desync: it moves the
 * mouse so the local view sweeps, jitters or snaps. It cannot change what the
 * server believes the character is facing. Modes are bounded per frame so a
 * stalled worker cannot fling the view.
 *
 * Every mode works in offsets from the centre, and the state remembers the
 * offset that was actually sent (`applied`). The input worker moves towards the
 * wanted offset and, when the mode stops, back to zero: a mode that is switched
 * off, gated by a menu or interrupted by a shot puts the view where it found it
 * instead of leaving the crosshair parked to the side for the rest of the match.
 */
#ifndef RR_ANTIAIM_H
#define RR_ANTIAIM_H

#ifdef __cplusplus
extern "C" {
#endif

enum { RR_ANTIAIM_SPIN = 0, RR_ANTIAIM_JITTER = 1, RR_ANTIAIM_STATIC = 2,
       RR_ANTIAIM_RANDOM = 3 };

typedef struct {
    unsigned last_at;      /* previous tick                              */
    unsigned next_at;      /* next scheduled change                      */
    unsigned seed;         /* random-mode generator state                */
    int      dir;          /* sweep direction / jitter side              */
    int      applied_x;    /* counts sent and not yet returned           */
    int      applied_y;
    int      want_x;       /* offset the current mode holds              */
    int      want_y;
    int      tx, ty;       /* random-mode target offsets                 */
    float    phase;        /* spin sweep position, in counts             */
} RR_ANTIAIM_STATE;

void rr_antiaim_reset(RR_ANTIAIM_STATE* state);

/* One worker frame of the active mode. The move is the difference between the
 * wanted offset and the offset already applied, so re-entering a mode, changing
 * its range or changing its speed never re-applies an offset that is already in
 * place. */
void rr_antiaim_tick(RR_ANTIAIM_STATE* state, int mode, int speed, int range,
                     unsigned now, int* mx, int* my);

/* Puts the view back where the mode found it, `limit` counts per call. Returns
 * non-zero while there is still an offset to return, then stays at zero. */
int rr_antiaim_return(RR_ANTIAIM_STATE* state, int limit, int* mx, int* my);

#ifdef __cplusplus
}
#endif
#endif
