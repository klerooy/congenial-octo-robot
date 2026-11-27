/* rr_aim.c - Select target points and convert projected errors to mouse counts.
 * Selection uses snapshot data; the caller decides whether to send input.
 */
#include "rr_aim.h"
#include "rr_cham_anchors.h"
#include "rr_body_joints.h"
#include "menu_bridge.h"
#include <math.h>
#include <windows.h>
#include <mmsystem.h>
#include <string.h>
#include "rr_los.h"
#include "rr_mem.h"
#include "rr_input_gate.h"

#define RR_AIM_DEG (180.0f / 3.14159265f)
#define RR_AIM_RAD (3.14159265f / 180.0f)

/* Auto-calibration limits, in degrees per mouse count. The range covers very
 * low and very high game sensitivities while rejecting absurd samples. */
#define AIM_DPC_MIN       0.0015f
#define AIM_DPC_MAX       1.0000f
/* Stop sending inside half a 1080p reference pixel of the joint. */
#define AIM_DEADZONE_REF  0.50f
/* Largest angular correction one frame may command (reference pixels). */
#define AIM_MAX_REF_STEP  300.0f
/* Absolute SendInput ceiling for one frame; keeps a badly guessed sensitivity
 * from turning the view by a huge angle before the servo can correct it. */
#define AIM_MAX_COUNTS    1500.0f
/* Design delay of the control loop, in aim frames. Deliberately conservative:
 * the game may answer a SendInput one or two of its own frames later, and a
 * frame here can be shorter than a game frame. Underestimating this delay is
 * what made 100% smoothing oscillate, while overestimating only slows the
 * follow. With this value the loop is well damped for real delays up to about
 * five frames, including while a badly seeded estimate is still being
 * corrected. Larger delays can still settle, but with a small residual limit
 * cycle at the deadzone edge rather than a clean stop. */
#define AIM_LATENCY_FRAMES 5

/* Legit mode. A new target is tracked but not steered for the reaction
 * window, the camera cannot turn faster than the cap, and acquisition
 * requires a clear line of sight. These bound the server-visible signals
 * (instant target switches, impossible angular velocity, aiming through
 * cover) without changing the selected joint or the smoothing slider. */
#define AIM_LEGIT_REACTION_MS 100
#define AIM_LEGIT_MAX_DPS     900
#define AIM_LEGIT_GRACE_MS    250
/* Nospread is a write; keep it to the shot window plus a short tail instead of
 * rewriting the carriers on every idle frame. */
#define AIM_NOSPREAD_TAIL_MS  120

/* Humanization. The offset is expressed in reference pixels so it means the
 * same angular wobble at every resolution and zoom. Two incommensurate sine
 * components per axis keep the path smooth without looking like a loop. */
#define AIM_HUMANIZE_MAX_REF  12.0f
#define AIM_TAU               6.28318530717958647692

/* Last decision, read by the host's footer and throttled diagnostics. */
static RR_AIM_FRAME g_frame;
static int g_nospread_writes;

void rr_aim_frame_stats(RR_AIM_FRAME* out) { if (out) *out = g_frame; }
int rr_nospread_writes(void) { return g_nospread_writes; }

static int aim_visible_point_bone(const RR_PLAYER* p, int group, const RR_CAMERA* cam,
                                  int width, int height, RR_V3* out, int* bone,
                                  int (*clear)(RR_V3, RR_V3));
static int head_aim_point(const RR_PLAYER* p, RR_V3* out);
static int aim_bone_point(const RR_PLAYER* p, int group, int bone, const RR_CAMERA* cam,
                          int width, int height, RR_V3* out,
                          int (*clear)(RR_V3, RR_V3));
static int aim_random_bone(const RR_PLAYER* p, int group, int current, unsigned now,
                           int interval_ms, int force);
static void aim_random_bone_reset(void);

/* A seated character's replicated transform can trail the rendered rig by more
 * than a metre, so the body-relative sanity bound is wider in a vehicle. */
static float aim_point_tolerance2(const RR_PLAYER* p) {
    return p && p->inveh ? 144.0f : 36.0f;
}

/* A vehicle hull is a runtime collider. The ray to a seated occupant starts
 * outside the vehicle and ends inside it, so the full trace sees the hull and
 * rejects an otherwise visible target. Static geometry still rejects walls. */
static int aim_clear_static_path(RR_V3 start, RR_V3 end) {
    return rr_los_check_static(start, end) == RR_LOS_CLEAR;
}

/* The target-filter panel is authoritative for aim and trigger. IsDead can
 * stay unresolved on a fresh corpse, so a zero health value counts as dead
 * while the switch is on. Knocked-out bodies are prone even when the rig or a
 * seat pose would otherwise resolve as standing. The switches are read from
 * the shared config every pick, so flipping one takes effect on the next
 * frame. */
static int aim_target_ignored(const RR_PLAYER* p, const BR_Config* config) {
    if (!p || !config) return 1;
    if (config->aim_ignore_dead && (p->dead || p->health == 0)) return 1;
    if (config->aim_ignore_prone && (p->prone == 1 || p->knocked)) return 1;
    if (config->aim_ignore_civilian && rr_role_category(p->role) == RR_ROLE_CIVILIAN) return 1;
    return 0;
}

RR_AIM_PICK rr_aim_pick(const RR_PLAYER* players, int count, const RR_CAMERA* camera,
                       int width, int height, const BR_Config* config,
                       uint64_t locked_addr, int32_t locked_id, int locked_bone,
                       int (*clear)(RR_V3, RR_V3)) {
    RR_AIM_PICK best = {0};
    best.bone = -1;
    if (!players || count <= 0 || !camera || !camera->valid || !config || width < 16 || height < 16) return best;
    if (count > RR_MAX_PLAYERS) count = RR_MAX_PLAYERS;
    float best_priority = INFINITY, best_d2 = INFINITY;
    float cx = width * 0.5f, cy = height * 0.5f;
    float radius = (float)(config->aim_fov > 0 ? config->aim_fov : 150);
    /* A lock may follow the target a little outside the acquisition ring, but
     * not across the whole screen where it would yank the view. */
    float limit = radius * radius * (locked_addr ? 4.0f : 1.0f);
    int group = config->aa_bone;
    for (int i = 0; i < count; ++i) {
        const RR_PLAYER* p = &players[i];
        if (locked_addr && (p->addr != locked_addr || p->id != locked_id)) continue;
        if (!p->addr || !p->valid || p->is_vehicle || p->is_loot || p->local || p->hidden_in_object) continue;
        if (aim_target_ignored(p, config)) continue;
        if (!rr_relationship_allowed(p->relationships, config->aim_targets & RR_REL_CLAN, config->aim_targets & RR_REL_POLICE, config->aim_targets & RR_REL_PARTY)) continue;
        if (!isfinite(p->dist) || (config->aa_dist > 0 && p->dist > (float)config->aa_dist)) continue;
        /* A seated occupant is inside their own vehicle; ignore runtime hulls
         * for that ray while keeping the static wall test. */
        int (*target_clear)(RR_V3, RR_V3) =
            clear ? (p->inveh ? aim_clear_static_path : clear) : NULL;

        float sx = 0.0f, sy = 0.0f;
        RR_V3 point;
        int bone = -1;
        int have = 0;

        /* Once locked, keep the same joint unless bone randomization asks for
         * another one inside the selected group. Re-selecting the nearest head
         * bone every frame made the crosshair hop between eyes, jaw and skull;
         * the interval below makes that hop deliberate and bounded. */
        if (locked_addr) {
            int use_bone = locked_bone;
            int randomized = 0;
            if (config->aim_bone_random && p->full_rig) {
                use_bone = aim_random_bone(p, group, locked_bone, (unsigned)timeGetTime(),
                                           config->aim_bone_interval, 0);
                randomized = use_bone >= 0;
            }
            if (use_bone >= 0 && use_bone < p->nbones &&
                (!p->full_rig || rr_aim_bone_in_group(group, use_bone))) {
                RR_V3 fixed;
                int valid = 0;
                if (randomized) {
                    /* Try the randomized joint first; if it is behind cover or
                     * off the body, walk the group before falling back to the
                     * normal nearest-joint search below. */
                    for (int attempt = 0; attempt < RR_CHAM_BONES; ++attempt) {
                        if (aim_bone_point(p, group, use_bone, camera, width, height,
                                           &fixed, target_clear)) { valid = 1; break; }
                        int previous = use_bone;
                        use_bone = aim_random_bone(p, group, use_bone,
                                                   (unsigned)timeGetTime(),
                                                   config->aim_bone_interval, 1);
                        if (use_bone < 0 || use_bone == previous) break;
                    }
                } else if (group == RR_AIM_HEAD) {
                    valid = head_aim_point(p, &fixed);
                } else {
                    fixed = (RR_V3){ p->bone[use_bone][0], p->bone[use_bone][1],
                                     p->bone[use_bone][2] };
                    valid = 1;
                }
                if (valid) {
                    float wx = fixed.x - p->pos.x, wy = fixed.y - p->pos.y, wz = fixed.z - p->pos.z;
                    if (isfinite(wx) && isfinite(wy) && isfinite(wz) &&
                        wx*wx + wy*wy + wz*wz <= aim_point_tolerance2(p) &&
                        game_w2s(camera, &fixed, width, height, &sx, &sy))
                        have = 1;
                    if (have && target_clear && !target_clear(camera->pos, fixed)) have = 0;
                    if (have) { point = fixed; bone = use_bone; }
                }
                if (!have && randomized) aim_random_bone_reset();
            }
        }
        if (!have && config->aim_bone_random && p->full_rig) {
            /* Acquire a random joint in the group instead of always the one
             * nearest the crosshair. The choice holds for the interval, so a
             * fresh lock does not hop joints every frame. */
            int pick_bone = aim_random_bone(p, group, -1, (unsigned)timeGetTime(),
                                            config->aim_bone_interval, 0);
            if (pick_bone >= 0 &&
                aim_bone_point(p, group, pick_bone, camera, width, height,
                               &point, target_clear) &&
                game_w2s(camera, &point, width, height, &sx, &sy)) {
                bone = pick_bone;
                have = 1;
            }
        }
        if (!have) {
            if (!aim_visible_point_bone(p, group, camera, width, height, &point, &bone, NULL) ||
                !game_w2s(camera, &point, width, height, &sx, &sy)) continue;
            /* Keep acquisition outside the FOV cheap. If the nearest member is
             * blocked, search the same area for another visible joint. */
            if (target_clear && !target_clear(camera->pos,point)) {
                if (!aim_visible_point_bone(p, group, camera, width, height, &point, &bone, target_clear) ||
                    !game_w2s(camera,&point,width,height,&sx,&sy)) continue;
            }
        }
        float dx = sx - cx, dy = sy - cy;
        float d2 = dx*dx + dy*dy;
        if (!isfinite(d2) || d2 > limit) continue;
        float priority = config->aa_closest_first ? p->dist : d2;
        if (priority < best_priority || (priority == best_priority && d2 < best_d2)) {
            best = (RR_AIM_PICK){p, point, dx, dy, bone};
            best_priority = priority;
            best_d2 = d2;
        }
        if (locked_addr) break;
    }
    return best;
}

