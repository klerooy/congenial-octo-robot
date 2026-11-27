/* Offline-only: the actual aim module is compiled with input/clock/LOS mocks. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "rr_game.h"
#include "menu_bridge.h"
#include "rr_esp_style.h"
#include "rr_label_layout.h"
#include "rr_desync.h"
static SHORT keys[256];
static DWORD fake_now;
static int los_result = 1, los_static_result = 1;
static int send_calls, packet_count, send_result = -1;
static int gate_foreground = 1, cursor_shown = 0;
static INPUT packets[64];
static BR_Config config;
static SHORT WINAPI fake_key(int vk) { return vk >= 0 && vk < 256 ? keys[vk] : 0; }
static DWORD WINAPI fake_clock(void) { return fake_now; }
static UINT WINAPI fake_send(UINT count, LPINPUT input, int size) {
    assert(size == sizeof(INPUT) && count <= 2 && packet_count + count < 64);
    memcpy(packets + packet_count, input, count * sizeof(INPUT));
    packet_count += count; ++send_calls;
    return send_result < 0 ? count : (UINT)send_result;
}
static BOOL WINAPI fake_cursor_info(PCURSORINFO ci) {
    if (!ci) return FALSE;
    ci->flags = cursor_shown ? CURSOR_SHOWING : 0;
    return TRUE;
}
#define SendInput fake_send
#define GetAsyncKeyState fake_key
#define timeGetTime fake_clock
#define GetCursorInfo fake_cursor_info
/* rr_aim.c reads a 64-bit tick in production so the follow filter cannot wrap;
 * the harness drives the same value from fake_now for deterministic timing. */
#define RR_AIM_NOW() ((ULONGLONG)fake_now)
#include "rr_aim.c"
int rr_raw_mouse_delta(int* dx, int* dy) { if (dx) *dx = 0; if (dy) *dy = 0; return 0; }
/* Nospread issues a write through the driver transport; the harness records
 * what was written instead of touching memory. */
static int write_calls;
static uint64_t write_addr[8];
static size_t write_len[8];
static unsigned char write_buf[8][16];
int rr_write(uint64_t at, const void* in, size_t n) {
    if (write_calls < 8 && in && n <= 16) {
        write_addr[write_calls] = at; write_len[write_calls] = n;
        memcpy(write_buf[write_calls], in, n);
    }
    ++write_calls;
    return 1;
}
BR_Config* br_get(void) { return &config; }
int rr_los_available(void) { return 1; }
int rr_los_check(RR_V3 from, RR_V3 to) { (void)from; (void)to; return los_result; }
int rr_los_check_static(RR_V3 from, RR_V3 to) { (void)from; (void)to; return los_static_result; }
int rr_gate_foreground(void) { return gate_foreground; }
HWND rr_gate_hwnd(void) { return NULL; }
int game_cam_track(const RR_CAMERA* cam, uint64_t id, RR_V3 target, int smooth,
                   int max_dps) {
    (void)cam; (void)id; (void)target; (void)smooth; (void)max_dps;
    return 0;   /* no camera hook: the worker falls back to mouse steering */
}
void game_cam_clear(void) {}
int game_w2s(const RR_CAMERA* camera, const RR_V3* point, int w, int h, float* x, float* y) {
    if (!camera || !camera->valid || !isfinite(point->x) || !isfinite(point->y) ||
        !isfinite(point->z) || point->z <= camera->pos.z) return 0;
    *x = w * 0.5f + point->x * 100.0f; *y = h * 0.5f - point->y * 100.0f;
    return 1;
}
static int clear_path(RR_V3 from, RR_V3 to) { return rr_los_check(from,to) == RR_LOS_CLEAR; }
static int blocked_path(RR_V3 from, RR_V3 to) { (void)from; (void)to; return 0; }
static RR_PLAYER targets[2];
static RR_CAMERA camera;
static void fixture(void) {
    memset(&config,0,sizeof(config)); memset(targets,0,sizeof(targets)); memset(keys,0,sizeof(keys));
    memset(&camera,0,sizeof(camera));
    camera.valid=1; camera.fov=60; camera.fwd.z=2; camera.right.x=1; camera.up.y=1;
    config.aim_key=VK_RBUTTON; config.aim_fov=100; config.aim_smooth=30; config.aa_aim_cal=100;
    config.trigger_delay=80; los_result=RR_LOS_CLEAR; los_static_result=RR_LOS_CLEAR;
    fake_now=1000; send_result=-1; gate_foreground=1; cursor_shown=0;
    send_calls=packet_count=0; write_calls=0;
    rr_aim_set_camera_mode(0);   /* deterministic mouse path; the camera mock is absent */
    for(int i=0;i<2;++i) {
        targets[i].valid=1; targets[i].addr=0x1000+i*0x100; targets[i].id=i+1;
        targets[i].dist=10; targets[i].pos=(RR_V3){0,0,10};
        targets[i].health=-1;   /* unknown: the filter must keep it          */
        targets[i].nbones=2; targets[i].bone_group[0]=0; targets[i].bone_group[2]=1;
        targets[i].bone[0][2]=10; targets[i].bone[1][2]=10;
    }
    rr_aim_update(NULL,0,NULL,1920,1080);
}
/* HOST_GATE_UNDER_TEST */

