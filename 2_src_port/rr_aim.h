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

typedef struct {
    float carry_yaw, carry_pitch;
    float pend_yaw, pend_pitch;   /* mouse counts still in flight */
    float dpc;
    int   dpc_valid;
    float last_yaw, last_pitch;
    int   sent_yaw, sent_pitch;
    int   sign_yaw, sign_pitch;
    int   have_last;
} RR_MAGNET;

typedef struct {
    int      picked;
    uint64_t addr;
    int32_t  id;
    int      bone;
    RR_V3    point;
    float    yaw, pitch;
    int      sent_x, sent_y;
    float    dpc;
    float    fov;
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

void rr_magnet_angles(const RR_CAMERA* cam, const RR_V3* point,
                      float* yaw_deg, float* pitch_deg);
void rr_magnet_step(RR_MAGNET* m, float yaw, float pitch, int smooth,
                    unsigned elapsed_ms, int* mx, int* my);
void rr_magnet_feedback(RR_MAGNET* m, float yaw_now, float pitch_now);

#ifdef __cplusplus
}
#endif
#endif