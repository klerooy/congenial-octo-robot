/* rr_aim.c - Select target points and convert projected errors to mouse counts.
   Selection uses snapshot data; the caller decides whether to send input.
*/
#include "rr_aim.h"
#include "rr_recoil.h"
#include "rr_antiaim.h"
#include "rr_cham_anchors.h"
#include "rr_body_joints.h"
#include "menu_bridge.h"
#include <math.h>
#include <windows.h>
#include <string.h>
#include "rr_los.h"
#include "rr_mem.h"

#define RR_AIM_PI 3.14159265358979f
#define RR_TAN30  0.57735026919f   /* tan(30 deg): reference half-FOV */

/* Worker-period clamp: covers stalls down to 20 fps without letting a long
   pause produce a single huge correction. Verified: at 33 ms the old 24 ms
   clamp under-delivered gain (0.646 vs 0.761 configured). */
#define RR_STEP_DT_MAX 50u

/* Rage ring: a pixel radius wider than any screen diagonal, so "any radius"
 * needs no special case in the picker - the same ring test runs, it just never
 * rejects a target the camera can see. */
#define RR_AIM_RAGE_RING 100000

/* Millisecond clock for elapsed-time maths. The default is the 64-bit tick so
 * a 49.7-day session cannot wrap the follow filter; the offline harness
 * substitutes a deterministic clock through this macro. */
#ifndef RR_AIM_NOW
#define RR_AIM_NOW() GetTickCount64()
#endif

/* A body keeps its roster entry and its replicated transform for a while, so it
 * still projects to a valid aim point. Health is the signal that survives that
 * window: a known zero is a corpse, and the death flag is a server event that
 * can lag the health field by a tick or two. A failed read is -1 and stays a
 * candidate, otherwise one bad pointer would empty the target list. */
static int aim_alive(const RR_PLAYER* p) {
    return !p->dead && p->health != 0;
}

/* Carriers zeroed by the last nospread pass, for the host log: zero means the
 * cone was already shut or the chain did not resolve, non-zero is the write
 * count. */
static int g_nospread_writes;

int rr_nospread_writes(void) { return g_nospread_writes; }

/* Nospread: the client keeps its cone in two places - a FloatData behind
 * BattleCharacterPrivateModel.WeaponSpread and the PlayerFakeSpreadModel trio
 * that carries the movement and airborne penalties. Both are local values the
 * weapon recomputes, so writing zero into whichever one is non-zero holds the
 * cone shut. The snapshot reader supplies the addresses; a chain that did not
 * resolve leaves them zero and that carrier is skipped.
 *
 * Only the local player is touched, only when a value is actually open, and
 * a write is attempted once per frame per carrier - an already-closed cone
 * costs nothing, which is what keeps this off the driver's hot path. */
static int nospread_apply(const RR_PLAYER* players, int count) {
    static const float zero = 0.0f;
    static const float zero3[3] = {0.0f, 0.0f, 0.0f};
    int wrote = 0;
    if (!players || count <= 0) return 0;
    for (int i = 0; i < count; ++i) {
        const RR_PLAYER* p = &players[i];
        if (!p->local || p->dead) continue;
        if (p->spread_addr && p->has_spread_data && p->spread_value != 0.0f &&
            rr_write(p->spread_addr, &zero, sizeof zero))
            ++wrote;
        if (p->fake_spread_addr && p->has_fake_spread_data &&
            (p->spread_move != 0.0f || p->spread_air != 0.0f ||
             (!p->spread_addr && p->spread_value != 0.0f)) &&
            rr_write(p->fake_spread_addr, zero3, sizeof zero3))
            ++wrote;
    }
    g_nospread_writes = wrote;
    return wrote;
}

RR_AIM_PICK rr_aim_pick(const RR_PLAYER* players, int count, const RR_CAMERA* camera,
                        int width, int height, const BR_Config* config,
                        uint64_t locked_addr, int32_t locked_id,
                        int (*clear)(RR_V3, RR_V3)) {
    RR_AIM_PICK best = {0};
    if (!players || count <= 0 || !camera || !camera->valid || !config ||
        width < 16 || height < 16) return best;
    if (count > RR_MAX_PLAYERS) count = RR_MAX_PLAYERS;

    float best_priority = INFINITY, best_d2 = INFINITY;
    const float cx = width * 0.5f, cy = height * 0.5f;
    const float radius = (float)(config->aim_fov > 0 ? config->aim_fov : 150);
    /* A held lock widens the ring instead of removing it: a target that leaves
     * the screen is dropped and re-acquired on the next frame rather than
     * dragging the view after it. */
    const float ring = locked_addr ? radius * 2.0f : radius;
    const float limit = ring * ring;
    const float max_dist = (float)config->aa_dist;
    const int group = config->aa_bone;
    const int want_clan   = config->aim_targets & RR_REL_CLAN;
    const int want_police = config->aim_targets & RR_REL_POLICE;
    const int want_party  = config->aim_targets & RR_REL_PARTY;

    for (int i = 0; i < count; ++i) {
        const RR_PLAYER* p = &players[i];
        /* Cheapest discriminators first: identity, then flags, then floats. */
        if (locked_addr && (p->addr != locked_addr || p->id != locked_id)) continue;
        if (!p->addr || !p->valid || p->local || !aim_alive(p) ||
            p->is_vehicle || p->is_loot || p->hidden_in_object) continue;
        if (!rr_relationship_allowed(p->relationships, want_clan, want_police, want_party))
            continue;
        if (!isfinite(p->dist) || (max_dist > 0.0f && p->dist > max_dist)) continue;

        RR_V3 point;
        float sx, sy;
        if (!rr_aim_point(p, group, camera, width, height, &point) ||
            !game_w2s(camera, &point, width, height, &sx, &sy)) continue;
        float dx = sx - cx, dy = sy - cy;
        float d2 = dx * dx + dy * dy;
        if (!isfinite(d2) || d2 > limit) continue;

        /* Keep acquisition outside the FOV cheap. If the nearest member is
         * blocked, search the same area for another visible joint. */
        if (clear && !clear(camera->pos, point)) {
            if (!rr_aim_visible_point(p, group, camera, width, height, &point, clear) ||
                !game_w2s(camera, &point, width, height, &sx, &sy)) continue;
            dx = sx - cx; dy = sy - cy; d2 = dx * dx + dy * dy;
            if (!isfinite(d2) || d2 > limit) continue;
        }

        const float priority = config->aa_closest_first ? p->dist : d2;
        if (priority < best_priority || (priority == best_priority && d2 < best_d2)) {
            best = (RR_AIM_PICK){p, point, dx, dy};
            best_priority = priority;
            best_d2 = d2;
        }
        if (locked_addr) break;
    }
    return best;
}

