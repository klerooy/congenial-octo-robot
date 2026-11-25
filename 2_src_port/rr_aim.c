/* rr_aim.c - vector servo aim.
 *
 * Why the previous version drifted sideways and into the floor: it estimated
 * degrees-per-count from (angle_before - angle_after) / counts_sent every frame.
 * On a moving target, and while an earlier move was still landing, that delta is
 * not the reply to the move being measured. The estimate collapsed far below the
 * truth, the loop then sent too many counts, and the next estimate collapsed
 * further - a positive feedback path. At smooth 100 the gain is 1, so the error
 * doubled every frame.
 *
 * This version:
 *   - measures sensitivity ONLY when the target is static in world space AND the
 *     view has been quiet for several frames, so the measurement is unambiguous;
 *   - keeps at most ONE move in flight while the target is static, so commands
 *     cannot stack into an overshoot;
 *   - clamps the proportional gain by the measured input latency, g <= 0.8*2/(L+1),
 *     which is the stability margin of a delayed proportional loop. The slider
 *     asks for speed up to that limit, never past it.
 *
 * Everything is one 2-D vector: direction is preserved by scaling both axes with
 * the same factor, so the crosshair travels along the line to the joint instead
 * of stair-stepping independently in yaw and pitch.
 */
#include "rr_aim.h"
#include "rr_cham_anchors.h"
#include "rr_body_joints.h"
#include "menu_bridge.h"
#include "rr_los.h"
#include "rr_mem.h"
#include <math.h>
#include <windows.h>
#include <string.h>

#define RR_PI  3.14159265358979f
#define RR_DEG (180.0f / RR_PI)

#ifndef RR_AIM_NOW
#define RR_AIM_NOW() GetTickCount64()
#endif

#define RR_DPC_SEED     0.050f
#define RR_DPC_MIN      0.002f
#define RR_DPC_MAX      0.600f
#define RR_LAT_DEFAULT  4        /* conservative until measured */
#define RR_LAT_MAX      8
#define RR_QUIET_NEED   4        /* quiet frames that mean "landed" */
#define RR_WAIT_MAX     26       /* hard ceiling on the wait */
#define RR_QUIET_EPS    0.005f   /* degrees: below this the view is still */
#define RR_MOVE_EPS     0.02f    /* metres: below this the target is static */
#define RR_GAIN_BUDGET  9.0f     /* degrees of command in flight, total */
#define RR_DT_MAX       50u

static volatile LONG g_active;
static RR_MAGNET     g_mag;
static RR_TRIGGER_STATE g_trig;
static ULONGLONG     g_last_at;
static uint64_t      g_lock_addr;
static int32_t       g_lock_id;
static int           g_lock_bone = -1;
static ULONGLONG     g_lock_at;
static RR_AIM_FRAME  g_frame;
static int           g_nospread_writes;

int rr_nospread_writes(void) { return g_nospread_writes; }
int rr_aim_is_active(void) { return InterlockedCompareExchange(&g_active,0,0)!=0; }
void rr_aim_frame_stats(RR_AIM_FRAME* out) { if (out) *out = g_frame; }

/* ---------------------------------------------------------------- geometry */

static float vdot(RR_V3 a, RR_V3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }

void rr_magnet_angles(const RR_CAMERA* cam, const RR_V3* point,
                      float* yaw_deg, float* pitch_deg) {
    *yaw_deg = *pitch_deg = 0.0f;
    if (!cam || !point || !cam->valid) return;
    RR_V3 rel = { point->x - cam->pos.x,
                  point->y - cam->pos.y,
                  point->z - cam->pos.z };
    const float len = sqrtf(vdot(rel, rel));
    if (!(len > 0.001f) || !isfinite(len)) return;
    rel.x /= len; rel.y /= len; rel.z /= len;

    RR_V3 f = cam->fwd, r = cam->right, u = cam->up;
    float fl = sqrtf(vdot(f,f)), rl = sqrtf(vdot(r,r)), ul = sqrtf(vdot(u,u));
    if (!(fl > 0.5f) || !(rl > 0.5f) || !(ul > 0.5f)) {
        f = cam->fwd;
        r = (RR_V3){ f.z, 0.0f, -f.x };
        u = (RR_V3){ 0.0f, 1.0f, 0.0f };
        fl = sqrtf(vdot(f,f)); rl = sqrtf(vdot(r,r)); ul = 1.0f;
        if (!(fl > 0.001f) || !(rl > 0.001f)) return;
    }
    f.x/=fl; f.y/=fl; f.z/=fl;
    r.x/=rl; r.y/=rl; r.z/=rl;
    u.x/=ul; u.y/=ul; u.z/=ul;

    const float z = vdot(rel, f), x = vdot(rel, r), y = vdot(rel, u);
    const float yaw   = atan2f(x, z) * RR_DEG;
    const float pitch = atan2f(y, sqrtf(x*x + z*z)) * RR_DEG;
    if (isfinite(yaw))   *yaw_deg   = yaw;
    if (isfinite(pitch)) *pitch_deg = pitch;
}

