/* rr_antiaim.c - Bounded view perturbation modes.
 *
 * spin   : sweep between -range and +range, direction reversing at the edges
 * jitter : alternate between -range and +range at the speed intervals
 * static : hold +range
 * random : random walk between targets inside the range
 *
 * Every mode is an offset, not a velocity. The state keeps the offset actually
 * applied, and each frame sends only the difference towards the wanted offset,
 * bounded by the range. That single rule is what makes the modes composable:
 *
 * - enabling anti-aim does not add a step on top of a step already applied;
 * - dragging the range or speed slider moves the view to the new offset instead
 *   of re-applying the old one;
 * - a mode that stops (key released, menu open, aim steering, a shot) no longer
 *   leaves the crosshair parked to the side, because rr_antiaim_return walks the
 *   offset back to zero.
 *
 * The previous version kept no applied offset: the state was wiped on every
 * gated frame, so the next frame re-measured the wanted offset from zero and
 * threw the view again. That reads as "as soon as I touch it, it starts jerking".
 */
#include "rr_antiaim.h"
#include <math.h>
#include <string.h>

void rr_antiaim_reset(RR_ANTIAIM_STATE* state) {
    if (!state) return;
    memset(state, 0, sizeof(*state));
}

static int antiaim_clamp(int v, int limit) {
    if (v > limit) return limit;
    if (v < -limit) return -limit;
    return v;
}

/* Move one axis towards `want`, at most `limit` counts, and remember where it
 * ended up. Returns the counts to send. */
static int antiaim_move(int* applied, int want, int limit) {
    int delta = want - *applied;
    delta = antiaim_clamp(delta, limit);
    *applied += delta;
    return delta;
}

static int antiaim_int(float v) {
    if (!isfinite(v)) return 0;
    return (int)(v < 0.0f ? ceilf(v) : floorf(v));
}

void rr_antiaim_tick(RR_ANTIAIM_STATE* state, int mode, int speed, int range,
                     unsigned now, int* mx, int* my) {
    if (!mx || !my) return;
    *mx = *my = 0;
    if (!state) return;

    if (range < 1) range = 1;
    if (range > 60) range = 60;
    if (speed < 1) speed = 1;
    if (speed > 100) speed = 100;

    unsigned dt = state->last_at ? now - state->last_at : 16;
    if (dt > 100) dt = 100;
    if (!dt) dt = 1;
    state->last_at = now;
    state->seed = state->seed * 1664525u + 1013904223u;

    int want_x = 0, want_y = 0;
    switch (mode) {
    case RR_ANTIAIM_SPIN: {
        /* A bounded sweep. The old spin added up forever: it clamped each frame
         * to the range but the sum kept growing, so the view panned away at a
         * fixed rate and never came back. Reversing at the edges keeps the same
         * moving view without rotating the player off the world. */
        float rate = (float)range * (0.2f + 0.8f * (float)speed / 100.0f) *
                     (float)dt / 16.0f;
        if (state->dir < 0) rate = -rate;
        state->phase += rate;
        if (state->phase > (float)range) { state->phase = (float)range; state->dir = -1; }
        if (state->phase < -(float)range) { state->phase = -(float)range; state->dir = 1; }
        want_x = antiaim_int(state->phase);
        break;
    }
    case RR_ANTIAIM_STATIC:
        want_x = range;
        break;
    case RR_ANTIAIM_JITTER: {
        unsigned interval = 520u - (unsigned)(speed * 4u);
        if (!state->next_at || now >= state->next_at) {
            state->next_at = now + interval;
            state->dir = state->dir >= 0 ? -1 : 1;
        }
        want_x = state->dir * range;
        break;
    }
    case RR_ANTIAIM_RANDOM: {
        unsigned interval = 420u - (unsigned)(speed * 3u);
        if (!state->next_at || now >= state->next_at) {
            state->next_at = now + interval;
            unsigned r = state->seed;
            state->tx = (int)(r % (unsigned)(2 * range + 1)) - range;
            r = r * 1103515245u + 12345u;
            state->ty = ((int)(r % (unsigned)(2 * range + 1)) - range) / 2;
        }
        want_x = state->tx;
        want_y = state->ty;
        break;
    }
    default:
        return;
    }

    state->want_x = want_x;
    state->want_y = want_y;
    /* A stalled worker (a 300 ms hitch) must not turn the sweep into one huge
     * jump: the per-frame move is bounded by the range. */
    const int limit = range > 2 ? range : 2;
    *mx = antiaim_move(&state->applied_x, want_x, limit);
    *my = antiaim_move(&state->applied_y, want_y, limit);
}

int rr_antiaim_return(RR_ANTIAIM_STATE* state, int limit, int* mx, int* my) {
    if (!mx || !my) return 0;
    *mx = *my = 0;
    if (!state) return 0;
    if (!state->applied_x && !state->applied_y) return 0;
    if (limit < 1) limit = 1;
    if (limit > 60) limit = 60;
    *mx = antiaim_move(&state->applied_x, 0, limit);
    *my = antiaim_move(&state->applied_y, 0, limit);
    /* The mode is over, so the next tick starts from the centre and its own
     * interval instead of the leftover phase of the last one. */
    state->phase = 0.0f;
    state->want_x = state->want_y = 0;
    state->next_at = 0;
    state->last_at = 0;
    return *mx || *my;
}