/* Response time of the smoothing filter, in milliseconds, used for target
 * lead. 100% is effectively immediate; lower values add a longer tail. */
static float aim_response_ms(int smooth) {
    float alpha = fmaxf(0.01f, fminf(1.0f, smooth / 100.0f));
    float slow = 1.0f - sqrtf(alpha);
    return 220.0f * slow * slow;
}

static float aim_dot(RR_V3 a, RR_V3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }

static RR_V3 aim_cross(RR_V3 a, RR_V3 b) {
    return (RR_V3){ a.y*b.z - a.z*b.y,
                    a.z*b.x - a.x*b.z,
                    a.x*b.y - a.y*b.x };
}

/* Orthonormal camera axes. The fallback matches the old aim_angles path for a
 * camera whose basis is missing or not unit length. */
static int aim_camera_basis(const RR_CAMERA* cam, RR_V3* f, RR_V3* r, RR_V3* u) {
    if (!cam || !cam->valid || !f || !r || !u) return 0;
    *f = cam->fwd; *r = cam->right; *u = cam->up;
    float fl = sqrtf(aim_dot(*f, *f));
    float rl = sqrtf(aim_dot(*r, *r));
    float ul = sqrtf(aim_dot(*u, *u));
    if (!(fl > 0.5f) || !(rl > 0.5f) || !(ul > 0.5f)) {
        *r = (RR_V3){ f->z, 0.0f, -f->x };
        *u = (RR_V3){ 0.0f, 1.0f, 0.0f };
        fl = sqrtf(aim_dot(*f, *f));
        rl = sqrtf(aim_dot(*r, *r));
        ul = 1.0f;
        if (!(fl > 0.001f) || !(rl > 0.001f)) return 0;
    }
    f->x /= fl; f->y /= fl; f->z /= fl;
    r->x /= rl; r->y /= rl; r->z /= rl;
    u->x /= ul; u->y /= ul; u->z /= ul;
    return 1;
}

/* Yaw and pitch of a world point relative to the current camera axes. */
static void aim_angles(const RR_CAMERA* cam, const RR_V3* point,
                       float* yaw_deg, float* pitch_deg) {
    *yaw_deg = *pitch_deg = 0.0f;
    if (!cam || !point || !cam->valid) return;
    RR_V3 rel = { point->x - cam->pos.x, point->y - cam->pos.y,
                  point->z - cam->pos.z };
    float len = sqrtf(aim_dot(rel, rel));
    if (!(len > 0.001f) || !isfinite(len)) return;
    rel.x /= len; rel.y /= len; rel.z /= len;

    RR_V3 f, r, u;
    if (!aim_camera_basis(cam, &f, &r, &u)) return;

    float z = aim_dot(rel, f), x = aim_dot(rel, r), y = aim_dot(rel, u);
    float yaw = atan2f(x, z) * RR_AIM_DEG;
    float pitch = atan2f(y, sqrtf(x*x + z*z)) * RR_AIM_DEG;
    if (isfinite(yaw)) *yaw_deg = yaw;
    if (isfinite(pitch)) *pitch_deg = pitch;
}

/* Apply a humanization offset, expressed in reference pixels, to a world
 * point. Positive x is screen-right and positive y is screen-down, matching
 * the projected dx/dy the mouse servo consumes. One reference pixel is 1/18
 * degree, the same convention rr_aim_step uses. */
static int aim_humanize_point(const RR_CAMERA* cam, RR_V3 point, float hx, float hy,
                              RR_V3* out) {
    if (!cam || !cam->valid || !out) return 0;
    *out = point;
    if (hx == 0.0f && hy == 0.0f) return 1;
    RR_V3 rel = { point.x - cam->pos.x, point.y - cam->pos.y,
                  point.z - cam->pos.z };
    float dist = sqrtf(aim_dot(rel, rel));
    if (!(dist > 0.001f) || !isfinite(dist)) return 0;
    RR_V3 dir = { rel.x / dist, rel.y / dist, rel.z / dist };
    RR_V3 f, r, u;
    if (!aim_camera_basis(cam, &f, &r, &u)) return 0;

    /* Rotate around up for yaw: positive hx turns toward screen-right. */
    float angle = hx * (RR_AIM_RAD / 18.0f);
    float ca = cosf(angle), sa = sinf(angle);
    RR_V3 axis = aim_cross(u, dir);
    float along = aim_dot(u, dir);
    RR_V3 d = { dir.x*ca + axis.x*sa + u.x*along*(1.0f - ca),
                dir.y*ca + axis.y*sa + u.y*along*(1.0f - ca),
                dir.z*ca + axis.z*sa + u.z*along*(1.0f - ca) };

    /* Rotate around right for pitch: positive hy turns toward screen-down. */
    angle = hy * (RR_AIM_RAD / 18.0f);
    ca = cosf(angle); sa = sinf(angle);
    axis = aim_cross(r, d);
    along = aim_dot(r, d);
    *out = (RR_V3){ cam->pos.x + (d.x*ca + axis.x*sa + r.x*along*(1.0f - ca)) * dist,
                    cam->pos.y + (d.y*ca + axis.y*sa + r.y*along*(1.0f - ca)) * dist,
                    cam->pos.z + (d.z*ca + axis.z*sa + r.z*along*(1.0f - ca)) * dist };
    return isfinite(out->x) && isfinite(out->y) && isfinite(out->z);
}

int rr_aim_motion_point(RR_V3 point, RR_V3 position, RR_V3 previous,
                        unsigned sample_ms, unsigned age_ms, int smooth, RR_V3* out) {
    if (sample_ms < 8 || sample_ms > 120 || age_ms > 80) return 0;
    float vx = (position.x - previous.x) * 1000.0f / sample_ms;
    float vz = (position.z - previous.z) * 1000.0f / sample_ms;
    float speed = hypotf(vx, vz);
    if (!(speed >= 0.2f && speed <= 20.0f)) return 0;

    /* Lead through the whole smoothing delay, not just part of it, plus the
     * age of the snapshot the point came from. At equilibrium that cancels the
     * lag of an exponential follow, so a constant-velocity target settles on
     * the crosshair instead of trailing behind it. The cap bounds the
     * extrapolation error when a target changes direction mid-lead. */
    float seconds = (aim_response_ms(smooth) + (float)age_ms) * 0.001f;
    seconds = fminf(seconds, 0.20f);
    *out = (RR_V3){ point.x + vx * seconds, point.y, point.z + vz * seconds };
    return 1;
}

/* One reference pixel is 1/18 degree: 60 degrees over 1080 rows. The seed is
 * 100 times the assumed counts per reference pixel; the servo stores the
 * inverse, degrees per count, because that is what the camera can be measured
 * in. Seed 100 therefore means one count per reference pixel. The runtime
 * estimate replaces this seed as soon as a clean camera sample lands. */
static float aim_cal_seed_dpc(int cal) {
    if (!(cal > 0)) cal = 100;
    float dpc = 100.0f / (18.0f * (float)cal);
    if (!isfinite(dpc) || dpc < AIM_DPC_MIN) dpc = AIM_DPC_MIN;
    if (dpc > AIM_DPC_MAX) dpc = AIM_DPC_MAX;
    return dpc;
}

/* Screen pixels to reference pixels (1080 rows, 60-degree vertical FOV). */
static float aim_ref_scale(float fov, int height) {
    if (!(fov > 10.0f && fov < 170.0f) || height < 16) return 0.0f;
    return (1080.0f / (float)height) *
           (tanf(fov * 3.14159265f / 360.0f) / 0.57735027f);
}

static uint32_t aim_hash32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static double aim_humanize_phase(uint32_t seed, uint32_t salt) {
    return (double)(aim_hash32(seed ^ salt) & 0xFFFFu) *
           (AIM_TAU / 65536.0);
}

/* Bounded, smooth wobble. The amplitude is linear in strength, which keeps the
 * slider predictable; speed only changes the frequency, so a low setting drifts
 * slowly while a high setting looks like a nervous hand. */
void rr_aim_humanize_offset(int strength, int speed, unsigned now, uint32_t seed,
                            float* x, float* y) {
    if (x) *x = 0.0f;
    if (y) *y = 0.0f;
    if (!x || !y || strength <= 0) return;
    if (strength > 100) strength = 100;
    if (speed < 1) speed = 1;
    if (speed > 100) speed = 100;

    const double amp = (double)AIM_HUMANIZE_MAX_REF * (double)strength / 100.0;
    const double freq = 0.30 + 2.70 * (double)speed / 100.0;
    const double t = (double)now * 0.001;
    const double px = aim_humanize_phase(seed, 0x9e3779b9u);
    const double py = aim_humanize_phase(seed, 0x85ebca6bu);
    const double px2 = aim_humanize_phase(seed, 0xc2b2ae35u);
    const double py2 = aim_humanize_phase(seed, 0x27d4eb2fu);

    double sx = 0.62 * sin(AIM_TAU * freq * t + px) +
                0.38 * sin(AIM_TAU * freq * 1.63 * t + px2);
    double sy = 0.62 * sin(AIM_TAU * freq * 0.87 * t + py) +
                0.38 * sin(AIM_TAU * freq * 1.41 * t + py2);
    *x = (float)(amp * sx);
    *y = (float)(amp * sy);
}

/* One stable phase per body, so switching targets does not look like the same
 * wobble copied onto every opponent. */
static uint32_t aim_humanize_seed(const RR_PLAYER* p) {
    if (!p) return 1u;
    return (uint32_t)(p->addr ^ (p->addr >> 32) ^ (uint32_t)p->id) | 1u;
}

