/* rr_recoil.c - Shot-pattern and camera-pitch recoil compensation.
 *
 * Pattern values are per-shot mouse counts at the reference calibration
 * (aa_aim_cal = 100) and are drained over a short window so the correction
 * follows the burst instead of arriving as one jerk. Camera compensation
 * compares the pitch the view actually gained with the pitch the observed
 * mouse movement should have produced; the difference is treated as recoil.
 */
#include "rr_recoil.h"
#include <ctype.h>
#include <math.h>
#include <string.h>

typedef struct { float x, y; } RP;

/* Positive y pulls the view down. Tune with strength, not by editing these
 * unless a weapon's class is wrong: rr_recoil_class() routes the name. */
static const RP k_pistol[] = {
    { 0.0f, 1.6f }, { 0.0f, 1.7f }, { 0.3f, 1.8f }, { -0.3f, 1.8f }, { 0.4f, 1.9f },
    { -0.4f, 1.9f }, { 0.5f, 2.0f }, { -0.5f, 2.0f }, { 0.6f, 2.0f }, { -0.6f, 2.1f }
};
static const RP k_rifle[] = {
    { 0.0f, 3.0f }, { 0.0f, 3.3f }, { 0.4f, 3.5f }, { -0.6f, 3.7f }, { -1.0f, 3.9f },
    { 0.8f, 4.0f }, { 1.2f, 4.1f }, { 1.5f, 4.2f }, { 1.6f, 4.3f }, { -1.4f, 4.4f },
    { -1.8f, 4.5f }, { -2.0f, 4.5f }, { -2.2f, 4.6f }, { 1.8f, 4.6f }, { 2.0f, 4.7f },
    { 2.2f, 4.7f }, { 2.3f, 4.8f }, { -2.0f, 4.8f }, { -2.2f, 4.8f }, { -2.4f, 4.9f },
    { 2.2f, 4.9f }, { 2.4f, 5.0f }, { 2.5f, 5.0f }, { -2.4f, 5.0f }, { -2.6f, 5.1f }
};
static const RP k_smg[] = {
    { 0.0f, 2.0f }, { 0.5f, 2.2f }, { -0.7f, 2.3f }, { 0.9f, 2.4f }, { -1.1f, 2.5f },
    { 1.2f, 2.6f }, { -1.3f, 2.6f }, { 1.4f, 2.7f }, { -1.5f, 2.7f }, { 1.6f, 2.8f },
    { -1.6f, 2.8f }, { 1.7f, 2.9f }, { -1.7f, 2.9f }, { 1.8f, 3.0f }, { -1.8f, 3.0f }
};
static const RP k_shotgun[] = {
    { 0.0f, 9.0f }, { 0.0f, 3.0f }, { 0.0f, 2.0f }, { 0.0f, 1.5f }
};
static const RP k_sniper[] = {
    { 0.0f, 16.0f }, { 0.0f, 2.0f }
};
static const RP k_other[] = {
    { 0.0f, 2.0f }, { 0.0f, 2.2f }, { 0.3f, 2.3f }, { -0.3f, 2.4f },
    { 0.4f, 2.5f }, { -0.4f, 2.5f }, { 0.5f, 2.6f }, { -0.5f, 2.6f }
};

typedef struct { const RP* rows; int count; int interval_ms; } RP_TABLE;

static RP_TABLE recoil_table(int weapon_class) {
    RP_TABLE t = { k_other, (int)(sizeof(k_other) / sizeof(k_other[0])), 120 };
    switch (weapon_class) {
    case RR_WCLASS_PISTOL:
        t.rows = k_pistol; t.count = (int)(sizeof(k_pistol) / sizeof(k_pistol[0])); t.interval_ms = 150; break;
    case RR_WCLASS_RIFLE:
        t.rows = k_rifle; t.count = (int)(sizeof(k_rifle) / sizeof(k_rifle[0])); t.interval_ms = 95; break;
    case RR_WCLASS_SMG:
        t.rows = k_smg; t.count = (int)(sizeof(k_smg) / sizeof(k_smg[0])); t.interval_ms = 75; break;
    case RR_WCLASS_SHOTGUN:
        t.rows = k_shotgun; t.count = (int)(sizeof(k_shotgun) / sizeof(k_shotgun[0])); t.interval_ms = 800; break;
    case RR_WCLASS_SNIPER:
        t.rows = k_sniper; t.count = (int)(sizeof(k_sniper) / sizeof(k_sniper[0])); t.interval_ms = 1100; break;
    default:
        break;
    }
    return t;
}

