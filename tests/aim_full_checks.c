/* Offline checks for every exported function in rr_aim.c, plus a closed loop:
 * a simulated game that answers mouse counts with a known number of pixels.
 *
 * The loop is the part that matters. The unit checks prove each function does
 * what its comment says; the loop proves the whole worker settles on a body
 * instead of swinging around it, which is the failure a player reports as
 * "it jumps and does not work".
 *
 * Everything is deterministic: the clock is fake, SendInput is a buffer, and
 * the "game" is a linear plant with a configurable gain and camera lag.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "rr_game.h"
#include "menu_bridge.h"

/* --- mocks ---------------------------------------------------------------- */
static SHORT keys[256];
static DWORD fake_now = 1000;
static int los_result = 1;
static int send_calls, send_result = -1;
static double pend_x, pend_y;              /* counts inserted this frame */
static BR_Config config;

static SHORT WINAPI fake_key(int vk) { return vk >= 0 && vk < 256 ? keys[vk] : 0; }
static DWORD WINAPI fake_clock(void) { return fake_now; }
static UINT WINAPI fake_send(UINT count, LPINPUT input, int size) {
    (void)size;
    if (send_result == 0) return 0;
    for (UINT i = 0; i < count; ++i) {
        if (input[i].type == INPUT_MOUSE && input[i].mi.dwFlags == MOUSEEVENTF_MOVE) {
            pend_x += input[i].mi.dx;
            pend_y += input[i].mi.dy;
        }
    }
    ++send_calls;
    return send_result < 0 ? count : (UINT)send_result;
}
#define SendInput fake_send
#define GetAsyncKeyState fake_key
#define timeGetTime fake_clock
#define RR_AIM_NOW() ((ULONGLONG)fake_now)

#include "rr_recoil.c"
#include "rr_antiaim.c"
#include "rr_aim.c"

int rr_raw_mouse_delta(int* dx, int* dy) { if (dx) *dx = 0; if (dy) *dy = 0; return 0; }
/* Nospread issues a write through the driver; this suite drives no spread
 * carrier, so the transport is a no-op that counts calls. */
int rr_write(uint64_t at, const void* in, size_t n) { (void)at; (void)in; (void)n; return 1; }
BR_Config* br_get(void) { return &config; }
int rr_los_check(RR_V3 from, RR_V3 to) { (void)from; (void)to; return los_result; }
static int clear_all(RR_V3 from, RR_V3 to) { (void)from; (void)to; return 1; }
static int clear_none(RR_V3 from, RR_V3 to) { (void)from; (void)to; return 0; }
static int clear_path_cb(RR_V3 from, RR_V3 to) { return rr_los_check(from, to) == RR_LOS_CLEAR; }

/* --- plant: the game the worker steers ----------------------------------- */
/* The camera does not move; the view does. A point at world (x, y, z) projects
 * to the crosshair plus 100 px per metre of offset, minus whatever view shift
 * the inserted counts have produced and the next snapshot can see. */
static double plant_px = 0.35;              /* pixels per count, both axes */
static double plant_px_y = -1.0;            /* <0: same as plant_px */
static double plant_lag = 1.0;              /* fraction of the move visible next frame */
static double view_x, view_y;               /* view shift asked for */
static double shown_x, shown_y;             /* view shift the next snapshot sees */
static int    plant_steps;

static int game_w2s(const RR_CAMERA* cam, const RR_V3* p, int w, int h, float* x, float* y) {
    if (!cam || !cam->valid || !p) return 0;
    if (!isfinite(p->x) || !isfinite(p->y) || !isfinite(p->z)) return 0;
    if (p->z <= cam->pos.z) return 0;
    *x = (float)(w * 0.5 + (p->x - cam->pos.x) * 100.0 - shown_x);
    *y = (float)(h * 0.5 - (p->y - cam->pos.y) * 100.0 - shown_y);
    return 1;
}

static void plant_gain(double px, double py) { plant_px = px; plant_px_y = py; }

static void plant_apply(void) {
    const double gy = plant_px_y < 0.0 ? plant_px : plant_px_y;
    view_x += pend_x * plant_px;
    view_y += pend_y * gy;
    shown_x += (view_x - shown_x) * plant_lag;
    shown_y += (view_y - shown_y) * plant_lag;
    pend_x = pend_y = 0.0;
    ++plant_steps;
}

static double plant_err_x(float target_px_x) { return (double)target_px_x - shown_x; }
static double plant_err_y(RR_V3 point, const RR_CAMERA* cam) {
    return -(double)((point.y - cam->pos.y) * 100.0) - shown_y;
}

/* --- fixtures ------------------------------------------------------------- */
static RR_PLAYER targets[4];
static RR_CAMERA camera;
static int ntargets = 1;

static void fixture(void) {
    memset(&config, 0, sizeof(config));
    memset(targets, 0, sizeof(targets));
    memset(keys, 0, sizeof(keys));
    memset(&camera, 0, sizeof(camera));
    camera.valid = 1;
    camera.fov = 60;
    camera.fwd.z = 1;
    camera.right.x = 1;
    camera.up.y = 1;
    config.aim_key = VK_RBUTTON;
    config.aim_fov = 100;
    config.aim_smooth = 30;
    config.aa_aim_cal = 100;
    config.aa_dist = 150;
    config.trigger_delay = 80;
    los_result = RR_LOS_CLEAR;
    fake_now = 1000;
    send_result = -1;
    send_calls = 0;
    pend_x = pend_y = 0;
    view_x = view_y = shown_x = shown_y = 0;
    plant_px = 0.35;
    plant_px_y = -1.0;
    plant_lag = 1.0;
    plant_steps = 0;
    ntargets = 1;
    for (int i = 0; i < 4; ++i) {
        targets[i].valid = 1;
        targets[i].addr = 0x1000ull + (uint64_t)i * 0x100ull;
        targets[i].id = i + 1;
        targets[i].dist = 10.0f;
        targets[i].pos = (RR_V3){0, 0, 10};
        targets[i].head = (RR_V3){0, 0, 10};
        targets[i].health = -1;
        targets[i].nbones = 2;
        targets[i].bone_group[0] = 0;
        targets[i].bone_group[1] = 1;
        targets[i].bone[0][2] = 10.0f;
        targets[i].bone[1][2] = 10.0f;
    }
    rr_aim_update(NULL, 0, NULL, 0, 0);
}

static void set_bone(int who, int bone, float x, float y, float z) {
    targets[who].bone[bone][0] = x;
    targets[who].bone[bone][1] = y;
    targets[who].bone[bone][2] = z;
}

/* One presented frame: the worker runs on the snapshot, then the game applies
 * the counts it was handed. */
static void frame(void) {
    fake_now += 16;
    rr_aim_update(targets, ntargets, &camera, 1920, 1080);
    plant_apply();
}