void rr_aim_step(RR_AIM_STEER* state, float dx, float dy, float fov, int height,
                 int smooth, int cal, unsigned elapsed_ms, int firing, int* mx, int* my) {
    (void)firing;
    (void)elapsed_ms;
    *mx = *my = 0;
    if (!state) return;
    if (!isfinite(dx) || !isfinite(dy) || !(fov > 10.0f && fov < 170.0f) || height < 16) {
        *state = (RR_AIM_STEER){0};
        return;
    }

    if (!(state->damp >= 0.05f && state->damp <= 1.0f)) state->damp = 1.0f;
    if (state->latency < 1 || state->latency > 8) state->latency = AIM_LATENCY_FRAMES;
    /* A slider change reseeds the estimate; otherwise the learned value wins. */
    if (cal != state->cal_seed || !(state->dpc > AIM_DPC_MIN * 0.5f && state->dpc < AIM_DPC_MAX * 2.0f)) {
        state->cal_seed = cal;
        state->dpc = aim_cal_seed_dpc(cal);
        state->dpc_valid = 0;
        state->dpc_samples = 0;
    }

    /* Project the error onto a reference screen (1080 rows, 60-degree FOV).
     * The same angular offset then needs the same correction at any zoom. */
    float scale = aim_ref_scale(fov, height);
    if (!(scale > 0.0f)) {
        *state = (RR_AIM_STEER){0};
        return;
    }
    float ex = dx * scale, ey = dy * scale;
    float mag = hypotf(ex, ey);

    /* Do not dribble a single count forever at the target. One mouse count
     * turns the view by dpc degrees; if that quantum is bigger than the fixed
     * deadzone, the loop must stop inside the quantum instead, or it buzzes
     * by one count forever. This is what caused the visible jitter at high
     * smoothing percentages. */
    float deadzone = AIM_DEADZONE_REF;
    float quantum = state->dpc * 18.0f;   /* reference pixels per count */
    if (quantum * 1.25f > deadzone) deadzone = quantum * 1.25f;
    if (deadzone > 8.0f) deadzone = 8.0f;
    if (mag < deadzone) {
        state->carry_x = state->carry_y = 0.0f;
        state->last_dx = dx;
        state->last_dy = dy;
        return;
    }

    /* The view crossed the target after a real correction: that is an
     * overshoot, so back the gain off briefly instead of pumping. This only
     * fires when both errors are meaningful, so a target sweeping smoothly
     * through the crosshair does not collapse the gain. Before the first
     * clean measurement the seed may be far off, so the backoff is deeper. */
    float prev_mag = hypotf(state->last_dx, state->last_dy);
    if (state->last_dx * dx + state->last_dy * dy < 0.0f &&
        fminf(mag, prev_mag) > 1.5f) {
        state->damp *= state->dpc_valid ? 0.85f : 0.60f;
        float floor = state->dpc_valid ? 0.20f : 0.05f;
        if (state->damp < floor) state->damp = floor;
        state->carry_x = state->carry_y = 0.0f;
    } else if (state->damp < 1.0f) {
        state->damp = fminf(1.0f, state->damp + 0.01f);
    }
    state->last_dx = dx;
    state->last_dy = dy;

    /* Bound one frame's angular correction; the loop still gets a fast flick
     * over several frames without a single giant jump. */
    if (mag > AIM_MAX_REF_STEP) {
        float shrink = AIM_MAX_REF_STEP / mag;
        ex *= shrink;
        ey *= shrink;
    }

    /* Maximum proportional gain for a loop delayed by `latency` frames. One
     * extra frame is reserved for the command being applied after the camera
     * snapshot. The smooth slider spans from a slow filter to that stable
     * maximum, so 100% is fast but can no longer overshoot the target. */
    float alpha = fmaxf(0.01f, fminf(1.0f, smooth / 100.0f));
    float gmax = 1.0f / (float)(state->latency + 1);
    /* Delay is counted in frames, so the per-frame gain must not grow on slow
     * frames. Scaling it with elapsed time was another source of the 100%
     * oscillation: a long frame got a bigger step while the view still had
     * older commands in flight. */
    float gain = gmax * sqrtf(alpha) * state->damp;

    /* Counts = angle / measured degrees-per-count. One reference pixel is
     * 1/18 degree, so a reference-pixel error needs ex / (18 * dpc) counts. */
    float counts_per_ref = 1.0f / (18.0f * state->dpc);
    float cx = ex * counts_per_ref * gain + state->carry_x;
    float cy = ey * counts_per_ref * gain + state->carry_y;
    float cmag = hypotf(cx, cy);
    if (cmag > AIM_MAX_COUNTS) {
        float shrink = AIM_MAX_COUNTS / cmag;
        cx *= shrink;
        cy *= shrink;
    }
    int nx = (int)cx, ny = (int)cy;
    state->carry_x = cx - (float)nx;
    state->carry_y = cy - (float)ny;
    *mx = nx;
    *my = ny;
}

/* Learn the real camera sensitivity. The camera forward vector is a gyro:
 * its rotation is produced only by injected mouse counts (plus recoil, which
 * is rejected), so it is independent of how the target moves. Counts and
 * signed rotation are accumulated over an episode and sampled once the view
 * has been still for several frames, which also guarantees that the last
 * command has landed. */
static void aim_servo_learn(RR_AIM_STEER* s, const RR_CAMERA* cam, int mx, int my, int firing) {
    if (!s || !cam || !cam->valid) return;
    if (s->latency < 1 || s->latency > 8) s->latency = AIM_LATENCY_FRAMES;

    RR_V3 f = cam->fwd;
    float fl = sqrtf(aim_dot(f, f));
    if (!(fl > 0.5f) || !isfinite(fl)) { s->have_prev = 0; return; }
    f.x /= fl; f.y /= fl; f.z /= fl;

    if (!s->have_prev) {
        s->prev_fwd = f;
        s->have_prev = 1;
        s->ep_yaw = s->ep_pitch = s->ep_sx = s->ep_sy = 0.0f;
        s->ep_frames = s->ep_still = s->ep_idle = 0;
        return;
    }

    RR_V3 prev = s->prev_fwd;
    float c = aim_dot(f, prev);
    if (c > 1.0f) c = 1.0f;
    if (c < -1.0f) c = -1.0f;
    float rot = acosf(c) * RR_AIM_DEG;
    s->prev_fwd = f;

    /* Signed yaw/pitch deltas around the previous camera axes. Yaw follows
     * +dx; screen +dy lowers the view, so pitch ends up inverted. */
    float dyaw = 0.0f, dpitch = 0.0f;
    RR_V3 r = cam->right, u = cam->up;
    float rl = sqrtf(aim_dot(r, r)), ul = sqrtf(aim_dot(u, u));
    if (rl > 0.5f && ul > 0.5f) {
        dyaw = atan2f(aim_dot(f, r) / rl, c) * RR_AIM_DEG;
        dpitch = atan2f(aim_dot(f, u) / ul, c) * RR_AIM_DEG;
    }

    /* A spectate cut or respawn rotates the camera far more than a mouse
     * packet can; drop the episode instead of poisoning the estimate. */
    if (!isfinite(rot) || rot > 45.0f) {
        s->ep_yaw = s->ep_pitch = s->ep_sx = s->ep_sy = 0.0f;
        s->ep_frames = s->ep_still = s->ep_idle = 0;
        s->ep_hold = 5;
        return;
    }

    /* Recoil moves the view without a matching command. Skip the frame and
     * the next few, so decaying recoil is not sampled as mouse response. */
    if (firing) {
        s->ep_yaw = s->ep_pitch = s->ep_sx = s->ep_sy = 0.0f;
        s->ep_frames = s->ep_still = s->ep_idle = 0;
        s->ep_hold = 5;
        return;
    }
    if (s->ep_hold > 0) {
        s->ep_hold--;
        /* Commands issued while the view is still catching up stay in the
         * denominator: their rotation lands after the hold and will be
         * accumulated then, so dropping the counts would bias the estimate. */
        s->ep_sx += (float)mx;
        s->ep_sy += (float)my;
        s->ep_yaw = s->ep_pitch = 0.0f;
        s->ep_frames = s->ep_still = s->ep_idle = 0;
        return;
    }

    /* Rotation that is not explained by a recent command is a manual drag or
     * world shake; do not let it inflate the sensitivity sample. */
    if (rot > 0.02f && s->ep_idle > 8) {
        s->ep_yaw = s->ep_pitch = s->ep_sx = s->ep_sy = 0.0f;
        s->ep_frames = s->ep_still = 0;
    } else {
        s->ep_yaw += dyaw;
        s->ep_pitch += dpitch;
    }
    s->ep_sx += (float)mx;
    s->ep_sy += (float)my;
    s->ep_frames++;
    if (mx || my) {
        s->ep_idle = 0;
        s->ep_still = 0;
    } else {
        s->ep_idle++;
        /* Quiet frame: no command was sent and the view did not move. */
        if (rot < 0.02f) s->ep_still++;
        else s->ep_still = 0;
    }

    /* Too much movement or mouse dragging to be one clean sample. */
    if (s->ep_frames > 600 || fabsf(s->ep_sx) > 20000.0f || fabsf(s->ep_sy) > 20000.0f) {
        s->ep_yaw = s->ep_pitch = s->ep_sx = s->ep_sy = 0.0f;
        s->ep_frames = s->ep_still = s->ep_idle = 0;
        return;
    }

    /* Wait until the command has surely landed; then sample the episode. */
    if (s->ep_still < 5 || s->ep_frames < 6) return;

    float num = 0.0f, den = 0.0f;
    if (fabsf(s->ep_sx) >= 8.0f && s->ep_yaw * s->ep_sx > 0.0f) {
        num += fabsf(s->ep_yaw);
        den += fabsf(s->ep_sx);
    }
    if (fabsf(s->ep_sy) >= 8.0f && s->ep_pitch * s->ep_sy < 0.0f) {
        num += fabsf(s->ep_pitch);
        den += fabsf(s->ep_sy);
    }
    if (den >= 10.0f && num > 0.003f) {
        float sample = num / den;
        if (isfinite(sample) && sample > AIM_DPC_MIN && sample < AIM_DPC_MAX) {
            if (!s->dpc_valid) {
                s->dpc = sample;
                s->dpc_valid = 1;
            } else {
                /* One bad sample may not collapse or blow up the servo. */
                if (sample < s->dpc * 0.5f) sample = s->dpc * 0.5f;
                if (sample > s->dpc * 2.0f) sample = s->dpc * 2.0f;
                s->dpc += 0.35f * (sample - s->dpc);
            }
            if (s->dpc_samples < 1000) s->dpc_samples++;
            /* A clean measurement is better evidence than the overshoot
             * history that drove the gain down. */
            if (s->damp < 0.5f) s->damp = 0.5f;
        }
    }
    s->ep_yaw = s->ep_pitch = s->ep_sx = s->ep_sy = 0.0f;
    s->ep_frames = s->ep_still = s->ep_idle = 0;
}


