/* rr_recoil.h - Recoil compensation for the input worker.
 *
 * The snapshot exposes no recoil angles, so compensation comes from two
 * sources: a per-weapon shot pattern, and (optionally) the pitch the camera
 * gained beyond the mouse movement the host observed through raw input.
 * Both are emitted as bounded mouse counts. Nothing here touches game memory.
 */
#ifndef RR_RECOIL_H
#define RR_RECOIL_H
#include "rr_game.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { RR_RECOIL_PATTERN = 0, RR_RECOIL_CAMERA = 1, RR_RECOIL_BOTH = 2 };
enum { RR_WCLASS_PISTOL, RR_WCLASS_RIFLE, RR_WCLASS_SMG, RR_WCLASS_SHOTGUN,
       RR_WCLASS_SNIPER, RR_WCLASS_OTHER, RR_WCLASS_COUNT };

typedef struct {
    int      burst;        /* inside a firing burst                       */
    int      shot;         /* shots counted since the burst started       */
    unsigned shot_at;      /* time of the last counted shot               */
    unsigned last_at;      /* time of the previous tick                   */
    float    pending_x;    /* pattern counts not emitted yet              */
    float    pending_y;
    float    pitch;        /* previous camera pitch, degrees              */
    int      have_pitch;
    int      raw_seen;     /* host mouse deltas observed this burst       */
    float    carry_x;      /* fractional counts between ticks             */
    float    carry_y;
} RR_RECOIL_STATE;

/* Weapon name -> recoil class. Unknown names use RR_WCLASS_OTHER. */
int  rr_recoil_class(const char* weapon);
/* Pattern offset in mouse counts for one shot of a burst; y > 0 pulls down. */
void rr_recoil_pattern(int weapon_class, int shot, float* x, float* y);
void rr_recoil_reset(RR_RECOIL_STATE* state);

/* Emit this frame's correction. mode is RR_RECOIL_*; strength and horizontal
 * are percentages. raw_dx/raw_dy are the mouse counts the host saw since the
 * previous call. Returns 1 when a non-zero correction is produced. */
int  rr_recoil_tick(RR_RECOIL_STATE* state, int mode, int strength, int horizontal,
                    int firing, int weapon_class, const RR_V3* fwd, float fov,
                    int height, int cal, int raw_dx, int raw_dy,
                    unsigned now, int* mx, int* my);

/* Host-provided mouse movement since the previous call. rr_esp.c implements
 * this from WM_INPUT; the offline checks return zero. */
int  rr_raw_mouse_delta(int* dx, int* dy);

#ifdef __cplusplus
}
#endif
#endif