/* Time constant of the exponential follow, in ms, for a 16 ms reference frame.
   smooth is the fraction of the error removed per reference frame. */
static float aim_response_ms(int smooth) {
    const float alpha = fmaxf(0.01f, fminf(1.0f, smooth / 100.0f));
    return alpha >= 1.0f ? 0.0f : -16.0f / logf(1.0f - alpha);
}

static float aim_gain(float response_ms, unsigned elapsed_ms) {
    const float dt = (float)(elapsed_ms > RR_STEP_DT_MAX ? RR_STEP_DT_MAX : elapsed_ms);
    return response_ms > 0.0f ? 1.0f - expf(-dt / response_ms) : 1.0f;
}

int rr_aim_motion_point(RR_V3 point, RR_V3 position, RR_V3 previous,
                        unsigned sample_ms, unsigned age_ms, int smooth,
                        unsigned elapsed_ms, RR_V3* out) {
    if (!out || sample_ms < 8 || sample_ms > 120 || age_ms > 80) return 0;
    const float vx = (position.x - previous.x) * 1000.0f / sample_ms;
    const float vz = (position.z - previous.z) * 1000.0f / sample_ms;
    if (!isfinite(vx) || !isfinite(vz)) return 0;
    const float speed = hypotf(vx, vz);
    if (!(speed >= 0.2f && speed <= 20.0f)) return 0;

    /* The worker corrects discretely: per frame it removes gain*error, so a
     * constant-velocity target trails by v*(dt/gain - age) unless the aim
     * point is shifted by exactly that amount. Signed: when the snapshot age
     * already exceeds the follow lag, aim slightly behind the stale point.
     * Bounds: at most 200 ms ahead (direction-change error cap) and never
     * further back than the snapshot age itself.
     * Verified in simulation: zero steady-state residual for smooth 10..99
     * at worker periods 8/16/33 ms. */
    const float gain = aim_gain(aim_response_ms(smooth), elapsed_ms);
    float lead_ms = (gain > 0.0f ? (float)(elapsed_ms > RR_STEP_DT_MAX ?
                                           RR_STEP_DT_MAX : elapsed_ms) / gain : 0.0f)
                    - (float)age_ms;
    lead_ms = fmaxf(-(float)age_ms, fminf(lead_ms, 200.0f));
    const float seconds = lead_ms * 0.001f;
    *out = (RR_V3){ point.x + vx * seconds, point.y, point.z + vz * seconds };
    return 1;
}

/* One axis of bounded steering. Hysteresis: enter the deadzone below 0.35
 * reference-scaled units, leave it only above 0.50, so sensor noise at the
 * boundary cannot chatter the output between 0 and 1 count. */
static int aim_axis(float error, float gain, float limit, float flick_cap,
                    float* carry, unsigned char* inside) {
    const float dz = *inside ? 0.50f : 0.35f;
    if (fabsf(error) < dz) {
        *carry = 0.0f;
        *inside = 1;
        return 0;
    }
    *inside = 0;
    if (error * *carry < 0.0f) *carry = 0.0f;
    /* A large correction must not be rate-limited by the smoothing limit, or
     * the first frames after a flick stay behind a fast target. The flick cap
     * is scaled by calibration so it bounds the same angular jump at any
     * sensitivity. */
    const float need = fabsf(error) * 0.5f;
    if (need > limit) limit = need < flick_cap ? need : flick_cap;
    const float step = fmaxf(-limit, fminf(limit, error * gain + *carry));
    const int counts = (int)step;
    *carry = step - counts;
    return counts;
}

void rr_aim_step(RR_AIM_STEER* state, float dx, float dy, float fov, int height,
                 int smooth, int cal, unsigned elapsed_ms, int firing,
                 int* mx, int* my) {
    *mx = *my = 0;
    if (!state || !isfinite(dx) || !isfinite(dy) ||
        !(fov > 10.0f && fov < 170.0f) || height < 16) {
        if (state) *state = (RR_AIM_STEER){0};
        return;
    }

    /* Project to the equivalent error at 1080p / 60 degrees, then apply the
     * sensitivity calibration. Both factors are required: dropping either
     * makes the correction depend on resolution or zoom. Verified: identical
     * count sequences for equal angular errors across resolutions/FOVs. */
    const float projection = (1080.0f / height) *
                             (tanf(fov * RR_AIM_PI / 360.0f) / RR_TAN30);
    const float calib = (cal > 0 ? (float)cal : 100.0f) / 100.0f;
    const float scale = projection * calib;

    /* Per-step limits represent similar angular corrections at any sens. */
    const float limit = fmaxf(8.0f, 32.0f * calib);
    const float flick_cap = 160.0f * calib;

    const float response = aim_response_ms(smooth);
    const float gain = aim_gain(response, elapsed_ms);
    const float vertical_gain = (firing && response > 0.0f)
        ? aim_gain(response * 0.8f, elapsed_ms) : gain;

    *mx = aim_axis(dx * scale, gain, limit, flick_cap,
                   &state->carry_x, &state->inside_x);
    *my = aim_axis(dy * scale, vertical_gain, limit, flick_cap,
                   &state->carry_y, &state->inside_y);
}