int rr_aim_bone_in_group(int group, int bone) {
    if (bone<0 || bone>=RR_CHAM_BONES) return 0;
    switch (group) {
    case RR_AIM_HEAD: return bone>=RR_RIG_M_HEAD_1_JNT && bone<=RR_RIG_M_TONGUE_3_JNT;
    case RR_AIM_NECK: return bone==RR_RIG_M_NECK_JNT || bone==RR_RIG_L_ARM_0_JNT || bone==RR_RIG_R_ARM_0_JNT;
    case RR_AIM_CHEST: return bone==RR_RIG_M_SPINE_2_JNT || bone==RR_RIG_M_SPINE_3_JNT;
    case RR_AIM_PELVIS: return bone==RR_RIG_M_SPINE_1_JNT || bone==RR_RIG_L_LEG_1_JNT || bone==RR_RIG_R_LEG_1_JNT;
    case RR_AIM_ARMS: return (bone>=RR_RIG_L_ARM_0_JNT && bone<=RR_RIG_L_ARM_3_JNT) ||
                            (bone>=RR_RIG_R_ARM_0_JNT && bone<=RR_RIG_R_ARM_3_JNT) ||
                            bone==RR_RIG_L_ARMTWIST_JNT || bone==RR_RIG_R_ARMTWIST_JNT;
    case RR_AIM_LEGS: return (bone>=RR_RIG_L_LEG_1_JNT && bone<=RR_RIG_L_LEG_3_JNT) ||
                            (bone>=RR_RIG_R_LEG_1_JNT && bone<=RR_RIG_R_LEG_3_JNT);
    case RR_AIM_FACE: return bone>=RR_RIG_L_EYELIDUP_JNT && bone<=RR_RIG_M_TONGUE_3_JNT;
    case RR_AIM_HANDS: return (bone>=RR_RIG_L_ARM_3_JNT && bone<=RR_RIG_L_THUMB_3_JNT) ||
                             (bone>=RR_RIG_R_ARM_3_JNT && bone<=RR_RIG_R_THUMB_3_JNT);
    case RR_AIM_FEET: return (bone>=RR_RIG_L_LEG_3_JNT && bone<=RR_RIG_L_FOOT_2_JNT) ||
                            (bone>=RR_RIG_R_LEG_3_JNT && bone<=RR_RIG_R_FOOT_2_JNT);
    case RR_AIM_CLOSEST: return rr_rig_is_body_joint(bone);
    default: return 0;
    }
}

/* The head group spans the whole face. Randomizing over every eyelid and
 * tongue joint would make the crosshair twitch inside the mouth, so the
 * randomized head set is the stable skull/face landmarks. */
static int aim_head_bone_candidate(int bone) {
    switch (bone) {
    case RR_RIG_M_HEAD_1_JNT:
    case RR_RIG_M_HEAD_2_JNT:
    case RR_RIG_L_HEAD01_EYE_JNT:
    case RR_RIG_R_HEAD01_EYE_JNT:
    case RR_RIG_M_JAW_JNT:
    case RR_RIG_M_FOREHEADDN_JNT:
    case RR_RIG_L_CHEEKUP_JNT:
    case RR_RIG_R_CHEEKUP_JNT:
    case RR_RIG_L_CHEEKDN_JNT:
    case RR_RIG_R_CHEEKDN_JNT:
        return 1;
    default:
        return 0;
    }
}

/* Bones that belong to the selected group and are safe to aim at. The
 * seven-joint fallback has one representative per group, so it never
 * randomizes. */
static int aim_group_bones(const RR_PLAYER* p, int group, int* out, int max) {
    if (!p || !out || max <= 0) return 0;
    if (!p->full_rig || p->nbones != RR_CHAM_BONES) {
        if (group < 0 || group >= RR_AIM_GROUP_COUNT) return 0;
        int bone = p->bone_group[group];
        if (bone < 0 || bone >= p->nbones || bone >= RR_CHAM_BONES) return 0;
        out[0] = bone;
        return 1;
    }
    int count = 0;
    for (int bone = 0; bone < RR_CHAM_BONES && count < max; ++bone) {
        if (!rr_aim_bone_in_group(group, bone)) continue;
        if (group == RR_AIM_HEAD && !aim_head_bone_candidate(bone)) continue;
        out[count++] = bone;
    }
    return count;
}

static uint64_t g_bone_random_addr;
static int32_t g_bone_random_id;
static int g_bone_random_group = -1;
static int g_bone_random_bone = -1;
static unsigned g_bone_random_at;
static uint32_t g_bone_random_seed = 0x9e3779b9u;

static void aim_random_bone_reset(void) {
    g_bone_random_addr = 0;
    g_bone_random_id = 0;
    g_bone_random_group = -1;
    g_bone_random_bone = -1;
    g_bone_random_at = 0;
    g_bone_random_seed = 0x9e3779b9u;
}

/* Pick a joint inside the group. A locked target keeps the chosen joint until
 * the interval expires; force asks for a different one immediately. */
static int aim_random_bone(const RR_PLAYER* p, int group, int current, unsigned now,
                           int interval_ms, int force) {
    if (!p || !p->full_rig || group < 0 || group >= RR_AIM_GROUP_COUNT) return -1;
    /* "Closest body joint" is a strategy, not a body region; randomizing it
     * would jump between the head and the feet. */
    if (group == RR_AIM_CLOSEST) return -1;
    if (g_bone_random_addr != p->addr || g_bone_random_id != p->id ||
        g_bone_random_group != group) {
        g_bone_random_addr = p->addr;
        g_bone_random_id = p->id;
        g_bone_random_group = group;
        g_bone_random_bone = -1;
        g_bone_random_at = now;
        g_bone_random_seed = (uint32_t)(p->addr ^ (p->addr >> 32) ^
                                        (uint32_t)p->id) | 1u;
        force = 1;
    }
    if (interval_ms < 50) interval_ms = 50;
    if (!force && g_bone_random_bone >= 0 &&
        (unsigned)(now - g_bone_random_at) < (unsigned)interval_ms)
        return g_bone_random_bone;

    int bones[RR_CHAM_BONES];
    int count = aim_group_bones(p, group, bones, RR_CHAM_BONES);
    if (count <= 0) return -1;
    int index = 0;
    if (count > 1) {
        g_bone_random_seed = g_bone_random_seed * 1664525u + 1013904223u;
        index = (int)((g_bone_random_seed >> 8) % (uint32_t)count);
        if (bones[index] == current) index = (index + 1) % count;
    }
    g_bone_random_bone = bones[index];
    g_bone_random_at = now;
    return g_bone_random_bone;
}

/* One specific rig joint. The full-rig path validates that the joint belongs
 * to the group; the seven-joint fallback stores one index per group. */
static int aim_bone_point(const RR_PLAYER* p, int group, int bone, const RR_CAMERA* cam,
                          int width, int height, RR_V3* out,
                          int (*clear)(RR_V3, RR_V3)) {
    if (!p || !out || !cam || !cam->valid || width < 1 || height < 1) return 0;
    if (bone < 0 || bone >= p->nbones || bone >= RR_CHAM_BONES) return 0;
    if (p->full_rig && !rr_aim_bone_in_group(group, bone)) return 0;
    RR_V3 point = { p->bone[bone][0], p->bone[bone][1], p->bone[bone][2] };
    float dx = point.x - p->pos.x, dy = point.y - p->pos.y, dz = point.z - p->pos.z;
    float x, y;
    if (!isfinite(dx) || !isfinite(dy) || !isfinite(dz) ||
        dx*dx + dy*dy + dz*dz > aim_point_tolerance2(p) ||
        !game_w2s(cam, &point, width, height, &x, &y) ||
        !isfinite(x) || !isfinite(y)) return 0;
    if (clear && !clear(cam->pos, point)) return 0;
    *out = point;
    return 1;
}

static int full_rig_point(const RR_PLAYER* p, int group, const RR_CAMERA* cam,
                          int width, int height, RR_V3* out, int (*clear)(RR_V3, RR_V3),
                          int* out_bone) {
    if (!cam || !cam->valid || width<1 || height<1 || p->nbones!=RR_CHAM_BONES) return 0;
    /* The head group contains every facial joint. Picking whichever one is
     * nearest the crosshair made the aim point crawl between eyes, brow, jaw
     * and crown as the target moved across the FOV. Use the Head joint
     * instead: the renderer centres it inside the head mesh. */
    if (group == RR_AIM_HEAD) {
        RR_V3 point;
        float x, y;
        if (!head_aim_point(p, &point)) return 0;
        float dx=point.x-p->pos.x,dy=point.y-p->pos.y,dz=point.z-p->pos.z;
        if (!isfinite(dx) || !isfinite(dy) || !isfinite(dz) || dx*dx+dy*dy+dz*dz>aim_point_tolerance2(p) ||
            !game_w2s(cam,&point,width,height,&x,&y) || !isfinite(x) || !isfinite(y) ||
            (clear && !clear(cam->pos,point))) return 0;
        *out=point;
        if (out_bone) *out_bone=RR_RIG_M_HEAD_1_JNT;
        return 1;
    }
    int found=0; float best=INFINITY;
    for (int bone=0;bone<RR_CHAM_BONES;++bone) {
        if (!rr_aim_bone_in_group(group,bone)) continue;
        RR_V3 point={p->bone[bone][0],p->bone[bone][1],p->bone[bone][2]};
        float dx=point.x-p->pos.x,dy=point.y-p->pos.y,dz=point.z-p->pos.z,x,y;
        if (!isfinite(dx) || !isfinite(dy) || !isfinite(dz) || dx*dx+dy*dy+dz*dz>aim_point_tolerance2(p) ||
            !game_w2s(cam,&point,width,height,&x,&y) || !isfinite(x) || !isfinite(y)) continue;
        float sx=x-width*.5f,sy=y-height*.5f,score=sx*sx+sy*sy;
        if (score>=best || !isfinite(score) || (clear && !clear(cam->pos,point))) continue;
        *out=point; best=score; found=1;
        if (out_bone) *out_bone=bone;
    }
    return found;
}

