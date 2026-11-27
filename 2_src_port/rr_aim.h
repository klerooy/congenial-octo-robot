/* rr_aim.h - Target-region selection and bounded mouse-steering math. */
#ifndef RR_AIM_H
#define RR_AIM_H
#include "rr_game.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Input-thread entry point. The host gates foreground game focus and snapshot age.
 * Pass NULL/zero on a gated frame to reset pending acquisition and steering. */
void rr_aim_update(const RR_PLAYER* players, int count, const RR_CAMERA* camera,
                   int width, int height);
int rr_aim_is_active(void);
/* 1 = camera lock (the default), 0 = mouse steering. The camera path falls
 * back to mouse steering automatically when the camera hook is unavailable. */
void rr_aim_set_camera_mode(int on);

/* Profile IDs 0..6 retain their saved-setting meanings. Each full-rig area
 * chooses among real joints; ties use renderer order.
 */
enum { RR_AIM_HEAD, RR_AIM_NECK, RR_AIM_CHEST, RR_AIM_PELVIS,
       RR_AIM_CLOSEST, RR_AIM_ARMS, RR_AIM_LEGS, RR_AIM_FACE,
       RR_AIM_HANDS, RR_AIM_FEET, RR_AIM_GROUP_COUNT };
int rr_aim_bone_in_group(int group, int bone);
int rr_aim_point(const RR_PLAYER* player, int group, const RR_CAMERA* camera,
                 int width, int height, RR_V3* out);
/* When choosing among points, prefer the closest one whose ray is clear. */
int rr_aim_visible_point(const RR_PLAYER* player, int group, const RR_CAMERA* camera,
                         int width, int height, RR_V3* out, int (*clear)(RR_V3, RR_V3));

struct BR_Config;
typedef struct {
    const RR_PLAYER* player;
    RR_V3 point;
    float dx, dy;
    int   bone;
} RR_AIM_PICK;

/* Per-frame snapshot of the last aim decision, for the menu footer and the
 * throttled log. px_x/px_y are pixels per mouse count at the current FOV and
 * calibration; err_x/err_y are the projected pixel offsets that produced the
 * last sent move. sign_x/sign_y report the axis direction the current
 * calibration implies. */
typedef struct {
    int      picked;
    uint64_t addr;
    int32_t  id;
    int      bone;
    RR_V3    point;
    float    yaw, pitch;
    int      sent_x, sent_y;
    float    dpc;
    /* Auto-calibration: cal is the learned calibration equivalent (hundredths
     * of a mouse count per reference pixel), samples is how many clean
     * measurements produced it, latency is the conservative design loop delay
     * in aim frames. */
    int      cal, cal_samples, latency;
    float    fov;
    /* Legacy members, kept so the host's existing log compiles unchanged. */
    RR_V3    body;
    float    err_x, err_y;
    float    px_x, px_y;
    int      sign_x, sign_y;
    int      cam;                  /* 1 = camera lock, 0 = mouse steering */
} RR_AIM_FRAME;

void rr_aim_frame_stats(RR_AIM_FRAME* out);
int  rr_nospread_writes(void);

/* A held lock restricts selection to the original player and joint, even
 * outside the acquisition ring. */
RR_AIM_PICK rr_aim_pick(const RR_PLAYER* players, int count, const RR_CAMERA* camera,
                       int width, int height, const struct BR_Config* config,
                       uint64_t locked_addr, int32_t locked_id, int locked_bone,
                       int (*clear)(RR_V3, RR_V3));

/* Steering and self-calibration state. All fields are maintained by
 * rr_aim_update/rr_aim_step; callers only need to zero it before first use. */
typedef struct {
    float carry_x, carry_y;       /* fractional counts carried between frames */
    float last_dx, last_dy;       /* previous projected error, for overshoot */
    float damp;                   /* gain backoff after a detected overshoot  */
    float dpc;                    /* measured degrees per mouse count         */
    int   dpc_valid, dpc_samples;
    int   latency;                /* design loop delay, in aim frames         */
    /* Episode accumulator: signed camera rotation vs counts sent. */
    float ep_yaw, ep_pitch, ep_sx, ep_sy;
    int   ep_frames, ep_still, ep_idle, ep_hold;
    RR_V3 prev_fwd;
    int   have_prev;
    int   cal_seed;               /* seed value the estimate started from      */
} RR_AIM_STEER;
typedef struct {
    uint64_t addr;
    int32_t id;
    unsigned acquired_at, last_shot;
    int tracked, fired;
} RR_TRIGGER_STATE;
RR_AIM_PICK rr_trigger_pick(const RR_PLAYER* players, int count, const RR_CAMERA* camera,
                            const struct BR_Config* config, int (*clear)(RR_V3, RR_V3));
int rr_trigger_ready(RR_TRIGGER_STATE* state, uint64_t addr, int32_t id,
                      unsigned now, int delay_ms);
/* Convert a fresh projected error into bounded mouse counts.
 * cal is only a seed: it is 100 times the assumed mouse counts per reference
 * pixel at 1080p and 60-degree vertical FOV. While the aim key is held the
 * worker measures the real degrees-per-count from the camera response and
 * replaces the seed. Changing the slider reseeds the estimate.
 * smooth is a response percentage: 1 is slow and 100 is the fastest gain the
 * conservative design delay keeps stable.
 */
void rr_aim_step(RR_AIM_STEER* state, float dx, float dy, float fov, int height,
                 int smooth, int cal, unsigned elapsed_ms, int firing, int* mx, int* my);

/* Humanization wobble in reference pixels (1080p, 60-degree vertical FOV).
 * strength 0..100 scales the amplitude, speed 1..100 scales the frequency.
 * The result is smooth, bounded and deterministic for a given seed/time. */
void rr_aim_humanize_offset(int strength, int speed, unsigned now, uint32_t seed,
                            float* x, float* y);

int rr_aim_motion_point(RR_V3 point, RR_V3 position, RR_V3 previous,
                        unsigned sample_ms, unsigned age_ms, int smooth, RR_V3* out);

#ifdef __cplusplus
}
#endif
#endif