int rr_aim_bone_in_group(int group, int bone) {
    if (bone < 0 || bone >= RR_CHAM_BONES) return 0;
    switch (group) {
    case RR_AIM_HEAD:   return bone >= RR_RIG_M_HEAD_1_JNT && bone <= RR_RIG_M_TONGUE_3_JNT;
    case RR_AIM_NECK:   return bone == RR_RIG_M_NECK_JNT ||
                               bone == RR_RIG_L_ARM_0_JNT || bone == RR_RIG_R_ARM_0_JNT;
    case RR_AIM_CHEST:  return bone == RR_RIG_M_SPINE_2_JNT || bone == RR_RIG_M_SPINE_3_JNT;
    case RR_AIM_PELVIS: return bone == RR_RIG_M_SPINE_1_JNT ||
                               bone == RR_RIG_L_LEG_1_JNT || bone == RR_RIG_R_LEG_1_JNT;
    case RR_AIM_ARMS:   return (bone >= RR_RIG_L_ARM_0_JNT && bone <= RR_RIG_L_ARM_3_JNT) ||
                               (bone >= RR_RIG_R_ARM_0_JNT && bone <= RR_RIG_R_ARM_3_JNT) ||
                               bone == RR_RIG_L_ARMTWIST_JNT || bone == RR_RIG_R_ARMTWIST_JNT;
    case RR_AIM_LEGS:   return (bone >= RR_RIG_L_LEG_1_JNT && bone <= RR_RIG_L_LEG_3_JNT) ||
                               (bone >= RR_RIG_R_LEG_1_JNT && bone <= RR_RIG_R_LEG_3_JNT);
    case RR_AIM_FACE:   return bone >= RR_RIG_L_EYELIDUP_JNT && bone <= RR_RIG_M_TONGUE_3_JNT;
    case RR_AIM_HANDS:  return (bone >= RR_RIG_L_ARM_3_JNT && bone <= RR_RIG_L_THUMB_3_JNT) ||
                               (bone >= RR_RIG_R_ARM_3_JNT && bone <= RR_RIG_R_THUMB_3_JNT);
    case RR_AIM_FEET:   return (bone >= RR_RIG_L_LEG_3_JNT && bone <= RR_RIG_L_FOOT_2_JNT) ||
                               (bone >= RR_RIG_R_LEG_3_JNT && bone <= RR_RIG_R_FOOT_2_JNT);
    case RR_AIM_CLOSEST: return rr_rig_is_body_joint(bone);
    default: return 0;
    }
}

/* Shared sanity gate for a candidate world point: finite and within 6 m of
 * the character origin (squared bound 36). */
static int point_near_origin(RR_V3 point, RR_V3 origin) {
    const float dx = point.x - origin.x, dy = point.y - origin.y, dz = point.z - origin.z;
    return isfinite(dx) && isfinite(dy) && isfinite(dz) &&
           dx * dx + dy * dy + dz * dz <= 36.0f;
}

/* Cheap behind-camera rejection before the full projection. Sign is valid
 * for an unnormalized forward vector. */
static int point_in_front(const RR_CAMERA* cam, RR_V3 point) {
    const float d = (point.x - cam->pos.x) * cam->fwd.x +
                    (point.y - cam->pos.y) * cam->fwd.y +
                    (point.z - cam->pos.z) * cam->fwd.z;
    return d > 0.05f;
}

static int full_rig_point(const RR_PLAYER* p, int group, const RR_CAMERA* cam,
                          int width, int height, RR_V3* out,
                          int (*clear)(RR_V3, RR_V3)) {
    if (!cam || !cam->valid || width < 1 || height < 1 || p->nbones != RR_CHAM_BONES)
        return 0;
    const float cx = width * 0.5f, cy = height * 0.5f;
    int found = 0;
    float best = INFINITY;
    for (int bone = 0; bone < RR_CHAM_BONES; ++bone) {
        if (!rr_aim_bone_in_group(group, bone)) continue;
        const RR_V3 point = {p->bone[bone][0], p->bone[bone][1], p->bone[bone][2]};
        if (!point_near_origin(point, p->pos) || !point_in_front(cam, point)) continue;
        float x, y;
        if (!game_w2s(cam, &point, width, height, &x, &y) ||
            !isfinite(x) || !isfinite(y)) continue;
        const float sx = x - cx, sy = y - cy;
        const float score = sx * sx + sy * sy;
        if (!(score < best) || (clear && !clear(cam->pos, point))) continue;
        *out = point; best = score; found = 1;
    }
    return found;
}

static int mesh_anchor(const RR_PLAYER* p, int index, RR_V3* out) {
    if (!p->cham_valid || index < 0 || index >= 12) return 0;
    const RR_CHAM_ANCHOR* a = &rr_cham_anchors[index];
    const float* m = p->cham_skin[a->bone];
    out->x = m[0]*a->p[0] + m[1]*a->p[1] + m[2]*a->p[2] + m[3];
    out->y = m[4]*a->p[0] + m[5]*a->p[1] + m[6]*a->p[2] + m[7];
    out->z = m[8]*a->p[0] + m[9]*a->p[1] + m[10]*a->p[2] + m[11];
    return point_near_origin(*out, p->pos);
}

static RR_V3 group_point(const RR_PLAYER* p, int group) {
    RR_V3 joint;
    if (mesh_anchor(p, group, &joint)) return joint;
    const int idx = p->bone_group[group];
    if (p->nbones > 0 && p->nbones <= RR_MAX_BONES && idx >= 0 && idx < p->nbones) {
        return (RR_V3){p->bone[idx][0], p->bone[idx][1], p->bone[idx][2]};
    }
    /* When the game has not created the skeleton, estimate from the character
     * origin. These are position estimates, not measured bone transforms. */
    static const float standing[4] = {1.65f, 1.48f, 1.20f, 0.88f};
    static const float crouched[4] = {1.00f, 0.90f, 0.72f, 0.48f};
    RR_V3 v = p->pos;
    v.y += p->crouch == 1 ? crouched[group] : standing[group];
    return v;
}