static int mesh_anchor(const RR_PLAYER* p, int index, RR_V3* out) {
    if (!p->cham_valid || index<0 || index>=12) return 0;
    const RR_CHAM_ANCHOR* a=&rr_cham_anchors[index];
    const float* m=p->cham_skin[a->bone];
    out->x=m[0]*a->p[0]+m[1]*a->p[1]+m[2]*a->p[2]+m[3];
    out->y=m[4]*a->p[0]+m[5]*a->p[1]+m[6]*a->p[2]+m[7];
    out->z=m[8]*a->p[0]+m[9]*a->p[1]+m[10]*a->p[2]+m[11];
    float dx=out->x-p->pos.x,dy=out->y-p->pos.y,dz=out->z-p->pos.z;
    return isfinite(dx) && isfinite(dy) && isfinite(dz) && dx*dx+dy*dy+dz*dz<aim_point_tolerance2(p);
}

static RR_V3 group_point(const RR_PLAYER* p, int group) {
    RR_V3 joint;
    if (mesh_anchor(p,group,&joint)) return joint;
    int idx = p->bone_group[group];
    if (p->nbones > 0 && p->nbones <= RR_MAX_BONES && idx >= 0 && idx < p->nbones) {
        RR_V3 v = { p->bone[idx][0], p->bone[idx][1], p->bone[idx][2] };
        return v;
    }
    /* When the game has not created the skeleton, estimate from the character
     * origin. These are position estimates, not measured bone transforms. */
    static const float standing[4] = { 1.65f, 1.48f, 1.20f, 0.88f };
    static const float crouched[4] = { 1.00f, 0.90f, 0.72f, 0.48f };
    RR_V3 v = p->pos;
    v.y += p->crouch == 1 ? crouched[group] : standing[group];
    return v;
}

/* One stable aim point for the head group. The renderer's Head bone
 * (M_head_1_jnt) is centred inside the head mesh, so it is the middle of the
 * head; M_head_2_jnt is the crown and its midpoint would sit at the forehead.
 * The seven-joint fallback stores the same Head joint as chain[5]. This keeps
 * the crosshair on one fixed spot instead of whichever facial joint happened
 * to be closest to the screen centre. */
static int head_aim_point(const RR_PLAYER* p, RR_V3* out) {
    if (!p || !out) return 0;
    if (p->nbones > 0 && p->nbones <= RR_MAX_BONES) {
        int idx = -1;
        if (p->full_rig && p->nbones == RR_CHAM_BONES) {
            idx = RR_RIG_M_HEAD_1_JNT;
        } else if (p->nbones >= 7) {
            idx = p->bone_group[RR_AIM_HEAD];
        }
        if (idx >= 0 && idx < p->nbones) {
            RR_V3 point = { p->bone[idx][0], p->bone[idx][1], p->bone[idx][2] };
            float dx=point.x-p->pos.x, dy=point.y-p->pos.y, dz=point.z-p->pos.z;
            if (isfinite(dx+dy+dz) && dx*dx+dy*dy+dz*dz <= aim_point_tolerance2(p)) {
                *out = point;
                return 1;
            }
        }
        *out = group_point(p, RR_AIM_HEAD);
        return isfinite(out->x) && isfinite(out->y) && isfinite(out->z);
    }
    /* A seated occupant can arrive without a resolved rig. The snapshot still
     * publishes a head estimate 1.45 m above the seat, which is the only aim
     * point available in that case. */
    if (p->inveh) {
        RR_V3 point = p->head;
        float dx=point.x-p->pos.x, dy=point.y-p->pos.y, dz=point.z-p->pos.z;
        if (isfinite(dx+dy+dz) && dx*dx+dy*dy+dz*dz <= aim_point_tolerance2(p)) {
            *out = point;
            return 1;
        }
    }
    return 0;
}

static int limb_point(const RR_PLAYER* p, int group, const RR_CAMERA* cam, int width, int height,
                      RR_V3* out, int (*clear)(RR_V3, RR_V3)) {
    int start=group==5?4:8,found=0; float best=0;
    for (int side=0;side<2;++side) {
        RR_V3 a,b; float x,y;
        if (!mesh_anchor(p,start+side*2,&a) || !mesh_anchor(p,start+side*2+1,&b)) continue;
        RR_V3 mid={(a.x+b.x)*0.5f,(a.y+b.y)*0.5f,(a.z+b.z)*0.5f};
        if (!game_w2s(cam,&mid,width,height,&x,&y)) continue;
        float dx=x-width*0.5f,dy=y-height*0.5f,dist=dx*dx+dy*dy;
        if ((!found || dist<best) && (!clear || clear(cam->pos,mid))) {
            *out=mid;best=dist;found=1;
        }
    }
    return found;
}

int rr_aim_visible_point(const RR_PLAYER* p, int group, const RR_CAMERA* cam,
                         int width, int height, RR_V3* out, int (*clear)(RR_V3, RR_V3)) {
    if (!p || !out || (clear && !cam)) return 0;
    if (group < 0 || group >= RR_AIM_GROUP_COUNT) group = RR_AIM_HEAD;
    if (p->full_rig) return full_rig_point(p,group,cam,width,height,out,clear,NULL);
    if (group == RR_AIM_HEAD) {
        if (!head_aim_point(p, out)) return 0;
        return !clear || clear(cam->pos,*out);
    }
    if (group>=RR_AIM_FACE) return 0;
    if (group>=5) return limb_point(p,group,cam,width,height,out,clear);
    if (group != 4) {
        *out = group_point(p, group);
        return !clear || clear(cam->pos,*out);
    }
    int found = 0; float best = 0;
    for (int i = 0; i < 6; ++i) {
        RR_V3 point; float x, y;
        if (i<4) point=group_point(p,i);
        else if (!limb_point(p,i+1,cam,width,height,&point,clear)) continue;
        if (!game_w2s(cam, &point, width, height, &x, &y)) continue;
        float dx = x - width * 0.5f, dy = y - height * 0.5f;
        float d = dx*dx + dy*dy;
        if ((!found || d < best) && (i >= 4 || !clear || clear(cam->pos,point))) {
            *out = point; best = d; found = 1;
        }
    }
    return found;
}

int rr_aim_point(const RR_PLAYER* p, int group, const RR_CAMERA* cam,
                 int width, int height, RR_V3* out) {
    return rr_aim_visible_point(p,group,cam,width,height,out,0);
}

/* Same selection as rr_aim_visible_point, plus the rig joint the full-rig path
 * chose. Non-rig snapshots report their group bone when one is available. */
static int aim_visible_point_bone(const RR_PLAYER* p, int group, const RR_CAMERA* cam,
                                  int width, int height, RR_V3* out, int* bone,
                                  int (*clear)(RR_V3, RR_V3)) {
    if (bone) *bone = -1;
    if (!p || !out) return 0;
    if (group < 0 || group >= RR_AIM_GROUP_COUNT) group = RR_AIM_HEAD;
    if (p->full_rig)
        return full_rig_point(p, group, cam, width, height, out, clear, bone);
    if (bone && group < RR_AIM_GROUP_COUNT) {
        int b = p->bone_group[group];
        if (p->nbones > 0 && b >= 0 && b < p->nbones) *bone = b;
    }
    return rr_aim_visible_point(p, group, cam, width, height, out, clear);
}

/* Input and trigger state belongs to the single input worker. */
static volatile LONG g_aim_active;
static RR_AIM_STEER g_input_steer;
static RR_TRIGGER_STATE g_trigger_state;
static DWORD g_input_at;

/* Camera lock is the default path. The mouse servo stays available as the
 * fallback when the game build does not match the camera-hook image. */
static void aim_steer_reset(void);
static int g_camera_mode = 1;
void rr_aim_set_camera_mode(int on) {
    const int was = g_camera_mode;
    g_camera_mode = on ? 1 : 0;
    if (!g_camera_mode) {
        game_cam_clear();
        /* Drop the half-finished mouse episode so a mode switch cannot resume
         * a correction that was aimed at the old view. */
        if (was) aim_steer_reset();
    }
}

/* Clear the per-target control state but keep the learned sensitivity,
 * latency and the running calibration episode, which do not depend on which
 * body is being followed. */
static void aim_steer_retarget(void) {
    g_input_steer.carry_x = g_input_steer.carry_y = 0.0f;
    g_input_steer.last_dx = g_input_steer.last_dy = 0.0f;
}

/* Clear the per-target control state but keep the learned sensitivity and
 * latency, which stay valid across targets and aim sessions. */
static void aim_steer_reset(void) {
    float dpc = g_input_steer.dpc;
    int valid = g_input_steer.dpc_valid;
    int samples = g_input_steer.dpc_samples;
    int latency = g_input_steer.latency;
    int seed = g_input_steer.cal_seed;
    memset(&g_input_steer, 0, sizeof(g_input_steer));
    g_input_steer.damp = 1.0f;
    g_input_steer.dpc = dpc;
    g_input_steer.dpc_valid = valid;
    g_input_steer.dpc_samples = samples;
    g_input_steer.latency = latency ? latency : AIM_LATENCY_FRAMES;
    g_input_steer.cal_seed = seed;
}

/* Drop the current calibration episode without touching the learned value.
 * While the local camera rides a moving vehicle its yaw can rotate without a
 * mouse command; sampling that as sensitivity would inflate degrees/count and
 * make the aim send too few counts afterwards. */
static void aim_learn_suspend(void) {
    g_input_steer.ep_yaw = g_input_steer.ep_pitch = 0.0f;
    g_input_steer.ep_sx = g_input_steer.ep_sy = 0.0f;
    g_input_steer.ep_frames = g_input_steer.ep_still = 0;
    g_input_steer.ep_idle = g_input_steer.ep_hold = 0;
    g_input_steer.have_prev = 0;
}

/* Target lock: a held aim key should not re-run target selection every frame. */
static uint64_t g_lock_addr;
static int32_t g_lock_id;
static int g_lock_bone = -1;
static int g_lock_group = -1;
static DWORD g_lock_at;
static DWORD g_lock_miss_at;
/* First frame this target was picked. Steering waits AIM_LEGIT_REACTION_MS
 * after a change so a lock-on is not a single-frame snap. */
static uint64_t g_react_addr;
static int32_t g_react_id;
static DWORD g_react_at;

/* The selected joint is tracked in world space between frames. Its velocity is
 * the fallback when the replicated root transform has no fresh sample (rig
 * poses, a seated body whose character transform stopped updating, ...). */
static uint64_t g_track_addr;
static int32_t g_track_id;
static int g_track_bone = -1;
static RR_V3 g_track_point;
static RR_V3 g_track_vel;
static DWORD g_track_at;
static int g_track_valid;
static int g_track_vel_valid;

/* Camera translation moves every projected point even when the target stands
 * still. Track it separately so lead also covers a running local player. */
static RR_V3 g_cam_vel;
static RR_V3 g_cam_prev;
static DWORD g_cam_prev_at;
static int g_cam_vel_valid;
static int g_cam_prev_valid;