/* -------------------------------------------------------------------- servo */

void rr_magnet_reset(RR_MAGNET* m) {
    if (!m) return;
    memset(m, 0, sizeof(*m));
    m->dpc = RR_DPC_SEED;
    m->latency = RR_LAT_DEFAULT;
    m->sign_x = 1;
    m->sign_y = 1;
}

void rr_magnet_step(RR_MAGNET* m, float yaw, float pitch, RR_V3 target,
                    int smooth, unsigned elapsed_ms, int* mx, int* my) {
    *mx = *my = 0;
    if (!m || !isfinite(yaw) || !isfinite(pitch)) return;
    if (!(m->dpc > RR_DPC_MIN) || !(m->dpc < RR_DPC_MAX) || !isfinite(m->dpc)) {
        m->dpc = RR_DPC_SEED;
        m->dpc_valid = 0;
    }
    if (m->sign_x != 1 && m->sign_x != -1) m->sign_x = 1;
    if (m->sign_y != 1 && m->sign_y != -1) m->sign_y = 1;
    if (m->latency < 1 || m->latency > RR_LAT_MAX) m->latency = RR_LAT_DEFAULT;

    const float ang = sqrtf(yaw*yaw + pitch*pitch);

    /* Is the target itself moving in world space? Only a static target gives an
     * unambiguous sensitivity measurement. */
    int still = 0;
    if (m->have_prev_target) {
        const float dx = target.x - m->prev_tx;
        const float dy = target.y - m->prev_ty;
        const float dz = target.z - m->prev_tz;
        still = (dx*dx + dy*dy + dz*dz) < (RR_MOVE_EPS * RR_MOVE_EPS);
    }
    m->prev_tx = target.x; m->prev_ty = target.y; m->prev_tz = target.z;
    m->have_prev_target = 1;

    /* ---- wait for the outstanding move, then learn from it ---- */
    if (m->pending) {
        m->timeout++;
        int moved = 0;
        if (m->have_last_ang) {
            const float d = fabsf(m->last_ang - ang);
            moved = d >= RR_QUIET_EPS;
            if (moved) {
                if (!m->first_motion) {
                    m->first_motion = m->timeout;
                    /* Latency is only readable on a static target: otherwise the
                     * angle changes because the target moved, not because our
                     * command arrived. */
                    if (still && !m->latency_valid) {
                        int L = m->timeout;
                        if (L < 1) L = 1;
                        if (L > RR_LAT_MAX) L = RR_LAT_MAX;
                        m->latency = L;
                        m->latency_valid = 1;
                    }
                }
                m->stable = 0;
            } else {
                m->stable++;
            }
        }
        m->last_ang = ang;
        m->have_last_ang = 1;

        const int landed = (m->stable >= RR_QUIET_NEED) || (m->timeout >= RR_WAIT_MAX);
        if (landed) {
            const float dy = m->base_yaw   - yaw;
            const float dp = m->base_pitch - pitch;
            const float sent = sqrtf((float)(m->sent_x*m->sent_x +
                                             m->sent_y*m->sent_y));
            const float got = sqrtf(dy*dy + dp*dp);
            /* Learn only from a clean pair: static target, big enough move,
             * motion along the commanded direction, and at least 60% of the
             * predicted angle actually consumed (otherwise we sampled early). */
            if (sent >= 5.0f && still &&
                (dy*(float)m->sent_x + dp*(float)m->sent_y) > 0.0f &&
                got >= 0.6f * m->predicted) {
                float meas = got / sent;
                if (meas > RR_DPC_MIN && meas < RR_DPC_MAX) {
                    if (!m->dpc_valid) {
                        m->dpc = meas;
                        m->dpc_valid = 1;
                    } else {
                        /* Rate limit: one update may not move the estimate more
                         * than 1.6x up or 0.625x down. A single bad sample can
                         * no longer collapse the gain. */
                        if (meas < m->dpc * 0.625f) meas = m->dpc * 0.625f;
                        if (meas > m->dpc * 1.600f) meas = m->dpc * 1.600f;
                        m->dpc += 0.25f * (meas - m->dpc);
                    }
                }
            } else if (sent >= 5.0f && still &&
                       (dy*(float)m->sent_x + dp*(float)m->sent_y) < 0.0f &&
                       got > 0.5f * m->predicted) {
                /* The view went the opposite way twice as far as asked: the axis
                 * is inverted, not mis-scaled. */
                m->sign_x = -m->sign_x;
                m->sign_y = -m->sign_y;
                m->carry_x = m->carry_y = 0.0f;
            }
            m->pending = 0;
            m->stable = 0;
            m->timeout = 0;
            m->first_motion = 0;
        } else if (still) {
            /* Static target: strictly one move at a time. */
            return;
        }
    }

    /* ---- issue the next vector move ---- */
    float a = (float)smooth / 100.0f;
    if (!(a > 0.0f)) a = 0.02f;
    if (a > 1.0f) a = 1.0f;
    float dt = (float)(elapsed_ms ? elapsed_ms : 16u);
    if (dt > (float)RR_DT_MAX) dt = (float)RR_DT_MAX;
    float g = 1.0f - powf(1.0f - a, dt / 16.0f);

    /* Stability margin of a proportional loop with L frames of delay. */
    const float gmax = 0.8f * 2.0f / (float)(m->latency + 1);
    if (g > gmax) g = gmax;

    float wy = yaw   * g;
    float wp = pitch * g;

    /* One angular budget shared by both axes: scaling both by the same factor
     * keeps the direction, so the path is a straight line to the joint. */
    const float cap = (RR_GAIN_BUDGET / (float)(m->latency + 1)) * (dt / 16.0f);
    const float mag = sqrtf(wy*wy + wp*wp);
    if (mag > cap && mag > 0.0f) {
        const float k = cap / mag;
        wy *= k; wp *= k;
    }

    const float dz = fmaxf(0.012f, m->dpc * 0.5f);
    if (sqrtf(wy*wy + wp*wp) < dz) {
        m->carry_x = m->carry_y = 0.0f;
        return;
    }

    const float cx = wy / m->dpc * (float)m->sign_x + m->carry_x;
    const float cy = wp / m->dpc * (float)m->sign_y + m->carry_y;
    const int nx = (int)cx;
    const int ny = (int)cy;
    m->carry_x = cx - (float)nx;
    m->carry_y = cy - (float)ny;
    if (!nx && !ny) return;

    if (!m->pending) {
        m->base_yaw   = yaw;
        m->base_pitch = pitch;
        m->sent_x = nx;
        m->sent_y = ny;
        m->predicted = sqrtf((float)(nx*nx + ny*ny)) * m->dpc;
        m->pending = 1;
        m->last_ang = ang;
        m->have_last_ang = 1;
        m->stable = 0;
        m->timeout = 0;
        m->first_motion = 0;
    }
    *mx = nx;
    *my = ny;
}