int rr_recoil_class(const char* weapon) {
    char key[24];
    size_t n = 0;
    while (weapon && *weapon && *weapon != '_' && n + 1 < sizeof(key))
        key[n++] = (char)tolower((unsigned char)*weapon++);
    key[n] = 0;
    if (!n) return RR_WCLASS_OTHER;
    if (!strcmp(key, "dp9") || !strcmp(key, "m9") || strstr(key, "pistol")) return RR_WCLASS_PISTOL;
    if (strstr(key, "shotgun") || !strncmp(key, "sg12", 4) || !strncmp(key, "ks12", 4) ||
        !strcmp(key, "m870")) return RR_WCLASS_SHOTGUN;
    if (strstr(key, "smg") || !strcmp(key, "mp5") || !strcmp(key, "uzi") ||
        !strcmp(key, "p90")) return RR_WCLASS_SMG;
    if (strstr(key, "sniper") || !strcmp(key, "svd") || !strcmp(key, "awm")) return RR_WCLASS_SNIPER;
    if (strstr(key, "rifle") || !strcmp(key, "rc556") || !strcmp(key, "kl545") ||
        !strcmp(key, "ak47") || !strcmp(key, "m4") || !strcmp(key, "m16")) return RR_WCLASS_RIFLE;
    return RR_WCLASS_OTHER;
}

void rr_recoil_pattern(int weapon_class, int shot, float* x, float* y) {
    RP_TABLE table = recoil_table(weapon_class);
    if (shot < 0) shot = 0;
    if (shot >= table.count) shot = table.count - 1;
    if (x) *x = table.rows[shot].x;
    if (y) *y = table.rows[shot].y;
}

void rr_recoil_reset(RR_RECOIL_STATE* state) {
    if (!state) return;
    memset(state, 0, sizeof(*state));
}

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

int rr_recoil_tick(RR_RECOIL_STATE* state, int mode, int strength, int horizontal,
                   int firing, int weapon_class, const RR_V3* fwd, float fov,
                   int height, int cal, int raw_dx, int raw_dy,
                   unsigned now, int* mx, int* my) {
    if (!mx || !my) return 0;
    *mx = *my = 0;
    if (!state) return 0;
    if (!firing || strength <= 0) {
        rr_recoil_reset(state);
        return 0;
    }

    unsigned dt = state->last_at ? now - state->last_at : 16;
    if (dt > 120) dt = 120;
    if (!dt) dt = 1;
    state->last_at = now;

    RP_TABLE table = recoil_table(weapon_class);
    int interval = table.interval_ms > 0 ? table.interval_ms : 120;
    float px = 0.0f, py = 0.0f;

    if (!state->burst) {
        state->burst = 1;
        state->shot = 0;
        state->shot_at = now;
        rr_recoil_pattern(weapon_class, 0, &px, &py);
        state->pending_x += px;
        state->pending_y += py;
    } else {
        unsigned advance = (now - state->shot_at) / (unsigned)interval;
        if (advance > 8) advance = 8;
        if (advance) {
            state->shot_at += advance * (unsigned)interval;
            for (unsigned k = 0; k < advance; ++k) {
                if (state->shot < 1000) ++state->shot;
                rr_recoil_pattern(weapon_class, state->shot, &px, &py);
                state->pending_x += px;
                state->pending_y += py;
            }
        }
    }

    float strength_scale = (float)strength / 100.0f;
    float horizontal_scale = (float)horizontal / 100.0f;
    float window = (float)(interval < 70 ? interval : 70);
    float drain = (float)dt / window;
    if (drain > 1.0f) drain = 1.0f;
    float step_x = state->pending_x * drain;
    float step_y = state->pending_y * drain;
    state->pending_x -= step_x;
    state->pending_y -= step_y;

    float out_x = step_x * strength_scale * horizontal_scale;
    float out_y = step_y * strength_scale;

    /* Camera assist: the view gained pitch the mouse did not ask for. */
    if (mode != RR_RECOIL_PATTERN && fwd && height >= 16 && fov > 10.0f && fov < 170.0f &&
        isfinite(fwd->y)) {
        float pitch = asinf(clampf(fwd->y, -1.0f, 1.0f)) * 57.2957795f;
        if (raw_dx || raw_dy) state->raw_seen = 1;
        if (state->have_pitch) {
            /* Degrees per mouse count, matching rr_aim_step's calibration. */
            float reference = 100.0f / (float)(cal > 0 ? cal : 100);
            float deg_per_count = (fov * 3.14159265f / 180.0f) / 1080.0f *
                                  (0.57735027f / tanf(fov * 3.14159265f / 360.0f)) * reference;
            if (deg_per_count > 1e-6f) {
                float expected = state->raw_seen ? -(float)raw_dy * deg_per_count : 0.0f;
                float residual = (pitch - state->pitch) - expected;
                /* Without raw input the split is unknown, so only a fraction
                 * of the upward motion is treated as recoil. */
                float weight = state->raw_seen ? 0.75f : 0.35f;
                if (residual > 0.005f) {
                    float add = residual / deg_per_count * strength_scale * weight;
                    out_y += clampf(add, 0.0f, 40.0f);
                }
            }
        }
        state->pitch = pitch;
        state->have_pitch = 1;
    } else {
        state->have_pitch = 0;
    }

    state->carry_x += out_x;
    state->carry_y += out_y;
    int ix = (int)state->carry_x;
    int iy = (int)state->carry_y;
    state->carry_x -= (float)ix;
    state->carry_y -= (float)iy;
    /* A stalled worker must not dump a whole burst in one frame. */
    if (ix > 90) ix = 90;
    if (ix < -90) ix = -90;
    if (iy > 90) iy = 90;
    if (iy < -90) iy = -90;
    *mx = ix;
    *my = iy;
    return (ix != 0 || iy != 0);
}