static void aim_track_reset(void) {
    g_track_addr = 0;
    g_track_id = 0;
    g_track_bone = -1;
    g_track_point = (RR_V3){0.0f, 0.0f, 0.0f};
    g_track_vel = (RR_V3){0.0f, 0.0f, 0.0f};
    g_track_at = 0;
    g_track_valid = 0;
    g_track_vel_valid = 0;
}

static void aim_cam_reset(void) {
    g_cam_vel = (RR_V3){0.0f, 0.0f, 0.0f};
    g_cam_prev = (RR_V3){0.0f, 0.0f, 0.0f};
    g_cam_prev_at = 0;
    g_cam_vel_valid = 0;
    g_cam_prev_valid = 0;
}

static void aim_lock_reset(void) {
    g_lock_addr = 0;
    g_lock_id = 0;
    g_lock_bone = -1;
    g_lock_group = -1;
    g_lock_at = 0;
    g_lock_miss_at = 0;
    g_react_addr = 0;
    g_react_id = 0;
    g_react_at = 0;
    aim_random_bone_reset();
    aim_track_reset();
}

/* Camera translation velocity, smoothed over a few frames. A teleport or a
 * spectate cut is rejected instead of becoming a huge lead. */
static void aim_cam_track(const RR_CAMERA* cam, DWORD now) {
    if (!cam || !cam->valid || !isfinite(cam->pos.x) || !isfinite(cam->pos.y) ||
        !isfinite(cam->pos.z)) {
        aim_cam_reset();
        return;
    }
    if (!g_cam_prev_valid) {
        g_cam_prev = cam->pos;
        g_cam_prev_at = now;
        g_cam_prev_valid = 1;
        g_cam_vel_valid = 0;
        return;
    }
    DWORD dt = now - g_cam_prev_at;
    if (dt >= 4 && dt <= 120) {
        float inv = 1000.0f / (float)dt;
        RR_V3 v = { (cam->pos.x - g_cam_prev.x) * inv,
                    (cam->pos.y - g_cam_prev.y) * inv,
                    (cam->pos.z - g_cam_prev.z) * inv };
        float s2 = v.x*v.x + v.y*v.y + v.z*v.z;
        if (isfinite(s2) && s2 <= 50.0f * 50.0f) {
            if (!g_cam_vel_valid) {
                g_cam_vel = v;
                g_cam_vel_valid = 1;
            } else {
                g_cam_vel.x += 0.5f * (v.x - g_cam_vel.x);
                g_cam_vel.y += 0.5f * (v.y - g_cam_vel.y);
                g_cam_vel.z += 0.5f * (v.z - g_cam_vel.z);
            }
        } else {
            g_cam_vel_valid = 0;
        }
        g_cam_prev = cam->pos;
        g_cam_prev_at = now;
    } else if (dt > 120) {
        g_cam_prev = cam->pos;
        g_cam_prev_at = now;
        g_cam_vel_valid = 0;
    }
}

/* World-space velocity of the selected joint, used when the replicated root
 * sample is stale. Only the same target and joint are compared; a bone swap or
 * a teleport starts a new sample instead of producing a fake velocity. */
static void aim_track_update(const RR_AIM_PICK* pick, DWORD now) {
    if (!pick || !pick->player) {
        aim_track_reset();
        return;
    }
    const RR_PLAYER* p = pick->player;
    int same_target = g_track_valid && g_track_addr == p->addr && g_track_id == p->id;
    if (!same_target) {
        g_track_addr = p->addr;
        g_track_id = p->id;
        g_track_bone = pick->bone;
        g_track_point = pick->point;
        g_track_at = now;
        g_track_valid = 1;
        g_track_vel = (RR_V3){0.0f, 0.0f, 0.0f};
        g_track_vel_valid = 0;
        return;
    }
    if (g_track_bone != pick->bone) {
        /* A full rig and its seven-joint fallback expose the head under
         * different indices. Keep the measured body velocity when the new
         * point is still on the same body; a real jump drops it. */
        float dx = pick->point.x - g_track_point.x;
        float dy = pick->point.y - g_track_point.y;
        float dz = pick->point.z - g_track_point.z;
        if (!isfinite(dx + dy + dz) || dx*dx + dy*dy + dz*dz > 2.25f)
            g_track_vel_valid = 0;
        g_track_bone = pick->bone;
        g_track_point = pick->point;
        g_track_at = now;
        return;
    }
    DWORD dt = now - g_track_at;
    if (dt >= 4 && dt <= 120) {
        float inv = 1000.0f / (float)dt;
        RR_V3 v = { (pick->point.x - g_track_point.x) * inv,
                    (pick->point.y - g_track_point.y) * inv,
                    (pick->point.z - g_track_point.z) * inv };
        float s2 = v.x*v.x + v.y*v.y + v.z*v.z;
        if (isfinite(s2) && s2 <= 40.0f * 40.0f) {
            if (!g_track_vel_valid) {
                g_track_vel = v;
                g_track_vel_valid = 1;
            } else {
                g_track_vel.x += 0.5f * (v.x - g_track_vel.x);
                g_track_vel.y += 0.5f * (v.y - g_track_vel.y);
                g_track_vel.z += 0.5f * (v.z - g_track_vel.z);
            }
        } else {
            g_track_vel_valid = 0;
        }
        g_track_point = pick->point;
        g_track_at = now;
    } else if (dt > 120) {
        g_track_point = pick->point;
        g_track_at = now;
        g_track_vel_valid = 0;
    }
}

/* Steady-state lag of the discrete servo, in seconds. The servo settles a
 * constant-velocity error over (1/gain - 1) frames; one more frame covers the
 * command being applied after the camera snapshot. */
static float aim_lead_seconds(const RR_AIM_STEER* steer, int smooth, unsigned elapsed_ms) {
    float alpha = fmaxf(0.01f, fminf(1.0f, (float)smooth / 100.0f));
    int latency = steer ? steer->latency : AIM_LATENCY_FRAMES;
    if (latency < 1 || latency > 8) latency = AIM_LATENCY_FRAMES;
    float gain = sqrtf(alpha) / (float)(latency + 1);
    if (!(gain > 0.001f)) gain = 0.001f;
    float frames = 1.0f / gain - 1.0f;
    float dt = (float)(elapsed_ms ? elapsed_ms : 16u);
    if (dt < 4.0f) dt = 4.0f;
    if (dt > 50.0f) dt = 50.0f;
    float seconds = (frames + 1.0f) * dt * 0.001f;
    if (seconds > 0.20f) seconds = 0.20f;
    return seconds;
}

/* Lead the selected point by the relative target/camera velocity. Returns 0
 * when there is no usable velocity or the prediction leaves the body, so the
 * caller keeps the plain projected error. */
static int aim_predict(const RR_AIM_PICK* pick, const RR_CAMERA* cam,
                       const RR_AIM_STEER* steer, int smooth, unsigned elapsed_ms,
                       int width, int height, RR_V3* out, float* dx, float* dy) {
    if (!pick || !pick->player || !cam || !cam->valid || !out || !dx || !dy) return 0;
    float seconds = aim_lead_seconds(steer, smooth, elapsed_ms);
    /* A fallback point already carries up to 70 ms of extrapolation. */
    seconds -= (float)pick->player->lead_ms * 0.001f;
    if (!(seconds > 0.0f)) return 0;

    RR_V3 vel = {0.0f, 0.0f, 0.0f};
    int have = 0;
    if (pick->player->vel_valid) {
        vel = pick->player->vel;
        have = 1;
    }
    if (g_track_vel_valid) {
        float root2 = have ? vel.x*vel.x + vel.y*vel.y + vel.z*vel.z : 0.0f;
        float point2 = g_track_vel.x*g_track_vel.x + g_track_vel.y*g_track_vel.y +
                       g_track_vel.z*g_track_vel.z;
        /* A fresh rig can move while the replicated character transform is
         * still parked at its first sample. The joint itself is the truth;
         * only override the root when it is clearly moving faster, so head
         * bob does not become body travel. */
        if (!have || point2 > root2 * 4.0f + 0.25f) {
            vel = g_track_vel;
            have = 1;
        }
    }
    if (!have && !g_cam_vel_valid) return 0;

    float s2 = vel.x*vel.x + vel.y*vel.y + vel.z*vel.z;
    if (!isfinite(s2) || s2 > 50.0f * 50.0f) return 0;

    /* Bound the angular lead: a very close, very fast target would otherwise
     * ask for a turn larger than the screen and swing the view. */
    float rx = pick->point.x - cam->pos.x;
    float ry = pick->point.y - cam->pos.y;
    float rz = pick->point.z - cam->pos.z;
    float range = sqrtf(rx*rx + ry*ry + rz*rz);

    /* Our own translation is already tracked by the servo feedback; the
     * full-lag lead here made the crosshair sit beside the head whenever the
     * local player strafed. Keep only a short, damped parallax lead. */
    float cam_seconds = fminf(seconds, 0.035f) * 0.6f;

    float lead_x = vel.x * seconds;
    /* Vertical lead is mostly head bob, jump arcs and vehicle suspension. A
     * full-lag vertical lead parks the crosshair above the head, so bound it;
     * horizontal lead carries the tracking. */
    float lead_y = vel.y * seconds;
    if (lead_y > 0.06f) lead_y = 0.06f;
    else if (lead_y < -0.06f) lead_y = -0.06f;
    float lead_z = vel.z * seconds;
    if (g_cam_vel_valid) {
        lead_x -= g_cam_vel.x * cam_seconds;
        lead_y -= g_cam_vel.y * cam_seconds;
        lead_z -= g_cam_vel.z * cam_seconds;
    }

    float lead2 = lead_x*lead_x + lead_y*lead_y + lead_z*lead_z;
    if (!isfinite(lead2)) return 0;
    if (range > 0.1f) {
        float max_lead = range * 0.2679f;   /* tan(15 degrees) */
        float wanted = sqrtf(lead2);
        if (wanted > max_lead && wanted > 0.0001f) {
            float shrink = max_lead / wanted;
            lead_x *= shrink;
            lead_y *= shrink;
            lead_z *= shrink;
        }
    }

    RR_V3 point = { pick->point.x + lead_x,
                    pick->point.y + lead_y,
                    pick->point.z + lead_z };
    float px = point.x - pick->player->pos.x;
    float py = point.y - pick->player->pos.y;
    float pz = point.z - pick->player->pos.z;
    if (!isfinite(px + py + pz) || px*px + py*py + pz*pz > 100.0f) return 0;

    float sx = 0.0f, sy = 0.0f;
    if (!game_w2s(cam, &point, width, height, &sx, &sy)) return 0;
    float ex = sx - width * 0.5f, ey = sy - height * 0.5f;
    if (!isfinite(ex) || !isfinite(ey) ||
        fabsf(ex) > (float)width || fabsf(ey) > (float)height) return 0;

    *out = point;
    *dx = ex;
    *dy = ey;
    return 1;
}