int main(void) {
    fixture();
    targets[0].bone[0][0]=0.3f; targets[1].bone[0][0]=0.8f;
    RR_AIM_PICK pick=rr_aim_pick(targets,2,&camera,1920,1080,&config,0,0,-1,NULL);
    assert(pick.player==&targets[0]);
    targets[0].relationships=RR_REL_CLAN|RR_REL_POLICE;
    config.aim_targets=RR_REL_CLAN;
    pick=rr_aim_pick(targets,2,&camera,1920,1080,&config,0,0,-1,NULL); assert(pick.player==&targets[1]);
    config.aim_targets=RR_REL_CLAN|RR_REL_POLICE;
    pick=rr_aim_pick(targets,2,&camera,1920,1080,&config,0,0,-1,NULL); assert(pick.player==&targets[0]);
    /* A zero-health body sits closer to the crosshair and still loses when the
     * Ignore dead switch is on: the pick walks past it to the living target. */
    config.aim_ignore_dead=1;
    targets[0].bone[0][0]=0.1f; targets[0].health=0;
    targets[1].bone[0][0]=0.6f; targets[1].health=100;
    pick=rr_aim_pick(targets,2,&camera,1920,1080,&config,0,0,-1,NULL); assert(pick.player==&targets[1]);
    targets[1].health=-1;
    pick=rr_aim_pick(targets,2,&camera,1920,1080,&config,0,0,-1,NULL); assert(pick.player==&targets[1]);
    targets[1].health=0;
    pick=rr_aim_pick(targets,2,&camera,1920,1080,&config,0,0,-1,NULL); assert(!pick.player);
    targets[0].health=-1; targets[1].health=-1;
    /* A held lock widens the ring to twice the FOV radius, so a target that
     * drifts just outside acquisition keeps the steering; without the lock the
     * same point is ignored. */
    fixture();
    config.aim_fov=100;
    targets[0].bone[0][0]=1.2f;
    assert(!rr_aim_pick(targets,1,&camera,1920,1080,&config,0,0,-1,NULL).player);
    assert(rr_aim_pick(targets,1,&camera,1920,1080,&config,
                       targets[0].addr,targets[0].id,-1,NULL).player==&targets[0]);
    fixture();
    targets[0].bone[0][0]=0.3f; targets[1].bone[0][0]=0.8f;
    config.aim_enabled=1; keys[VK_RBUTTON]=(SHORT)0x8000;
    rr_aim_update(targets,2,&camera,1920,1080); assert(send_calls==1&&rr_aim_is_active());
    assert(packets[0].mi.dwFlags==MOUSEEVENTF_MOVE&&packets[0].mi.dx>0&&packets[0].mi.dx<30);
    keys[VK_RBUTTON]=0; rr_aim_update(targets,2,&camera,1920,1080); assert(send_calls==1&&!rr_aim_is_active());
    keys[VK_RBUTTON]=(SHORT)0x8000; config.menu_open=1;
    rr_aim_update(targets,2,&camera,1920,1080); assert(send_calls==1&&!rr_aim_is_active());
    /* The picker is strict about line of sight: an unresolved query cannot
     * certify a clear ray, so the target is skipped until the collision worker
     * reports CLEAR. */
    config.menu_open=0; config.aa_los_on=1; los_result=RR_LOS_UNKNOWN;
    rr_aim_update(targets,2,&camera,1920,1080); assert(send_calls==1&&!rr_aim_is_active());
    los_result=RR_LOS_BLOCKED; rr_aim_update(targets,2,&camera,1920,1080); assert(send_calls==1&&!rr_aim_is_active());
    los_result=RR_LOS_CLEAR; fake_now+=16;
    rr_aim_update(targets,2,&camera,1920,1080); assert(send_calls==2&&rr_aim_is_active());
    los_result=RR_LOS_UNKNOWN;
    assert(!aim_clear_path((RR_V3){0},(RR_V3){0}));
    los_result=RR_LOS_BLOCKED;
    assert(!aim_clear_path((RR_V3){0},(RR_V3){0}));
    los_result=RR_LOS_CLEAR;
    assert(aim_clear_path((RR_V3){0},(RR_V3){0}));
    puts("AIM CHECKS OK: crosshair selection, living-target filter, relation masks, keys, menu, strict LOS, smoothing");

    fixture();
    pick=rr_trigger_pick(targets,1,&camera,&config,clear_path); assert(pick.player==&targets[0]);
    /* Angular tolerance: the head accepts 0.15 m of lateral error and the chest
     * 0.20 m at any range. 5 cm is a hit, 40 cm is not; with the head off-axis
     * the 18 cm chest point still fires. */
    targets[0].bone[0][0]=targets[0].bone[1][0]=0.05f;
    assert(rr_trigger_pick(targets,1,&camera,&config,clear_path).player==&targets[0]);
    targets[0].bone[0][0]=targets[0].bone[1][0]=0.4f;
    assert(!rr_trigger_pick(targets,1,&camera,&config,clear_path).player);
    targets[0].bone[0][0]=0.4f; targets[0].bone[1][0]=0.18f;
    assert(rr_trigger_pick(targets,1,&camera,&config,clear_path).player==&targets[0]);
    targets[0].bone[0][0]=targets[0].bone[1][0]=0;
    /* The trigger never fires without a line-of-sight callback. */
    assert(!rr_trigger_pick(targets,1,&camera,&config,NULL).player);
    los_result=RR_LOS_UNKNOWN; assert(!rr_trigger_pick(targets,1,&camera,&config,clear_path).player);
    los_result=RR_LOS_BLOCKED; assert(!rr_trigger_pick(targets,1,&camera,&config,clear_path).player);
    los_result=RR_LOS_CLEAR;
    targets[0].bone[0][1]=NAN; targets[0].bone[1][1]=NAN;
    assert(!rr_trigger_pick(targets,1,&camera,&config,clear_path).player);
    fixture(); targets[0].local=1; assert(!rr_trigger_pick(targets,1,&camera,&config,clear_path).player);
    targets[0].local=0; config.aim_ignore_dead=1;
    targets[0].dead=1; assert(!rr_trigger_pick(targets,1,&camera,&config,clear_path).player);
    targets[0].dead=0; targets[0].health=0; assert(!rr_trigger_pick(targets,1,&camera,&config,clear_path).player);
    targets[0].health=-1; config.aim_ignore_dead=0;
    targets[0].hidden_in_object=1; assert(!rr_trigger_pick(targets,1,&camera,&config,clear_path).player);
    targets[0].hidden_in_object=0; targets[0].relationships=RR_REL_CLAN|RR_REL_POLICE;
    config.trigger_targets=RR_REL_CLAN; assert(!rr_trigger_pick(targets,1,&camera,&config,clear_path).player);
    config.trigger_targets=RR_REL_CLAN|RR_REL_POLICE; assert(rr_trigger_pick(targets,1,&camera,&config,clear_path).player);
    puts("TRIGGER GEOMETRY OK: crosshair tolerance, NaN, corpses, exclusions and optional CLEAR LOS");

    RR_TRIGGER_STATE state={0};
    assert(!rr_trigger_ready(&state,1,1,100,80)); assert(!rr_trigger_ready(&state,1,1,179,80));
    assert(rr_trigger_ready(&state,1,1,180,80)); assert(!rr_trigger_ready(&state,1,1,181,80));
    assert(rr_trigger_ready(&state,1,1,260,80)); assert(!rr_trigger_ready(&state,1,2,261,80));
    assert(!rr_trigger_ready(&state,0,0,300,80)); assert(!rr_trigger_ready(&state,1,2,301,80));
    rr_trigger_ready(&state,0,0,0,0); assert(rr_trigger_ready(&state,1,1,100,0));
    assert(!rr_trigger_ready(&state,1,1,101,0)); assert(rr_trigger_ready(&state,1,1,116,0));
    rr_trigger_ready(&state,0,0,0,80); assert(!rr_trigger_ready(&state,1,1,0xFFFFFFD0u,80));
    assert(rr_trigger_ready(&state,1,1,0x30u,80));
    puts("TRIGGER TIMING OK: acquisition delay, target changes/loss, click interval and clock wrap");

    fixture(); config.trigger_enabled=1; keys[VK_RBUTTON]=(SHORT)0x8000;
    rr_aim_update(targets,1,&camera,1920,1080); assert(send_calls==0);
    fake_now=1080; rr_aim_update(targets,1,&camera,1920,1080);
    assert(send_calls==1&&packet_count==2&&!rr_aim_is_active());
    assert(packets[0].mi.dwFlags==MOUSEEVENTF_LEFTDOWN&&packets[1].mi.dwFlags==MOUSEEVENTF_LEFTUP);
    keys[VK_RBUTTON]=0; fake_now=1090; rr_aim_update(targets,1,&camera,1920,1080);
    keys[VK_RBUTTON]=(SHORT)0x8000; fake_now=1100; rr_aim_update(targets,1,&camera,1920,1080); assert(send_calls==1);
    keys[VK_LBUTTON]=(SHORT)0x8000; fake_now=1180; rr_aim_update(targets,1,&camera,1920,1080); assert(send_calls==1);
    fixture(); config.trigger_enabled=1; config.trigger_key=VK_SHIFT; config.trigger_delay=0;
    keys[VK_SHIFT]=(SHORT)0x8000; send_result=1;
    rr_aim_update(targets,1,&camera,1920,1080);
    assert(send_calls==2&&packet_count==3&&packets[2].mi.dwFlags==MOUSEEVENTF_LEFTUP);
    config.menu_open=1; rr_aim_update(targets,1,&camera,1920,1080); assert(send_calls==2);
    puts("TRIGGER INPUT OK: independent enable/key, delay, manual-fire exclusion and partial-input release");
    fixture(); config.trigger_enabled=1; config.trigger_delay=0; keys[VK_RBUTTON]=(SHORT)0x8000;
    g_rcam=camera; g_nrender=1; g_render=targets; focus_allowed=0;
    aa_tick(1,fake_now); assert(send_calls==0);
    focus_allowed=1; aa_tick(1,fake_now-51); assert(send_calls==0);
    aa_tick(1,fake_now); assert(send_calls==1);
    config.menu_open=1; fake_now+=16; aa_tick(2,fake_now); assert(send_calls==1);
    config.menu_open=0; aa_tick(2,fake_now); assert(send_calls==2);
    puts("HOST INPUT GATES OK: no input without focus or fresh snapshot; opening menu resets state");
    assert(rr_weapon_style("dp9_m1").atlas_cell==0);
    assert(rr_weapon_style("rc556").atlas_cell==1&&rr_weapon_style("kl545").atlas_cell==2);
    assert(rr_weapon_style("shotgun_01").atlas_cell==3&&rr_weapon_style("unknown").atlas_cell==7);
    assert(rr_health_fraction(-1)==0&&rr_health_fraction(250)==1&&rr_health_fraction(50)==0.5f);
    assert(rr_health_color(0.3f)==RRGB(255,210,73)&&rr_health_color(0.6f)==RRGB(255,210,73));
    assert(rr_health_color(0.61f)==RRGB(94,230,126));
    assert(rr_esp_labels_visible(150,15000)&&!rr_esp_labels_visible(151,15000));
    assert(rr_esp_labels_visible(10000,0)&&!rr_esp_labels_visible(NAN,0));
    assert(rr_esp_role_color("cop")!=rr_esp_role_color("civilian")&&rr_esp_role_label("prisoner"));
    assert((rr_skeleton_fade(0xFFFFFFFF,200)&255)<(rr_skeleton_fade(0xFFFFFFFF,10)&255));
    char label[96],price[96];
    RR_PLAYER owner={0};
    rr_format_vehicle_owner(&owner,label,sizeof(label));assert(!strcmp(label,"Owner: unavailable"));
    owner.vehicle_owner_known=1;
    rr_format_vehicle_owner(&owner,label,sizeof(label));assert(!strcmp(label,"No player owner"));
    snprintf(owner.role,sizeof(owner.role),"cop");
    rr_format_vehicle_owner(&owner,label,sizeof(label));assert(!strcmp(label,"Owner role: [COP]"));
    owner.vehicle_owner_public_id=INT64_MAX;
    rr_format_vehicle_owner(&owner,label,sizeof(label));assert(!strcmp(label,"Owner ID: 9223372036854775807"));
    snprintf(owner.vehicle_owner_name,sizeof(owner.vehicle_owner_name),"Alice");
    rr_format_vehicle_owner(&owner,label,sizeof(label));assert(!strcmp(label,"Owner: Alice"));
    puts("VEHICLE OWNER LABEL CHECKS OK: nickname, full public ID, owner role and empty/unknown state");
    rr_format_weapon_label("dp9_m1",1,label,sizeof(label));assert(!strcmp(label,"DP9 MK 1"));
    rr_format_weapon_label("dp9_m1",0,label,sizeof(label));assert(!strcmp(label,"DP9"));
    rr_format_weapon_label("rc556_MK12",1,label,sizeof(label));assert(!strcmp(label,"RC556 MK 12"));
    rr_format_weapon_label("kl545",1,label,sizeof(label));assert(!strcmp(label,"KL545"));
    assert(rr_weapon_mk("dp9_m1_other",NULL)==-1&&rr_weapon_mk("dp9_m9999999",NULL)==-1);
    rr_format_loot_price(0,3,1200,500,price,sizeof(price));assert(!strcmp(price,"NPC $500/ea"));
    rr_format_loot_price(1,3,1200,500,price,sizeof(price));assert(!strcmp(price,"Base $1200/ea"));
    rr_format_loot_price(2,3,1200,500,price,sizeof(price));assert(strstr(price,"NPC $500")&&strstr(price,"Base $1200"));
    rr_format_loot_price(0,1,1200,0,price,sizeof(price));assert(!price[0]);
    assert(!rr_relationship_allowed(RR_REL_PARTY,1,1,0)&&rr_relationship_allowed(RR_REL_PARTY,0,0,1));
    puts("MK/PRICE LABEL CHECKS OK: explicit levels, toggle, malformed IDs, unit prices, source choices and unknown values");
    assert(rr_loot_weapon_cell("AK-47")==1&&rr_loot_weapon_cell("M4A1 Carbine")==1);
    assert(rr_loot_weapon_cell("Sniper Rifle")==5&&rr_loot_weapon_cell("12 Gauge Shotgun")==3);
    assert(rr_loot_weapon_cell("MP5 SMG")==4&&rr_loot_weapon_cell("Combat Knife")==6);
    assert(rr_loot_weapon_cell("Combat Helmet")==-1&&rr_loot_weapon_cell("Gold Bar")==-1);
    assert(rr_loot_weapon_cell(NULL)==-1&&rr_loot_weapon_cell("")==-1);
    puts("LOOT WEAPON CHECKS OK: display-name tokens, class priority, whole-token matching and non-weapon items");
    {
        RR_LABEL_LAYOUT layout;
        float y;
        rr_label_reset(&layout);
        assert(layout.count == 0);
        y = 100.0f; assert(rr_label_place(&layout, 50.0f, &y, 80.0f, 16.0f, 18.0f) && y == 100.0f);
        /* The same corner steps one line down, then one line up, instead of
         * stacking two names on top of each other. */
        y = 100.0f; assert(rr_label_place(&layout, 50.0f, &y, 80.0f, 16.0f, 18.0f) && y == 118.0f);
        y = 100.0f; assert(rr_label_place(&layout, 50.0f, &y, 80.0f, 16.0f, 18.0f) && y == 82.0f);
        /* A free column to the side is accepted at the wanted line. */
        y = 100.0f; assert(rr_label_place(&layout, 400.0f, &y, 80.0f, 16.0f, 18.0f) && y == 100.0f);
        /* Six lines fit inside the search window; the seventh label is dropped. */
        rr_label_reset(&layout);
        for (int i = 0; i < 6; ++i) {
            float ty = 0.0f;
            assert(rr_label_place(&layout, 10.0f, &ty, 60.0f, 14.0f, 16.0f) && ty == (float)(i * 16));
        }
        y = 0.0f; assert(!rr_label_place(&layout, 10.0f, &y, 60.0f, 14.0f, 16.0f));
        /* A reserved anchor (player column, radar panel) blocks the area without
         * being moved by a later placement. */
        rr_label_reset(&layout);
        assert(rr_label_reserve(&layout, 200.0f, 90.0f, 120.0f, 60.0f) && layout.count == 1);
        assert(layout.rect[0].x == 200.0f && layout.rect[0].y == 90.0f);
        y = 100.0f; assert(rr_label_place(&layout, 210.0f, &y, 80.0f, 16.0f, 20.0f) && y == 60.0f);
        y = 100.0f; assert(rr_label_place(&layout, 900.0f, &y, 80.0f, 16.0f, 20.0f) && y == 100.0f);
        assert(!rr_label_reserve(&layout, 0.0f, 0.0f, 0.0f, 10.0f));
        puts("LABEL LAYOUT CHECKS OK: nearest free line, sideways space, dropped label when full, "
             "fixed anchors and reset");
    }
    {
        RR_DESYNC_STATE st, st2;
        assert(rr_desync_wrap_deg(0.0f) == 0.0f);
        assert(rr_desync_wrap_deg(90.0f) == 90.0f);
        assert(rr_desync_wrap_deg(180.0f) == -180.0f);
        assert(rr_desync_wrap_deg(190.0f) == -170.0f);
        assert(rr_desync_wrap_deg(-190.0f) == 170.0f);
        assert(rr_desync_wrap_deg(540.0f) == -180.0f);
        assert(rr_desync_wrap_deg(NAN) == 0.0f);
        assert(rr_desync_wrap_deg(INFINITY) == 0.0f);
        /* Spin advances 45 degrees per step and wraps back into range. */
        rr_desync_reset(&st, 7u);
        assert(rr_desync_step(&st, RR_DESYNC_SPIN, 100, 90, 1000) && st.offset_deg == 45.0f);
        assert(!rr_desync_step(&st, RR_DESYNC_SPIN, 100, 90, 1010));
        assert(rr_desync_step(&st, RR_DESYNC_SPIN, 100, 90, 1033) && st.offset_deg == 90.0f);
        assert(rr_desync_step(&st, RR_DESYNC_SPIN, 100, 90, 1066) && st.offset_deg == 135.0f);
        assert(rr_desync_step(&st, RR_DESYNC_SPIN, 100, 90, 1099) && st.offset_deg == -180.0f);
        /* Jitter alternates, static holds, random stays inside the range. */
        rr_desync_reset(&st, 7u);
        assert(rr_desync_step(&st, RR_DESYNC_JITTER, 100, 30, 0) && st.offset_deg == 30.0f);
        assert(rr_desync_step(&st, RR_DESYNC_JITTER, 100, 30, 34) && st.offset_deg == -30.0f);
        rr_desync_reset(&st, 7u);
        assert(rr_desync_step(&st, RR_DESYNC_STATIC, 50, 75, 0) && st.offset_deg == 75.0f);
        rr_desync_reset(&st, 99u);
        rr_desync_reset(&st2, 99u);
        for (int i = 0; i < 8; ++i) {
            unsigned at = (unsigned)(i * 34);
            assert(rr_desync_step(&st, RR_DESYNC_RANDOM, 100, 60, at));
            assert(rr_desync_step(&st2, RR_DESYNC_RANDOM, 100, 60, at));
            assert(st.offset_deg == st2.offset_deg);
            assert(st.offset_deg >= -60.0f && st.offset_deg <= 60.0f);
        }
        /* Rate mapping: 1 percent is 1 Hz, 100 percent is the 30 Hz ceiling. */
        rr_desync_reset(&st, 7u);
        assert(rr_desync_step(&st, RR_DESYNC_STATIC, 1, 10, 0));
        assert(!rr_desync_step(&st, RR_DESYNC_STATIC, 1, 10, 999));
        assert(rr_desync_step(&st, RR_DESYNC_STATIC, 1, 10, 1000));
        /* A disabled planner publishes nothing until it is stepped again. */
        rr_desync_reset(&st, 7u);
        assert(!st.started && st.offset_deg == 0.0f);
        puts("MODEL ANTI-AIM CHECKS OK: angle wrap, spin step, jitter/static/random ranges, "
             "rate mapping, determinism and reset");
    }
    puts("ESP STYLE CHECKS OK: weapon atlas mapping, health thresholds, role colors, distance fade, configurable/unlimited label range");

    /* Manual fire suppresses the trigger: while the left button is held no
     * synthetic click is sent; releasing it arms the trigger again. */
    fixture(); config.trigger_enabled=1; config.trigger_delay=0;
    keys[VK_RBUTTON]=(SHORT)0x8000; keys[VK_LBUTTON]=(SHORT)0x8000;
    rr_aim_update(targets,1,&camera,1920,1080); assert(send_calls==0);
    keys[VK_LBUTTON]=0; fake_now+=16;
    rr_aim_update(targets,1,&camera,1920,1080); assert(send_calls==1&&packet_count==2);
    assert(packets[0].mi.dwFlags==MOUSEEVENTF_LEFTDOWN&&packets[1].mi.dwFlags==MOUSEEVENTF_LEFTUP);
    puts("TRIGGER FIRE GATE OK: a manual shot suppresses the synthetic click");

    /* Nospread writes zero into both carriers the snapshot resolved, so the
     * cone is closed at the source and not merely hidden in the HUD. */
    fixture(); config.nospread=1;
    targets[0].local=1;
    targets[0].has_spread_data=1; targets[0].spread_value=0.02f; targets[0].spread_addr=0x5000;
    targets[0].has_fake_spread_data=1; targets[0].spread_move=0.005f;
    targets[0].spread_air=0.004f; targets[0].fake_spread_addr=0x6000;
    rr_aim_update(targets,1,&camera,1920,1080);
    assert(write_calls==2);
    assert(write_addr[0]==0x5000&&write_len[0]==sizeof(float));
    {
        float v; memcpy(&v,write_buf[0],sizeof(v)); assert(v==0.0f);
    }
    assert(write_addr[1]==0x6000&&write_len[1]==3*sizeof(float));
    {
        float t[3]; memcpy(t,write_buf[1],sizeof(t));
        assert(t[0]==0.0f&&t[1]==0.0f&&t[2]==0.0f);
    }
    /* A closed cone is not rewritten every frame... */
    targets[0].spread_value=0.0f; targets[0].spread_move=0.0f; targets[0].spread_air=0.0f;
    rr_aim_update(targets,1,&camera,1920,1080);
    assert(write_calls==2);
    /* ...and the switch really gates it. */
    fixture(); targets[0].local=1;
    targets[0].has_spread_data=1; targets[0].spread_value=0.02f; targets[0].spread_addr=0x5000;
    rr_aim_update(targets,1,&camera,1920,1080);
    assert(write_calls==0);
    puts("NOSPREAD OK: both carriers zeroed, closed cone silent, switch gated");

    /* Ignore prone must treat a knocked-out body as prone even when the rig and
     * the replicated prone flag still read upright. */
    fixture(); config.aim_ignore_prone=1;
    targets[0].knocked=1; targets[0].prone=0;
    assert(!rr_aim_pick(targets,1,&camera,1920,1080,&config,0,0,-1,NULL).player);
    assert(!rr_trigger_pick(targets,1,&camera,&config,clear_path).player);
    targets[0].knocked=0; targets[0].prone=1;
    assert(!rr_aim_pick(targets,1,&camera,1920,1080,&config,0,0,-1,NULL).player);
    assert(!rr_trigger_pick(targets,1,&camera,&config,clear_path).player);
    targets[0].prone=0;
    assert(rr_aim_pick(targets,1,&camera,1920,1080,&config,0,0,-1,NULL).player==&targets[0]);
    assert(rr_trigger_pick(targets,1,&camera,&config,clear_path).player==&targets[0]);
    puts("IGNORE PRONE OK: knocked and prone bodies are skipped by aim and trigger");

    /* Ignore civilian filters only the civilian role; police stay targetable. */
    fixture(); config.aim_ignore_civilian=1;
    snprintf(targets[0].role,sizeof(targets[0].role),"civilian");
    assert(!rr_aim_pick(targets,1,&camera,1920,1080,&config,0,0,-1,NULL).player);
    assert(!rr_trigger_pick(targets,1,&camera,&config,clear_path).player);
    snprintf(targets[0].role,sizeof(targets[0].role),"cop");
    assert(rr_aim_pick(targets,1,&camera,1920,1080,&config,0,0,-1,NULL).player==&targets[0]);
    assert(rr_trigger_pick(targets,1,&camera,&config,clear_path).player==&targets[0]);
    puts("IGNORE CIVILIAN OK: civilians skipped by aim and trigger, police untouched");

    /* A seated occupant's own vehicle hull blocks the full trace. The static
     * trace is clear, so both pickers keep the target; a wall still rejects. */
    fixture(); targets[0].inveh=1;
    los_result=RR_LOS_BLOCKED; los_static_result=RR_LOS_CLEAR;
    assert(rr_aim_pick(targets,1,&camera,1920,1080,&config,0,0,-1,blocked_path).player==&targets[0]);
    assert(rr_trigger_pick(targets,1,&camera,&config,blocked_path).player==&targets[0]);
    los_static_result=RR_LOS_BLOCKED;
    assert(!rr_aim_pick(targets,1,&camera,1920,1080,&config,0,0,-1,blocked_path).player);
    assert(!rr_trigger_pick(targets,1,&camera,&config,blocked_path).player);
    puts("SEATED STATIC LOS OK: vehicle hull ignored, static wall still blocks");

    /* A seated body can arrive without a resolved rig. The snapshot head
     * estimate is the fallback for both pickers; on foot the same body is
     * still rejected because the estimate is not trustworthy there. */
    fixture(); targets[0].inveh=1; targets[0].nbones=0;
    targets[0].head=(RR_V3){0,1.45f,10}; camera.pos.y=1.45f;
    config.aim_fov=300;   /* the synthetic projection is 100 px per metre */
    {
        RR_AIM_PICK hp=rr_aim_pick(targets,1,&camera,1920,1080,&config,0,0,-1,blocked_path);
        assert(hp.player==&targets[0]);
        assert(fabsf(hp.point.y-1.45f)<0.001f);
    }
    assert(rr_trigger_pick(targets,1,&camera,&config,blocked_path).player==&targets[0]);
    targets[0].inveh=0;
    assert(!rr_aim_pick(targets,1,&camera,1920,1080,&config,0,0,-1,blocked_path).player);
    assert(!rr_trigger_pick(targets,1,&camera,&config,blocked_path).player);
    puts("SEATED NO-RIG OK: head estimate fallback in a vehicle, rejected on foot");


    puts("ALL AIM/TRIGGER CHECKS PASSED; SendInput was mocked throughout");
    return 0;
}
