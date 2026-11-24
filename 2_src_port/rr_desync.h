/* rr_desync.h - model anti-aim planner.
 *
 * The view anti-aim in rr_antiaim.h only moves the local crosshair. This module
 * plans the angle that is written into the character's replicated transform, so
 * the facing other clients predict against is not the facing the shots come
 * from. Pure arithmetic: no memory access, so the host checks can drive it.
 *
 * Angles are degrees in (-180, 180]. The caller converts to radians (the
 * model field is a normal angle) and writes it.
 */
#ifndef RR_DESYNC_H
#define RR_DESYNC_H

#include <math.h>

typedef struct {
    unsigned next_ms;      /* earliest time the next step may run            */
    unsigned seed;         /* deterministic random source                    */
    int      started;      /* a step has been published at least once        */
    int      phase;        /* jitter / stance alternation                    */
    float    offset_deg;   /* last published offset, degrees                 */
} RR_DESYNC_STATE;

enum { RR_DESYNC_SPIN = 0, RR_DESYNC_JITTER = 1, RR_DESYNC_STATIC = 2,
       RR_DESYNC_RANDOM = 3 };

/* Wrap any angle into (-180, 180]. A NaN or infinity becomes zero so a broken
 * sample can never reach the model field. */
static inline float rr_desync_wrap_deg(float deg) {
    if (!(deg > -1.0e9f) || !(deg < 1.0e9f)) return 0.0f;
    deg = (float)fmod((double)deg + 180.0, 360.0);
    if (deg < 0.0f) deg += 360.0f;
    return deg - 180.0f;
}

static inline void rr_desync_reset(RR_DESYNC_STATE* state, unsigned seed) {
    if (!state) return;
    state->next_ms = 0;
    state->seed = seed ? seed : 0x9E3779B9u;
    state->started = 0;
    state->phase = 0;
    state->offset_deg = 0.0f;
}

/* Speed is a percentage of the 30 Hz ceiling: 1 -> 1 Hz, 100 -> 30 Hz. Spin
 * advances a fixed 45 degrees per step, so the rate sets how fast the facing
 * rotates; the other modes republish an offset at that rate. Returns 1 when a
 * new offset was published. */
static inline int rr_desync_step(RR_DESYNC_STATE* state, int mode, int speed,
                                 int range_deg, unsigned now_ms) {
    if (!state) return 0;
    if (speed < 1) speed = 1;
    if (speed > 100) speed = 100;
    if (range_deg < 1) range_deg = 1;
    if (range_deg > 180) range_deg = 180;
    unsigned hz = 1u + (unsigned)speed * 29u / 100u;
    unsigned interval = 1000u / hz;
    if (interval < 1u) interval = 1u;
    if (state->started && (int)(now_ms - state->next_ms) < 0) return 0;
    state->started = 1;
    state->next_ms = now_ms + interval;
    state->phase ^= 1;
    float range = (float)range_deg;
    switch (mode) {
    case RR_DESYNC_SPIN:
        state->offset_deg = rr_desync_wrap_deg(state->offset_deg + 45.0f);
        break;
    case RR_DESYNC_JITTER:
        state->offset_deg = state->phase ? range : -range;
        break;
    case RR_DESYNC_STATIC:
        state->offset_deg = range;
        break;
    default:
        state->seed = state->seed * 1664525u + 1013904223u;
        state->offset_deg = ((float)(state->seed >> 8) / 8388608.0f - 1.0f) * range;
        break;
    }
    return 1;
}

#endif