/* A relative SendInput moves the desktop cursor as well as the game camera.
 * The game normally hides/clips it, but a windowed client does not. While the
 * move is delivered, clamp the cursor to the game client so it cannot walk on
 * to another monitor; restore the previous clip rectangle immediately. */
static void aim_send_move(int dx, int dy) {
    /* Re-check at the moment of injection: focus can move between the frame
     * gate and this call, and a relative move in another app would walk the
     * desktop cursor. Fail closed. */
    if (!rr_gate_foreground()) return;
    RECT old_clip;
    int had_clip = GetClipCursor(&old_clip);
    int clipped = 0;
    HWND hwnd = rr_gate_hwnd();
    if (hwnd && IsWindow(hwnd)) {
        RECT rc;
        if (GetClientRect(hwnd, &rc)) {
            POINT corners[2] = { { rc.left, rc.top }, { rc.right, rc.bottom } };
            MapWindowPoints(hwnd, NULL, corners, 2);
            rc.left = corners[0].x; rc.top = corners[0].y;
            rc.right = corners[1].x; rc.bottom = corners[1].y;
            if (!had_clip || old_clip.left < rc.left || old_clip.top < rc.top ||
                old_clip.right > rc.right || old_clip.bottom > rc.bottom) {
                if (ClipCursor(&rc)) clipped = 1;
            }
        }
    }

    INPUT input = {0};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    SendInput(1, &input, sizeof(input));

    if (clipped) ClipCursor(had_clip ? &old_clip : NULL);
}

static int aim_clear_path(RR_V3 start, RR_V3 end) {
    return rr_los_check(start, end) == RR_LOS_CLEAR;
}

/* The internal host has no LOS worker; this callback keeps the trigger's
 * geometric crosshair test available instead of disabling the trigger. */
static int aim_clear_path_none(RR_V3 start, RR_V3 end) {
    (void)start; (void)end;
    return 1;
}

/* The map, inventory and death screen show the OS cursor and expect it to
 * move freely. Never inject mouse counts in that state: SendInput would drag
 * the UI cursor, and the camera hook would keep rotating a view the game is
 * not using for aiming. */
static int game_cursor_visible(void) {
    CURSORINFO ci;
    ci.cbSize = sizeof(ci);
    return GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING);
}

RR_AIM_PICK rr_trigger_pick(const RR_PLAYER* players, int count, const RR_CAMERA* camera,
                            const BR_Config* config, int (*clear)(RR_V3, RR_V3)) {
    RR_AIM_PICK best = {0};
    best.bone = -1;
    if (!players || count <= 0 || !camera || !camera->valid || !config || !clear) return best;
    if (count > RR_MAX_PLAYERS) count = RR_MAX_PLAYERS;
    RR_V3 ray = camera->fwd;
    float length = sqrtf(ray.x*ray.x + ray.y*ray.y + ray.z*ray.z);
    if (!isfinite(length) || length < 0.001f || !isfinite(camera->pos.x) ||
        !isfinite(camera->pos.y) || !isfinite(camera->pos.z)) return best;
    ray.x /= length; ray.y /= length; ray.z /= length;
    float nearest = INFINITY;
    for (int i = 0; i < count; ++i) {
        const RR_PLAYER* p = &players[i];
        if (!p->addr || !p->valid || p->local || p->hidden_in_object || p->is_vehicle || p->is_loot ||
            (!p->inveh && p->nbones <= 0) || p->nbones > RR_MAX_BONES || !isfinite(p->dist) ||
            (config->aa_dist > 0 && p->dist > config->aa_dist) ||
            aim_target_ignored(p, config) ||
            !rr_relationship_allowed(p->relationships, config->trigger_targets & RR_REL_CLAN,
                                     config->trigger_targets & RR_REL_POLICE,
                                     config->trigger_targets & RR_REL_PARTY)) continue;
        int (*target_clear)(RR_V3, RR_V3) = p->inveh ? aim_clear_static_path : clear;
        for (int group = RR_AIM_HEAD; group <= RR_AIM_CHEST; group += 2) {
            int bone;
            RR_V3 point;
            if (group == RR_AIM_HEAD) {
                if (!head_aim_point(p, &point)) continue;
                bone = p->nbones > 0
                    ? (p->full_rig ? RR_RIG_M_HEAD_1_JNT : p->bone_group[RR_AIM_HEAD])
                    : -1;
            } else {
                bone = p->full_rig ? RR_RIG_M_SPINE_3_JNT : p->bone_group[group];
                if (bone < 0 || bone >= p->nbones) continue;
                point = (RR_V3){p->bone[bone][0], p->bone[bone][1], p->bone[bone][2]};
            }
            RR_V3 oc = {point.x-camera->pos.x, point.y-camera->pos.y, point.z-camera->pos.z};
            float distance2 = oc.x*oc.x + oc.y*oc.y + oc.z*oc.z;
            float t = oc.x*ray.x + oc.y*ray.y + oc.z*ray.z;
            float radius = group == RR_AIM_HEAD ? 0.15f : 0.20f;
            if (!isfinite(distance2) || !isfinite(t) || t <= 0 || t >= nearest ||
                distance2 - t*t > radius*radius || !target_clear(camera->pos, point)) continue;
            nearest = t;
            best.player = p;
            best.point = point;
            best.bone = bone;
        }
    }
    return best;
}

int rr_trigger_ready(RR_TRIGGER_STATE* state, uint64_t addr, int32_t id,
                      unsigned now, int delay_ms) {
    if (!state) return 0;
    if (!addr) { memset(state, 0, sizeof(*state)); return 0; }
    unsigned delay = delay_ms < 0 ? 0u : delay_ms > 300 ? 300u : (unsigned)delay_ms;
    if (!state->tracked || state->addr != addr || state->id != id) {
        memset(state, 0, sizeof(*state));
        state->tracked = 1; state->addr = addr; state->id = id; state->acquired_at = now;
    }
    if (now - state->acquired_at < delay ||
        (state->fired && now - state->last_shot < (delay > 16 ? delay : 16))) return 0;
    state->last_shot = now;
    state->fired = 1;
    return 1;
}

/* Nospread closes the client cone by zeroing the two spread carriers the
 * snapshot reader resolved. It is a write, so it only runs when enabled. */