/* ------------------------------------------------------------- body points */

int rr_aim_bone_in_group(int group, int bone) {
    if (bone < 0 || bone >= RR_CHAM_BONES) return 0;
    switch (group) {
    case RR_AIM_HEAD:
        return bone == RR_RIG_M_HEAD_1_JNT || bone == RR_RIG_M_HEAD_2_JNT;
    case RR_AIM_NECK:
        return bone == RR_RIG_M_NECK_JNT;
    case RR_AIM_CHEST:
        return bone == RR_RIG_M_SPINE_3_JNT || bone == RR_RIG_M_SPINE_2_JNT;
    case RR_AIM_PELVIS:
        return bone == RR_RIG_M_SPINE_1_JNT;
    case RR_AIM_FACE:
        return bone >= RR_RIG_L_EYELIDUP_JNT && bone <= RR_RIG_M_TONGUE_3_JNT;
    case RR_AIM_HANDS:
        return (bone >= RR_RIG_L_INDEX_1_JNT && bone <= RR_RIG_L_THUMB_3_JNT) ||
               (bone >= RR_RIG_R_INDEX_1_JNT && bone <= RR_RIG_R_THUMB_3_JNT);
    case RR_AIM_FEET:
        return bone == RR_RIG_L_FOOT_1_JNT || bone == RR_RIG_L_FOOT_2_JNT ||
               bone == RR_RIG_R_FOOT_1_JNT || bone == RR_RIG_R_FOOT_2_JNT;
    case RR_AIM_ARMS:
        return (bone >= RR_RIG_L_ARM_0_JNT && bone <= RR_RIG_L_ARM_3_JNT) ||
               (bone >= RR_RIG_R_ARM_0_JNT && bone <= RR_RIG_R_ARM_3_JNT) ||
               bone == RR_RIG_L_ARMTWIST_JNT || bone == RR_RIG_R_ARMTWIST_JNT;
    case RR_AIM_LEGS:
        return (bone >= RR_RIG_L_LEG_1_JNT && bone <= RR_RIG_L_FOOT_2_JNT) ||
               (bone >= RR_RIG_R_LEG_1_JNT && bone <= RR_RIG_R_FOOT_2_JNT);
    default:
        return 0;
    }
}