static int limb_point(const RR_PLAYER* p, int group, const RR_CAMERA* cam,
                      int width, int height, RR_V3* out,
                      int (*clear)(RR_V3, RR_V3)) {
    const int start = group == RR_AIM_ARMS ? 4 : 8;
    const float cx = width * 0.5f, cy = height * 0.5f;
    int found = 0;
    float best = INFINITY;
    for (int side = 0; side < 2; ++side) {
        RR_V3 a, b;
        if (!mesh_anchor(p, start + side * 2, &a) ||
            !mesh_anchor(p, start + side * 2 + 1, &b)) continue;
        const RR_V3 mid = {(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f};
        if (!point_in_front(cam, mid)) continue;
        float x, y;
        if (!game_w2s(cam, &mid, width, height, &x, &y)) continue;
        const float dx = x - cx, dy = y - cy;
        const float dist = dx * dx + dy * dy;
        if (dist < best && (!clear || clear(cam->pos, mid))) {
            *out = mid; best = dist; found = 1;
        }
    }
    return found;
}

int rr_aim_visible_point(const RR_PLAYER* p, int group, const RR_CAMERA* cam,
                         int width, int height, RR_V3* out,
                         int (*clear)(RR_V3, RR_V3)) {
    if (!p || !out || (clear && !cam)) return 0;
    if (group < 0 || group >= RR_AIM_GROUP_COUNT) group = RR_AIM_HEAD;
    if (p->full_rig) return full_rig_point(p, group, cam, width, height, out, clear);
    if (group >= RR_AIM_FACE) return 0;
    if (group >= RR_AIM_ARMS) return limb_point(p, group, cam, width, height, out, clear);
    if (group != RR_AIM_CLOSEST) {
        *out = group_point(p, group);
        /* A joint the game has not filled is NaN. Reporting it as a selected
         * point pushed the NaN one call further, where the projection dropped it
         * silently; a point that cannot be projected is not a point. */
        if (!isfinite(out->x) || !isfinite(out->y) || !isfinite(out->z)) return 0;
        if (!clear) return 1;
        return clear(cam->pos, *out);
    }
    int found = 0;
    float best = INFINITY;
    for (int i = 0; i < 6; ++i) {
        RR_V3 point;
        float x, y;
        if (i < 4) point = group_point(p, i);
        else if (!limb_point(p, i + 1, cam, width, height, &point, clear)) continue;
        if (!game_w2s(cam, &point, width, height, &x, &y)) continue;
        const float dx = x - width * 0.5f, dy = y - height * 0.5f;
        const float d = dx * dx + dy * dy;
        if (d < best && (i >= 4 || !clear || clear(cam->pos, point))) {
            *out = point; best = d; found = 1;
        }
    }
    return found;
}

int rr_aim_point(const RR_PLAYER* p, int group, const RR_CAMERA* cam,
                 int width, int height, RR_V3* out) {
    return rr_aim_visible_point(p, group, cam, width, height, out, NULL);
}

/* Input and trigger state belongs to the single input worker. */
static volatile LONG g_aim_active;
static RR_AIM_STEER g_input_steer;
static RR_TRIGGER_STATE g_trigger_state;
static RR_RECOIL_STATE g_recoil_state;
static RR_ANTIAIM_STATE g_antiaim_state;
static ULONGLONG g_input_at;
/* Sticky aim lock: the player the steering is currently committed to, with the
 * tick it was last confirmed on. Re-picking every frame makes the crosshair
 * jump between two bodies that straddle the same screen distance. */
static uint64_t g_aim_lock_addr;
static int32_t g_aim_lock_id;
static ULONGLONG g_aim_lock_at;

/* Instant lock. At full smoothing the worker does not blend a fraction of the
 * error per frame: it computes the counts that move the crosshair onto the
 * point this frame. The game's yaw-per-count cannot be read from any struct we
 * own, and guessing it is what makes a "snap" feel slow - a seed that is half
 * the real sensitivity needs twice the frames, and the per-step ceiling caps it
 * further. So the gain is measured: the error change the previous move produced
 * divided by the counts that were sent is pixels-per-count, directly. The seed
 * only has to point the right way; the first reply replaces it.
 *
 * Two things a single-frame loop gets wrong and this one does not:
 *
 * - The first move is a probe, not a correction. Against a seed half the real
 *   sensitivity the old first move threw the view past the body and the ten
 *   frames of gain averaging that followed rang around it, which reads as "the
 *   aim jumps". A probe is bounded, its measured gain is adopted whole, and the
 *   frame after it is already a full correction.
 * - The move is sized for the error that will remain once the input already in
 *   flight lands. A camera that damps (a Cinemachine blend, a frame of input
 *   latency) shows only part of a correction next frame; counting that as "the
 *   move failed" and sending a fresh full correction every frame is what turns
 *   a damped camera into a runaway oscillation.
 *
 * RR_AIM_SNAP_SMOOTH is the smoothing value that selects this path, so the
 * existing slider keeps its meaning and the profile files stay compatible. */
#define RR_AIM_SNAP_SMOOTH 100
#define RR_AIM_SNAP_MAX    3000.0f
#define RR_AIM_SNAP_TAU_MS 12.0f   /* gain-averaging time constant */
#define RR_AIM_SNAP_MIN_SENT 6     /* smaller moves say nothing about the gain */
#define RR_AIM_SNAP_PEND_MAX 2000.0f /* in-flight pixels: windup ceiling */
#define RR_AIM_SNAP_STARVE 10      /* silent frames before a move is written off */

/* One axis of the instant lock: the learned gain, the direction the axis
 * actually turns, and the input still in flight. Kept per axis because a game
 * can invert the vertical axis and leave the horizontal alone. */
typedef struct {
    float    px;       /* learned screen pixels per count */
    int      valid;    /* a measured sample backs px */
    int      sign;     /* +1 when positive counts shrink a positive error */
    int      sent;     /* counts the last committed move asked for */
    float    err;      /* error that move was sized from */
    float    pend;     /* counts in flight */
    unsigned starve;   /* consecutive frames a move has gone unanswered */
    unsigned wrong;    /* consecutive moves that moved the error the wrong way */
    float    wrong_px; /* how far those moves were wrong, in pixels */
} SNAP_AXIS;

static SNAP_AXIS g_snap_x = { 0.35f, 0, 1, 0, 0.0f, 0.0f, 0, 0, 0.0f };
static SNAP_AXIS g_snap_y = { 0.35f, 0, 1, 0, 0.0f, 0.0f, 0, 0, 0.0f };
static float    g_snap_fov = -1.0f;
static uint64_t g_snap_addr;
static int32_t  g_snap_id;
static RR_AIM_FRAME g_snap_last;   /* host log snapshot */

static int aim_clear_path(RR_V3 start, RR_V3 end) {
    /* Fail open on UNKNOWN. The query answers UNKNOWN while the collision worker
     * has no fresh snapshot - the first second of a session, a map change, a
     * stalled worker - and treating that as "blocked" silences the whole picker,
     * which is indistinguishable from an aimbot that does not work. Only a
     * measured BLOCKED excludes a target from steering. */
    return rr_los_check(start, end) != RR_LOS_BLOCKED;
}

/* Firing is the strict side of the same query: a shot is worth a bullet only
 * when the line was actually measured clear. */
static int aim_fire_path(RR_V3 start, RR_V3 end) {
    return rr_los_check(start, end) == RR_LOS_CLEAR;
}

RR_AIM_PICK rr_trigger_pick(const RR_PLAYER* players, int count, const RR_CAMERA* camera,
                            int width, int height, const BR_Config* config,
                            int (*clear)(RR_V3, RR_V3)) {
    RR_AIM_PICK best = {0};
    if (!players || count <= 0 || !camera || !camera->valid || !config ||
        width < 16 || height < 16)
        return best;
    if (count > RR_MAX_PLAYERS) count = RR_MAX_PLAYERS;

    /* The crosshair is a pixel, not a ray. Comparing projections keeps the test
     * the same at every range: a joint centre twelve pixels off the crosshair is
     * a hit whether the target stands at 5 m or 90 m, which is what the player
     * sees and what the weapon actually converges on. The old metre-radius test
     * on bare joint centres needed a near-perfect pixel hit and went dead as
     * soon as the crosshair sat on an arm or a shoulder. */
    const float cx = width * 0.5f, cy = height * 0.5f;
    float tolerance = (float)(config->trigger_fov > 0 ? config->trigger_fov : 10);
    if (tolerance > 80.0f) tolerance = 80.0f;
    float best_d2 = tolerance * tolerance;
    float best_dist = INFINITY;

    const float max_dist = (float)config->aa_dist;
    const int want_clan   = config->trigger_targets & RR_REL_CLAN;
    const int want_police = config->trigger_targets & RR_REL_POLICE;
    const int want_party  = config->trigger_targets & RR_REL_PARTY;
    const int last_group  = config->trigger_head_only ? RR_AIM_HEAD : RR_AIM_PELVIS;

    for (int i = 0; i < count; ++i) {
        const RR_PLAYER* p = &players[i];
        if (!p->addr || !p->valid || p->local || !aim_alive(p) || p->hidden_in_object ||
            p->is_vehicle || p->is_loot ||
            !isfinite(p->dist) || (max_dist > 0.0f && p->dist > max_dist) ||
            !rr_relationship_allowed(p->relationships, want_clan, want_police, want_party))
            continue;
        for (int group = RR_AIM_HEAD; group <= last_group; ++group) {
            RR_V3 point;
            float sx, sy;
            if (!rr_aim_point(p, group, camera, width, height, &point) ||
                !game_w2s(camera, &point, width, height, &sx, &sy)) continue;
            const float dx = sx - cx, dy = sy - cy;
            const float d2 = dx * dx + dy * dy;
            if (!isfinite(d2) || d2 > best_d2) continue;
            /* Only the point that could actually be fired at pays for a
             * geometry trace. */
            if (clear && !clear(camera->pos, point)) continue;
            /* Closest to the crosshair wins; a draw goes to the nearer body so
             * two overlapping players resolve to the one in front. */
            if (d2 < best_d2 - 0.25f || (d2 <= best_d2 + 0.25f && p->dist < best_dist)) {
                best = (RR_AIM_PICK){p, point, dx, dy};
                best_d2 = d2;
                best_dist = p->dist;
            }
        }
    }
    return best;
}

int rr_trigger_ready(RR_TRIGGER_STATE* state, uint64_t addr, int32_t id,
                     unsigned now, int delay_ms) {
    if (!state) return 0;
    if (!addr) { memset(state, 0, sizeof(*state)); return 0; }
    const unsigned delay = delay_ms < 0 ? 0u : delay_ms > 300 ? 300u : (unsigned)delay_ms;
    if (!state->tracked || state->addr != addr || state->id != id) {
        memset(state, 0, sizeof(*state));
        state->tracked = 1; state->addr = addr; state->id = id; state->acquired_at = now;
    }
    /* Unsigned subtraction stays correct across the 32-bit tick wrap. */
    if (now - state->acquired_at < delay ||
        (state->fired && now - state->last_shot < (delay > 16 ? delay : 16))) return 0;
    state->last_shot = now;
    state->fired = 1;
    return 1;
}

/* Ends a measurement sequence: the next move is sized from the error alone. */
static void snap_break(SNAP_AXIS* a) {
    a->sent = 0;
    a->err = 0.0f;
    a->pend = 0.0f;
    a->starve = 0;
    a->wrong = 0;
    a->wrong_px = 0.0f;
}

static void aim_reset_worker_state(void) {
    memset(&g_input_steer, 0, sizeof(g_input_steer));
    memset(&g_trigger_state, 0, sizeof(g_trigger_state));
    rr_recoil_reset(&g_recoil_state);
    /* The anti-aim is deliberately not reset here. This routine runs on every
     * gated frame (menu open, no snapshot, no focus), and wiping the applied
     * offset there is what made the view jump: the next live frame measured
     * the wanted offset from zero and threw the crosshair again. The offset is
     * walked back by rr_antiaim_return instead, which needs it kept. */
    g_aim_lock_addr = 0;
    g_aim_lock_id = 0;
    g_aim_lock_at = 0;
    g_input_at = 0;
    snap_break(&g_snap_x);
    snap_break(&g_snap_y);
    g_snap_addr = 0;
    g_snap_id = 0;
    g_snap_fov = -1.0f;
}

/* One axis of the instant lock. Learns the axis gain from the move the previous
 * frame sent, then asks for exactly the counts the remaining error needs.
 *
 * Learning rules: a move that shrank the error in the direction it was sent
 * measures the gain; a move large enough that it should have moved the error but
 * did not means the seed was too optimistic and is halved. A deadzone that scales
 * with the measured gain keeps a high-sensitivity game from chattering one count
 * back and forth across a sub-pixel error.
 *
 * Two guards keep a bad sample out of the estimate: a non-finite error is
 * dropped before anything is written, and the averaging weight follows elapsed
 * time instead of the frame count, so the loop behaves the same at 60 and at
 * 240 fps. */
/* The instant lock's probe. A fresh sequence does not know the game's
 * sensitivity, and the first move in the measured task was a full correction
 * against a guessed seed: with the game twice as sensitive as the seed the view
 * flew a hundred pixels past the body and came back, which is what a player
 * reports as "the aim jumps". So the first move of a sequence is a probe: half
 * of what the seed asks for, and never more than half the error at the highest
 * sensitivity a first-person game ships (three screen pixels per count). The
 * gain measured from the probe is adopted whole, and the next frame is a full
 * correction, so a correct seed still locks in one frame and a wrong one costs
 * one extra frame instead of a swing.
 *
 * The learned gain is also rescaled, not discarded, when the camera zooms: the
 * game's angular sensitivity does not change, only how many pixels a degree is
 * worth. */
#define RR_AIM_SNAP_PX_MAX 3.0f    /* screen pixels per count, high-sens bound */
#define RR_AIM_SNAP_PROBE  0.5f    /* fraction of the error the first move asks */
#define RR_AIM_SNAP_PX_MIN 0.02f   /* floor: a count always buys something */
#define RR_AIM_SNAP_FOV_STEP 1.0f  /* degrees; below this the zoom is camera noise */

static float snap_alpha(unsigned dt_ms) {
    float alpha;
    if (!dt_ms) dt_ms = 16;
    alpha = 1.0f - expf(-(float)dt_ms / RR_AIM_SNAP_TAU_MS);
    /* A 240 fps loop must not take four times as long in real time to learn the
     * gain as a 60 fps one, and a stalled clock must not stop the learning
     * altogether: the floor keeps the lock a two-frame affair at any rate. */
    if (!(alpha > 0.30f)) alpha = 0.30f;
    if (alpha > 0.85f) alpha = 0.85f;
    return alpha;
}

/* One axis of the instant lock: learn the gain and the direction from what the
 * last move actually did, then ask for exactly the counts the error still needs.
 *
 * A move that shrank the error the way it was told measures the gain. A move
 * that shifted nothing while it should have bought several pixels halves the
 * seed. A move that shifted the error the *other* way is not a gain problem at
 * all - the axis turns backwards - and two of those flip it. */
static int snap_axis(SNAP_AXIS* a, float err, unsigned dt_ms) {
    if (!a || !isfinite(a->px) || !isfinite(err)) return 0;
    const float s = (float)a->sign;

    if (isfinite(a->err)) {
        /* What arrived since the last committed move, in counts of the direction
         * this axis really turns. A move that is still arriving is not a move
         * that failed. The in-flight figure is kept in counts, so a gain learned
         * later in the same frame applies to the counts already sent. */
        float moved = a->err - err;
        if (!isfinite(moved)) moved = 0.0f;
        const float landed = moved * s / a->px;
        if (a->pend > 0.0f && landed > 0.0f) a->pend = fmaxf(0.0f, a->pend - landed);
        else if (a->pend < 0.0f && landed < 0.0f) a->pend = fminf(0.0f, a->pend - landed);
        /* A move nothing ever answers (input swallowed by another window, the
         * game paused) must not starve the loop forever. */
        if (fabsf(a->pend * a->px) > 1.0f && fabsf(moved) < 0.1f) {
            if (++a->starve > RR_AIM_SNAP_STARVE) { a->pend = 0.0f; a->starve = 0; }
        } else {
            a->starve = 0;
        }

        if (a->sent && (a->sent >= RR_AIM_SNAP_MIN_SENT || a->sent <= -RR_AIM_SNAP_MIN_SENT)) {
            const float expect = a->px * (float)a->sent;
            const float eff = moved * s * (float)a->sent;   /* > 0: went where told */
            if (eff > 0.0f && fabsf(moved) > 0.4f) {
                a->wrong = 0; a->wrong_px = 0.0f;
                const float gain = fabsf(moved) / fabsf((float)a->sent);
                if (gain > 0.01f && gain < 16.0f) {
                    /* The first measured sample is the truth about the game, not
                     * a vote: averaging it into a guessed seed is what made a
                     * wrong seed ring for ten frames. */
                    if (!a->valid) { a->px = gain; a->valid = 1; }
                    else a->px += snap_alpha(dt_ms) * (gain - a->px);
                }
            } else if (eff < 0.0f) {
                /* The counts were answered against the direction they asked for,
                 * so whatever the reason they are not still in flight. */
                a->pend = 0.0f;
                if (!a->valid && a->err * err > 0.0f && fabsf(err) > fabsf(a->err)) {
                    /* The error grew, on the same side, the wrong way. A zeroed
                     * or inverted axis (a player with "invert Y" on, a pointer
                     * feed that reports screen deltas) turns every correction
                     * into a doubling of the error; reading that as "the gain is
                     * too high" halves the gain, sends four times the counts, and
                     * walks the crosshair off the body and then off the screen.
                     * Two moves that both grew the error the wrong way flip the
                     * axis instead.
                     *
                     * Only until a correct-direction sample exists: once the gain
                     * has been measured the sign is proven, and an error that
                     * crosses the target is an overshoot, not an inversion. */
                    a->wrong_px += fabsf(err) - fabsf(a->err);
                    if (++a->wrong >= 2 &&
                        (a->wrong_px > 6.0f || (a->wrong >= 3 && a->wrong_px > 1.0f))) {
                        a->sign = -a->sign;
                        a->wrong = 0; a->wrong_px = 0.0f;
                        a->pend = 0.0f; a->sent = 0; a->err = 0.0f;
                    }
                } else {
                    a->wrong = 0; a->wrong_px = 0.0f;
                }
            } else if (isfinite(expect) && fabsf(expect) >= 3.0f &&
                       fabsf(err) > fabsf(a->err) - 0.5f && a->err * err > 0.0f) {
                /* Nothing arrived at all: the seed is too optimistic. */
                a->wrong = 0; a->wrong_px = 0.0f;
                a->px *= 0.5f;
            } else {
                a->wrong = 0; a->wrong_px = 0.0f;
            }
        }
    }

    if (!isfinite(a->px)) { a->px = 0.35f; a->valid = 0; }
    if (a->px < RR_AIM_SNAP_PX_MIN) a->px = RR_AIM_SNAP_PX_MIN;
    if (a->px > 16.0f) a->px = 16.0f;
    if (!isfinite(a->pend)) { a->pend = 0.0f; a->starve = 0; }
    const float pend_max = RR_AIM_SNAP_PEND_MAX / a->px;
    if (a->pend > pend_max) a->pend = pend_max;
    if (a->pend < -pend_max) a->pend = -pend_max;

    /* The correction is for what will still be wrong once the input in flight
     * lands, not for the error this frame happens to show. */
    const float want = err - a->pend * a->px * s;
    const float deadzone = a->px * 0.75f > 0.6f ? a->px * 0.75f : 0.6f;
    if (fabsf(want) < deadzone) return 0;

    float counts = want / a->px * s;
    if (!a->valid) {
        const float probe = fabsf(want) / RR_AIM_SNAP_PX_MAX * RR_AIM_SNAP_PROBE;
        if (fabsf(counts) > probe) counts = counts > 0.0f ? probe : -probe;
        /* A probe smaller than one count never leaves the buffer, so the error
         * would never be measured: one count is the smallest useful probe. */
        if (fabsf(counts) < 1.0f) counts = counts > 0.0f ? 1.0f : -1.0f;
    }
    if (counts > RR_AIM_SNAP_MAX) counts = RR_AIM_SNAP_MAX;
    if (counts < -RR_AIM_SNAP_MAX) counts = -RR_AIM_SNAP_MAX;
    if (!isfinite(counts)) return 0;
    int n = (int)(counts < 0.0f ? ceilf(counts) : floorf(counts));
    if (!n && fabsf(counts) >= 0.5f) n = counts > 0.0f ? 1 : -1;
    return n;
}

/* Sizes one frame of the instant lock against the picked point. The counts are
 * recorded here and only committed by rr_aim_snap_commit once SendInput has
 * accepted them: a refused input must not be mistaken for a move that failed to
 * move the view, or the learned gain would collapse.
 *
 * A target switch ends a measurement sequence but keeps the learned gain and
 * direction: the drop between the two frames is a change of body, not a reply to
 * the move. A zoom keeps both and rescales the gain, because a count still turns
 * the same angle while a degree is worth a different number of pixels. */
static void rr_aim_snap_step(uint64_t addr, int32_t id, float fov, float dx, float dy,
                             unsigned dt_ms, int* mx, int* my) {
    const int fov_ok = isfinite(fov) && fov > 10.0f && fov < 170.0f;
    if (addr != g_snap_addr || id != g_snap_id) {
        snap_break(&g_snap_x);
        snap_break(&g_snap_y);
        g_snap_addr = addr;
        g_snap_id = id;
    } else if (fov_ok && g_snap_fov > 0.0f &&
               fabsf(fov - g_snap_fov) > RR_AIM_SNAP_FOV_STEP) {
        const float a = tanf(g_snap_fov * RR_AIM_PI / 360.0f);
        const float b = tanf(fov * RR_AIM_PI / 360.0f);
        if (a > 0.0f && b > 0.0f && isfinite(a) && isfinite(b)) {
            g_snap_x.px *= a / b;
            g_snap_y.px *= a / b;
        }
        g_snap_x.sent = 0;
        g_snap_x.err = 0.0f;
        g_snap_y.sent = 0;
        g_snap_y.err = 0.0f;
    }
    if (fov_ok) g_snap_fov = fov;
    *mx = snap_axis(&g_snap_x, dx, dt_ms);
    *my = snap_axis(&g_snap_y, dy, dt_ms);
}

static void rr_aim_snap_commit(int mx, int my, float dx, float dy) {
    /* The counts are now in flight; the next snapshots will show them landing. */
    g_snap_x.pend += (float)mx;
    g_snap_y.pend += (float)my;
    g_snap_x.sent = mx;
    g_snap_y.sent = my;
    g_snap_x.err = dx;
    g_snap_y.err = dy;
}

static void rr_aim_snap_drop(void) {
    /* The counts in flight still land: only the reply they would have produced
     * is gone, because the next frame may show a different body. */
    g_snap_x.sent = 0;
    g_snap_x.err = 0.0f;
    g_snap_y.sent = 0;
    g_snap_y.err = 0.0f;
}

void rr_aim_frame_stats(RR_AIM_FRAME* out) {
    if (out) *out = g_snap_last;
}

static void send_mouse_move(int dx, int dy) {
    INPUT move = {0};
    move.type = INPUT_MOUSE;
    move.mi.dwFlags = MOUSEEVENTF_MOVE;
    move.mi.dx = dx;
    move.mi.dy = dy;
    SendInput(1, &move, sizeof(move));
}

void rr_aim_update(const RR_PLAYER* players, int count, const RR_CAMERA* camera,
                   int width, int height) {
    InterlockedExchange(&g_aim_active, 0);
    BR_Config* config = br_get();
    if (!config || !players || count <= 0 || !camera || !camera->valid ||
        width < 16 || height < 16 || config->menu_open) {
        aim_reset_worker_state();
        return;
    }
    if (count > RR_MAX_PLAYERS) count = RR_MAX_PLAYERS;

    /* RAGE is a preset applied to a copy of the settings, not a second set of
     * switches written over the user's: the panel sliders keep their values, so
     * turning rage off gives the old behaviour back on the same frame with no
     * ini churn. Everything below reads `config`, which is now the copy. */
    BR_Config rage;
    if (config->rage) {
        rage = *config;
        rage.aim_enabled = 1;
        rage.aim_smooth = RR_AIM_SNAP_SMOOTH;      /* instant-lock path            */
        /* Any radius: the ring is a pixel radius, so a value beyond the widest
         * screen corner is "everything the camera can see". The slider is left
         * alone and comes back the moment rage is switched off. */
        rage.aim_fov = RR_AIM_RAGE_RING;
        rage.aa_bone = RR_AIM_HEAD;
        rage.aa_dist = 0;                          /* no range limit               */
        rage.aim_require_fire = 0;
        rage.aa_los_on = 1;
        rage.antiaim_enabled = 0;                  /* a held view is a missed shot */
        rage.trigger_enabled = 1;
        rage.trigger_key = 0;                      /* armed while aiming or rage   */
        rage.trigger_delay = 0;
        /* Body-capable trigger: the head is still what the aim locks, but any
         * sampled joint inside the ring fires, so a sweep across a torso is a
         * shot instead of a near miss. */
        rage.trigger_fov = 8;
        rage.trigger_head_only = 0;
        rage.trigger_los = 1;
        rage.norecoil_enabled = 1;
        rage.nospread = 1;                         /* the cone is closed as well   */
        config = &rage;
    }

    /* 64-bit tick: no 49.7-day wrap in the worker clock. Trigger timestamps
     * stay 32-bit; their wrap-safe subtraction is verified. */
    const ULONGLONG now64 = RR_AIM_NOW();
    const unsigned now = (unsigned)now64;
    /* Two updates inside the same millisecond must not zero the follow gain:
     * a zero-length step would stall steering until the next tick. */
    unsigned elapsed = g_input_at ? (unsigned)(now64 - g_input_at) : 16u;
    if (!elapsed) elapsed = 1u;
    g_input_at = now64;

    /* Rage is key-free by definition: the mode exists to shred whatever the
     * camera can see, so the hold key is not consulted while it is on. Every
     * other mode keeps the key exactly as before. */
    const int held = config->rage ||
        (GetAsyncKeyState(config->aim_key) & 0x8000) != 0;
    const int firing = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;

    /* The cone is closed before the pick: a shot fired this frame is already
     * inside the zeroed spread, and the visualizer reads back zero on the next
     * snapshot, which is how the user sees that it took. */
    if (config->nospread) nospread_apply(players, count);

    /* A dead local player gets the death camera and has no weapon: steering and
     * clicking there only fights the spectate view. */
    int local_dead = 0;
    for (int i = 0; i < count; ++i)
        if (players[i].local) { local_dead = players[i].dead; break; }

    if (config->aim_enabled && held && !local_dead &&
        (!config->aim_require_fire || firing)) {
        const int lock_fresh = g_aim_lock_id && (now64 - g_aim_lock_at) < 500;
        RR_AIM_PICK pick = rr_aim_pick(players, count, camera, width, height, config,
                                       lock_fresh ? g_aim_lock_addr : 0,
                                       lock_fresh ? g_aim_lock_id : 0,
                                       config->aa_los_on ? aim_clear_path : NULL);
        /* The held target died, left the widened ring or lost its sight line:
         * acquire again in the same frame instead of idling. */
        if (!pick.player && lock_fresh)
            pick = rr_aim_pick(players, count, camera, width, height, config,
                               0, 0, config->aa_los_on ? aim_clear_path : NULL);
        if (pick.player) {
            g_aim_lock_addr = pick.player->addr;
            g_aim_lock_id = pick.player->id;
            g_aim_lock_at = now64;
            g_snap_last.picked = 1;
            g_snap_last.addr = pick.player->addr;
            g_snap_last.id = pick.player->id;
            g_snap_last.point = pick.point;
            g_snap_last.body = pick.player->pos;
            g_snap_last.err_x = pick.dx;
            g_snap_last.err_y = pick.dy;
            g_snap_last.fov = camera->fov;
            int mx, my;
            if (config->aim_smooth >= RR_AIM_SNAP_SMOOTH) {
                rr_aim_snap_step(pick.player->addr, pick.player->id, camera->fov,
                                 pick.dx, pick.dy, elapsed, &mx, &my);
                if (mx || my) {
                    INPUT input = {0};
                    input.type = INPUT_MOUSE;
                    input.mi.dwFlags = MOUSEEVENTF_MOVE;
                    input.mi.dx = mx; input.mi.dy = my;
                    if (SendInput(1, &input, sizeof(input)) == 1) {
                        rr_aim_snap_commit(mx, my, pick.dx, pick.dy);
                        InterlockedExchange(&g_aim_active, 1);
                    } else {
                        rr_aim_snap_drop();
                    }
                } else {
                    rr_aim_snap_commit(0, 0, pick.dx, pick.dy);
                }
            } else {
                rr_aim_step(&g_input_steer, pick.dx, pick.dy, camera->fov, height,
                            config->aim_smooth, config->aa_aim_cal, elapsed, firing, &mx, &my);
                if (mx || my) {
                    INPUT input = {0};
                    input.type = INPUT_MOUSE;
                    input.mi.dwFlags = MOUSEEVENTF_MOVE;
                    input.mi.dx = mx; input.mi.dy = my;
                    if (SendInput(1, &input, sizeof(input)) == 1)
                        InterlockedExchange(&g_aim_active, 1);
                }
            }
            g_snap_last.sent_x = mx;
            g_snap_last.sent_y = my;
            g_snap_last.px_x = g_snap_x.px;
            g_snap_last.px_y = g_snap_y.px;
            g_snap_last.sign_x = g_snap_x.sign;
            g_snap_last.sign_y = g_snap_y.sign;
        } else {
            g_snap_last.picked = 0;
            g_aim_lock_addr = 0;
            g_aim_lock_id = 0;
            rr_aim_snap_drop();
            memset(&g_input_steer, 0, sizeof(g_input_steer));
        }
    } else {
        g_snap_last.picked = 0;
        g_aim_lock_addr = 0;
        g_aim_lock_id = 0;
        rr_aim_snap_drop();
        memset(&g_input_steer, 0, sizeof(g_input_steer));
    }

    /* Key zero follows the aim hold key, even when aim steering is disabled.
     * RAGE arms the trigger without either key: the shot lands the moment a body
     * is inside the trigger ring, which is the whole point of the mode. */
    const int trigger_held = config->trigger_key
        ? (GetAsyncKeyState(config->trigger_key) & 0x8000) != 0 : (held || config->rage);
    RR_AIM_PICK trigger = {0};
    if (config->trigger_enabled && trigger_held && !firing && !local_dead)
        trigger = rr_trigger_pick(players, count, camera, width, height, config,
                                  config->trigger_los ? aim_fire_path : NULL);
    if (rr_trigger_ready(&g_trigger_state,
                         trigger.player ? trigger.player->addr : 0,
                         trigger.player ? trigger.player->id : 0,
                         now, config->trigger_delay)) {
        INPUT click[2] = {0};
        click[0].type = click[1].type = INPUT_MOUSE;
        click[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        click[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        /* A partial insertion must not leave the synthetic left button held. */
        if (SendInput(2, click, sizeof(INPUT)) == 1)
            SendInput(1, &click[1], sizeof(INPUT));
    }

    /* Recoil control and view perturbation share the worker but not the aim
     * hold key. Recoil follows the fire button; anti-aim stays out of the way
     * while the aimbot is steering or a shot is being fired. */
    const int aim_active = InterlockedCompareExchange(&g_aim_active, 0, 0) != 0;
    if (config->norecoil_enabled) {
        int weapon = RR_WCLASS_OTHER;
        for (int i = 0; i < count; ++i)
            if (players[i].local) { weapon = rr_recoil_class(players[i].weapon); break; }
        int raw_x = 0, raw_y = 0;
        rr_raw_mouse_delta(&raw_x, &raw_y);
        int rmx = 0, rmy = 0;
        if (rr_recoil_tick(&g_recoil_state, config->norecoil_mode, config->norecoil_strength,
                           config->norecoil_horizontal, firing, weapon, &camera->fwd,
                           camera->fov, height, config->aa_aim_cal, raw_x, raw_y,
                           now, &rmx, &rmy))
            send_mouse_move(rmx, rmy);
    } else {
        rr_recoil_reset(&g_recoil_state);
    }

    const int antiaim_held = config->antiaim_key
        ? (GetAsyncKeyState(config->antiaim_key) & 0x8000) != 0 : 1;
    /* The perturbation is an offset from the centre, so the frame that owns it
     * is the frame that moves it: towards the wanted offset while the mode runs,
     * back to zero the moment it stops (key released, aim steering, the menu).
     * Nothing is reset here, so a gated frame can no longer leave a half-applied
     * offset for the next frame to measure from zero and apply all over again. */
    const int antiaim_limit = config->antiaim_range > 2 ? config->antiaim_range : 2;
    int amx = 0, amy = 0;
    if (config->antiaim_enabled && antiaim_held && !aim_active && !firing) {
        rr_antiaim_tick(&g_antiaim_state, config->antiaim_mode, config->antiaim_speed,
                        config->antiaim_range, now, &amx, &amy);
    } else if (!firing) {
        /* A live click owns the frame and is left alone; everything else returns
         * the view so the aim loop starts from the centre it measured. */
        rr_antiaim_return(&g_antiaim_state, antiaim_limit, &amx, &amy);
    }
    if (amx || amy) send_mouse_move(amx, amy);
}

int rr_aim_is_active(void) {
    return InterlockedCompareExchange(&g_aim_active, 0, 0) != 0;
}