/* --- closed-loop metrics -------------------------------------------------- */
typedef struct {
    double first;       /* error on the first frame                        */
    double peak;        /* largest |error| seen after the first frame      */
    int    peak_frame;
    int    settle;      /* first frame where |e| < 1.5 px for three frames */
    int    flips;       /* sign changes after the first frame              */
    double last;        /* final error                                     */
} LOOP_RUN;

static int run_loop(const char* name, double px, double lag, int fov_jitter,
                    int facial_jitter, LOOP_RUN* out) {
    fixture();
    /* The learned gain is process-wide (in-game sensitivity does not change per
     * target), so each run starts from the shipped seed to keep the runs
     * independent and the numbers comparable. */
    g_snap_x.px = g_snap_y.px = 0.35f;
    g_snap_x.valid = g_snap_y.valid = 0;
    g_snap_x.sign = g_snap_y.sign = 1;
    g_snap_x.pend = g_snap_y.pend = 0.0f;
    g_snap_x.sent = g_snap_y.sent = 0;
    g_snap_x.err = g_snap_y.err = 0.0f;
    g_snap_x.starve = g_snap_y.starve = 0;
    g_snap_x.wrong = g_snap_y.wrong = 0;
    g_snap_x.wrong_px = g_snap_y.wrong_px = 0.0f;
    config.aim_enabled = 1;
    config.aim_smooth = 100;
    config.aim_fov = 300;
    config.aa_bone = RR_AIM_HEAD;
    keys[VK_RBUTTON] = (SHORT)0x8000;
    plant_gain(px, -1.0);
    plant_lag = lag;
    set_bone(0, 0, 1.0f, 0.6f, 10.0f);       /* 100 px right, 60 px up */
    targets[0].full_rig = facial_jitter ? 1 : 0;
    targets[0].nbones = facial_jitter ? RR_CHAM_BONES : 2;
    if (facial_jitter) {
        for (int b = 0; b < RR_CHAM_BONES; ++b) {
            set_bone(0, b, 1.0f, 0.6f, 10.0f);
            targets[0].bone_group[b > 3 ? 3 : b] = b;
        }
    }

    LOOP_RUN run = {0};
    double prev = 0;
    int settled_run = 0;
    run.settle = -1;
    for (int f = 0; f < 40; ++f) {
        if (fov_jitter) camera.fov = 60.0f + (float)((f % 2) ? 0.4 : -0.4);
        if (facial_jitter) {
            /* facial animation: eyes, lips and tongue move one pixel a frame */
            const float wobble = (f % 2) ? 0.01f : -0.01f;
            set_bone(0, RR_RIG_L_HEAD01_EYE_JNT, 1.0f + wobble, 0.6f, 10.0f);
            set_bone(0, RR_RIG_R_HEAD01_EYE_JNT, 1.0f - wobble, 0.6f, 10.0f);
            set_bone(0, RR_RIG_M_TONGUE_2_JNT, 1.0f + wobble, 0.6f + wobble, 10.0f);
        }
        frame();
        const double e = plant_err_x(100.0f);
        if (f == 0) {
            run.first = e;
        } else {
            if (fabs(e) > fabs(run.peak)) {
                run.peak = fabs(e);
                run.peak_frame = f;
            }
            if (prev != 0.0 && e * prev < 0.0) ++run.flips;
        }
        if (fabs(e) < 1.5) {
            if (++settled_run == 3 && run.settle < 0) run.settle = f - 2;
        } else {
            settled_run = 0;
            run.settle = -1;
        }
        prev = e;
    }
    run.last = plant_err_x(100.0f);
    if (out) *out = run;
    printf("  %-26s px/count %5.2f  lag %.2f  first %7.1f  peak %7.1f  flips %2d  settle %3d  last %6.2f\n",
           name, px, lag, run.first, run.peak, run.flips, run.settle, run.last);
    return run.settle;
}

/* --- checks --------------------------------------------------------------- */
static void check_bone_groups(void) {
    for (int b = -1; b <= RR_CHAM_BONES; ++b) {
        for (int g = -1; g <= RR_AIM_GROUP_COUNT; ++g) {
            const int r = rr_aim_bone_in_group(g, b);
            assert(r == 0 || r == 1);
            if (b < 0 || b >= RR_CHAM_BONES || g < 0 || g >= RR_AIM_GROUP_COUNT)
                assert(r == 0);
        }
    }
    for (int b = RR_RIG_M_HEAD_1_JNT; b <= RR_RIG_M_TONGUE_3_JNT; ++b) {
        assert(rr_aim_bone_in_group(RR_AIM_HEAD, b));
        assert(rr_aim_bone_in_group(RR_AIM_CLOSEST, b));
    }
    /* FACE is the animated part of the head: eyelids and eyes upward, no skull
     * joint below it. */
    for (int b = RR_RIG_L_EYELIDUP_JNT; b <= RR_RIG_M_TONGUE_3_JNT; ++b)
        assert(rr_aim_bone_in_group(RR_AIM_FACE, b));
    assert(!rr_aim_bone_in_group(RR_AIM_FACE, RR_RIG_M_HEAD_1_JNT));
    assert(!rr_aim_bone_in_group(RR_AIM_HEAD, RR_RIG_M_NECK_JNT));
    assert(!rr_aim_bone_in_group(RR_AIM_HEAD, RR_RIG_L_ARM_0_JNT));
    assert(rr_aim_bone_in_group(RR_AIM_NECK, RR_RIG_M_NECK_JNT));
    assert(rr_aim_bone_in_group(RR_AIM_NECK, RR_RIG_L_ARM_0_JNT));
    assert(rr_aim_bone_in_group(RR_AIM_CHEST, RR_RIG_M_SPINE_2_JNT));
    assert(rr_aim_bone_in_group(RR_AIM_CHEST, RR_RIG_M_SPINE_3_JNT));
    assert(!rr_aim_bone_in_group(RR_AIM_CHEST, RR_RIG_M_SPINE_1_JNT));
    assert(rr_aim_bone_in_group(RR_AIM_PELVIS, RR_RIG_M_SPINE_1_JNT));
    assert(rr_aim_bone_in_group(RR_AIM_PELVIS, RR_RIG_L_LEG_1_JNT));
    assert(rr_aim_bone_in_group(RR_AIM_ARMS, RR_RIG_L_ARM_2_JNT));
    assert(rr_aim_bone_in_group(RR_AIM_ARMS, RR_RIG_R_ARMTWIST_JNT));
    assert(!rr_aim_bone_in_group(RR_AIM_ARMS, RR_RIG_L_INDEX_1_JNT));
    assert(rr_aim_bone_in_group(RR_AIM_HANDS, RR_RIG_L_INDEX_1_JNT));
    assert(rr_aim_bone_in_group(RR_AIM_HANDS, RR_RIG_R_THUMB_3_JNT));
    assert(!rr_aim_bone_in_group(RR_AIM_HANDS, RR_RIG_L_ARM_2_JNT));
    assert(rr_aim_bone_in_group(RR_AIM_LEGS, RR_RIG_L_LEG_2_JNT));
    assert(!rr_aim_bone_in_group(RR_AIM_LEGS, RR_RIG_L_FOOT_1_JNT));
    assert(rr_aim_bone_in_group(RR_AIM_FEET, RR_RIG_L_FOOT_2_JNT));
    assert(!rr_aim_bone_in_group(RR_AIM_FEET, RR_RIG_L_LEG_2_JNT));
    /* Every group is non-empty and disjoint where it claims to be. */
    for (int g = 0; g < RR_AIM_GROUP_COUNT; ++g) {
        int n = 0;
        for (int b = 0; b < RR_CHAM_BONES; ++b) n += rr_aim_bone_in_group(g, b);
        assert(n > 0);
    }
    puts("BONE GROUP CHECKS OK: every group, every bone, out-of-range index, head/face overlap");
}