static int bone_ok(const RR_PLAYER* p, int b) {
    if (b < 0 || b >= RR_MAX_BONES) return 0;
    return isfinite(p->bone[b][0]) && isfinite(p->bone[b][1]) && isfinite(p->bone[b][2]);
}

static RR_V3 bone_pos(const RR_PLAYER* p, int b) {
    return (RR_V3){ p->bone[b][0], p->bone[b][1], p->bone[b][2] };
}

static RR_V3 group_point(const RR_PLAYER* p, int group) {
    static const float lift[4] = { 0.0f, -0.12f, -0.35f, -0.70f };
    if (group < 0 || group > 3) group = 0;
    const int idx = p->bone_group[group];
    if (bone_ok(p, idx)) return bone_pos(p, idx);
    RR_V3 v = p->head;
    v.y += lift[group];
    return v;
}

static int group_best_bone(const RR_PLAYER* p, int group, const RR_CAMERA* cam) {
    int best = -1;
    float best_ang = 1e9f;
    for (int b = 0; b < RR_CHAM_BONES; ++b) {
        if (!rr_aim_bone_in_group(group, b) || !bone_ok(p, b)) continue;
        float y, pt;
        const RR_V3 v = bone_pos(p, b);
        rr_magnet_angles(cam, &v, &y, &pt);
        const float a = y*y + pt*pt;
        if (a < best_ang - 1e-6f) { best_ang = a; best = b; }
    }
    return best;
}