static int aim_nospread_apply(const RR_PLAYER* players, int count) {
    static const float zero = 0.0f;
    static const float zero3[3] = { 0.0f, 0.0f, 0.0f };
    int wrote = 0;
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

void rr_aim_update(const RR_PLAYER* players, int count, const RR_CAMERA* camera,
                   int width, int height) {
    InterlockedExchange(&g_aim_active, 0);
    memset(&g_frame, 0, sizeof(g_frame));
    g_nospread_writes = 0;
    BR_Config* config = br_get();
    if (!config || !players || count <= 0 || !camera || !camera->valid ||
        width < 16 || height < 16 || config->menu_open) {
        aim_steer_reset();
        memset(&g_trigger_state, 0, sizeof(g_trigger_state));
        aim_lock_reset();
        game_cam_clear();
        g_input_at = 0;
        return;
    }
    if (count > RR_MAX_PLAYERS) count = RR_MAX_PLAYERS;

    DWORD now = timeGetTime();
    /* Keep the camera-velocity estimate warm even on frames where the aim key
     * is up or another app owns focus, so the first correction already leads. */
    aim_cam_track(camera, now);
    int local_inveh = 0;
    for (int i = 0; i < count; ++i) {
        if (players[i].valid && players[i].local && players[i].inveh) {
            local_inveh = 1;
            break;
        }
    }
    float cam_speed2 = g_cam_vel.x*g_cam_vel.x + g_cam_vel.y*g_cam_vel.y +
                       g_cam_vel.z*g_cam_vel.z;
    /* A parked vehicle is safe to learn from; a moving one can rotate the view
     * without a mouse command. */
    int learn_ok = !local_inveh || !g_cam_vel_valid || cam_speed2 <= 0.09f;

    /* Never inject input while another app owns the desktop. GetAsyncKeyState
     * would otherwise fire on the aim key in a browser and move the cursor on
     * whichever monitor has focus. */
    if (!rr_gate_foreground()) {
        aim_steer_reset();
        memset(&g_trigger_state, 0, sizeof(g_trigger_state));
        aim_lock_reset();
        game_cam_clear();
        g_input_at = 0;
        return;
    }

    /* Map, inventory and death UI use the desktop cursor. Mouse aim would
     * drag it across the screen, and camera aim would fight the UI camera. */
    if (game_cursor_visible()) {
        aim_steer_reset();
        memset(&g_trigger_state, 0, sizeof(g_trigger_state));
        aim_lock_reset();
        game_cam_clear();
        g_input_at = 0;
        return;
    }

    unsigned elapsed = g_input_at ? now - g_input_at : 16;
    g_input_at = now;
    int held = (GetAsyncKeyState(config->aim_key) & 0x8000) != 0;
    int firing = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    int legit = config->aim_legit && !config->rage;
    /* Only enforce the filter when the host can answer; a host without the
     * collision worker reports unavailable, so an UNKNOWN result cannot make
     * the aim dead. */
    int require_clear = rr_los_available() && (config->aa_los_on || legit);

    /* Nospread is a write, and the server can see the carrier change. Only
     * touch it while a shot is in flight (plus a short tail), instead of
     * rewriting the carriers on every idle frame for the whole session. */
    static DWORD g_nospread_fire_at;
    if (firing) g_nospread_fire_at = now;
    if (config->nospread &&
        (firing || (g_nospread_fire_at &&
                    now - g_nospread_fire_at < AIM_NOSPREAD_TAIL_MS)))
        aim_nospread_apply(players, count);

    if (g_lock_group != config->aa_bone) aim_lock_reset();
    g_lock_group = config->aa_bone;

    if (config->aim_enabled && held) {
        const int fresh = g_lock_addr && (now - g_lock_at) < 500;
        RR_AIM_PICK pick = rr_aim_pick(players, count, camera, width, height, config,
                                      fresh ? g_lock_addr : 0,
                                      fresh ? g_lock_id : 0,
                                      fresh ? g_lock_bone : -1,
                                      require_clear ? aim_clear_path : NULL);

        /* Do not swap to a different body on a single blocked frame. Give the
         * locked target a short grace period before acquiring someone else. */
        if (!pick.player && fresh) {
            if (!g_lock_miss_at) g_lock_miss_at = now;
            if (now - g_lock_miss_at >= (legit ? AIM_LEGIT_GRACE_MS : 150u))
                pick = rr_aim_pick(players, count, camera, width, height, config,
                                   0, 0, -1,
                                   require_clear ? aim_clear_path : NULL);
        }

        /* Legit mode: track a newly acquired target but do not steer it for
         * the reaction window. A switch to another body restarts the window,
         * so the view cannot jump between heads on consecutive frames. */
        int reacting = 0;
        if (pick.player && legit) {
            if (pick.player->addr != g_react_addr || pick.player->id != g_react_id) {
                g_react_addr = pick.player->addr;
                g_react_id = pick.player->id;
                g_react_at = now;
            }
            reacting = g_react_at && (now - g_react_at) < AIM_LEGIT_REACTION_MS;
        } else if (!pick.player) {
            g_react_addr = 0;
            g_react_id = 0;
            g_react_at = 0;
        }

        if (pick.player) {
            if (pick.player->addr != g_lock_addr || pick.player->id != g_lock_id)
                aim_steer_retarget();
            g_lock_addr = pick.player->addr;
            g_lock_id = pick.player->id;
            g_lock_bone = pick.bone;
            g_lock_group = config->aa_bone;
            g_lock_at = now;
            g_lock_miss_at = 0;

            /* Track the selected joint before either path. The camera lock
             * used to run without this sample, so a target that moved while
             * the view was being pushed was always one frame behind. */
            aim_track_update(&pick, now);

            /* Humanization is a bounded wobble around the selected joint. It is
             * generated in reference pixels so the same setting looks the same
             * at every resolution and zoom, then applied to whichever steering
             * path is active. */
            float human_x = 0.0f, human_y = 0.0f;
            if (config->aim_humanize)
                rr_aim_humanize_offset(config->aim_humanize_strength,
                                       config->aim_humanize_speed,
                                       (unsigned)now, aim_humanize_seed(pick.player),
                                       &human_x, &human_y);

            int camera_engaged = 0;
            int cam_smooth = 0;
            RR_V3 cam_point = pick.point;
            if (g_camera_mode && !reacting) {
                /* The slider is a response percentage: 100 is the instant
                 * look-at, 1 is the slowest slew. */
                cam_smooth = (100 - config->aim_smooth) * 20 / 99;
                if (cam_smooth < 0) cam_smooth = 0;
                if (cam_smooth > 20) cam_smooth = 20;
                /* Lead the camera target too. The hook applies on the next
                 * presented frame, so it needs a shorter design latency than
                 * the five-frame mouse servo. A slower camera blend gets a
                 * longer lead through the response mapping. */
                RR_AIM_STEER cam_steer = g_input_steer;
                cam_steer.latency = 1;
                int cam_response = 100 / (1 + cam_smooth);
                if (cam_response < 1) cam_response = 1;
                float cdx = pick.dx, cdy = pick.dy;
                if (!aim_predict(&pick, camera, &cam_steer, cam_response, elapsed,
                                 width, height, &cam_point, &cdx, &cdy) ||
                    (require_clear && !aim_clear_path(camera->pos, cam_point)))
                    cam_point = pick.point;
                if (config->aim_humanize) {
                    RR_V3 human_point;
                    if (aim_humanize_point(camera, cam_point, human_x, human_y,
                                           &human_point) &&
                        (!require_clear || aim_clear_path(camera->pos, human_point)))
                        cam_point = human_point;
                }
                camera_engaged = game_cam_track(camera, pick.player->addr, cam_point,
                                                cam_smooth,
                                                legit ? AIM_LEGIT_MAX_DPS : 0);
            }

            if (camera_engaged) {
                /* The camera hook owns the view. Keep the same pick and lock
                 * state, but do not inject mouse counts: a second correction
                 * would fight the hook. */
                InterlockedExchange(&g_aim_active, 1);
                float yaw = 0.0f, pitch = 0.0f;
                aim_angles(camera, &cam_point, &yaw, &pitch);
                g_frame.picked = 1;
                g_frame.addr = pick.player->addr;
                g_frame.id = pick.player->id;
                g_frame.bone = pick.bone;
                g_frame.point = cam_point;
                g_frame.yaw = yaw;
                g_frame.pitch = pitch;
                g_frame.sent_x = 0;
                g_frame.sent_y = 0;
                g_frame.dpc = 0.0f;
                g_frame.cal = 0;
                g_frame.cal_samples = 0;
                g_frame.latency = 0;
                g_frame.fov = camera->fov;
                g_frame.body = pick.player->pos;
                g_frame.err_x = 0.0f;
                g_frame.err_y = 0.0f;
                g_frame.px_x = g_frame.px_y = 0.0f;
                g_frame.sign_x = g_frame.sign_y = 1;
                g_frame.cam = 1;
            } else if (reacting) {
                /* The target is acquired but the reaction window is still
                 * open. Keep the lock state and diagnostics; drop any camera
                 * target from the previous body so the view does not carry it
                 * into the new one. */
                game_cam_clear();
                g_frame.picked = 1;
                g_frame.addr = pick.player->addr;
                g_frame.id = pick.player->id;
                g_frame.bone = pick.bone;
                g_frame.point = pick.point;
                g_frame.fov = camera->fov;
                g_frame.body = pick.player->pos;
                g_frame.sign_x = g_frame.sign_y = 1;
            } else {
                /* Follow the joint where it will be when this correction lands.
                 * The plain projected error always trails a moving body by the
                 * servo's response time, which is exactly what made a running or
                 * seated target slip out from under the crosshair. The joint
                 * velocity was already sampled above, before the camera try. */
                RR_V3 aim_point = pick.point;
                float aim_dx = pick.dx, aim_dy = pick.dy;
                if (!aim_predict(&pick, camera, &g_input_steer, config->aim_smooth,
                                 elapsed, width, height, &aim_point, &aim_dx, &aim_dy) ||
                    (require_clear && !aim_clear_path(camera->pos, aim_point))) {
                    /* No usable lead, or the lead crosses cover: keep the visible
                     * joint instead of steering into a wall. */
                    aim_point = pick.point;
                    aim_dx = pick.dx;
                    aim_dy = pick.dy;
                }
                if (config->aim_humanize) {
                    RR_V3 human_point;
                    float ref_scale = aim_ref_scale(camera->fov, height);
                    if (ref_scale > 0.0f &&
                        aim_humanize_point(camera, aim_point, human_x, human_y,
                                           &human_point) &&
                        (!require_clear || aim_clear_path(camera->pos, human_point))) {
                        aim_dx += human_x / ref_scale;
                        aim_dy += human_y / ref_scale;
                    }
                }

                int mx, my;
                rr_aim_step(&g_input_steer, aim_dx, aim_dy, camera->fov, height,
                            config->aim_smooth, config->aa_aim_cal, elapsed, firing, &mx, &my);
                if (mx || my) {
                    aim_send_move(mx, my);
                    InterlockedExchange(&g_aim_active, 1);
                }
                /* Feed the camera response back into the sensitivity estimate.
                 * Runs on quiet frames too, so the episode can settle and sample. */
                if (learn_ok) aim_servo_learn(&g_input_steer, camera, mx, my, firing);
                else aim_learn_suspend();

                float yaw = 0.0f, pitch = 0.0f;
                aim_angles(camera, &aim_point, &yaw, &pitch);
                float dpc = g_input_steer.dpc;
                if (!(dpc > AIM_DPC_MIN) || !isfinite(dpc)) dpc = aim_cal_seed_dpc(config->aa_aim_cal);
                /* Screen pixels per mouse count at the current FOV. */
                float px = dpc * (float)height / camera->fov;
                if (!isfinite(px) || px < 0.0f) px = 0.0f;
                g_frame.picked = 1;
                g_frame.addr = pick.player->addr;
                g_frame.id = pick.player->id;
                g_frame.bone = pick.bone;
                g_frame.point = aim_point;
                g_frame.yaw = yaw;
                g_frame.pitch = pitch;
                g_frame.sent_x = mx;
                g_frame.sent_y = my;
                g_frame.dpc = dpc;
                g_frame.cal = (int)(100.0f / (18.0f * dpc) + 0.5f);
                g_frame.cal_samples = g_input_steer.dpc_samples;
                g_frame.latency = g_input_steer.latency;
                g_frame.fov = camera->fov;
                g_frame.body = pick.player->pos;
                g_frame.err_x = aim_dx;
                g_frame.err_y = aim_dy;
                g_frame.px_x = g_frame.px_y = px;
                g_frame.sign_x = g_frame.sign_y = 1;
            }
        } else {
            /* Keep observing the view while the lock is in its grace window,
             * so the response to the last correction is not lost. The camera
             * target is held the same way: a one-frame occlusion must not snap
             * the view back and forth. */
            if (!fresh) game_cam_clear();
            if (learn_ok) aim_servo_learn(&g_input_steer, camera, 0, 0, firing);
            else aim_learn_suspend();
            if (fresh) {
                aim_steer_retarget();
            } else {
                aim_steer_reset();
                aim_lock_reset();
            }
        }
    } else {
        aim_steer_reset();
        aim_lock_reset();
        game_cam_clear();
    }

    /* Key zero follows the aim hold key, even when aim steering is disabled. */
    int trigger_held = config->trigger_key ? (GetAsyncKeyState(config->trigger_key) & 0x8000) != 0 : held;
    RR_AIM_PICK trigger = {0};
    if (config->trigger_enabled && trigger_held && !firing && rr_gate_foreground())
        trigger = rr_trigger_pick(players, count, camera, config,
                                  config->trigger_los ? aim_clear_path : aim_clear_path_none);
    if (rr_gate_foreground() &&
        rr_trigger_ready(&g_trigger_state, trigger.player ? trigger.player->addr : 0,
                         trigger.player ? trigger.player->id : 0, now, config->trigger_delay)) {
        INPUT click[2] = {0};
        click[0].type = click[1].type = INPUT_MOUSE;
        click[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        click[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        /* A partial insertion must not leave the synthetic left button held. */
        if (SendInput(2, click, sizeof(INPUT)) == 1) SendInput(1, &click[1], sizeof(INPUT));
    }
}

int rr_aim_is_active(void) {
    return InterlockedCompareExchange(&g_aim_active, 0, 0) != 0;
}