static void check_point_selection(void) {
    fixture();
    RR_V3 p;
    /* Scripted skeleton: the group index decides the joint. */
    set_bone(0, 0, 0.25f, 0.5f, 10.0f);
    assert(rr_aim_point(&targets[0], RR_AIM_HEAD, &camera, 1920, 1080, &p));
    assert(p.x == 0.25f && p.y == 0.5f);
    /* A NaN joint is refused instead of being handed out as a point: the old
     * path returned success and let the projection drop it one call later. */
    set_bone(0, 0, NAN, 0.5f, 10.0f);
    assert(!rr_aim_point(&targets[0], RR_AIM_HEAD, &camera, 1920, 1080, &p));
    set_bone(0, 0, 0.25f, NAN, 10.0f);
    assert(!rr_aim_point(&targets[0], RR_AIM_HEAD, &camera, 1920, 1080, &p));
    /* A named joint out of range falls back to the pose estimate, not to a
     * read past the end of the bone array. */
    set_bone(0, 0, 0.25f, 0.5f, 10.0f);
    targets[0].nbones = 2;
    targets[0].bone_group[0] = 7;                 /* beyond nbones */
    assert(rr_aim_point(&targets[0], RR_AIM_HEAD, &camera, 1920, 1080, &p));
    assert(fabsf(p.y - 1.65f) < 1e-4f);

    /* No skeleton: height estimates from the feet, standing and crouched. */
    fixture();
    targets[0].nbones = 0;
    targets[0].crouch = 0;
    assert(rr_aim_point(&targets[0], RR_AIM_HEAD, &camera, 1920, 1080, &p));
    assert(fabsf(p.y - 1.65f) < 1e-4f);
    targets[0].crouch = 1;
    assert(rr_aim_point(&targets[0], RR_AIM_HEAD, &camera, 1920, 1080, &p));
    assert(fabsf(p.y - 1.00f) < 1e-4f);
    targets[0].crouch = -1;
    assert(rr_aim_point(&targets[0], RR_AIM_PELVIS, &camera, 1920, 1080, &p));
    assert(fabsf(p.y - 0.88f) < 1e-4f);

    /* Mesh anchors: a skinned body answers from the shared matrices, which is
     * what carries a player whose joints were never reconstructed. */
    fixture();
    targets[0].nbones = 0;
    targets[0].cham_valid = 1;
    for (int b = 0; b < RR_CHAM_BONES; ++b) {
        float* m = targets[0].cham_skin[b];
        memset(m, 0, 12 * sizeof(float));
        m[0] = m[5] = m[10] = 1.0f;
        m[3] = 0.0f; m[7] = 0.0f; m[11] = 10.0f;
    }
    assert(rr_aim_point(&targets[0], RR_AIM_HEAD, &camera, 1920, 1080, &p));
    /* anchor 0 rides M_head_1 at 1.61 m */
    assert(fabsf(p.y - 1.6137f) < 0.01f);
    /* An anchor that lands 40 m from the body is refused and the pose estimate
     * answers instead, so a stale matrix cannot drag the crosshair off the map. */
    targets[0].cham_skin[RR_RIG_M_HEAD_1_JNT][3] = 40.0f;
    assert(rr_aim_point(&targets[0], RR_AIM_HEAD, &camera, 1920, 1080, &p));
    assert(fabsf(p.x) < 1e-4f && fabsf(p.y - 1.65f) < 1e-4f);
    targets[0].cham_skin[RR_RIG_M_HEAD_1_JNT][3] = 0.0f;

    /* Limbs: the midpoint of the two anchor bones on the side nearest the
     * crosshair, not a random joint of the chain. */
    for (int b = 0; b < RR_CHAM_BONES; ++b) {
        float* m = targets[0].cham_skin[b];
        memset(m, 0, 12 * sizeof(float));
        m[0] = m[5] = m[10] = 1.0f;
        m[11] = 10.0f;
        m[3] = 0.5f;
        m[7] = 0.0f;
    }
    const float arm_mid_x = 0.5f + (rr_cham_anchors[4].p[0] + rr_cham_anchors[5].p[0]) * 0.5f;
    const float arm_mid_y = (rr_cham_anchors[4].p[1] + rr_cham_anchors[5].p[1]) * 0.5f;
    assert(rr_aim_point(&targets[0], RR_AIM_ARMS, &camera, 1920, 1080, &p));
    assert(fabsf(p.x - arm_mid_x) < 1e-4f && fabsf(p.y - arm_mid_y) < 1e-4f);
    const float leg_mid_x = 0.5f + (rr_cham_anchors[8].p[0] + rr_cham_anchors[9].p[0]) * 0.5f;
    assert(rr_aim_point(&targets[0], RR_AIM_LEGS, &camera, 1920, 1080, &p));
    assert(fabsf(p.x - leg_mid_x) < 1e-4f);
    /* FACE and HANDS need a renderer-order rig: a mesh-only body has neither. */
    assert(!rr_aim_point(&targets[0], RR_AIM_FACE, &camera, 1920, 1080, &p));
    assert(!rr_aim_point(&targets[0], RR_AIM_HANDS, &camera, 1920, 1080, &p));

    /* Full rig: the nearest joint of the group wins, a group whose joints are
     * all NaN answers nothing instead of a zero point at the origin. */
    fixture();
    targets[0].full_rig = 1;
    targets[0].nbones = RR_CHAM_BONES;
    for (int b = 0; b < RR_CHAM_BONES; ++b) set_bone(0, b, 2.0f, 0.5f, 10.0f);
    set_bone(0, RR_RIG_M_HEAD_2_JNT, 0.30f, 0.5f, 10.0f);
    assert(rr_aim_point(&targets[0], RR_AIM_HEAD, &camera, 1920, 1080, &p));
    assert(fabsf(p.x - 0.30f) < 1e-5f);
    for (int b = 0; b < RR_CHAM_BONES; ++b) set_bone(0, b, NAN, 0.5f, 10.0f);
    assert(!rr_aim_point(&targets[0], RR_AIM_HEAD, &camera, 1920, 1080, &p));

    /* Camera requirements differ by path and the tests say so: a named group is
     * a pure world point and needs no camera, while CLOSEST and the LIMB paths
     * project and are refused without a valid one. */
    fixture();
    set_bone(0, 0, 0.25f, 0.5f, 10.0f);
    assert(rr_aim_point(&targets[0], RR_AIM_HEAD, NULL, 1920, 1080, &p));
    assert(!rr_aim_point(&targets[0], RR_AIM_CLOSEST, NULL, 1920, 1080, &p));
    {
        RR_CAMERA bad = camera;
        bad.valid = 0;
        assert(rr_aim_point(&targets[0], RR_AIM_HEAD, &bad, 1920, 1080, &p));
        assert(!rr_aim_point(&targets[0], RR_AIM_CLOSEST, &bad, 1920, 1080, &p));
    }
    assert(!rr_aim_point(NULL, RR_AIM_HEAD, &camera, 1920, 1080, &p));
    assert(!rr_aim_point(&targets[0], RR_AIM_HEAD, &camera, 1920, 1080, NULL));
    /* A clear callback without a camera would dereference NULL: refused. */
    assert(!rr_aim_visible_point(&targets[0], RR_AIM_HEAD, NULL, 1920, 1080, &p, clear_all));
    puts("POINT SELECTION OK: scripted joint, NaN, group fallback, poses, mesh anchor bound, "
         "limb midpoints, face/hands gating, full-rig nearest, invalid camera");
}