int rr_aim_visible_point(const RR_PLAYER* p, int group, const RR_CAMERA* cam,
                         int width, int height, RR_V3* out,
                         int (*clear)(RR_V3, RR_V3)) {
    (void)width; (void)height;
    if (!p || !out || !cam) return 0;
    if (group < 0 || group >= RR_AIM_GROUP_COUNT) group = RR_AIM_HEAD;

    if (group == RR_AIM_CLOSEST) {
        int found = 0; float best = 1e9f;
        for (int g2 = RR_AIM_HEAD; g2 <= RR_AIM_PELVIS; ++g2) {
            RR_V3 v;
            if (!rr_aim_visible_point(p, g2, cam, width, height, &v, clear)) continue;
            float y, pt; rr_magnet_angles(cam, &v, &y, &pt);
            const float a = y*y + pt*pt;
            if (a < best) { best = a; *out = v; found = 1; }
        }
        return found;
    }

    if (p->full_rig) {
        const int b = group_best_bone(p, group, cam);
        if (b >= 0) {
            const RR_V3 v = bone_pos(p, b);
            if (!clear || clear(cam->pos, v)) { *out = v; return 1; }
            float best = 1e9f; int found = 0;
            for (int k = 0; k < RR_CHAM_BONES; ++k) {
                if (!rr_aim_bone_in_group(group, k) || !bone_ok(p, k)) continue;
                const RR_V3 w = bone_pos(p, k);
                if (!clear(cam->pos, w)) continue;
                float y, pt; rr_magnet_angles(cam, &w, &y, &pt);
                const float a = y*y + pt*pt;
                if (a < best) { best = a; *out = w; found = 1; }
            }
            if (found) return 1;
        }
    }
    if (group >= RR_AIM_FACE) return 0;
    if (group >= RR_AIM_ARMS) {
        const RR_V3 a = group_point(p, RR_AIM_CHEST);
        const RR_V3 b = group_point(p, RR_AIM_PELVIS);
        RR_V3 v = { (a.x+b.x)*0.5f, (a.y+b.y)*0.5f, (a.z+b.z)*0.5f };
        if (group == RR_AIM_LEGS) v.y = p->pos.y + 0.35f;
        if (!isfinite(v.x) || !isfinite(v.y) || !isfinite(v.z)) return 0;
        if (clear && !clear(cam->pos, v)) return 0;
        *out = v; return 1;
    }
    {
        const RR_V3 v = group_point(p, group);
        if (!isfinite(v.x) || !isfinite(v.y) || !isfinite(v.z)) return 0;
        if (clear && !clear(cam->pos, v)) return 0;
        *out = v; return 1;
    }
}

int rr_aim_point(const RR_PLAYER* p, int group, const RR_CAMERA* cam,
                 int width, int height, RR_V3* out) {
    return rr_aim_visible_point(p, group, cam, width, height, out, NULL);
}

/* ------------------------------------------------------------------ filters */

static int aim_alive(const RR_PLAYER* p) { return !p->dead && p->health != 0; }

static int aim_allowed(const RR_PLAYER* p, const BR_Config* c) {
    if (c->aim_ignore_dead  && !aim_alive(p))                     return 0;
    if (c->aim_ignore_prone && p->prone == 1)                     return 0;
    if (c->aim_ignore_party && (p->relationships & RR_REL_PARTY)) return 0;
    if (c->aim_ignore_clan  && (p->relationships & RR_REL_CLAN))  return 0;
    return 1;
}

static int los_clear(RR_V3 a, RR_V3 b) { return rr_los_check(a,b) != RR_LOS_BLOCKED; }
static int los_fire (RR_V3 a, RR_V3 b) { return rr_los_check(a,b) == RR_LOS_CLEAR; }

/* ------------------------------------------------------------------- picker */

RR_AIM_PICK rr_aim_pick(const RR_PLAYER* players, int count, const RR_CAMERA* camera,
                        int width, int height, const BR_Config* config,
                        uint64_t locked_addr, int32_t locked_id, int locked_bone,
                        int (*clear)(RR_V3, RR_V3)) {
    RR_AIM_PICK best = {0};
    best.bone = -1;
    if (!players || count <= 0 || !camera || !camera->valid || !config) return best;
    if (count > RR_MAX_PLAYERS) count = RR_MAX_PLAYERS;

    float cone = (float)config->aim_fov;
    if (!(cone > 0.0f)) cone = 10.0f;
    if (cone > 180.0f) cone = 180.0f;
    const float cone2 = cone * cone;
    const float max_dist = (float)config->aa_dist;
    const int group = config->aa_bone;
    const int want_clan   = config->aim_targets & RR_REL_CLAN;
    const int want_police = config->aim_targets & RR_REL_POLICE;
    const int want_party  = config->aim_targets & RR_REL_PARTY;

    float best_pri = 1e9f, best_ang = 1e9f;

    for (int i = 0; i < count; ++i) {
        const RR_PLAYER* p = &players[i];
        const int is_lock = locked_addr && p->addr == locked_addr && p->id == locked_id;
        if (locked_addr && !is_lock) continue;
        if (!p->addr || !p->valid || p->local ||
            p->is_vehicle || p->is_loot || p->hidden_in_object) continue;
        if (!aim_allowed(p, config)) continue;
        if (!rr_relationship_allowed(p->relationships, want_clan, want_police, want_party))
            continue;
        if (!isfinite(p->dist) || (max_dist > 0.0f && p->dist > max_dist)) continue;

        RR_V3 point;
        int bone = -1;
        int have = 0;

        if (is_lock && locked_bone >= 0 && p->full_rig && bone_ok(p, locked_bone)) {
            point = bone_pos(p, locked_bone);
            bone = locked_bone;
            have = 1;
            if (clear && !clear(camera->pos, point)) have = 0;
        }
        if (!have) {
            if (!rr_aim_visible_point(p, group, camera, width, height, &point, clear))
                continue;
            if (p->full_rig) bone = group_best_bone(p, group, camera);
        }

        float yaw, pitch;
        rr_magnet_angles(camera, &point, &yaw, &pitch);
        const float ang2 = yaw*yaw + pitch*pitch;
        if (!isfinite(ang2)) continue;
        const float lim2 = is_lock ? cone2 * 4.0f : cone2;
        if (ang2 > lim2) continue;

        const float pri = config->aa_closest_first ? p->dist : ang2;
        if (pri < best_pri - 1e-6f || (fabsf(pri - best_pri) <= 1e-6f && ang2 < best_ang)) {
            best_pri = pri; best_ang = ang2;
            best.player = p; best.point = point; best.bone = bone;
            float sx, sy;
            if (game_w2s(camera, &point, width, height, &sx, &sy)) {
                best.dx = sx - width * 0.5f;
                best.dy = sy - height * 0.5f;
            }
        }
        if (is_lock) break;
    }
    return best;
}

