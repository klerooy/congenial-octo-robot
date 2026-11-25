/* rr_aim.h - vector servo aim, trigger, nospread. */
#ifndef RR_AIM_H
#define RR_AIM_H
#include "rr_game.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    RR_AIM_HEAD = 0, RR_AIM_NECK, RR_AIM_CHEST, RR_AIM_PELVIS, RR_AIM_CLOSEST,
    RR_AIM_ARMS, RR_AIM_LEGS, RR_AIM_FACE, RR_AIM_HANDS, RR_AIM_FEET,
    RR_AIM_GROUP_COUNT
};

typedef struct {
    const RR_PLAYER* player;
    RR_V3 point;
    float dx, dy;
    int   bone;
} RR_AIM_PICK;

typedef struct {
    uint64_t addr;
    int32_t  id;
    unsigned acquired_at, last_shot;
    int tracked, fired;
} RR_TRIGGER_STATE;

/* Vector servo state. POD: zero-init then set dpc/sign fields, or call
 * rr_magnet_reset. One 2-D direction and one magnitude per issued move. */
typedef struct {
    float dpc;              /* degrees per mouse count, measured */
    int   dpc_valid;
    float carry_x, carry_y; /* fractional counts */

    int   pending;          /* a move is in flight */
    int   stable;           /* consecutive quiet frames */
    int   timeout;          /* frames since the move was issued */
    int   first_motion;     /* frame index of the first observed reply */
    float base_yaw, base_pitch;
    int   sent_x, sent_y;
    float predicted;        /* degrees that move should consume */
    float last_ang;         /* previous total angle, for the quiet detector */
    int   have_last_ang;

    float prev_tx, prev_ty, prev_tz;
    int   have_prev_target;

    int   latency;          /* measured input latency in frames */
    int   latency_valid;

    int   sign_x, sign_y;   /* +1 normal, -1 inverted axis */
} RR_MAGNET;

typedef struct {
    /* current */
    int      picked;
    uint64_t addr;
    int32_t  id;
    int      bone;
    RR_V3    point;
    float    yaw, pitch;
    int      sent_x, sent_y;
    float    dpc;
    float    fov;
    /* legacy members, kept so the host's existing log compiles unchanged */
    RR_V3    body;
    float    err_x, err_y;
    float    px_x, px_y;
    int      sign_x, sign_y;
} RR_AIM_FRAME;

void rr_aim_update(const RR_PLAYER* players, int count, const RR_CAMERA* camera,
                   int width, int height);
int  rr_aim_is_active(void);
void rr_aim_frame_stats(RR_AIM_FRAME* out);
int  rr_nospread_writes(void);

int  rr_aim_bone_in_group(int group, int bone);
int  rr_aim_point(const RR_PLAYER* player, int group, const RR_CAMERA* camera,
                  int width, int height, RR_V3* out);
int  rr_aim_visible_point(const RR_PLAYER* player, int group, const RR_CAMERA* camera,
                          int width, int height, RR_V3* out,
                          int (*clear)(RR_V3, RR_V3));

struct BR_Config;
RR_AIM_PICK rr_aim_pick(const RR_PLAYER* players, int count, const RR_CAMERA* camera,
                        int width, int height, const struct BR_Config* config,
                        uint64_t locked_addr, int32_t locked_id, int locked_bone,
                        int (*clear)(RR_V3, RR_V3));
RR_AIM_PICK rr_trigger_pick(const RR_PLAYER* players, int count, const RR_CAMERA* camera,
                            int width, int height, const struct BR_Config* config,
                            int (*clear)(RR_V3, RR_V3));
int rr_trigger_ready(RR_TRIGGER_STATE* state, uint64_t addr, int32_t id,
                     unsigned now, int delay_ms);

void rr_magnet_reset(RR_MAGNET* m);
void rr_magnet_angles(const RR_CAMERA* cam, const RR_V3* point,
                      float* yaw_deg, float* pitch_deg);
/* Returns the counts to send this frame, or 0/0 while waiting for the previous
 * move to land. target is the world point, used to detect a moving target. */
void rr_magnet_step(RR_MAGNET* m, float yaw, float pitch, RR_V3 target,
                    int smooth, unsigned elapsed_ms, int* mx, int* my);

#ifdef __cplusplus
}
#endif
#endif