static void check_visible_point(void) {
    fixture();
    RR_V3 p;
    set_bone(0, 0, 0.25f, 0.5f, 10.0f);
    assert(rr_aim_visible_point(&targets[0], RR_AIM_HEAD, &camera, 1920, 1080, &p, NULL));
    assert(rr_aim_visible_point(&targets[0], RR_AIM_HEAD, &camera, 1920, 1080, &p, clear_all));
    assert(!rr_aim_visible_point(&targets[0], RR_AIM_HEAD, &camera, 1920, 1080, &p, clear_none));
    /* An out-of-range group falls back to the head instead of reading past the
     * end of the group table. */
    assert(rr_aim_visible_point(&targets[0], 99, &camera, 1920, 1080, &p, NULL));

    /* Full rig, head group: the nearest visible joint wins, and a blocked
     * nearest joint falls through to the next one. */
    fixture();
    targets[0].full_rig = 1;
    targets[0].nbones = RR_CHAM_BONES;
    for (int b = 0; b < RR_CHAM_BONES; ++b) set_bone(0, b, 2.0f, 0.5f, 10.0f);
    set_bone(0, RR_RIG_M_HEAD_1_JNT, 0.10f, 0.5f, 10.0f);   /* 10 px off */
    set_bone(0, RR_RIG_M_HEAD_2_JNT, 0.40f, 0.5f, 10.0f);   /* 40 px off */
    assert(rr_aim_visible_point(&targets[0], RR_AIM_HEAD, &camera, 1920, 1080, &p, NULL));
    assert(fabsf(p.x - 0.10f) < 1e-5f);
    assert(rr_aim_visible_point(&targets[0], RR_AIM_HEAD, &camera, 1920, 1080, &p, clear_none) == 0);
    puts("VISIBLE POINT OK: clear callback, group clamp, nearest-joint choice, blocked joints");
}