/* ------------------------------------------------------------------ trigger */

RR_AIM_PICK rr_trigger_pick(const RR_PLAYER* players, int count, const RR_CAMERA* camera,
                            int width, int height, const BR_Config* config,
                            int (*clear)(RR_V3, RR_V3)) {
    RR_AIM_PICK best = {0};
    best.bone = -1;
    if (!players || count <= 0 || !camera || !camera->valid || !config) return best;
    if (count > RR_MAX_PLAYERS) count = RR_MAX_PLAYERS;

    float tol = (float)(config->trigger_fov > 0 ? config->trigger_fov : 10);
    if (tol > 80.0f) tol = 80.0f;
    float best_d2 = tol * tol, best_dist = 1e9f;
    const float cx = width * 0.5f, cy = height * 0.5f;
    const float max_dist = (float)config->aa_dist;
    const int want_clan   = config->trigger_targets & RR_REL_CLAN;
    const int want_police = config->trigger_targets & RR_REL_POLICE;
    const int want_party  = config->trigger_targets & RR_REL_PARTY;
    const int last_group  = config->trigger_head_only ? RR_AIM_HEAD : RR_AIM_PELVIS;

    for (int i = 0; i < count; ++i) {
        const RR_PLAYER* p = &players[i];
        if (!p->addr || !p->valid || p->local || p->hidden_in_object ||
            p->is_vehicle || p->is_loot ||
            !isfinite(p->dist) || (max_dist > 0.0f && p->dist > max_dist)) continue;
        if (!aim_allowed(p, config)) continue;
        if (!rr_relationship_allowed(p->relationships, want_clan, want_police, want_party))
            continue;
        for (int g = RR_AIM_HEAD; g <= last_group; ++g) {
            RR_V3 point; float sx, sy;
            if (!rr_aim_point(p, g, camera, width, height, &point)) continue;
            if (!game_w2s(camera, &point, width, height, &sx, &sy)) continue;
            const float dx = sx - cx, dy = sy - cy, d2 = dx*dx + dy*dy;
            if (!isfinite(d2) || d2 > best_d2) continue;
            if (clear && !clear(camera->pos, point)) continue;
            if (d2 < best_d2 - 0.25f || (d2 <= best_d2 + 0.25f && p->dist < best_dist)) {
                best.player = p; best.point = point;
                best.dx = dx; best.dy = dy;
                best_d2 = d2; best_dist = p->dist;
            }
        }
    }
    return best;
}

