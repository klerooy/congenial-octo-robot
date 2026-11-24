/* rr_antiaim.h - compatibility stub.
 *
 * Anti-aim was removed from the aim worker. The panel and the config still
 * carry its fields, so the names stay defined here and the entry points become
 * no-ops. Nothing in this header moves the view any more. */
#ifndef RR_ANTIAIM_H
#define RR_ANTIAIM_H
#include "rr_game.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    RR_ANTIAIM_STATIC = 0,
    RR_ANTIAIM_JITTER = 1,
    RR_ANTIAIM_SPIN   = 2,
    RR_ANTIAIM_RANDOM = 3
};

typedef struct {
    float off_x, off_y;
    unsigned last_at;
    int   mode;
    int   phase;
} RR_ANTIAIM_STATE;

static __inline void rr_antiaim_reset(RR_ANTIAIM_STATE* s) {
    if (s) { s->off_x = 0.0f; s->off_y = 0.0f; s->last_at = 0; s->mode = 0; s->phase = 0; }
}

static __inline int rr_antiaim_tick(RR_ANTIAIM_STATE* s, int mode, int speed,
                                    int range, unsigned now, int* mx, int* my) {
    (void)s; (void)mode; (void)speed; (void)range; (void)now;
    if (mx) *mx = 0;
    if (my) *my = 0;
    return 0;
}

static __inline int rr_antiaim_return(RR_ANTIAIM_STATE* s, int limit,
                                      int* mx, int* my) {
    (void)s; (void)limit;
    if (mx) *mx = 0;
    if (my) *my = 0;
    return 0;
}

#ifdef __cplusplus
}
#endif
#endif