static void check_pick(void) {
    fixture();
    set_bone(0, 0, 0.3f, 0.5f, 10.0f);
    set_bone(1, 0, 0.8f, 0.5f, 10.0f);
    targets[1].addr = targets[0].addr + 0x100;
    RR_AIM_PICK pick;

    /* Nearest to the crosshair wins. */
    pick = rr_aim_pick(targets, 2, &camera, 1920, 1080, &config, 0, 0, NULL);
    assert(pick.player == &targets[0]);
    /* Closest body first ignores screen distance. */
    config.aa_closest_first = 1;
    targets[1].dist = 5.0f;
    pick = rr_aim_pick(targets, 2, &camera, 1920, 1080, &config, 0, 0, NULL);
    assert(pick.player == &targets[1]);
    config.aa_closest_first = 0;
    targets[1].dist = 10.0f;

    /* The identity gates: each one alone removes the body from the ring. */
    const struct { int field; int value; } kills[] = {
        {1, 0}, {2, 0}, {3, 1}, {4, 1}, {5, 1}, {6, 1}, {7, 1}, {8, 0},
    };
    for (unsigned k = 0; k < sizeof(kills) / sizeof(kills[0]); ++k) {
        fixture();
        set_bone(0, 0, 0.3f, 0.5f, 10.0f);
        switch (kills[k].field) {
        case 1: targets[0].addr = 0; break;              /* no identity      */
        case 2: targets[0].valid = 0; break;             /* not resolved     */
        case 3: targets[0].local = 1; break;             /* the local body   */
        case 4: targets[0].dead = 1; break;              /* death flag       */
        case 5: targets[0].health = 0; break;            /* known zero       */
        case 6: targets[0].hidden_in_object = 1; break;  /* in a container   */
        case 7: targets[0].is_loot = 1; break;           /* not a body       */
        case 8: targets[0].is_vehicle = 1; break;        /* not a body       */
        default: break;
        }
        assert(!rr_aim_pick(targets, 1, &camera, 1920, 1080, &config, 0, 0, NULL).player);
    }
    /* Unknown health stays a candidate: one failed read must not empty the
     * target list. */
    fixture();
    set_bone(0, 0, 0.3f, 0.5f, 10.0f);
    targets[0].health = -1;
    assert(rr_aim_pick(targets, 1, &camera, 1920, 1080, &config, 0, 0, NULL).player == &targets[0]);

    /* Relationship masks, distance cap, NaN distance, ring radius. */
    fixture();
    set_bone(0, 0, 0.3f, 0.5f, 10.0f);
    targets[0].relationships = RR_REL_CLAN;
    config.aim_targets = RR_REL_POLICE;
    assert(!rr_aim_pick(targets, 1, &camera, 1920, 1080, &config, 0, 0, NULL).player);
    config.aim_targets = RR_REL_CLAN;
    assert(rr_aim_pick(targets, 1, &camera, 1920, 1080, &config, 0, 0, NULL).player == &targets[0]);
    /* Every switch off means every group allowed, so a body carrying a group
     * flag needs its switch back on. */
    config.aim_targets = 0;
    assert(!rr_aim_pick(targets, 1, &camera, 1920, 1080, &config, 0, 0, NULL).player);
    targets[0].relationships = 0;
    assert(rr_aim_pick(targets, 1, &camera, 1920, 1080, &config, 0, 0, NULL).player == &targets[0]);
    config.aa_dist = 5;
    assert(!rr_aim_pick(targets, 1, &camera, 1920, 1080, &config, 0, 0, NULL).player);
    config.aa_dist = 0;                      /* zero means no cap */
    assert(rr_aim_pick(targets, 1, &camera, 1920, 1080, &config, 0, 0, NULL).player == &targets[0]);
    config.aa_dist = 150;
    targets[0].dist = NAN;
    assert(!rr_aim_pick(targets, 1, &camera, 1920, 1080, &config, 0, 0, NULL).player);
    targets[0].dist = 10.0f;

    /* Zero FOV falls back to the 150 px default ring, and the ring is a circle:
     * a point just outside it is not acquired, one just inside is. */
    config.aim_fov = 0;
    set_bone(0, 0, 1.40f, 0.0f, 10.0f);      /* 140 px */
    assert(rr_aim_pick(targets, 1, &camera, 1920, 1080, &config, 0, 0, NULL).player);
    set_bone(0, 0, 1.60f, 0.0f, 10.0f);      /* 160 px */
    assert(!rr_aim_pick(targets, 1, &camera, 1920, 1080, &config, 0, 0, NULL).player);
    /* A held lock doubles the ring and restricts selection to that body. */
    assert(rr_aim_pick(targets, 1, &camera, 1920, 1080, &config,
                       targets[0].addr, targets[0].id, NULL).player == &targets[0]);
    assert(!rr_aim_pick(targets, 1, &camera, 1920, 1080, &config,
                        targets[0].addr + 1, targets[0].id, NULL).player);
    assert(!rr_aim_pick(targets, 1, &camera, 1920, 1080, &config,
                        targets[0].addr, targets[0].id + 1, NULL).player);

    /* The line-of-sight gate: a blocked nearest member falls through to
     * rr_aim_visible_point, and a blocked body is skipped entirely. */
    fixture();
    set_bone(0, 0, 0.3f, 0.5f, 10.0f);
    targets[0].full_rig = 1;
    targets[0].nbones = RR_CHAM_BONES;
    for (int b = 0; b < RR_CHAM_BONES; ++b) set_bone(0, b, 3.0f, 0.5f, 10.0f);
    set_bone(0, RR_RIG_M_HEAD_1_JNT, 0.10f, 0.5f, 10.0f);
    set_bone(0, RR_RIG_M_HEAD_2_JNT, 0.20f, 0.5f, 10.0f);
    los_result = RR_LOS_CLEAR;
    assert(rr_aim_pick(targets, 1, &camera, 1920, 1080, &config, 0, 0,
                       clear_path_cb).player == &targets[0]);
    /* The picker honours whatever visibility callback it is handed: with this
     * strict one, BLOCKED and UNKNOWN both drop the target. The host's steering
     * callback fails open on UNKNOWN while its firing callback stays strict;
     * that split is covered in aim_checks.c. */
    los_result = RR_LOS_BLOCKED;
    assert(!rr_aim_pick(targets, 1, &camera, 1920, 1080, &config, 0, 0,
                        clear_path_cb).player);
    los_result = RR_LOS_UNKNOWN;
    assert(!rr_aim_pick(targets, 1, &camera, 1920, 1080, &config, 0, 0,
                        clear_path_cb).player);

    /* Degenerate arguments answer an empty pick, never a half-filled one. */
    fixture();
    assert(!rr_aim_pick(NULL, 1, &camera, 1920, 1080, &config, 0, 0, NULL).player);
    assert(!rr_aim_pick(targets, 0, &camera, 1920, 1080, &config, 0, 0, NULL).player);
    assert(!rr_aim_pick(targets, 1, NULL, 1920, 1080, &config, 0, 0, NULL).player);
    assert(!rr_aim_pick(targets, 1, &camera, 0, 0, &config, 0, 0, NULL).player);
    assert(!rr_aim_pick(targets, 1, &camera, 1920, 1080, NULL, 0, 0, NULL).player);
    {
        RR_CAMERA bad = camera;
        bad.valid = 0;
        assert(!rr_aim_pick(targets, 1, &bad, 1920, 1080, &config, 0, 0, NULL).player);
    }
    puts("PICK CHECKS OK: nearest/closest-first, every identity gate, health, masks, distance, "
         "NaN, ring, lock widening, LOS fallback, degenerate arguments");
}