int rr_trigger_ready(RR_TRIGGER_STATE* s, uint64_t addr, int32_t id,
                     unsigned now, int delay_ms) {
    if (!s) return 0;
    if (!addr) { memset(s, 0, sizeof(*s)); return 0; }
    const unsigned d = delay_ms < 0 ? 0u : delay_ms > 300 ? 300u : (unsigned)delay_ms;
    if (!s->tracked || s->addr != addr || s->id != id) {
        memset(s, 0, sizeof(*s));
        s->tracked = 1; s->addr = addr; s->id = id; s->acquired_at = now;
    }
    if (now - s->acquired_at < d ||
        (s->fired && now - s->last_shot < (d > 16 ? d : 16))) return 0;
    s->last_shot = now; s->fired = 1;
    return 1;
}

/* ----------------------------------------------------------------- nospread */

static int nospread_apply(const RR_PLAYER* players, int count) {
    static const float zero = 0.0f;
    static const float zero3[3] = { 0.0f, 0.0f, 0.0f };
    int wrote = 0;
    if (!players || count <= 0) { g_nospread_writes = 0; return 0; }
    for (int i = 0; i < count; ++i) {
        const RR_PLAYER* p = &players[i];
        if (!p->local || p->dead) continue;
        if (p->spread_addr && p->has_spread_data && p->spread_value != 0.0f &&
            rr_write(p->spread_addr, &zero, sizeof zero)) ++wrote;
        if (p->fake_spread_addr && p->has_fake_spread_data &&
            (p->spread_move != 0.0f || p->spread_air != 0.0f ||
             (!p->spread_addr && p->spread_value != 0.0f)) &&
            rr_write(p->fake_spread_addr, zero3, sizeof zero3)) ++wrote;
    }
    g_nospread_writes = wrote;
    return wrote;
}

/* -------------------------------------------------------------------- input */

#define RR_GATE_MAGIC ((ULONG_PTR)0x52524741u)

static void send_move(int dx, int dy) {
    INPUT in = {0};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    in.mi.dx = dx; in.mi.dy = dy;
    in.mi.dwExtraInfo = RR_GATE_MAGIC;
    SendInput(1, &in, sizeof(in));
}

static void reset_worker(void) {
    rr_magnet_reset(&g_mag);
    memset(&g_trig, 0, sizeof(g_trig));
    g_lock_addr = 0; g_lock_id = 0; g_lock_bone = -1; g_lock_at = 0;
    g_last_at = 0;
    memset(&g_frame, 0, sizeof(g_frame));
}

