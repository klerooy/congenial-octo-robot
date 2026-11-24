/* rr_aim.h - Target-region selection and bounded mouse-steering math. */
#ifndef RR_AIM_H
#define RR_AIM_H
#include "rr_game.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Input-thread entry point. The host gates foreground game focus and snapshot age.
   Pass NULL/zero on a gated frame to reset pending acquisition and steering. */
void rr_aim_update(const RR_PLAYER* players, int count, const RR_CAMERA* camera,
                   int width, int height);
int rr_aim_is_active(void);

/* Last instant-lock frame, for the host's diagnostics log. Read-only snapshot:
   pixels per count and the direction each axis turns are measured at runtime,
   so this is the only way to see what the loop concluded inside the game. */
typedef struct {
    int      picked;
    uint64_t addr;
    int32_t  id;
    RR_V3    point, body;
    float    err_x, err_y;      /* screen pixels to the point */
    int      sent_x, sent_y;    /* counts asked for this frame */
    float    px_x, px_y;        /* learned pixels per count */
    int      sign_x, sign_y;    /* +1 when positive counts shrink a positive error */
    float    fov;
} RR_AIM_FRAME;

void rr_aim_frame_stats(RR_AIM_FRAME* out);

/* Carriers zeroed by the last nospread pass: 0 when the cone was already shut
   or a chain did not resolve, otherwise how many writes were issued. */
int rr_nospread_writes(void);

/* Profile IDs 0..6 retain their saved-setting meanings. Each full-rig area
   chooses among real joints; ties use renderer order. */
enum { RR_AIM_HEAD, RR_AIM_NECK, RR_AIM_CHEST, RR_AIM_PELVIS,
       RR_AIM_CLOSEST, RR_AIM_ARMS, RR_AIM_LEGS, RR_AIM_FACE,
       RR_AIM_HANDS, RR_AIM_FEET, RR_AIM_GROUP_COUNT };

int rr_aim_bone_in_group(int group, int bone);
int rr_aim_point(const RR_PLAYER* player, int group, const RR_CAMERA* camera,
                 int width, int height, RR_V3* out);

/* When choosing among points, prefer the closest one whose ray is clear. */
int rr_aim_visible_point(const RR_PLAYER* player, int group, const RR_CAMERA* camera,
                         int width, int height, RR_V3* out,
                         int (*clear)(RR_V3, RR_V3));

struct BR_Config;
typedef struct {
    const RR_PLAYER* player;
    RR_V3 point;
    float dx, dy;
} RR_AIM_PICK;

/* A held lock restricts selection to the original player, even outside the
   acquisition ring. */
RR_AIM_PICK rr_aim_pick(const RR_PLAYER* players, int count, const RR_CAMERA* camera,
                        int width, int height, const struct BR_Config* config,
                        uint64_t locked_addr, int32_t locked_id,
                        int (*clear)(RR_V3, RR_V3));

/* Fractional-count carry plus deadzone hysteresis state per axis.
   POD: zero-initialize with {0} or memset; no other setup needed. */
typedef struct {
    float carry_x, carry_y;
    unsigned char inside_x, inside_y;   /* 1 while error is inside the deadzone */
} RR_AIM_STEER;

typedef struct {
    uint64_t addr;
    int32_t id;
    unsigned acquired_at, last_shot;
    int tracked, fired;
} RR_TRIGGER_STATE;

/* Screen-space trigger test: a sample joint counts as a hit when its projection
   lands within trigger_fov pixels of the crosshair. Head, neck, chest and pelvis
   are sampled unless trigger_head_only is set. clear may be NULL when the caller
   has switched the line-of-sight requirement off. */
RR_AIM_PICK rr_trigger_pick(const RR_PLAYER* players, int count, const RR_CAMERA* camera,
                            int width, int height, const struct BR_Config* config,
                            int (*clear)(RR_V3, RR_V3));
int rr_trigger_ready(RR_TRIGGER_STATE* state, uint64_t addr, int32_t id,
                     unsigned now, int delay_ms);

/* Convert a fresh projected error into bounded mouse counts.
   cal is 100 times the mouse counts per reference pixel at 1080p and 60-degree
   vertical FOV. Measure it with --aimtest after changing game sensitivity.
   A value of 100 assumes one mouse count per reference pixel.
   smooth is a response percentage: 1 is slow and 100 is immediate. */
void rr_aim_step(RR_AIM_STEER* state, float dx, float dy, float fov, int height,
                 int smooth, int cal, unsigned elapsed_ms, int firing,
                 int* mx, int* my);

/* Lead a moving point so a constant-velocity target settles on the crosshair.
   elapsed_ms is the input-worker period actually used by rr_aim_step; the lead
   compensates the discrete follow lag (dt/gain) minus snapshot age, and may be
   negative when the snapshot age already over-compensates. */
int rr_aim_motion_point(RR_V3 point, RR_V3 position, RR_V3 previous,
                        unsigned sample_ms, unsigned age_ms, int smooth,
                        unsigned elapsed_ms, RR_V3* out);

#ifdef __cplusplus
}
#endif

#endif