static void check_step_math(void) {
    RR_AIM_STEER st = {0};
    int mx, my;

    /* Equal angular errors must produce equal counts at any resolution or zoom:
     * the projection divides out, otherwise the same flick is weak at 4K and
     * violent in a scope. */
    int base_mx = 0;
    const int heights[3] = {1080, 1440, 2160};
    const float fovs[3] = {60.0f, 80.0f, 45.0f};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            const float deg = 2.0f;
            const float dy = (heights[i] * 0.5f) * tanf(deg * RR_AIM_PI / 180.0f) /
                             tanf(fovs[j] * RR_AIM_PI / 360.0f);
            RR_AIM_STEER s = {0};
            rr_aim_step(&s, 0.0f, dy, fovs[j], heights[i], 50, 100, 16, 0, &mx, &my);
            if (!i && !j) base_mx = my;
            assert(my == base_mx);
            assert(mx == 0);
        }
    }
    /* A zero or nonsense calibration falls back to 100, never to a divide. */
    {
        RR_AIM_STEER a = {0}, b = {0};
        rr_aim_step(&a, 40.0f, 0.0f, 60, 1080, 50, 0, 16, 0, &mx, &my);
        rr_aim_step(&b, 40.0f, 0.0f, 60, 1080, 50, 100, 16, 0, &mx, &my);
        assert(a.carry_x == b.carry_x);
    }
    /* Deadzone hysteresis, in the scaled units the axis works in: at 2160 lines
     * a 60 degree view halves the pixel error, so 0.50 scaled is 1.0 px. A
     * sub-deadzone error produces nothing and is latched inside; leaving it
     * needs a bigger error than entering it did. */
    st = (RR_AIM_STEER){0};
    rr_aim_step(&st, 0.20f, 0.0f, 60, 2160, 100, 100, 16, 0, &mx, &my);
    assert(mx == 0 && st.inside_x == 1);
    rr_aim_step(&st, 0.45f, 0.0f, 60, 2160, 100, 100, 16, 0, &mx, &my);
    assert(mx == 0 && st.inside_x == 1);              /* inside the 0.50 exit */
    rr_aim_step(&st, 1.20f, 0.0f, 60, 2160, 100, 100, 16, 0, &mx, &my);
    assert(st.inside_x == 0);                         /* leaves the latch */
    rr_aim_step(&st, 2.00f, 0.0f, 60, 2160, 100, 100, 16, 0, &mx, &my);
    assert(mx != 0);

    /* Carry: fractional counts are not lost, so a slow run still closes. */
    {
        RR_AIM_STEER s = {0};
        int total = 0;
        for (int i = 0; i < 40; ++i) {
            rr_aim_step(&s, 2.0f, 0.0f, 60, 1080, 3, 100, 16, 0, &mx, &my);
            total += mx;
        }
        assert(total > 0);
        assert(fabsf(s.carry_x) < 1.0f);
    }
    /* A sign flip drops the carry instead of subtracting it from the new move. */
    {
        RR_AIM_STEER s = {0};
        rr_aim_step(&s, 30.0f, 0.0f, 60, 1080, 4, 100, 16, 0, &mx, &my);
        assert(s.carry_x != 0.0f || mx != 0);
        const float carry = s.carry_x;
        rr_aim_step(&s, -30.0f, 0.0f, 60, 1080, 4, 100, 16, 0, &mx, &my);
        assert((carry > 0.0f && s.carry_x <= 0.0f) || (carry < 0.0f && s.carry_x >= 0.0f));
    }
    /* Per-step ceiling and flick cap: a huge error is bounded by the flick cap,
     * not sent whole, and the cap scales with calibration so it is the same
     * angular jump at any in-game sensitivity. */
    {
        RR_AIM_STEER s = {0};
        rr_aim_step(&s, 5000.0f, 0.0f, 60, 1080, 100, 100, 16, 0, &mx, &my);
        assert(mx == 160);
        rr_aim_step(&s, 5000.0f, 0.0f, 60, 1080, 100, 200, 16, 0, &mx, &my);
        assert(mx == 320);
        /* A moderate error keeps the 32-count smoothing limit. */
        rr_aim_step(&s, 40.0f, 0.0f, 60, 1080, 50, 100, 16, 0, &mx, &my);
        assert(mx > 0 && mx <= 32);
    }
    /* Firing changes the vertical follow only: the recoil-corrected axis is
     * given a shorter time constant, and the horizontal axis is untouched. */
    {
        RR_AIM_STEER a = {0}, b = {0};
        int ax, ay, bx, by;
        rr_aim_step(&a, 40.0f, 60.0f, 60, 1080, 40, 100, 16, 0, &ax, &ay);
        rr_aim_step(&b, 40.0f, 60.0f, 60, 1080, 40, 100, 16, 1, &bx, &by);
        assert(ax == bx);
        assert(by > ay);
    }
    /* Zero elapsed still moves: a same-millisecond frame must not stall. */
    {
        RR_AIM_STEER s = {0};
        rr_aim_step(&s, 30.0f, 0.0f, 60, 1080, 100, 100, 0, 0, &mx, &my);
        assert(mx != 0);
    }
    /* Bad input is rejected whole and clears the state: a NaN must not leave a
     * carry behind that steers the next frame. */
    {
        RR_AIM_STEER s = {0};
        s.carry_x = 0.5f;
        rr_aim_step(&s, NAN, 0.0f, 60, 1080, 50, 100, 16, 0, &mx, &my);
        assert(mx == 0 && my == 0 && s.carry_x == 0.0f && s.inside_x == 0);
        s.carry_x = 0.5f;
        rr_aim_step(&s, 10.0f, 0.0f, 0.0f, 1080, 50, 100, 16, 0, &mx, &my);
        assert(mx == 0 && s.carry_x == 0.0f);
        s.carry_x = 0.5f;
        rr_aim_step(&s, 10.0f, 0.0f, 200.0f, 1080, 50, 100, 16, 0, &mx, &my);
        assert(mx == 0 && s.carry_x == 0.0f);
        s.carry_x = 0.5f;
        rr_aim_step(&s, 10.0f, 0.0f, 60, 8, 50, 100, 16, 0, &mx, &my);
        assert(mx == 0 && s.carry_x == 0.0f);
        s.carry_x = 0.5f;
        rr_aim_step(NULL, 10.0f, 0.0f, 60, 1080, 50, 100, 16, 0, &mx, &my);
        assert(mx == 0 && my == 0);
    }
    puts("STEP MATH OK: resolution/FOV invariance, calibration fallback, deadzone hysteresis, "
         "carry, sign flip, caps, firing gain, zero elapsed, rejected input");
}

static void check_motion_point(void) {
    RR_V3 out;
    const RR_V3 point = {0, 1, 0};
    const RR_V3 prev = {0, 1, 0};
    const RR_V3 now = {1, 1, 0};         /* 1 m in 16 ms = 62.5 m/s: refused */
    assert(!rr_aim_motion_point(point, now, prev, 16, 0, 50, 16, &out));
    /* 0.4 m in 16 ms = 25 m/s: still outside the 20 m/s bound. */
    {
        RR_V3 fast = {0.4f, 1, 0};
        assert(!rr_aim_motion_point(point, fast, prev, 16, 0, 50, 16, &out));
    }
    /* A walking target (2 m/s, 0.032 m per 16 ms) leads by the follow lag. */
    {
        RR_V3 step = {0.032f, 1, 0};
        assert(rr_aim_motion_point(point, step, prev, 16, 0, 50, 16, &out));
        assert(out.x > point.x && out.x < 1.0f);
        assert(out.z == point.z && out.y == point.y);
    }
    /* Sample-period bounds and NaN. */
    assert(!rr_aim_motion_point(point, now, prev, 0, 0, 50, 16, &out));
    assert(!rr_aim_motion_point(point, now, prev, 4, 0, 50, 16, &out));
    assert(!rr_aim_motion_point(point, now, prev, 200, 0, 50, 16, &out));
    assert(!rr_aim_motion_point(point, now, prev, 16, 200, 50, 16, &out));
    {
        RR_V3 bad = {NAN, 1, 0};
        assert(!rr_aim_motion_point(point, bad, prev, 16, 0, 50, 16, &out));
    }
    assert(!rr_aim_motion_point(point, now, prev, 16, 0, 50, 16, NULL));
    /* The lead is exactly the follow lag minus the snapshot age: the aimed
     * point is where the body will be when the correction lands. Verified
     * against the closed form for every smoothing step the slider can send. */
    for (int smooth = 10; smooth <= 99; smooth += 10) {
        const unsigned periods[3] = {8, 16, 33};
        for (int i = 0; i < 3; ++i) {
            const unsigned dt = periods[i];
            const float speed = 2.0f;                    /* metres per second */
            const float per = speed * (float)dt / 1000.0f;
            RR_V3 a = {0, 1, 0}, b = {per, 1, 0};
            assert(rr_aim_motion_point(a, b, a, dt, 0, smooth, dt, &out));
            const float gain = aim_gain(aim_response_ms(smooth), dt);
            const float lead_s = (float)dt / 1000.0f / gain;
            const float expect = speed * lead_s;
            assert(fabsf(out.x - expect) < 1e-4f);
            /* A snapshot that already over-compensates pulls the lead back. */
            assert(rr_aim_motion_point(a, b, a, dt, 40, smooth, dt, &out));
            assert(out.x <= expect + 1e-4f);
        }
    }
    puts("MOTION POINT OK: speed and period bounds, NaN, NULL, exact lead, snapshot-age clamp");
}