void rr_aim_update(const RR_PLAYER* players, int count, const RR_CAMERA* camera,
                   int width, int height) {
    InterlockedExchange(&g_active, 0);
    BR_Config* config = br_get();
    if (!config || !players || count <= 0 || !camera || !camera->valid ||
        width < 16 || height < 16 || config->menu_open) {
        reset_worker();
        return;
    }
    if (count > RR_MAX_PLAYERS) count = RR_MAX_PLAYERS;

    BR_Config rage;
    if (config->rage) {
        rage = *config;
        rage.aim_enabled = 1;
        rage.aim_smooth = 100;
        rage.aim_fov = 180;
        rage.aa_bone = RR_AIM_HEAD;
        rage.aa_dist = 0;
        rage.aim_require_fire = 0;
        rage.aa_los_on = 1;
        rage.trigger_enabled = 1;
        rage.trigger_key = 0;
        rage.trigger_delay = 0;
        rage.trigger_fov = 8;
        rage.trigger_head_only = 0;
        rage.trigger_los = 1;
        rage.nospread = 1;
        config = &rage;
    }

    const ULONGLONG now64 = RR_AIM_NOW();
    const unsigned now = (unsigned)now64;
    unsigned elapsed = g_last_at ? (unsigned)(now64 - g_last_at) : 16u;
    if (!elapsed) elapsed = 1u;
    g_last_at = now64;

    const int held = config->rage ||
        (GetAsyncKeyState(config->aim_key) & 0x8000) != 0;
    const int firing = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;

    if (config->nospread) nospread_apply(players, count);

    int local_dead = 0;
    for (int i = 0; i < count; ++i)
        if (players[i].local) { local_dead = players[i].dead; break; }

    if (config->aim_enabled && held && !local_dead &&
        (!config->aim_require_fire || firing)) {
        const int fresh = g_lock_id && (now64 - g_lock_at) < 500;
        RR_AIM_PICK pick = rr_aim_pick(players, count, camera, width, height, config,
                                       fresh ? g_lock_addr : 0,
                                       fresh ? g_lock_id : 0,
                                       fresh ? g_lock_bone : -1,
                                       config->aa_los_on ? los_clear : NULL);
        if (!pick.player && fresh)
            pick = rr_aim_pick(players, count, camera, width, height, config,
                               0, 0, -1, config->aa_los_on ? los_clear : NULL);

        if (pick.player) {
            /* A new body invalidates the outstanding measurement, but the learned
             * sensitivity and latency stay: they are properties of the game. */
            if (pick.player->addr != g_lock_addr || pick.player->id != g_lock_id) {
                g_mag.pending = 0;
                g_mag.stable = 0;
                g_mag.timeout = 0;
                g_mag.first_motion = 0;
                g_mag.have_last_ang = 0;
                g_mag.have_prev_target = 0;
                g_mag.carry_x = g_mag.carry_y = 0.0f;
            }
            g_lock_addr = pick.player->addr;
            g_lock_id   = pick.player->id;
            g_lock_bone = pick.bone;
            g_lock_at   = now64;

            float yaw, pitch;
            rr_magnet_angles(camera, &pick.point, &yaw, &pitch);

            int mx, my;
            rr_magnet_step(&g_mag, yaw, pitch, pick.point,
                           config->aim_smooth, elapsed, &mx, &my);
            if (mx || my) {
                send_move(mx, my);
                InterlockedExchange(&g_active, 1);
            }

            g_frame.picked  = 1;
            g_frame.addr    = pick.player->addr;
            g_frame.id      = pick.player->id;
            g_frame.bone    = pick.bone;
            g_frame.point   = pick.point;
            g_frame.yaw     = yaw;
            g_frame.pitch   = pitch;
            g_frame.sent_x  = mx;
            g_frame.sent_y  = my;
            g_frame.dpc     = g_mag.dpc;
            g_frame.fov     = camera->fov;

            /* Legacy members for the host's existing diagnostics block. */
            g_frame.body    = pick.player->pos;
            g_frame.err_x   = pick.dx;
            g_frame.err_y   = pick.dy;
            {
                const float half = tanf(camera->fov * RR_PI / 360.0f);
                const float px_per_deg = (half > 0.0001f)
                    ? ((float)height * 0.5f / half) * (RR_PI / 180.0f)
                    : 0.0f;
                g_frame.px_x = g_mag.dpc * px_per_deg;
                g_frame.px_y = g_mag.dpc * px_per_deg;
            }
            g_frame.sign_x  = g_mag.sign_x;
            g_frame.sign_y  = g_mag.sign_y;
        } else {
            g_frame.picked = 0;
            g_lock_addr = 0; g_lock_id = 0; g_lock_bone = -1;
            g_mag.pending = 0;
            g_mag.have_last_ang = 0;
            g_mag.have_prev_target = 0;
            g_mag.carry_x = g_mag.carry_y = 0.0f;
        }
    } else {
        g_frame.picked = 0;
        g_lock_addr = 0; g_lock_id = 0; g_lock_bone = -1;
        g_mag.pending = 0;
        g_mag.have_last_ang = 0;
        g_mag.have_prev_target = 0;
    }

    const int trig_held = config->trigger_key
        ? (GetAsyncKeyState(config->trigger_key) & 0x8000) != 0
        : (held || config->rage);
    RR_AIM_PICK t = {0};
    if (config->trigger_enabled && trig_held && !firing && !local_dead)
        t = rr_trigger_pick(players, count, camera, width, height, config,
                            config->trigger_los ? los_fire : NULL);
    if (rr_trigger_ready(&g_trig, t.player ? t.player->addr : 0,
                         t.player ? t.player->id : 0, now, config->trigger_delay)) {
        INPUT click[2] = {0};
        click[0].type = click[1].type = INPUT_MOUSE;
        click[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        click[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        click[0].mi.dwExtraInfo = click[1].mi.dwExtraInfo = RR_GATE_MAGIC;
        if (SendInput(2, click, sizeof(INPUT)) == 1)
            SendInput(1, &click[1], sizeof(INPUT));
    }
}