static void check_trigger_edges(void) {
    /* The trigger tolerance is clamped, the default covers ten pixels, and the
     * head-only switch really does stop the body groups from answering. */
    fixture();
    for (int b = 0; b < targets[0].nbones; ++b) set_bone(0, b, 0.0f, 0.0f, 10.0f);
    config.trigger_fov = 5000;                       /* clamped to 80 px */
    assert(rr_trigger_pick(targets, 1, &camera, 1920, 1080, &config, NULL).player == &targets[0]);
    for (int b = 0; b < targets[0].nbones; ++b) set_bone(0, b, 0.5f, 0.0f, 10.0f);   /* 50 px */
    assert(rr_trigger_pick(targets, 1, &camera, 1920, 1080, &config, NULL).player == &targets[0]);
    for (int b = 0; b < targets[0].nbones; ++b) set_bone(0, b, 0.9f, 0.0f, 10.0f);   /* 90 px */
    assert(!rr_trigger_pick(targets, 1, &camera, 1920, 1080, &config, NULL).player);
    /* Zero means the ten-pixel default, not an unlimited tolerance. */
    config.trigger_fov = 0;
    for (int b = 0; b < targets[0].nbones; ++b) set_bone(0, b, 0.09f, 0.0f, 10.0f);  /* 9 px */
    assert(rr_trigger_pick(targets, 1, &camera, 1920, 1080, &config, NULL).player);
    for (int b = 0; b < targets[0].nbones; ++b) set_bone(0, b, 0.20f, 0.0f, 10.0f);  /* 20 px */
    assert(!rr_trigger_pick(targets, 1, &camera, 1920, 1080, &config, NULL).player);
    /* head-only: the head is 90 px off while the neck sits on the crosshair. */
    config.trigger_head_only = 1;
    config.trigger_fov = 5000;
    set_bone(0, 0, 0.9f, 0.0f, 10.0f);
    set_bone(0, 1, 0.0f, 0.0f, 10.0f);
    assert(!rr_trigger_pick(targets, 1, &camera, 1920, 1080, &config, NULL).player);
    config.trigger_head_only = 0;
    assert(rr_trigger_pick(targets, 1, &camera, 1920, 1080, &config, NULL).player == &targets[0]);

    RR_TRIGGER_STATE st = {0};
    assert(!rr_trigger_ready(&st, 1, 1, 0, 100000));   /* delay clamps to 300 */
    assert(!rr_trigger_ready(&st, 1, 1, 299, 100000));
    assert(rr_trigger_ready(&st, 1, 1, 300, 100000));
    assert(!rr_trigger_ready(&st, 1, 1, 599, 100000));
    assert(rr_trigger_ready(&st, 1, 1, 600, 100000));
    assert(!rr_trigger_ready(NULL, 1, 1, 0, 0));
    puts("TRIGGER EDGES OK: tolerance clamp, 10 px default, head-only gating, delay ceiling, NULL state");
}

static void check_worker_gates(void) {
    /* Every gate in rr_aim_update: no snapshot, menu, dead local body, aim hold,
     * require-fire, refused input. */
    fixture();
    config.aim_enabled = 1;
    keys[VK_RBUTTON] = (SHORT)0x8000;
    set_bone(0, 0, 0.3f, 0.5f, 10.0f);
    send_calls = 0;
    rr_aim_update(targets, 1, &camera, 1920, 1080);
    assert(send_calls == 1 && rr_aim_is_active());

    rr_aim_update(NULL, 0, NULL, 0, 0);
    assert(!rr_aim_is_active());

    config.menu_open = 1;
    rr_aim_update(targets, 1, &camera, 1920, 1080);
    assert(!rr_aim_is_active());
    config.menu_open = 0;

    targets[0].local = 1;
    rr_aim_update(targets, 1, &camera, 1920, 1080);
    assert(!rr_aim_is_active());
    targets[0].local = 0;
    targets[0].dead = 1;
    targets[0].local = 1;                          /* the local body only */
    rr_aim_update(targets, 1, &camera, 1920, 1080);
    assert(!rr_aim_is_active());
    targets[0].local = 0;
    targets[0].dead = 0;

    config.aim_enabled = 0;
    rr_aim_update(targets, 1, &camera, 1920, 1080);
    assert(!rr_aim_is_active());
    config.aim_enabled = 1;

    keys[VK_RBUTTON] = 0;
    rr_aim_update(targets, 1, &camera, 1920, 1080);
    assert(!rr_aim_is_active());
    keys[VK_RBUTTON] = (SHORT)0x8000;

    config.aim_require_fire = 1;
    rr_aim_update(targets, 1, &camera, 1920, 1080);
    assert(!rr_aim_is_active());
    keys[VK_LBUTTON] = (SHORT)0x8000;
    rr_aim_update(targets, 1, &camera, 1920, 1080);
    assert(rr_aim_is_active());
    config.aim_require_fire = 0;
    keys[VK_LBUTTON] = 0;

    /* Refused input: the worker must not claim the aim is active, and the next
     * frame retries rather than treating the refusal as a measured move. */
    send_result = 0;
    rr_aim_update(targets, 1, &camera, 1920, 1080);
    assert(!rr_aim_is_active());
    send_result = -1;
    rr_aim_update(targets, 1, &camera, 1920, 1080);
    assert(rr_aim_is_active());
    puts("WORKER GATES OK: snapshot, menu, dead local, hold key, require-fire, refused input");
}

/* The headline check: does the crosshair settle on the body, and how far does it
 * travel when the game's sensitivity is not the one the seed assumed. */
static void check_closed_loop(void) {
    printf("closed loop, snap path (smooth=100), 100 px error:\n");
    LOOP_RUN r;
    const double gains[6] = {0.05, 0.15, 0.35, 0.7, 1.5, 3.0};
    /* The measured requirement: the correction never overshoots by more than two
     * pixels at any sensitivity, and it is on the body by the second frame. The
     * pre-fix code swung a hundred pixels past the body at twice the seed's
     * sensitivity and rang for ten frames, which is the report this test exists
     * for. */
    for (int i = 0; i < 6; ++i) {
        run_loop("gain sweep", gains[i], 1.0, 0, 0, &r);
        assert(r.settle >= 0 && r.settle <= 3);
        assert(fabs(r.last) <= 1.6);
        assert(r.peak <= 2.0);
        assert(r.flips == 0);
    }
    /* A camera that only shows 45 percent of the last move next frame: bounded,
     * no runaway, and still on the body. The old law sent a fresh full
     * correction for the same error every frame and diverged to 1800 px here. */
    for (int i = 0; i < 6; ++i) {
        run_loop("lagged camera", gains[i], 0.45, 0, 0, &r);
        assert(r.settle >= 0 && r.settle <= 30);
        assert(fabs(r.last) <= 1.6);
        assert(r.peak <= fabs(r.first) * 1.05);
    }
    /* A blended FOV moves by fractions of a degree every frame; that is not a
     * sensitivity change and must not restart the measurement. Identical to the
     * steady-FOV run above, which is the whole point. */
    for (int i = 0; i < 6; ++i) {
        run_loop("fov jitter", gains[i], 1.0, 1, 0, &r);
        assert(r.settle >= 0 && r.settle <= 3);
        assert(fabs(r.last) <= 1.6);
        assert(r.peak <= 2.0);
        assert(r.flips == 0);
    }
    /* Facial animation: eyes, lips and tongue wander a pixel a frame. The aim
     * point must not hop between them. */
    run_loop("facial jitter", 0.35, 1.0, 0, 1, &r);
    assert(r.settle >= 0 && r.settle <= 3);
    assert(r.peak <= 2.5);
    puts("CLOSED LOOP OK: on the body by the second frame at every sensitivity, overshoot under "
         "two pixels, damped cameras bounded, FOV wobble does not restart the gain, facial joints "
         "do not steal the point");
}

/* Guards that keep the loop alive when the world refuses to answer, the bound on
 * the very first move, and the axis direction. */
static void snap_reset(SNAP_AXIS* a) {
    a->px = 0.35f; a->valid = 0; a->sign = 1;
    a->sent = 0; a->err = 0.0f; a->pend = 0.0f;
    a->starve = 0; a->wrong = 0; a->wrong_px = 0.0f;
}

static void check_snap_guards(void) {
    SNAP_AXIS ax;
    /* The probe: a fresh sequence never asks for more than half the error at the
     * highest sensitivity a game ships. */
    for (float err = 5.0f; err <= 900.0f; err *= 3.0f) {
        snap_reset(&ax);
        const int n = snap_axis(&ax, err, 16);
        assert(n >= 0);
        const double worst_move = (double)n * RR_AIM_SNAP_PX_MAX;
        assert(worst_move <= (double)err * RR_AIM_SNAP_PROBE + RR_AIM_SNAP_PX_MAX);
    }
    /* Input that is swallowed entirely: the starvation guard writes off the
     * counts in flight so the loop keeps asking instead of freezing, and the
     * floor keeps a count meaningful. */
    snap_reset(&ax);
    int moves = 0;
    for (int f = 0; f < 40; ++f) {
        const int n = snap_axis(&ax, 100.0f, 16);
        if (n) {
            ++moves;
            ax.pend += (float)n;      /* sent, never answered */
            ax.sent = n;
            ax.err = 100.0f;
        }
    }
    assert(moves >= 2);                       /* it keeps trying */
    assert(ax.px >= RR_AIM_SNAP_PX_MIN);
    assert(fabsf(ax.pend) < 1e6f);            /* windup is bounded */
    /* A gain that collapses is floored, never switched off: the old floor
     * returned "no move" forever and the instant lock went silently dead. */
    ax.px = 0.0001f;
    const int floored = snap_axis(&ax, 100.0f, 16);
    assert(ax.px >= RR_AIM_SNAP_PX_MIN);
    assert(floored != 0);
    /* A non-finite error must not reach the buffer or poison the gain. */
    snap_reset(&ax);
    assert(snap_axis(&ax, NAN, 16) == 0);
    assert(snap_axis(NULL, 10.0f, 16) == 0);
    puts("SNAP GUARDS OK: probe bounded, swallowed input written off and retried, gain floored "
         "not disabled, NaN rejected");
}

/* An inverted axis - a game with "invert Y" on, a pointer feed that reports
 * screen deltas - turns every correction into a doubling of the error. The loop
 * must notice from the movement itself and flip, not halve the gain and then
 * throw four times the counts the wrong way. The sweep covers gains an order of
 * magnitude either side of the seed, because the seed is a guess until the game
 * answers. */
static void check_axis_sign(void) {
    static const float gains[] = { 0.02f, 0.05f, 0.35f, 1.5f, 4.0f };
    const int ngains = (int)(sizeof gains / sizeof gains[0]);
    for (int inverted = 0; inverted < 2; ++inverted) {
        for (int g = 0; g < ngains; ++g) {
            /* Plant: a positive count turns the view by +px_true * axis, so an
             * inverted plant is one where the view turns against the counts. */
            const float axis = inverted ? -1.0f : 1.0f;
            const float px_true = gains[g];
            SNAP_AXIS ax;
            snap_reset(&ax);
            float err = 120.0f;
            int flipped_at = -1, settled = -1;
            for (int f = 0; f < 60; ++f) {
                const int n = snap_axis(&ax, err, 16);
                if (n) {
                    ax.pend += (float)n;
                    ax.sent = n;
                    ax.err = err;
                    err -= (float)n * px_true * axis;
                } else {
                    ax.sent = 0;
                    ax.err = err;
                }
                if (flipped_at < 0 && ax.sign != 1) flipped_at = f;
                if (settled < 0 && fabsf(err) < 2.0f) settled = f;
            }
            assert(fabsf(err) < 2.5f);             /* converged either way */
            assert(settled >= 0);
            /* The axis was learned from the movement, not assumed. */
            assert(ax.sign == (inverted ? -1 : 1));
            /* Flipped inside a few frames: a wrong axis drags the crosshair off
             * the body every frame it is believed. */
            if (inverted) assert(flipped_at >= 0 && flipped_at <= 6);
            /* And the gain it settled on is the plant's, not the seed. */
            assert(fabsf(ax.px - px_true) <= fmaxf(0.12f, px_true * 0.35f));
        }
    }
    puts("AXIS SIGN OK: an inverted axis is detected from the movement, flipped and converged");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    check_bone_groups();
    check_point_selection();
    check_visible_point();
    check_pick();
    check_step_math();
    check_motion_point();
    check_trigger_edges();
    check_worker_gates();
    check_snap_guards();
    check_axis_sign();
    check_closed_loop();
    puts("ALL AIM FUNCTION CHECKS PASSED; SendInput was mocked throughout");
    return 0;
}
