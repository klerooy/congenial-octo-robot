/* rr_int.cpp - in-process payload for ROB RUN.
 *
 * Loaded by the import-table trampoline (rr_map.c). Once inside, the module
 * reads the game through the same reader layer the external overlay uses
 * (rr_game.c) and draws with Direct2D on the game's own swapchain, hooked at
 * Present. No window, no second device, no overlay surface: the game's
 * backbuffer is the canvas.
 *
 *   DllMain      -> starts one worker thread, returns
 *   worker       -> own pid, transport, game_init(), config, hotkeys, heartbeat
 *   Present hook -> menu input -> snapshot -> aim tick -> draw -> original
 *
 * Interaction: F1 opens the in-process menu (rows: ESP, boxes, names, skeleton,
 * health, distance, range, aim, aim key, FOV, smoothing, bone, aim range,
 * trigger, trigger key, recoil, radar, radar range, radar loot, loot markers,
 * cash nodes, containers, doors, dropped loot, vehicle loot, prices, auto loot,
 * loot range, interact key). The menu is drawn by this module on the game's
 * own swapchain, so nothing external has to run. Insert (45) is the master
 * switch for the whole overlay.
 *
 * Configuration: C:\Users\Public\rr_int.ini, written on every menu change and
 * read once at load.
 *   esp=1 names=1 boxes=1 distance=1 health=1 skeleton=1 max_dist=30000
 *   aim=1 aim_key=6 aim_fov=150 aim_smooth=30 aim_bone=2 aim_dist=150
 *   trigger=0 trigger_key=0 trigger_delay=80 norecoil=0
 *   radar=1 radar_range=120 radar_loot=1
 *   loot=1 loot_nodes=1 containers=1 doors=0 dropped=1 vehicle_loot=1 prices=1
 *   autoloot=1 autoloot_range=12 autoloot_key=69
 *   insert_key=45 menu_key=112
 *
 * Log: C:\Users\Public\rr_int.log
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>   /* DXGI_PRESENT_PARAMETERS, IDXGISwapChain1::Present1 */
#include <d2d1.h>
#include <d2d1_1.h>
#include <dwrite.h>

#include "rr_mem.h"
#include "rr_game.h"
#include "rr_aim.h"
#include "rr_esp_style.h"
#include "menu_bridge.h"
#include "rr_input_gate.h"
#include "rr_antiaim.h"

#define RR_INT_LOG   "C:\\Users\\Public\\rr_int.log"
#define RR_INT_INI   "C:\\Users\\Public\\rr_int.ini"

/* ---- logging ------------------------------------------------------------- */

static CRITICAL_SECTION g_log_cs;
static int g_log_ready = 0;

static HINSTANCE g_hinst;

static void ilog(const char* fmt, ...) {
    FILE* f;
    va_list ap;
    SYSTEMTIME st;
    char line[1024];
    int n;

    if (!g_log_ready) return;
    GetLocalTime(&st);
    n = _snprintf_s(line, sizeof line, _TRUNCATE, "%02u:%02u:%02u.%03u t%u ",
                   st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                   (unsigned)GetCurrentThreadId());
    if (n > 0) {
        va_start(ap, fmt);
        _vsnprintf_s(line + n, sizeof line - (size_t)n, _TRUNCATE, fmt, ap);
        va_end(ap);
    }
    EnterCriticalSection(&g_log_cs);
    f = fopen(RR_INT_LOG, "a");
    if (f) {
        fputs(line, f);
        fputc('\n', f);
        fclose(f);
    }
    LeaveCriticalSection(&g_log_cs);
}

/* ---- config -------------------------------------------------------------- */

/* Interaction model: F1 opens the in-process menu, the menu edits this struct,
 * and this struct is the single source the renderer and the reader layer read
 * from. Every change is written back to the ini, so the next launch starts
 * where the last one stopped. Insert stays the master switch for the overlay. */
typedef struct {
    int esp, names, boxes, distance, health, skeleton;
    int fov_circle;    /* aim ring, read by rr_draw.c as aa_show_fov */
    int insert_key, menu_key;
    int max_dist;      /* centimetres, as the overlay menu stores them */
    /* rage is a preset the aim worker applies to a copy of the shared settings:
     * instant snap, head, no range limit, autofire with no delay, recoil off,
     * anti-aim off. The sliders below keep their values, so clearing rage gives
     * the old aim back on the same frame. */
    int rage;
    int aim, aim_key, aim_fov, aim_smooth, aim_bone, aim_dist, aim_cal, aim_fire;
    /* Anti-aim: a bounded view perturbation, not a server-side desync. It rides
     * the same worker as the aim, is gated off while steering, firing or while
     * the menu is up, and a bound key turns it into a hold. */
    int aa, aa_mode, aa_speed, aa_range, aa_key;
    int trigger, trigger_key, trigger_delay, trigger_fov, trigger_head;
    /* Nospread closes the client cone by zeroing the two spread carriers the
     * snapshot reader resolved. It is a write, so it is a switch of its own. */
    int nospread;
    int norecoil, recoil_mode, recoil_strength;
    /* World markers and radar. Each loot switch maps onto the reader layer's
     * BR_Config flag of the same name, so the caches only fill for the kinds
     * that are actually drawn. */
    int radar, radar_range, radar_loot;
    int loot, loot_nodes, containers, doors, dropped, vehicle_loot, prices;
    /* Auto-loot: the vacuum write runs on a timer, and the interact key is held
     * while a container or a body is in reach so the game opens it itself. */
    int autoloot, autoloot_range, autoloot_key;
    int ign_dead, ign_prone, ign_party, ign_clan;
} INT_CFG;

static INT_CFG g_cfg = {
    1, 1, 1, 1, 1, 1,
    1,
    VK_INSERT, VK_F1,
    30000,
    0,
    1, VK_XBUTTON2, 300, 100, RR_AIM_HEAD, 150, 100,
    0,
    0, RR_ANTIAIM_JITTER, 60, 6, 0,
    0, 0, 80, 10, 0,
    1,
    0, 0, 100,
    1, 120, 1,
    1, 1, 1, 0, 1, 1, 1,
    1, 12, 0x45
    , 1, 0, 1, 1            /* 'E' - VK_E is not a windows.h constant */
};

/* Owned by the render thread: it is the thread that polls the menu keys. */
static int g_menu_open = 0;
static int g_menu_sel = 0;
static int g_menu_tab = 0;   /* TAB_AIM; the tab enum lives with the panel below */
static int g_menu_rebind = 0;
static DWORD g_menu_rebind_at = 0;
static int g_menu_hover = -1;
static int g_menu_lmb = 0;
/* Panel layout is shared by the painter and the pointer hit test; the window
 * handle and the last painted surface size are what map a cursor position into
 * the swapchain's pixel space. */
static HWND  g_game_hwnd = NULL;
static float g_vw = 0.0f, g_vh = 0.0f;

static int ini_int(const char* text, const char* key, int fallback) {
    const char* p = text;
    size_t klen = strlen(key);
    while (p && *p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
        if (_strnicmp(p, key, klen) == 0) {
            const char* v = p + klen;
            while (*v == ' ' || *v == '\t' || *v == '=') ++v;
            return atoi(v);
        }
        p = strchr(p, '\n');
        if (p) ++p;
    }
    return fallback;
}

static int vk_usable(int vk) { return vk >= 1 && vk <= 254; }

/* The reader layer gates its scanning on the same object the external build
 * shares with the menu, and the aim worker reads the same object. Pushing the
 * internal config here keeps one switch set instead of two that can disagree. */
static void cfg_apply(void) {
    BR_Config* br = br_get();
    br->enabled = g_cfg.esp ? 1 : 0;
    br->show_box = g_cfg.boxes ? 1 : 0;
    br->aim_ignore_dead  = g_cfg.ign_dead  ? 1 : 0;
    br->aim_ignore_prone = g_cfg.ign_prone ? 1 : 0;
    br->aim_ignore_party = g_cfg.ign_party ? 1 : 0;
    br->aim_ignore_clan  = g_cfg.ign_clan  ? 1 : 0;
    br->show_name = g_cfg.names ? 1 : 0;
    br->show_distance = g_cfg.distance ? 1 : 0;
    br->show_health = g_cfg.health ? 1 : 0;
    br->show_skeleton = g_cfg.skeleton ? 1 : 0;
    br->aa_show_fov = g_cfg.fov_circle ? 1 : 0;
    br->max_distance = g_cfg.max_dist;
    br->esp_text_distance = g_cfg.max_dist;
    br->skel_distance = g_cfg.max_dist;
    br->rage = g_cfg.rage ? 1 : 0;
    br->aim_enabled = g_cfg.aim ? 1 : 0;
    br->aim_key = g_cfg.aim_key;
    br->aim_fov = g_cfg.aim_fov;
    br->aim_smooth = g_cfg.aim_smooth;
    br->aa_bone = g_cfg.aim_bone;
    br->aa_dist = g_cfg.aim_dist;
    br->aa_aim_cal = g_cfg.aim_cal;
    br->aim_require_fire = g_cfg.aim_fire ? 1 : 0;
    br->antiaim_enabled = g_cfg.aa ? 1 : 0;
    br->antiaim_mode = g_cfg.aa_mode;
    br->antiaim_speed = g_cfg.aa_speed;
    br->antiaim_range = g_cfg.aa_range;
    br->antiaim_key = g_cfg.aa_key;
    br->trigger_enabled = g_cfg.trigger ? 1 : 0;
    br->trigger_key = g_cfg.trigger_key;
    br->trigger_delay = g_cfg.trigger_delay;
    br->trigger_fov = g_cfg.trigger_fov;
    br->trigger_head_only = g_cfg.trigger_head ? 1 : 0;
    br->nospread = g_cfg.nospread ? 1 : 0;
    br->norecoil_enabled = g_cfg.norecoil ? 1 : 0;
    br->norecoil_mode = g_cfg.recoil_mode;
    br->norecoil_strength = g_cfg.recoil_strength;
    /* Marker and radar switches. Loot scanning is what fills the caches the
     * radar and the world markers read, so a switch that draws also enables the
     * matching reader flag. Prices ride on the same pass. */
    br->radar_on = g_cfg.radar ? 1 : 0;
    br->radar_range = g_cfg.radar_range;
    br->radar_world = g_cfg.radar_loot ? 1 : 0;
    br->show_loot = g_cfg.loot ? 1 : 0;
    br->show_loot_nodes = g_cfg.loot_nodes ? 1 : 0;
    br->show_containers = g_cfg.containers ? 1 : 0;
    br->show_doors = g_cfg.doors ? 1 : 0;
    br->show_dropped_loot = g_cfg.dropped ? 1 : 0;
    br->show_vehicle_loot = g_cfg.vehicle_loot ? 1 : 0;
    br->show_loot_price = g_cfg.prices ? 1 : 0;
    br->loot_price_source = RR_LOOT_PRICE_BASE;
    br->loot_tier_none = br->loot_tier_common = br->loot_tier_uncommon =
        br->loot_tier_rare = br->loot_tier_epic = br->loot_tier_legendary =
        br->loot_tier_exotic = 1;
    /* The aim worker keeps its hands off while the menu is up. */
    br->menu_open = g_menu_open ? 1 : 0;
}

static void cfg_save(void) {
    FILE* f = fopen(RR_INT_INI, "wb");
    if (!f) return;
    fprintf(f, "esp=%d\nnames=%d\nboxes=%d\ndistance=%d\nhealth=%d\nskeleton=%d\n",
            g_cfg.esp, g_cfg.names, g_cfg.boxes, g_cfg.distance, g_cfg.health, g_cfg.skeleton);
    fprintf(f, "max_dist=%d\ninsert_key=%d\nmenu_key=%d\n",
            g_cfg.max_dist, g_cfg.insert_key, g_cfg.menu_key);
    fprintf(f, "fov_circle=%d\nrage=%d\n", g_cfg.fov_circle, g_cfg.rage);
    fprintf(f, "aim=%d\naim_key=%d\naim_fov=%d\naim_smooth=%d\naim_bone=%d\naim_dist=%d\n",
            g_cfg.aim, g_cfg.aim_key, g_cfg.aim_fov, g_cfg.aim_smooth,
            g_cfg.aim_bone, g_cfg.aim_dist);
    fprintf(f, "aim_cal=%d\naim_fire=%d\n", g_cfg.aim_cal, g_cfg.aim_fire);
    fprintf(f, "aa=%d\naa_mode=%d\naa_speed=%d\naa_range=%d\naa_key=%d\n",
            g_cfg.aa, g_cfg.aa_mode, g_cfg.aa_speed, g_cfg.aa_range, g_cfg.aa_key);
    fprintf(f, "trigger=%d\ntrigger_key=%d\ntrigger_delay=%d\ntrigger_fov=%d\ntrigger_head=%d\n"
               "norecoil=%d\nrecoil_mode=%d\nrecoil_strength=%d\n",
            g_cfg.trigger, g_cfg.trigger_key, g_cfg.trigger_delay, g_cfg.trigger_fov,
            g_cfg.trigger_head, g_cfg.norecoil, g_cfg.recoil_mode, g_cfg.recoil_strength);
    fprintf(f, "nospread=%d\n", g_cfg.nospread);
    fprintf(f, "radar=%d\nradar_range=%d\nradar_loot=%d\n",
            g_cfg.radar, g_cfg.radar_range, g_cfg.radar_loot);
    fprintf(f, "loot=%d\nloot_nodes=%d\ncontainers=%d\ndoors=%d\ndropped=%d\n"
               "vehicle_loot=%d\nprices=%d\n",
            g_cfg.loot, g_cfg.loot_nodes, g_cfg.containers, g_cfg.doors,
            g_cfg.dropped, g_cfg.vehicle_loot, g_cfg.prices);
    fprintf(f, "autoloot=%d\nautoloot_range=%d\nautoloot_key=%d\n",
            g_cfg.autoloot, g_cfg.autoloot_range, g_cfg.autoloot_key);
    fclose(f);
}

static void cfg_load(void) {
    FILE* f = fopen(RR_INT_INI, "rb");
    char buf[4096];
    size_t n;
    if (!f) return;
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    g_cfg.esp      = ini_int(buf, "esp", g_cfg.esp);
    g_cfg.names    = ini_int(buf, "names", g_cfg.names);
    g_cfg.boxes    = ini_int(buf, "boxes", g_cfg.boxes);
    g_cfg.distance = ini_int(buf, "distance", g_cfg.distance);
    g_cfg.health   = ini_int(buf, "health", g_cfg.health);
    g_cfg.skeleton = ini_int(buf, "skeleton", g_cfg.skeleton);
    g_cfg.max_dist = ini_int(buf, "max_dist", g_cfg.max_dist);
    g_cfg.insert_key = ini_int(buf, "insert_key", g_cfg.insert_key);
    g_cfg.menu_key = ini_int(buf, "menu_key", g_cfg.menu_key);
    g_cfg.fov_circle = ini_int(buf, "fov_circle", g_cfg.fov_circle);
    g_cfg.rage     = ini_int(buf, "rage", g_cfg.rage);
    g_cfg.aim      = ini_int(buf, "aim", g_cfg.aim);
    g_cfg.aim_key  = ini_int(buf, "aim_key", g_cfg.aim_key);
    g_cfg.aim_fov  = ini_int(buf, "aim_fov", g_cfg.aim_fov);
    g_cfg.aim_smooth = ini_int(buf, "aim_smooth", g_cfg.aim_smooth);
    g_cfg.aim_bone = ini_int(buf, "aim_bone", g_cfg.aim_bone);
    g_cfg.aim_dist = ini_int(buf, "aim_dist", g_cfg.aim_dist);
    g_cfg.aim_cal  = ini_int(buf, "aim_cal", g_cfg.aim_cal);
    g_cfg.aim_fire = ini_int(buf, "aim_fire", g_cfg.aim_fire);
    g_cfg.aa       = ini_int(buf, "aa", g_cfg.aa);
    g_cfg.aa_mode  = ini_int(buf, "aa_mode", g_cfg.aa_mode);
    g_cfg.aa_speed = ini_int(buf, "aa_speed", g_cfg.aa_speed);
    g_cfg.aa_range = ini_int(buf, "aa_range", g_cfg.aa_range);
    g_cfg.aa_key   = ini_int(buf, "aa_key", g_cfg.aa_key);
    g_cfg.trigger  = ini_int(buf, "trigger", g_cfg.trigger);
    g_cfg.trigger_key = ini_int(buf, "trigger_key", g_cfg.trigger_key);
    g_cfg.trigger_delay = ini_int(buf, "trigger_delay", g_cfg.trigger_delay);
    g_cfg.trigger_fov = ini_int(buf, "trigger_fov", g_cfg.trigger_fov);
    g_cfg.trigger_head = ini_int(buf, "trigger_head", g_cfg.trigger_head);
    g_cfg.nospread  = ini_int(buf, "nospread", g_cfg.nospread);
    g_cfg.norecoil = ini_int(buf, "norecoil", g_cfg.norecoil);
    g_cfg.recoil_mode = ini_int(buf, "recoil_mode", g_cfg.recoil_mode);
    g_cfg.recoil_strength = ini_int(buf, "recoil_strength", g_cfg.recoil_strength);
    g_cfg.radar    = ini_int(buf, "radar", g_cfg.radar);
    g_cfg.radar_range = ini_int(buf, "radar_range", g_cfg.radar_range);
    g_cfg.radar_loot  = ini_int(buf, "radar_loot", g_cfg.radar_loot);
    g_cfg.loot     = ini_int(buf, "loot", g_cfg.loot);
    g_cfg.loot_nodes = ini_int(buf, "loot_nodes", g_cfg.loot_nodes);
    g_cfg.containers = ini_int(buf, "containers", g_cfg.containers);
    g_cfg.doors    = ini_int(buf, "doors", g_cfg.doors);
    g_cfg.dropped  = ini_int(buf, "dropped", g_cfg.dropped);
    g_cfg.vehicle_loot = ini_int(buf, "vehicle_loot", g_cfg.vehicle_loot);
    g_cfg.prices   = ini_int(buf, "prices", g_cfg.prices);
    g_cfg.autoloot = ini_int(buf, "autoloot", g_cfg.autoloot);
    g_cfg.autoloot_range = ini_int(buf, "autoloot_range", g_cfg.autoloot_range);
    g_cfg.autoloot_key = ini_int(buf, "autoloot_key", g_cfg.autoloot_key);

    if (g_cfg.max_dist < 1000) g_cfg.max_dist = 1000;
    if (g_cfg.max_dist > 200000) g_cfg.max_dist = 200000;
    if (!vk_usable(g_cfg.insert_key)) g_cfg.insert_key = VK_INSERT;
    if (!vk_usable(g_cfg.menu_key)) g_cfg.menu_key = VK_F1;
    if (!vk_usable(g_cfg.aim_key)) g_cfg.aim_key = VK_XBUTTON2;
    if (g_cfg.aim_fov < 10) g_cfg.aim_fov = 10;
    if (g_cfg.aim_fov > 300) g_cfg.aim_fov = 300;
    if (g_cfg.aim_smooth < 1) g_cfg.aim_smooth = 1;
    if (g_cfg.aim_smooth > 100) g_cfg.aim_smooth = 100;
    if (g_cfg.aim_bone < 0 || g_cfg.aim_bone >= RR_AIM_GROUP_COUNT) g_cfg.aim_bone = RR_AIM_CHEST;
    if (g_cfg.aim_dist < 0) g_cfg.aim_dist = 0;
    if (g_cfg.aim_dist > 1000) g_cfg.aim_dist = 1000;
    if (g_cfg.aim_cal < 10) g_cfg.aim_cal = 10;
    if (g_cfg.aim_cal > 2000) g_cfg.aim_cal = 2000;
    if (g_cfg.trigger_fov < 1) g_cfg.trigger_fov = 1;
    if (g_cfg.trigger_fov > 80) g_cfg.trigger_fov = 80;
    if (g_cfg.recoil_mode < 0) g_cfg.recoil_mode = 0;
    if (g_cfg.recoil_mode > 2) g_cfg.recoil_mode = 2;
    if (g_cfg.recoil_strength < 0) g_cfg.recoil_strength = 0;
    if (g_cfg.recoil_strength > 200) g_cfg.recoil_strength = 200;
    if (g_cfg.aa_mode < RR_ANTIAIM_SPIN || g_cfg.aa_mode > RR_ANTIAIM_RANDOM)
        g_cfg.aa_mode = RR_ANTIAIM_JITTER;
    if (g_cfg.aa_speed < 1) g_cfg.aa_speed = 1;
    if (g_cfg.aa_speed > 100) g_cfg.aa_speed = 100;
    if (g_cfg.aa_range < 1) g_cfg.aa_range = 1;
    if (g_cfg.aa_range > 60) g_cfg.aa_range = 60;
    if (g_cfg.aa_key < 0 || g_cfg.aa_key > 254) g_cfg.aa_key = 0;
    if (g_cfg.trigger_key < 0) g_cfg.trigger_key = 0;
    if (g_cfg.trigger_delay < 0) g_cfg.trigger_delay = 0;
    if (g_cfg.radar_range < 25) g_cfg.radar_range = 25;
    if (g_cfg.radar_range > 500) g_cfg.radar_range = 500;
    if (g_cfg.autoloot_range < 2) g_cfg.autoloot_range = 2;
    if (g_cfg.autoloot_range > 50) g_cfg.autoloot_range = 50;
    if (g_cfg.autoloot_key < 0 || g_cfg.autoloot_key > 254) g_cfg.autoloot_key = 0;
    cfg_apply();
}

/* ---- Direct2D / DirectWrite overlay ------------------------------------- */

static IDXGISwapChain*       g_swap = NULL;
static ID3D11Device*         g_dev = NULL;
static ID2D1Factory1*        g_d2d_factory = NULL;
static ID2D1Device*          g_d2d_dev = NULL;
static ID2D1DeviceContext*   g_d2d_ctx = NULL;
static ID2D1Bitmap1*         g_target = NULL;
static ID2D1SolidColorBrush* g_brush = NULL;
static IDWriteFactory*       g_dw = NULL;
static IDWriteTextFormat*    g_fmt = NULL;
static IDWriteTextFormat*    g_fmt_l = NULL;
static IDWriteTextFormat*    g_fmt_r = NULL;
static IDWriteTextFormat*    g_fmt_t = NULL;
static volatile LONG         g_draw_on = 1;
static volatile LONG         g_frames = 0;
static volatile LONG         g_players_seen = 0;

typedef HRESULT(STDMETHODCALLTYPE* PresentFn)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* Present1Fn)(IDXGISwapChain*, UINT, UINT,
                                               const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* ResizeFn)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static PresentFn  g_present_orig = NULL;
static Present1Fn g_present1_orig = NULL;
static ResizeFn   g_resize_orig = NULL;
static void**    g_vt = NULL;
static int       g_fmt_logged = 0;
static int       g_fail_logged = 0;

static D2D1_COLOR_F col(UINT32 argb, float mul) {
    D2D1_COLOR_F c;
    float a = ((argb >> 24) & 0xFF) / 255.0f * mul;
    c.a = a;
    c.r = ((argb >> 16) & 0xFF) / 255.0f * a;
    c.g = ((argb >> 8) & 0xFF) / 255.0f * a;
    c.b = (argb & 0xFF) / 255.0f * a;
    return c;
}

static void drop_target(void) {
    if (g_d2d_ctx) g_d2d_ctx->SetTarget(NULL);
    if (g_target) { g_target->Release(); g_target = NULL; }
}

static int make_target(void) {
    IDXGISurface* surf = NULL;
    DXGI_SWAP_CHAIN_DESC desc;
    D2D1_BITMAP_PROPERTIES1 bp;
    DXGI_FORMAT fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
    HRESULT hr;
    int attempt;

    if (!g_swap || !g_d2d_ctx) return 0;
    drop_target();

    /* The buffer format belongs to the swapchain, not to us: Unity presents
     * R8G8B8A8 on some paths and B8G8R8A8 on others, and D2D rejects a bitmap
     * whose description does not match the surface. */
    memset(&desc, 0, sizeof desc);
    if (SUCCEEDED(g_swap->GetDesc(&desc)) && desc.BufferDesc.Format != DXGI_FORMAT_UNKNOWN) {
        fmt = desc.BufferDesc.Format;
        if (!g_fmt_logged) {
            g_fmt_logged = 1;
            ilog("target: swapchain %ux%u fmt=%d count=%u effect=%u",
                 desc.BufferDesc.Width, desc.BufferDesc.Height, (int)fmt,
                 desc.BufferCount, (unsigned)desc.SwapEffect);
        }
    }

    hr = g_swap->GetBuffer(0, __uuidof(IDXGISurface), (void**)&surf);
    if (FAILED(hr) || !surf) {
        if (!g_fail_logged) { g_fail_logged = 1; ilog("target: GetBuffer failed hr=0x%08lX", (unsigned)hr); }
        return 0;
    }

    /* Premultiplied first, then ignore-alpha: one of the two matches the
     * surface's own alpha mode on every swapchain we have seen. */
    for (attempt = 0; attempt < 2 && !g_target; ++attempt) {
        memset(&bp, 0, sizeof bp);
        bp.pixelFormat.format = fmt;
        bp.pixelFormat.alphaMode = attempt == 0 ? D2D1_ALPHA_MODE_PREMULTIPLIED
                                               : D2D1_ALPHA_MODE_IGNORE;
        bp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        bp.dpiX = bp.dpiY = 96.0f;
        hr = g_d2d_ctx->CreateBitmapFromDxgiSurface(surf, &bp, &g_target);
        if (FAILED(hr)) g_target = NULL;
    }
    surf->Release();

    if (!g_target) {
        if (!g_fail_logged) {
            g_fail_logged = 1;
            ilog("target: CreateBitmapFromDxgiSurface failed hr=0x%08lX (fmt=%d)", (unsigned)hr, (int)fmt);
        }
        return 0;
    }
    g_fail_logged = 0;
    g_d2d_ctx->SetTarget(g_target);
    return 1;
}

static int d2d_init(IDXGISwapChain* swap) {
    ID3D11Device* dev = NULL;
    IDXGIDevice* dxgi_dev = NULL;
    HRESULT hr;

    if (g_d2d_ctx) return 1;
    hr = swap->GetDevice(__uuidof(ID3D11Device), (void**)&dev);
    if (FAILED(hr) || !dev) { ilog("d2d: swapchain GetDevice failed hr=0x%08lX", (unsigned)hr); return 0; }
    g_dev = dev;
    if (FAILED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi_dev)) || !dxgi_dev) {
        ilog("d2d: no IDXGIDevice on the game's swapchain");
        return 0;
    }
    if (!g_d2d_factory) {
        D2D1_FACTORY_OPTIONS o;
        memset(&o, 0, sizeof o);
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                     __uuidof(ID2D1Factory1), &o, (void**)&g_d2d_factory))) {
            dxgi_dev->Release();
            return 0;
        }
    }
    hr = g_d2d_factory->CreateDevice(dxgi_dev, &g_d2d_dev);
    if (FAILED(hr) || !g_d2d_dev) {
        ilog("d2d: CreateDevice failed hr=0x%08lX", (unsigned)hr);
        dxgi_dev->Release();
        return 0;
    }
    dxgi_dev->Release();
    hr = g_d2d_dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g_d2d_ctx);
    if (FAILED(hr)) { ilog("d2d: CreateDeviceContext failed hr=0x%08lX", (unsigned)hr); return 0; }
    if (SUCCEEDED(g_d2d_ctx->CreateSolidColorBrush(col(0xFFFFFFFF, 1.0f), &g_brush))) {
        /* brush is recoloured per primitive; creation only reserves it */
    }

    if (!g_dw)
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            (IUnknown**)&g_dw);
    if (g_dw && !g_fmt) {
        g_dw->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                               DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                               12.0f, L"en-us", &g_fmt);
    }
    if (g_fmt) {
        g_fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        g_fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    /* Menu faces: rows are left-aligned labels with right-aligned values, and
     * the panel header is one weight heavier than the world labels. */
    if (g_dw) {
        if (!g_fmt_l)
            g_dw->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                   DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                   16.0f, L"en-us", &g_fmt_l);
        if (!g_fmt_r)
            g_dw->CreateTextFormat(L"Consolas", NULL, DWRITE_FONT_WEIGHT_NORMAL,
                                   DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                   16.0f, L"en-us", &g_fmt_r);
        if (!g_fmt_t)
            g_dw->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_BOLD,
                                   DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                   19.0f, L"en-us", &g_fmt_t);
        if (g_fmt_l) {
            g_fmt_l->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            g_fmt_l->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        if (g_fmt_r) {
            g_fmt_r->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            g_fmt_r->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        if (g_fmt_t) g_fmt_t->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    ilog("d2d: context ready, device=%p", (void*)g_dev);
    return 1;
}

static UINT32 color_for(const RR_PLAYER* p) {
    if (p->relationships & RR_REL_PARTY) return 0xFF7FB2FF;
    if (p->relationships & RR_REL_CLAN)  return 0xFF7CFF7C;
    switch (rr_role_category(p->role)) {
        case RR_ROLE_COP:   return 0xFF4D7CFF;
        case RR_ROLE_NPC:   return 0xFFB4B4B4;
        case RR_ROLE_ROBBER: return 0xFFFF6B6B;
        default:             return 0xFFFFFFFF;
    }
}

static void draw_text(const wchar_t* s, float cx, float y, float maxw, UINT32 argb) {
    D2D1_RECT_F r;
    if (!g_d2d_ctx || !g_fmt || !g_brush) return;
    r.left = cx - maxw * 0.5f;
    r.top = y;
    r.right = cx + maxw * 0.5f;
    r.bottom = y + 16.0f;
    g_brush->SetColor(col(argb, 1.0f));
    g_d2d_ctx->DrawText(s, (UINT32)wcslen(s), g_fmt, r, g_brush,
                        D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
}

/* Unity stores strings as UTF-8, so a Cyrillic nickname converted with the
 * ANSI code page turns into question marks. Convert as UTF-8 and fall back to
 * ANSI only when the bytes are not valid UTF-8 at all (a name read out of a
 * mangled field), which keeps legacy readings readable instead of blank. */
static void narrow_to_wide(const char* s, wchar_t* out, size_t cap) {
    if (!out || !cap) return;
    out[0] = 0;
    if (!s || !s[0]) return;
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, out, (int)cap))
        MultiByteToWideChar(CP_ACP, 0, s, -1, out, (int)cap);
}

/* ---- in-process menu ----------------------------------------------------- */

/* One table drives the panel: the row order is the enum, the row text is the
 * switch in menu_text, and the edit is the switch in menu_set. Rows of one page
 * are contiguous in the enum, which is what row_tab[] relies on: a page is a
 * first index plus a count, so adding a row means adding one entry to the enum,
 * one to row_tab, one case to menu_text and one to menu_set. */
enum {
    /* AIM */
    M_RAGE,
    M_AIM, M_AIMKEY, M_AIMFOV, M_AIMSMOOTH, M_AIMBONE, M_AIMDIST, M_AIMCAL, M_AIMFIRE,
    M_AA, M_AAMODE, M_AASPD, M_AARANGE, M_AAKEY,
    M_TRIG, M_TRIGKEY, M_TRIGDELAY, M_TRIGFOV, M_TRIGHEAD,
    M_NOSPREAD, M_NORECOIL, M_NCMODE, M_NCSTR,
    /* VISUAL */
    M_ESP, M_BOX, M_NAME, M_SKEL, M_HEALTH, M_DIST, M_MAXD, M_FOVC,
    M_RADAR, M_RADRANGE, M_RADLOOT,
    /* LOOT */
    M_LOOT, M_LOOTNODE, M_CONT, M_DOOR, M_DROP, M_VEHSL, M_PRICE,
    M_AUTOLOOT, M_ALLRANGE, M_ALLKEY,
    /* SYSTEM */
    M_MENUKEY, M_INKEY,
    M_COUNT
};

enum { TAB_AIM, TAB_VIS, TAB_LOOT, TAB_SYS, TAB_COUNT };

static const wchar_t* const tab_label[TAB_COUNT] = {
    L"AIM", L"VISUAL", L"LOOT", L"SYSTEM"
};

static const unsigned char row_tab[M_COUNT] = {
    TAB_AIM,
    TAB_AIM, TAB_AIM, TAB_AIM, TAB_AIM, TAB_AIM, TAB_AIM, TAB_AIM, TAB_AIM,
    TAB_AIM, TAB_AIM, TAB_AIM, TAB_AIM, TAB_AIM,
    TAB_AIM, TAB_AIM, TAB_AIM, TAB_AIM, TAB_AIM, TAB_AIM,
    TAB_AIM, TAB_AIM, TAB_AIM,
    TAB_VIS, TAB_VIS, TAB_VIS, TAB_VIS, TAB_VIS, TAB_VIS, TAB_VIS, TAB_VIS,
    TAB_VIS, TAB_VIS, TAB_VIS,
    TAB_LOOT, TAB_LOOT, TAB_LOOT, TAB_LOOT, TAB_LOOT, TAB_LOOT, TAB_LOOT,
    TAB_LOOT, TAB_LOOT, TAB_LOOT,
    TAB_SYS, TAB_SYS
};

static int tab_first(int tab) {
    int i;
    for (i = 0; i < M_COUNT; ++i)
        if (row_tab[i] == tab) return i;
    return 0;
}

static int tab_rows(int tab) {
    int i, n = 0;
    for (i = 0; i < M_COUNT; ++i)
        if (row_tab[i] == tab) ++n;
    return n;
}

static int tab_max_rows(void) {
    int t, best = 1;
    for (t = 0; t < TAB_COUNT; ++t) {
        const int n = tab_rows(t);
        if (n > best) best = n;
    }
    return best;
}

/* The page's first row index for the row the cursor sits on. */
static int tab_row_at(int tab, int slot) {
    const int n = tab_rows(tab);
    if (slot < 0) slot = 0;
    if (n > 0 && slot >= n) slot = n - 1;
    return tab_first(tab) + slot;
}

static void text_in(const wchar_t* s, D2D1_RECT_F r, UINT32 argb, IDWriteTextFormat* f) {
    if (!g_d2d_ctx || !g_brush || !f || !s || !*s) return;
    g_brush->SetColor(col(argb, 1.0f));
    g_d2d_ctx->DrawText(s, (UINT32)wcslen(s), f, r, g_brush,
                        D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
}

static const wchar_t* aa_label(int mode) {
    (void)mode;
    return L"removed";
}

static const wchar_t* recoil_label(int mode) {
    switch (mode) {
    case 0:  return L"pattern";
    case 1:  return L"camera";
    case 2:  return L"both";
    default: return L"pattern";
    }
}

static const wchar_t* bone_label(int bone) {
    switch (bone) {
    case RR_AIM_HEAD: return L"head";
    case RR_AIM_NECK: return L"neck";
    case RR_AIM_CHEST: return L"chest";
    case RR_AIM_PELVIS: return L"pelvis";
    case RR_AIM_CLOSEST: return L"closest";
    case RR_AIM_ARMS: return L"arms";
    case RR_AIM_LEGS: return L"legs";
    case RR_AIM_FACE: return L"face";
    case RR_AIM_HANDS: return L"hands";
    case RR_AIM_FEET: return L"feet";
    default: return L"chest";
    }
}

static const wchar_t* key_label(int vk, wchar_t* buf, size_t cap) {
    switch (vk) {
    case 0:            return L"none";
    case VK_LBUTTON:   return L"mouse1";
    case VK_RBUTTON:   return L"mouse2";
    case VK_MBUTTON:   return L"mouse3";
    case VK_XBUTTON1:  return L"mouse4";
    case VK_XBUTTON2:  return L"mouse5";
    case VK_SHIFT:     return L"shift";
    case VK_CONTROL:   return L"ctrl";
    case VK_MENU:      return L"alt";
    case VK_SPACE:     return L"space";
    case VK_TAB:       return L"tab";
    default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F12) {
        _snwprintf_s(buf, cap, _TRUNCATE, L"F%d", vk - VK_F1 + 1);
        return buf;
    }
    if (vk >= 'A' && vk <= 'Z') {
        _snwprintf_s(buf, cap, _TRUNCATE, L"%c", (wchar_t)(vk - 'A' + 'a'));
        return buf;
    }
    if (vk >= '0' && vk <= '9') {
        _snwprintf_s(buf, cap, _TRUNCATE, L"%c", (wchar_t)vk);
        return buf;
    }
    _snwprintf_s(buf, cap, _TRUNCATE, L"vk%02X", vk);
    return buf;
}

static void menu_text(int row, wchar_t* label, wchar_t* value, size_t cap) {
    wchar_t kb[32];
    label[0] = value[0] = 0;
#define MENU_ONOFF(v) _snwprintf_s(value, cap, _TRUNCATE, L"%s", (v) ? L"on" : L"off")
    switch (row) {
    case M_ESP:      _snwprintf_s(label, cap, _TRUNCATE, L"esp");      MENU_ONOFF(g_cfg.esp);      break;
    case M_BOX:      _snwprintf_s(label, cap, _TRUNCATE, L"boxes");    MENU_ONOFF(g_cfg.boxes);    break;
    case M_NAME:     _snwprintf_s(label, cap, _TRUNCATE, L"names");    MENU_ONOFF(g_cfg.names);    break;
    case M_SKEL:     _snwprintf_s(label, cap, _TRUNCATE, L"skeleton"); MENU_ONOFF(g_cfg.skeleton); break;
    case M_HEALTH:   _snwprintf_s(label, cap, _TRUNCATE, L"health");   MENU_ONOFF(g_cfg.health);   break;
    case M_DIST:     _snwprintf_s(label, cap, _TRUNCATE, L"distance"); MENU_ONOFF(g_cfg.distance); break;
    case M_MAXD:
        _snwprintf_s(label, cap, _TRUNCATE, L"range");
        _snwprintf_s(value, cap, _TRUNCATE, L"%d m", g_cfg.max_dist / 100);
        break;
    case M_FOVC:     _snwprintf_s(label, cap, _TRUNCATE, L"fov circle"); MENU_ONOFF(g_cfg.fov_circle); break;
    case M_RAGE:
        _snwprintf_s(label, cap, _TRUNCATE, L"rage");
        MENU_ONOFF(g_cfg.rage);
        break;
    case M_AIM:      _snwprintf_s(label, cap, _TRUNCATE, L"aim");      MENU_ONOFF(g_cfg.aim);      break;
    case M_AIMKEY:
        _snwprintf_s(label, cap, _TRUNCATE, L"aim key");
        _snwprintf_s(value, cap, _TRUNCATE, L"%s",
                     key_label(g_cfg.aim_key, kb, 32));
        break;
    case M_AIMFOV:
        _snwprintf_s(label, cap, _TRUNCATE, L"aim fov");
        _snwprintf_s(value, cap, _TRUNCATE, L"%d deg", g_cfg.aim_fov);
        break;
    case M_AIMSMOOTH:
        _snwprintf_s(label, cap, _TRUNCATE, L"smoothing");
        _snwprintf_s(value, cap, _TRUNCATE, L"%d", g_cfg.aim_smooth);
        break;
    case M_AIMBONE:
        _snwprintf_s(label, cap, _TRUNCATE, L"aim bone");
        _snwprintf_s(value, cap, _TRUNCATE, L"%s", bone_label(g_cfg.aim_bone));
        break;
    case M_AIMDIST:
        _snwprintf_s(label, cap, _TRUNCATE, L"aim range");
        _snwprintf_s(value, cap, _TRUNCATE, L"%d m", g_cfg.aim_dist);
        break;
    case M_AIMCAL:
        _snwprintf_s(label, cap, _TRUNCATE, L"aim cal");
        _snwprintf_s(value, cap, _TRUNCATE, L"%d", g_cfg.aim_cal);
        break;
    case M_AIMFIRE:  _snwprintf_s(label, cap, _TRUNCATE, L"fire only"); MENU_ONOFF(g_cfg.aim_fire); break;
    case M_AA:       _snwprintf_s(label, cap, _TRUNCATE, L"anti-aim");   MENU_ONOFF(g_cfg.aa); break;
    case M_AAMODE:
        _snwprintf_s(label, cap, _TRUNCATE, L"aa mode");
        _snwprintf_s(value, cap, _TRUNCATE, L"removed");
        break;
    case M_AASPD:
        _snwprintf_s(label, cap, _TRUNCATE, L"aa speed");
        _snwprintf_s(value, cap, _TRUNCATE, L"%d", g_cfg.aa_speed);
        break;
    case M_AARANGE:
        _snwprintf_s(label, cap, _TRUNCATE, L"aa range");
        _snwprintf_s(value, cap, _TRUNCATE, L"%d", g_cfg.aa_range);
        break;
    case M_AAKEY:
        _snwprintf_s(label, cap, _TRUNCATE, L"aa key");
        _snwprintf_s(value, cap, _TRUNCATE, L"%s",
                     g_cfg.aa_key ? key_label(g_cfg.aa_key, kb, 32) : L"always");
        break;
    case M_TRIG:     _snwprintf_s(label, cap, _TRUNCATE, L"trigger");  MENU_ONOFF(g_cfg.trigger);  break;
    case M_TRIGKEY:
        _snwprintf_s(label, cap, _TRUNCATE, L"trigger key");
        _snwprintf_s(value, cap, _TRUNCATE, L"%s",
                     key_label(g_cfg.trigger_key, kb, 32));
        break;
    case M_TRIGDELAY:
        _snwprintf_s(label, cap, _TRUNCATE, L"trig delay");
        _snwprintf_s(value, cap, _TRUNCATE, L"%d ms", g_cfg.trigger_delay);
        break;
    case M_TRIGFOV:
        _snwprintf_s(label, cap, _TRUNCATE, L"trig fov");
        _snwprintf_s(value, cap, _TRUNCATE, L"%d px", g_cfg.trigger_fov);
        break;
    case M_TRIGHEAD: _snwprintf_s(label, cap, _TRUNCATE, L"head only"); MENU_ONOFF(g_cfg.trigger_head); break;
    case M_NOSPREAD: _snwprintf_s(label, cap, _TRUNCATE, L"nospread"); MENU_ONOFF(g_cfg.nospread); break;
    case M_NORECOIL: _snwprintf_s(label, cap, _TRUNCATE, L"norecoil"); MENU_ONOFF(g_cfg.norecoil); break;
    case M_NCMODE:
        _snwprintf_s(label, cap, _TRUNCATE, L"recoil mode");
        _snwprintf_s(value, cap, _TRUNCATE, L"%s", recoil_label(g_cfg.recoil_mode));
        break;
    case M_NCSTR:
        _snwprintf_s(label, cap, _TRUNCATE, L"recoil power");
        _snwprintf_s(value, cap, _TRUNCATE, L"%d", g_cfg.recoil_strength);
        break;
    case M_RADAR:    _snwprintf_s(label, cap, _TRUNCATE, L"radar");      MENU_ONOFF(g_cfg.radar);      break;
    case M_RADRANGE:
        _snwprintf_s(label, cap, _TRUNCATE, L"radar range");
        _snwprintf_s(value, cap, _TRUNCATE, L"%d m", g_cfg.radar_range);
        break;
    case M_RADLOOT:  _snwprintf_s(label, cap, _TRUNCATE, L"radar loot"); MENU_ONOFF(g_cfg.radar_loot); break;
    case M_LOOT:     _snwprintf_s(label, cap, _TRUNCATE, L"loot items"); MENU_ONOFF(g_cfg.loot);       break;
    case M_LOOTNODE: _snwprintf_s(label, cap, _TRUNCATE, L"cash / nodes"); MENU_ONOFF(g_cfg.loot_nodes); break;
    case M_CONT:     _snwprintf_s(label, cap, _TRUNCATE, L"containers"); MENU_ONOFF(g_cfg.containers);  break;
    case M_DOOR:     _snwprintf_s(label, cap, _TRUNCATE, L"doors");      MENU_ONOFF(g_cfg.doors);       break;
    case M_DROP:     _snwprintf_s(label, cap, _TRUNCATE, L"dropped loot"); MENU_ONOFF(g_cfg.dropped);   break;
    case M_VEHSL:    _snwprintf_s(label, cap, _TRUNCATE, L"vehicle loot"); MENU_ONOFF(g_cfg.vehicle_loot); break;
    case M_PRICE:    _snwprintf_s(label, cap, _TRUNCATE, L"loot price"); MENU_ONOFF(g_cfg.prices);      break;
    case M_AUTOLOOT: _snwprintf_s(label, cap, _TRUNCATE, L"auto loot");  MENU_ONOFF(g_cfg.autoloot);    break;
    case M_ALLRANGE:
        _snwprintf_s(label, cap, _TRUNCATE, L"loot range");
        _snwprintf_s(value, cap, _TRUNCATE, L"%d m", g_cfg.autoloot_range);
        break;
    case M_ALLKEY:
        _snwprintf_s(label, cap, _TRUNCATE, L"interact key");
        _snwprintf_s(value, cap, _TRUNCATE, L"%s",
                     key_label(g_cfg.autoloot_key, kb, 32));
        break;
    case M_MENUKEY:
        _snwprintf_s(label, cap, _TRUNCATE, L"menu key");
        _snwprintf_s(value, cap, _TRUNCATE, L"%s",
                     key_label(g_cfg.menu_key, kb, 32));
        break;
    case M_INKEY:
        _snwprintf_s(label, cap, _TRUNCATE, L"insert key");
        _snwprintf_s(value, cap, _TRUNCATE, L"%s",
                     key_label(g_cfg.insert_key, kb, 32));
        break;
    default: break;
    }
#undef MENU_ONOFF
}

static void menu_save(void) {
    cfg_apply();
    cfg_save();
}

static void menu_set(int row, int dir, int act) {
    int toggle = act || dir != 0;
    switch (row) {
    case M_RAGE:     if (toggle) g_cfg.rage = !g_cfg.rage; break;
    case M_ESP:      if (toggle) g_cfg.esp = !g_cfg.esp; break;
    case M_BOX:      if (toggle) g_cfg.boxes = !g_cfg.boxes; break;
    case M_NAME:     if (toggle) g_cfg.names = !g_cfg.names; break;
    case M_SKEL:     if (toggle) g_cfg.skeleton = !g_cfg.skeleton; break;
    case M_HEALTH:   if (toggle) g_cfg.health = !g_cfg.health; break;
    case M_DIST:     if (toggle) g_cfg.distance = !g_cfg.distance; break;
    case M_MAXD:
        if (dir) {
            g_cfg.max_dist += dir * 5000;
            if (g_cfg.max_dist < 5000) g_cfg.max_dist = 5000;
            if (g_cfg.max_dist > 200000) g_cfg.max_dist = 200000;
        }
        break;
    case M_AIM:      if (toggle) g_cfg.aim = !g_cfg.aim; break;
    case M_AIMKEY:   if (act) { g_menu_rebind = 1; g_menu_rebind_at = GetTickCount(); return; } break;
    case M_AIMFOV:
        if (dir) {
            g_cfg.aim_fov += dir * 10;
            if (g_cfg.aim_fov < 10) g_cfg.aim_fov = 10;
            if (g_cfg.aim_fov > 300) g_cfg.aim_fov = 300;
        }
        break;
    case M_AIMSMOOTH:
        if (dir) {
            g_cfg.aim_smooth += dir;
            if (g_cfg.aim_smooth < 1) g_cfg.aim_smooth = 1;
            if (g_cfg.aim_smooth > 100) g_cfg.aim_smooth = 100;
        }
        break;
    case M_AIMBONE:
        if (dir)
            g_cfg.aim_bone = (g_cfg.aim_bone + dir + RR_AIM_GROUP_COUNT) % RR_AIM_GROUP_COUNT;
        break;
    case M_AIMDIST:
        if (dir) {
            g_cfg.aim_dist += dir * 10;
            if (g_cfg.aim_dist < 0) g_cfg.aim_dist = 0;
            if (g_cfg.aim_dist > 1000) g_cfg.aim_dist = 1000;
        }
        break;
    case M_AIMCAL:
        if (dir) {
            g_cfg.aim_cal += dir * 10;
            if (g_cfg.aim_cal < 10) g_cfg.aim_cal = 10;
            if (g_cfg.aim_cal > 2000) g_cfg.aim_cal = 2000;
        }
        break;
    case M_AIMFIRE:  if (toggle) g_cfg.aim_fire = !g_cfg.aim_fire; break;
    case M_AA:       if (toggle) g_cfg.aa = !g_cfg.aa; break;
    case M_AAMODE:
        if (dir || act)
            g_cfg.aa_mode = RR_ANTIAIM_STATIC;
        break;
    case M_AASPD:
        if (dir) {
            g_cfg.aa_speed += dir * 5;
            if (g_cfg.aa_speed < 1) g_cfg.aa_speed = 1;
            if (g_cfg.aa_speed > 100) g_cfg.aa_speed = 100;
        }
        break;
    case M_AARANGE:
        if (dir) {
            g_cfg.aa_range += dir;
            if (g_cfg.aa_range < 1) g_cfg.aa_range = 1;
            if (g_cfg.aa_range > 60) g_cfg.aa_range = 60;
        }
        break;
    case M_AAKEY:    if (act) { g_menu_rebind = 4; g_menu_rebind_at = GetTickCount(); return; } break;
    case M_TRIG:     if (toggle) g_cfg.trigger = !g_cfg.trigger; break;
    case M_TRIGKEY:  if (act) { g_menu_rebind = 2; g_menu_rebind_at = GetTickCount(); return; } break;
    case M_TRIGDELAY:
        if (dir) {
            g_cfg.trigger_delay += dir * 10;
            if (g_cfg.trigger_delay < 0) g_cfg.trigger_delay = 0;
            if (g_cfg.trigger_delay > 300) g_cfg.trigger_delay = 300;
        }
        break;
    case M_TRIGFOV:
        if (dir) {
            g_cfg.trigger_fov += dir;
            if (g_cfg.trigger_fov < 1) g_cfg.trigger_fov = 1;
            if (g_cfg.trigger_fov > 80) g_cfg.trigger_fov = 80;
        }
        break;
    case M_TRIGHEAD: if (toggle) g_cfg.trigger_head = !g_cfg.trigger_head; break;
    case M_NOSPREAD: if (toggle) g_cfg.nospread = !g_cfg.nospread; break;
    case M_NORECOIL: if (toggle) g_cfg.norecoil = !g_cfg.norecoil; break;
    case M_NCMODE:
        if (dir || act)
            g_cfg.recoil_mode = (g_cfg.recoil_mode + (dir ? dir : 1) + 3) % 3;
        break;
    case M_NCSTR:
        if (dir) {
            g_cfg.recoil_strength += dir * 10;
            if (g_cfg.recoil_strength < 0) g_cfg.recoil_strength = 0;
            if (g_cfg.recoil_strength > 200) g_cfg.recoil_strength = 200;
        }
        break;
    case M_RADAR:    if (toggle) g_cfg.radar = !g_cfg.radar; break;
    case M_RADRANGE:
        if (dir) {
            g_cfg.radar_range += dir * 10;
            if (g_cfg.radar_range < 25) g_cfg.radar_range = 25;
            if (g_cfg.radar_range > 500) g_cfg.radar_range = 500;
        }
        break;
    case M_RADLOOT:  if (toggle) g_cfg.radar_loot = !g_cfg.radar_loot; break;
    case M_LOOT:     if (toggle) g_cfg.loot = !g_cfg.loot; break;
    case M_LOOTNODE: if (toggle) g_cfg.loot_nodes = !g_cfg.loot_nodes; break;
    case M_CONT:     if (toggle) g_cfg.containers = !g_cfg.containers; break;
    case M_DOOR:     if (toggle) g_cfg.doors = !g_cfg.doors; break;
    case M_DROP:     if (toggle) g_cfg.dropped = !g_cfg.dropped; break;
    case M_VEHSL:    if (toggle) g_cfg.vehicle_loot = !g_cfg.vehicle_loot; break;
    case M_PRICE:    if (toggle) g_cfg.prices = !g_cfg.prices; break;
    case M_AUTOLOOT: if (toggle) g_cfg.autoloot = !g_cfg.autoloot; break;
    case M_ALLRANGE:
        if (dir) {
            g_cfg.autoloot_range += dir;
            if (g_cfg.autoloot_range < 2) g_cfg.autoloot_range = 2;
            if (g_cfg.autoloot_range > 50) g_cfg.autoloot_range = 50;
        }
        break;
    case M_ALLKEY:   if (act) { g_menu_rebind = 3; g_menu_rebind_at = GetTickCount(); return; } break;
    case M_FOVC:     if (toggle) g_cfg.fov_circle = !g_cfg.fov_circle; break;
    case M_MENUKEY:  if (act) { g_menu_rebind = 5; g_menu_rebind_at = GetTickCount(); return; } break;
    case M_INKEY:    if (act) { g_menu_rebind = 6; g_menu_rebind_at = GetTickCount(); return; } break;
    default: return;
    }
    menu_save();
}

/* Keys are polled, not hooked: a low-level hook would fight the game's input
 * and the anti-cheat's own hooks. The repeat timer makes an arrow key behave
 * like a keyboard rather than a once-per-press button. */
typedef struct { int held; DWORD next; } KEYREP;

static int key_rep(int vk, KEYREP* st) {
    DWORD now = GetTickCount();
    if (GetAsyncKeyState(vk) & 0x8000) {
        if (!st->held) { st->held = 1; st->next = now + 320; return 1; }
        if ((long)(now - st->next) >= 0) { st->next = now + 90; return 1; }
        return 0;
    }
    st->held = 0;
    return 0;
}

/* ---- panel geometry and pointer ------------------------------------------ */

/* One geometry function for painting and for hit testing: a panel that answers
 * a pointer has to be laid out from the numbers it was painted with, or the
 * click lands a row off at a different resolution. The row shrinks on a short
 * viewport so the whole panel always fits. */
typedef struct { float x, y, w, pad, row, rows, tabbar, h; } MENU_GEO;

static MENU_GEO menu_geo(float vh) {
    MENU_GEO g;
    int n = tab_rows(g_menu_tab);
    if (n < 1) n = 1;
    g.pad = 16.0f;
    g.x = 48.0f;
    g.y = 60.0f;
    g.w = 620.0f;
    g.row = 26.0f;
    g.tabbar = 28.0f;
    g.rows = (float)n;
    if (vh > 140.0f) {
        const float avail = vh - g.y - 8.0f - 34.0f - g.tabbar - 34.0f;
        const float fit = avail / g.rows;
        if (fit < g.row) g.row = fit;
    }
    if (g.row < 15.0f) g.row = 15.0f;
    g.h = 34.0f + g.tabbar + g.rows * g.row + 34.0f;
    return g;
}

/* Rows start under the title strip and the tab bar. Painting and the hit test
 * both read this, so a click on a row lands on that row at every resolution. */
static float menu_rows_top(const MENU_GEO* g) { return g->y + 34.0f + g->tabbar; }

/* Cursor position in the swapchain's pixel space. The game hides its own
 * pointer during a match, so the panel paints one at this position. */
static int menu_cursor(float* out_x, float* out_y) {
    POINT pt;
    if (!g_game_hwnd) return 0;
    if (!GetCursorPos(&pt)) return 0;
    if (!ScreenToClient(g_game_hwnd, &pt)) return 0;
    *out_x = (float)pt.x;
    *out_y = (float)pt.y;
    return 1;
}

/* Tab cell under a point, or -1. The bar is TAB_COUNT equal cells across the
 * panel width, which is what draw_menu paints. */
static int menu_tab_at(float cx, float cy) {
    const MENU_GEO g = menu_geo(g_vh);
    const float top = g.y + 34.0f;
    const float cell = g.w / (float)TAB_COUNT;
    int t;
    if (cx < g.x || cx > g.x + g.w) return -1;
    if (cy < top || cy > top + g.tabbar) return -1;
    if (cell <= 0.0f) return -1;
    t = (int)((cx - g.x) / cell);
    if (t < 0 || t >= TAB_COUNT) return -1;
    return t;
}

/* Screen row to the global row index of the active tab. */
static int menu_row_at(float cx, float cy) {
    const MENU_GEO g = menu_geo(g_vh);
    const float top = menu_rows_top(&g);
    int slot;
    if (cx < g.x || cx > g.x + g.w) return -1;
    if (cy < top || cy > top + g.rows * g.row) return -1;
    slot = (int)((cy - top) / g.row);
    if (slot < 0) slot = 0;
    if (slot >= (int)g.rows) slot = (int)g.rows - 1;
    return tab_row_at(g_menu_tab, slot);
}

/* Keeps the cursor inside the page after a tab switch or a resize. */
static void menu_clamp_sel(void) {
    const int n = tab_rows(g_menu_tab);
    if (n < 1) { g_menu_sel = 0; return; }
    if (g_menu_sel < 0) g_menu_sel = 0;
    if (g_menu_sel >= n) g_menu_sel = n - 1;
}

static void menu_tick(void) {
    static KEYREP k_menu, k_up, k_down, k_left, k_right, k_enter, k_space,
                 k_pgup, k_pgdn;
    int dir = 0, act = 0, vk;

    if (key_rep(g_cfg.menu_key, &k_menu)) {
        g_menu_open = !g_menu_open;
        g_menu_rebind = 0;
        g_menu_hover = -1;
        /* The press that opened the panel must not also click a row. */
        g_menu_lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        cfg_apply();
        ilog("menu: %s tab=%d", g_menu_open ? "open" : "closed", g_menu_tab);
    }
    if (!g_menu_open) return;

    /* The game clips the pointer to its window while the match runs; releasing
     * the clip is what lets the OS position follow the mouse over the panel. */
    ClipCursor(NULL);

    if (g_menu_rebind) {
        const DWORD since = GetTickCount() - g_menu_rebind_at;
        for (vk = 1; vk <= 254; ++vk) {
            if (!(GetAsyncKeyState(vk) & 1)) continue;
            if (vk == VK_ESCAPE) {
                g_menu_rebind = 0;
                ilog("menu: rebind cancelled");
                return;
            }
            /* The click that started a bind is still set in the key state for
             * a moment; ignore mouse buttons that long so a click cannot bind
             * itself to the row it landed on. */
            if (since < 300 && (vk == VK_LBUTTON || vk == VK_RBUTTON ||
                                vk == VK_MBUTTON || vk == VK_XBUTTON1 ||
                                vk == VK_XBUTTON2)) continue;
            if (g_menu_rebind == 1) g_cfg.aim_key = vk;
            else if (g_menu_rebind == 2) g_cfg.trigger_key = vk;
            else if (g_menu_rebind == 4) g_cfg.aa_key = vk;
            else if (g_menu_rebind == 5) g_cfg.menu_key = vk;
            else if (g_menu_rebind == 6) g_cfg.insert_key = vk;
            else g_cfg.autoloot_key = vk;
            ilog("menu: bind %s vk=%d",
                 g_menu_rebind == 1 ? "aim" : g_menu_rebind == 2 ? "trigger"
                 : g_menu_rebind == 4 ? "anti-aim" : g_menu_rebind == 5 ? "menu"
                 : g_menu_rebind == 6 ? "insert" : "interact", vk);
            g_menu_rebind = 0;
            menu_save();
            return;
        }
        return;
    }

    /* Tab bar first: a click there switches page and must not fall through to
     * the row under the pointer. */
    if (g_vh > 140.0f) {
        float cx = 0.0f, cy = 0.0f;
        const int lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        if (menu_cursor(&cx, &cy)) {
            const int tab = menu_tab_at(cx, cy);
            if (tab >= 0 && lmb && !g_menu_lmb) {
                g_menu_tab = tab;
                g_menu_sel = 0;
                g_menu_hover = -1;
                g_menu_lmb = lmb;
                cfg_apply();
                return;
            }
        }
    }

    /* Pointer next: hover paints the highlight, a left click commits the row.
     * A value row steps toward the half that was clicked, so one button covers
     * both directions without a modifier, and a switch toggles from either
     * half. */
    if (g_vh > 140.0f) {
        float cx = 0.0f, cy = 0.0f;
        const int lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        if (menu_cursor(&cx, &cy)) {
            const int row = menu_row_at(cx, cy);
            g_menu_hover = row;
            if (row >= 0 && lmb && !g_menu_lmb) {
                const MENU_GEO g = menu_geo(g_vh);
                g_menu_sel = row - tab_first(g_menu_tab);
                g_menu_lmb = lmb;
                menu_set(row, cx > g.x + g.w * 0.5f ? 1 : -1, 1);
                return;
            }
        } else {
            g_menu_hover = -1;
        }
        g_menu_lmb = lmb;
    }

    if (key_rep(VK_PRIOR, &k_pgup)) {
        g_menu_tab = (g_menu_tab + TAB_COUNT - 1) % TAB_COUNT;
        g_menu_sel = 0;
        g_menu_hover = -1;
        return;
    }
    if (key_rep(VK_NEXT, &k_pgdn)) {
        g_menu_tab = (g_menu_tab + 1) % TAB_COUNT;
        g_menu_sel = 0;
        g_menu_hover = -1;
        return;
    }

    menu_clamp_sel();
    {
        const int n = tab_rows(g_menu_tab);
        if (n > 0) {
            if (key_rep(VK_UP, &k_up))   g_menu_sel = (g_menu_sel + n - 1) % n;
            if (key_rep(VK_DOWN, &k_down)) g_menu_sel = (g_menu_sel + 1) % n;
        }
    }
    if (key_rep(VK_LEFT, &k_left))   dir = -1;
    if (key_rep(VK_RIGHT, &k_right)) dir = 1;
    if (key_rep(VK_RETURN, &k_enter) || key_rep(VK_SPACE, &k_space)) act = 1;
    if (dir || act) menu_set(tab_row_at(g_menu_tab, g_menu_sel), dir, act);
}

static void draw_menu(float W, float H, int seen) {
    const MENU_GEO g = menu_geo(H);
    D2D1_RECT_F box;
    wchar_t title[96];
    int k;

    (void)W;
    if (!g_fmt_l || !g_fmt_r || !g_fmt_t) return;
    if (g.h > H - g.y - 8.0f) return;   /* skip rather than draw a clipped panel */

    box.left = g.x; box.top = g.y; box.right = g.x + g.w; box.bottom = g.y + g.h;
    g_brush->SetColor(col(0xF20B1017, 1.0f));
    g_d2d_ctx->FillRectangle(box, g_brush);
    g_brush->SetColor(col(0xFF39FF88, 1.0f));
    g_d2d_ctx->DrawRectangle(box, g_brush, 2.0f);

    _snwprintf_s(title, 96, _TRUNCATE, L"rr_int   menu %s   seen %d",
                 g_cfg.rage ? L"[RAGE]" : L"", seen);
    {
        D2D1_RECT_F r;
        r.left = g.x + g.pad; r.top = g.y + 8.0f;
        r.right = g.x + g.w - g.pad; r.bottom = g.y + 30.0f;
        text_in(title, r, g_cfg.rage ? 0xFFFF7A5C : 0xFF39FF88, g_fmt_t);
    }

    /* Tab bar. The active page is the only one with a filled cell, and the
     * cells are the same split menu_tab_at() hit-tests. */
    {
        const float cell = g.w / (float)TAB_COUNT;
        int t;
        for (t = 0; t < TAB_COUNT; ++t) {
            D2D1_RECT_F rc;
            wchar_t name[24];
            rc.left = g.x + t * cell + 4.0f;
            rc.right = g.x + (t + 1) * cell - 4.0f;
            rc.top = g.y + 34.0f + 2.0f;
            rc.bottom = g.y + 34.0f + g.tabbar - 2.0f;
            if (t == g_menu_tab) {
                g_brush->SetColor(col(0x3339FF88, 1.0f));
                g_d2d_ctx->FillRectangle(rc, g_brush);
                g_brush->SetColor(col(0xFF39FF88, 1.0f));
                g_d2d_ctx->DrawRectangle(rc, g_brush, 1.4f);
            }
            _snwprintf_s(name, 24, _TRUNCATE, L"%s", tab_label[t]);
            rc.left += 8.0f;
            text_in(name, rc, t == g_menu_tab ? 0xFF39FF88 : 0xFF98A1B2, g_fmt_l);
        }
    }

    for (k = 0; k < (int)g.rows; ++k) {
        const int row = tab_row_at(g_menu_tab, k);
        wchar_t label[64], value[64];
        const float top = menu_rows_top(&g) + k * g.row;
        int lit;
        D2D1_RECT_F rl, rv;

        menu_text(row, label, value, 64);
        lit = (k == g_menu_sel) || (row == g_menu_hover);
        if (lit) {
            D2D1_RECT_F sel;
            sel.left = g.x + 5.0f; sel.top = top + 1.0f;
            sel.right = g.x + g.w - 5.0f; sel.bottom = top + g.row - 2.0f;
            g_brush->SetColor(col(k == g_menu_sel ? 0x30FFFFFF : 0x1CFFFFFF, 1.0f));
            g_d2d_ctx->FillRectangle(sel, g_brush);
            if (k == g_menu_sel) {
                g_brush->SetColor(col(0xFF39FF88, 1.0f));
                sel.left = g.x + 5.0f; sel.right = g.x + 8.0f;
                g_d2d_ctx->FillRectangle(sel, g_brush);
            }
        }
        if (row == M_RAGE && g_cfg.rage) {
            rl.left = g.x + g.pad; rl.top = top; rl.right = g.x + g.w * 0.58f; rl.bottom = top + g.row;
            text_in(label, rl, 0xFFFF7A5C, g_fmt_l);
        } else {
            rl.left = g.x + g.pad; rl.top = top; rl.right = g.x + g.w * 0.58f; rl.bottom = top + g.row;
            text_in(label, rl, lit ? 0xFFFFFFFF : 0xFFC6CBD5, g_fmt_l);
        }
        rv.left = g.x + g.w * 0.50f; rv.top = top; rv.right = g.x + g.w - g.pad; rv.bottom = top + g.row;
        text_in(value, rv, k == g_menu_sel ? 0xFF39FF88 : lit ? 0xFFDDE3EC : 0xFF98A1B2, g_fmt_r);
    }

    /* Footer: the key hint, plus the live instant-lock numbers the aim worker
     * learned on this machine (pixels per count and axis direction). Those two
     * are what a wrong-sensitivity aim looks like from the outside. */
    {
        D2D1_RECT_F rf;
        wchar_t hint[192];
        if (g_menu_rebind) {
            _snwprintf_s(hint, 192, _TRUNCATE, L"press a key - esc cancels");
        } else {
            RR_AIM_FRAME af;
            rr_aim_frame_stats(&af);
            if (af.picked)
                _snwprintf_s(hint, 192, _TRUNCATE,
                             L"click a row  -  arrows + enter  -  pgup/pgdn tabs   "
                             L"px %.2f,%.2f  sign %+d,%+d  err %.0f,%.0f",
                             af.px_x, af.px_y, af.sign_x, af.sign_y, af.err_x, af.err_y);
            else
                _snwprintf_s(hint, 192, _TRUNCATE,
                             L"click a row  -  arrows + enter  -  pgup/pgdn tabs");
        }
        rf.left = g.x + g.pad; rf.top = g.y + g.h - 28.0f;
        rf.right = g.x + g.w - g.pad; rf.bottom = g.y + g.h - 4.0f;
        text_in(hint, rf, 0xFF7C8595, g_fmt_l);
    }

    /* Painted pointer: the game hides the OS cursor, so without this the panel
     * would take clicks from an invisible hand. */
    {
        float cx = 0.0f, cy = 0.0f;
        if (menu_cursor(&cx, &cy) && cx >= 0.0f && cy >= 0.0f &&
            cx <= W + 40.0f && cy <= H + 40.0f) {
            g_brush->SetColor(col(0xFF39FF88, 1.0f));
            g_d2d_ctx->DrawLine(D2D1::Point2F(cx - 9.0f, cy), D2D1::Point2F(cx - 2.5f, cy), g_brush, 1.6f);
            g_d2d_ctx->DrawLine(D2D1::Point2F(cx + 2.5f, cy), D2D1::Point2F(cx + 9.0f, cy), g_brush, 1.6f);
            g_d2d_ctx->DrawLine(D2D1::Point2F(cx, cy - 9.0f), D2D1::Point2F(cx, cy - 2.5f), g_brush, 1.6f);
            g_d2d_ctx->DrawLine(D2D1::Point2F(cx, cy + 2.5f), D2D1::Point2F(cx, cy + 9.0f), g_brush, 1.6f);
            g_d2d_ctx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 2.2f, 2.2f), g_brush, 1.4f);
        }
    }
}

/* Full rig skeleton. The generated bone table lives in rr_game.c, so the
 * payload asks for one parent index at a time instead of compiling the model
 * header in twice. */
static void draw_skeleton(const RR_PLAYER* p, const RR_CAMERA* cam, int vw, int vh, UINT32 argb) {
    RR_V3 world[RR_MAX_BONES];
    float xy[RR_MAX_BONES * 2];
    int nb = rr_bone_count();
    int b;

    if (nb > RR_MAX_BONES) nb = RR_MAX_BONES;
    if (!p->nbones || nb < 2) return;
    memcpy(world, p->bone, sizeof(RR_V3) * (size_t)nb);
    game_w2s_many(cam, world, nb, vw, vh, xy);
    g_brush->SetColor(col(argb, 0.9f));
    for (b = 0; b < nb; ++b) {
        int par = rr_bone_parent(b);
        float x, y, px, py;
        if (par < 0 || par >= nb) continue;
        if (!rr_bone_drawable(b) || !rr_bone_drawable(par)) continue;
        x = xy[b * 2]; y = xy[b * 2 + 1];
        px = xy[par * 2]; py = xy[par * 2 + 1];
        if (!isfinite(x) || !isfinite(y) || !isfinite(px) || !isfinite(py)) continue;
        g_d2d_ctx->DrawLine(D2D1::Point2F(px, py), D2D1::Point2F(x, y), g_brush, 1.1f);
    }
}

/* Rarity drives the item and node hues; the fixed kinds keep their own colour
 * so a container never looks like a legendary rifle. Mirrors the external
 * overlay's palette so both builds read the same in a screenshot. */
static UINT32 loot_color(int kind, int rarity) {
    static const UINT32 rarity_colors[] = {
        0xFFE6E6E6, 0xFFE6E6E6, 0xFF91E891, 0xFF69B2FF,
        0xFFD39BFF, 0xFFFFBE5C, 0xFFFF7195
    };
    switch (kind) {
    case 1: case 2:
        if (rarity >= 1 && rarity <= 6) return rarity_colors[rarity];
        return 0xFF69B2FF;
    case 3: return 0xFFFFBE5C;
    case 4: return 0xFFB4C8E1;
    case 5: return 0xFF9CDA72;
    case 6: return 0xFFFF9C66;
    default: return 0xFFEBEBEB;
    }
}

/* A loot kind is drawn only when its own switch is on; the same table gates the
 * radar blips so the dial and the world never disagree. */
static int loot_kind_on(int kind) {
    switch (kind) {
    case 1: return g_cfg.loot;
    case 2: return g_cfg.loot_nodes;
    case 3: return g_cfg.containers;
    case 4: return g_cfg.doors;
    case 5: return g_cfg.dropped;
    case 6: return g_cfg.vehicle_loot;
    default: return 0;
    }
}

/* Set every frame by the marker pass: a container or a body inside loot range
 * is what the auto-loot key hold reacts to. */
static int g_interact_near = 0;

static void draw_loot_markers(const RR_PLAYER* players, int n, const RR_CAMERA* cam,
                              int vw, int vh) {
    float maxm = g_cfg.max_dist / 100.0f;
    int i;

    g_interact_near = 0;
    for (i = 0; i < n; ++i) {
        const RR_PLAYER* p = &players[i];
        wchar_t wname[RR_NAME_MAX], line[160];
        D2D1_ELLIPSE dot;
        float x, y;
        UINT32 rgb;

        if (!p->valid || !p->is_loot) continue;
        if (p->is_loot == 3 || p->is_loot == 5) {
            if (p->dist <= (float)g_cfg.autoloot_range) g_interact_near = 1;
        }
        if (!loot_kind_on(p->is_loot)) continue;
        if (p->dist > maxm) continue;
        if (!game_w2s(cam, &p->pos, vw, vh, &x, &y)) continue;
        if (x < -200.0f || x > vw + 200.0f || y < -100.0f || y > vh + 100.0f) continue;

        rgb = loot_color(p->is_loot, p->loot_rarity);
        g_brush->SetColor(col(rgb, 1.0f));
        dot = D2D1::Ellipse(D2D1::Point2F(x, y), 3.6f, 3.6f);
        g_d2d_ctx->FillEllipse(dot, g_brush);

        narrow_to_wide(p->name[0] ? p->name : "loot", wname, RR_NAME_MAX);
        if (g_cfg.prices && (p->is_loot == 1 || p->is_loot == 5 || p->is_loot == 6) &&
            (p->loot_sell_price > 0 || p->loot_base_price > 0)) {
            int price = p->loot_sell_price > 0 ? p->loot_sell_price : p->loot_base_price;
            _snwprintf_s(line, 160, _TRUNCATE, L"%s  %d  %.0fm", wname, price, (double)p->dist);
        } else {
            _snwprintf_s(line, 160, _TRUNCATE, L"%s  %.0fm", wname, (double)p->dist);
        }
        draw_text(line, x, y - 16.0f, 320.0f, rgb);
    }
}

/* Top-right dial. Up is the direction the camera looks, so the picture matches
 * what the player sees; the local player sits in the middle and everything past
 * the range is dropped rather than pinned to the rim. */
static void draw_radar(const RR_PLAYER* players, int n, const RR_CAMERA* cam,
                       float W, float H) {
    const RR_PLAYER* local = NULL;
    float cx, cy, R, range, fx, fz, len;
    D2D1_ELLIPSE dial;
    wchar_t label[32];
    int i;

    for (i = 0; i < n; ++i) {
        if (players[i].valid && players[i].local && !players[i].is_loot) { local = &players[i]; break; }
    }
    if (!local || g_cfg.radar_range <= 0) return;

    R = fminf(H * 0.17f, W * 0.13f);
    if (R < 55.0f) return;
    cx = W - R - 26.0f;
    cy = R + 26.0f;
    range = (float)g_cfg.radar_range;

    fx = cam->valid ? cam->fwd.x : 0.0f;
    fz = cam->valid ? cam->fwd.z : 0.0f;
    len = hypotf(fx, fz);
    if (!(len > 0.01f) || !isfinite(len)) { fx = 0.0f; fz = 1.0f; }
    else { fx /= len; fz /= len; }

    g_brush->SetColor(col(0x99000000, 1.0f));
    dial = D2D1::Ellipse(D2D1::Point2F(cx, cy), R, R);
    g_d2d_ctx->FillEllipse(dial, g_brush);
    g_brush->SetColor(col(0xCC39FF88, 1.0f));
    g_d2d_ctx->DrawEllipse(dial, g_brush, 1.6f);
    dial = D2D1::Ellipse(D2D1::Point2F(cx, cy), R * 0.5f, R * 0.5f);
    g_brush->SetColor(col(0x4439FF88, 1.0f));
    g_d2d_ctx->DrawEllipse(dial, g_brush, 1.0f);
    g_d2d_ctx->DrawLine(D2D1::Point2F(cx - R * 0.5f, cy), D2D1::Point2F(cx + R * 0.5f, cy), g_brush, 1.0f);
    g_d2d_ctx->DrawLine(D2D1::Point2F(cx, cy - R * 0.5f), D2D1::Point2F(cx, cy + R * 0.5f), g_brush, 1.0f);

    for (i = 0; i < n; ++i) {
        const RR_PLAYER* p = &players[i];
        float dx, dz, lateral, forward, d, x, y;
        UINT32 rgb;

        if (!p->valid || p == local) continue;
        if (p->is_loot) {
            if (!g_cfg.radar_loot || !loot_kind_on(p->is_loot)) continue;
        }
        dx = p->pos.x - local->pos.x;
        dz = p->pos.z - local->pos.z;
        lateral = dx * fz - dz * fx;
        forward = dx * fx + dz * fz;
        d = sqrtf(lateral * lateral + forward * forward);
        if (d > range) continue;
        x = cx + lateral * (R / range);
        y = cy - forward * (R / range);

        if (p->is_loot) rgb = loot_color(p->is_loot, p->loot_rarity);
        else if (p->is_vehicle) rgb = 0xFFFFD66E;
        else rgb = color_for(p);

        g_brush->SetColor(col(rgb, 1.0f));
        if (!p->is_loot && !p->is_vehicle && p->dead) {
            D2D1_ELLIPSE ring = D2D1::Ellipse(D2D1::Point2F(x, y), 3.0f, 3.0f);
            g_d2d_ctx->DrawEllipse(ring, g_brush, 1.4f);
        } else {
            D2D1_ELLIPSE blip = D2D1::Ellipse(D2D1::Point2F(x, y),
                                             p->is_loot ? 2.4f : 3.2f,
                                             p->is_loot ? 2.4f : 3.2f);
            g_d2d_ctx->FillEllipse(blip, g_brush);
        }
    }

    /* Self marker: a short heading tick pointing at the top of the dial. */
    g_brush->SetColor(col(0xFF39FF88, 1.0f));
    g_d2d_ctx->DrawLine(D2D1::Point2F(cx, cy), D2D1::Point2F(cx, cy - 9.0f), g_brush, 2.0f);

    _snwprintf_s(label, 32, _TRUNCATE, L"%d m", g_cfg.radar_range);
    draw_text(label, cx, cy + R + 4.0f, 90.0f, 0xFF39FF88);
}

/* ---- caption placement --------------------------------------------------- */

/* Captions are queued during the player pass and placed after it. A caption
 * that has no free line is dropped for that frame instead of being pushed
 * across the screen: pushing is what turns a doorway with two players into a
 * text pile, because a displaced caption lands on the next one's box.
 *
 * Three pieces:
 *   - one DirectWrite layout per distinct string, kept with its measured width,
 *     so a name is converted, shaped and measured once and not sixty times a
 *     second;
 *   - an 8 px occupancy grid with conservative rounding, so a candidate rect
 *     never slips between two marks;
 *   - per-caption state keyed by model address: the last valid displacement is
 *     offered first and a caption that lost its line waits a few frames before
 *     it tries again, which is what stops the near-two-bodies jitter. */

#define RR_HUD_LABEL_MAX   96
#define RR_HUD_TEXT_MAX   128
#define RR_HUD_CELL         8.0f
#define RR_HUD_GRID_W     512                    /* 4096 px at 8 px cells */
#define RR_HUD_GRID_H     320                    /* 2560 px at 8 px cells */
#define RR_HUD_TEXT_CACHE 192
#define RR_HUD_TEXT_CAP   320.0f
#define RR_HUD_TEXT_H      18.0f
#define RR_HUD_SLOTS      128
#define RR_HUD_REVEAL       5                    /* frames of free space */

typedef struct {
    uint64_t id;
    float    ax, ay;        /* centred on ax, painted at ay + dy */
    float    w, h;
    float    prio;          /* lower is placed first */
    int      rank;          /* order in the previous frame */
    UINT32   argb;
    wchar_t  text[RR_HUD_TEXT_MAX];
} RR_HUD_LABEL;

typedef struct {
    wchar_t            text[RR_HUD_TEXT_MAX];
    UINT32             hash;
    IDWriteTextLayout* layout;
    float              width;
    unsigned           stamp;
} RR_HUD_TEXT_SLOT;

typedef struct {
    uint64_t id;
    float    dy;            /* displacement that fit last time */
    int      visible;
    int      hidden;        /* frames spent without a line */
    int      rank;
    unsigned seen;
} RR_HUD_SLOT;

static RR_HUD_LABEL     g_hud_labels[RR_HUD_LABEL_MAX];
static int              g_hud_label_count;
static RR_HUD_TEXT_SLOT g_hud_text[RR_HUD_TEXT_CACHE];
static RR_HUD_SLOT      g_hud_slots[RR_HUD_SLOTS];
static unsigned         g_hud_grid[RR_HUD_GRID_H][RR_HUD_GRID_W / 32];
static unsigned         g_hud_stamp;
static unsigned         g_hud_frame;

static UINT32 hud_hash(const wchar_t* s) {
    UINT32 h = 2166136261u;
    while (s && *s) { h ^= (UINT32)(unsigned)*s; h *= 16777619u; ++s; }
    return h;
}

/* Returns the cached layout for this exact string, building it on a miss and
 * evicting the least recently used entry when the table is full. NULL means the
 * caller falls back to the uncached DrawText path. */
static RR_HUD_TEXT_SLOT* hud_text_slot(const wchar_t* s) {
    const UINT32 h = hud_hash(s);
    int i, victim = -1;
    unsigned oldest = ~0u;

    if (!s || !s[0]) return NULL;
    for (i = 0; i < RR_HUD_TEXT_CACHE; ++i) {
        RR_HUD_TEXT_SLOT* e = &g_hud_text[i];
        if (e->layout && e->hash == h && !wcscmp(e->text, s)) {
            e->stamp = ++g_hud_stamp;
            return e;
        }
    }
    for (i = 0; i < RR_HUD_TEXT_CACHE; ++i) {
        RR_HUD_TEXT_SLOT* e = &g_hud_text[i];
        if (!e->layout) { if (victim < 0) victim = i; continue; }
        if (e->stamp < oldest) { oldest = e->stamp; victim = i; }
    }
    if (victim < 0) victim = 0;
    if (!g_dw || !g_fmt) return NULL;
    {
        IDWriteTextLayout* layout = NULL;
        DWRITE_TEXT_METRICS m;
        float width;
        RR_HUD_TEXT_SLOT* e = &g_hud_text[victim];

        if (FAILED(g_dw->CreateTextLayout(s, (UINT32)wcslen(s), g_fmt,
                                          RR_HUD_TEXT_CAP, RR_HUD_TEXT_H + 6.0f,
                                          &layout)) || !layout) return NULL;
        width = RR_HUD_TEXT_CAP * 0.5f;
        if (SUCCEEDED(layout->GetMetrics(&m)) && isfinite(m.widthIncludingTrailingWhitespace))
            width = m.widthIncludingTrailingWhitespace;
        if (!(width > 4.0f)) width = 4.0f;
        if (width > RR_HUD_TEXT_CAP) width = RR_HUD_TEXT_CAP;

        if (e->layout) { e->layout->Release(); e->layout = NULL; }
        wcsncpy_s(e->text, RR_HUD_TEXT_MAX, s, _TRUNCATE);
        e->hash = h;
        e->layout = layout;
        e->width = width;
        e->stamp = ++g_hud_stamp;
        return e;
    }
}

/* Cached path for a centred caption. The clip rectangle is the reserved width
 * from the layout, so the pixels drawn and the space reserved agree. */
static void hud_draw_text(const wchar_t* s, float cx, float y, UINT32 argb) {
    RR_HUD_TEXT_SLOT* slot = hud_text_slot(s);
    if (!slot || !g_d2d_ctx || !g_brush) {
        draw_text(s, cx, y, 260.0f, argb);
        return;
    }
    {
        const float left = cx - slot->width * 0.5f;
        D2D1_RECT_F clip;
        clip.left = left;
        clip.top = y;
        clip.right = left + slot->width;
        clip.bottom = y + RR_HUD_TEXT_H;
        g_brush->SetColor(col(argb, 1.0f));
        g_d2d_ctx->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
        g_d2d_ctx->DrawTextLayout(D2D1::Point2F(left, y), slot->layout, g_brush);
        g_d2d_ctx->PopAxisAlignedClip();
    }
}

/* Outward-rounded cell span. Rounding out is what keeps two rectangles that
 * are 3 px apart from sharing no cell and therefore never colliding. */
static void hud_cells(float x, float y, float w, float h,
                      int* cx0, int* cy0, int* cx1, int* cy1) {
    int a = (int)floorf(x / RR_HUD_CELL);
    int b = (int)ceilf((x + w) / RR_HUD_CELL);
    int c = (int)floorf(y / RR_HUD_CELL);
    int d = (int)ceilf((y + h) / RR_HUD_CELL);
    if (a < 0) a = 0;
    if (c < 0) c = 0;
    if (b > RR_HUD_GRID_W) b = RR_HUD_GRID_W;
    if (d > RR_HUD_GRID_H) d = RR_HUD_GRID_H;
    *cx0 = a; *cy0 = c; *cx1 = b; *cy1 = d;
}

static int hud_grid_free(float x, float y, float w, float h) {
    int x0, y0, x1, y1, r, c;
    hud_cells(x, y, w, h, &x0, &y0, &x1, &y1);
    for (r = y0; r < y1; ++r)
        for (c = x0; c < x1; ++c)
            if (g_hud_grid[r][c >> 5] & (1u << (c & 31))) return 0;
    return 1;
}

static void hud_grid_mark(float x, float y, float w, float h) {
    int x0, y0, x1, y1, r, c;
    hud_cells(x, y, w, h, &x0, &y0, &x1, &y1);
    for (r = y0; r < y1; ++r)
        for (c = x0; c < x1; ++c)
            g_hud_grid[r][c >> 5] |= 1u << (c & 31);
}

/* Per-caption state. Full table evicts the entry seen longest ago, so a body
 * that walked off screen does not hold a displacement slot forever. */
static RR_HUD_SLOT* hud_slot(uint64_t id) {
    int i, victim = -1;
    unsigned oldest = ~0u;

    for (i = 0; i < RR_HUD_SLOTS; ++i)
        if (g_hud_slots[i].id == id && id) return &g_hud_slots[i];
    for (i = 0; i < RR_HUD_SLOTS; ++i) {
        if (!g_hud_slots[i].id) return &g_hud_slots[i];
        if (g_hud_slots[i].seen < oldest) { oldest = g_hud_slots[i].seen; victim = i; }
    }
    if (victim < 0) return NULL;
    memset(&g_hud_slots[victim], 0, sizeof(g_hud_slots[victim]));
    return &g_hud_slots[victim];
}

/* Queue one caption. prio carries the distance bucket and the kind so a near
 * meta line outranks a far name, and both of one body stay in order. */
static void hud_push(uint64_t id, const wchar_t* text, float ax, float ay,
                     float maxw, float prio, UINT32 argb) {
    RR_HUD_LABEL* l;
    RR_HUD_TEXT_SLOT* slot;
    if (!text || !text[0]) return;
    if (g_hud_label_count >= RR_HUD_LABEL_MAX) return;
    /* A body without a model address still gets its own slot, keyed on the text
     * so two captions can never share one displacement. */
    if (!id) id = 0x8000000000000000ull | hud_hash(text);
    slot = hud_text_slot(text);
    l = &g_hud_labels[g_hud_label_count++];
    l->id = id;
    l->ax = ax;
    l->ay = ay;
    l->w = slot ? slot->width : maxw * 0.5f;
    if (l->w > maxw) l->w = maxw;
    l->h = RR_HUD_TEXT_H;
    l->prio = prio;
    l->rank = 0x7FFF;
    l->argb = argb;
    wcsncpy_s(l->text, RR_HUD_TEXT_MAX, text, _TRUNCATE);
}

static int hud_after(const RR_HUD_LABEL* a, const RR_HUD_LABEL* b) {
    if (a->prio != b->prio) return a->prio > b->prio;
    if (a->rank != b->rank) return a->rank > b->rank;
    return a->id > b->id;
}

static void hud_sort(void) {
    int i, j;
    for (i = 1; i < g_hud_label_count; ++i) {
        const RR_HUD_LABEL key = g_hud_labels[i];
        for (j = i - 1; j >= 0 && hud_after(&g_hud_labels[j], &key); --j)
            g_hud_labels[j + 1] = g_hud_labels[j];
        g_hud_labels[j + 1] = key;
    }
}

/* The dial and the open panel are reserved before any caption: a caption never
 * covers a control the player has to click. */
static void hud_reserve_fixed(const RR_PLAYER* players, int n, float W, float H) {
    int i;
    if (g_menu_open) {
        const MENU_GEO mg = menu_geo(H);
        hud_grid_mark(mg.x - 4.0f, mg.y - 4.0f, mg.w + 8.0f, mg.h + 8.0f);
    }
    if (!g_cfg.radar || g_cfg.radar_range <= 0) return;
    for (i = 0; i < n; ++i)
        if (players[i].valid && players[i].local && !players[i].is_loot) break;
    if (i >= n) return;
    {
        const float R = fminf(H * 0.17f, W * 0.13f);
        float cx, cy;
        if (R < 55.0f) return;
        cx = W - R - 26.0f;
        cy = R + 26.0f;
        hud_grid_mark(cx - R - 8.0f, cy - R - 8.0f, 2.0f * R + 16.0f, 2.0f * R + 34.0f);
    }
}

static void hud_place_and_draw(const RR_PLAYER* players, int n, float W, float H) {
    static const float offset[] = { 0.0f, -12.0f, 12.0f, -24.0f, 24.0f,
                                    -36.0f, 36.0f, -48.0f, 48.0f };
    const int n_offset = (int)(sizeof offset / sizeof offset[0]);
    int i, k;

    if (!g_hud_label_count || !g_d2d_ctx || !g_brush) return;

    ++g_hud_frame;
    memset(g_hud_grid, 0, sizeof g_hud_grid);
    hud_reserve_fixed(players, n, W, H);

    for (i = 0; i < g_hud_label_count; ++i) {
        const RR_HUD_SLOT* s = hud_slot(g_hud_labels[i].id);
        if (s) g_hud_labels[i].rank = s->rank;
    }
    hud_sort();

    for (i = 0; i < g_hud_label_count; ++i) {
        const RR_HUD_LABEL* l = &g_hud_labels[i];
        RR_HUD_SLOT* s = hud_slot(l->id);
        int placed = 0;

        if (!s) continue;
        if (!s->id) s->id = l->id;
        s->seen = g_hud_frame;
        s->rank = i;

        if (!s->visible && s->hidden > 0 && s->hidden < RR_HUD_REVEAL) {
            ++s->hidden;
            continue;
        }

        for (k = -1; k < n_offset && !placed; ++k) {
            const float dy = (k < 0) ? s->dy : offset[k];
            float x, y;
            if (k >= 0 && dy == s->dy) continue;
            x = l->ax - l->w * 0.5f;
            y = l->ay + dy;
            if (y < 0.0f || y + l->h > H) continue;
            if (x < 2.0f) x = 2.0f;
            if (x + l->w > W - 2.0f) x = W - 2.0f - l->w;
            if (x < 0.0f || !hud_grid_free(x, y, l->w, l->h)) continue;
            hud_grid_mark(x, y, l->w, l->h);
            s->dy = dy;
            s->visible = 1;
            s->hidden = 0;
            placed = 1;
            hud_draw_text(l->text, x + l->w * 0.5f, y, l->argb);
        }
        if (!placed) {
            s->visible = 0;
            if (s->hidden < RR_HUD_REVEAL) ++s->hidden;
        }
    }
    g_hud_label_count = 0;
}

/* ---- target-filter panel -------------------------------------------------
 * Four independent switches drawn as sliding toggles in their own strip under
 * the main panel. Self-contained: its own geometry, its own hit test, its own
 * mouse edge. Nothing in the row table or the big switches is touched, so the
 * existing keyboard navigation keeps working unchanged.
 *
 * A toggle is a pill with a knob that slides left (off) to right (on). Click
 * anywhere on the pill flips it; the knob animates over ~120 ms so the state
 * change reads as motion rather than a jump. */

typedef struct { float x, y, w, h; } IGN_RECT;

static float g_ign_anim[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
static int   g_ign_lmb = 0;
static DWORD g_ign_last = 0;

static const wchar_t* const ign_name[4] = {
    L"Ignore dead", L"Ignore prone", L"Ignore party", L"Ignore clan"
};

static int ign_value(int k) {
    switch (k) {
    case 0: return g_cfg.ign_dead;
    case 1: return g_cfg.ign_prone;
    case 2: return g_cfg.ign_party;
    case 3: return g_cfg.ign_clan;
    default: return 0;
    }
}

static void ign_set(int k, int v) {
    switch (k) {
    case 0: g_cfg.ign_dead  = v; break;
    case 1: g_cfg.ign_prone = v; break;
    case 2: g_cfg.ign_party = v; break;
    case 3: g_cfg.ign_clan  = v; break;
    default: break;
    }
}

/* Strip geometry in surface pixels. Sits at the bottom-left, clear of the main
 * panel and of the radar in the top-right. */
static IGN_RECT ign_strip(float W, float H) {
    IGN_RECT r;
    r.w = 300.0f;
    r.h = 132.0f;
    r.x = 24.0f;
    r.y = H - r.h - 24.0f;
    if (r.y < 24.0f) r.y = 24.0f;
    if (r.x + r.w > W - 24.0f) r.x = W - r.w - 24.0f;
    if (r.x < 8.0f) r.x = 8.0f;
    return r;
}

/* Pill rect for row k inside the strip. */
static IGN_RECT ign_pill(IGN_RECT s, int k) {
    IGN_RECT p;
    p.w = 46.0f;
    p.h = 20.0f;
    p.x = s.x + s.w - p.w - 14.0f;
    p.y = s.y + 34.0f + (float)k * 24.0f;
    return p;
}

static int ign_cursor(float* mx, float* my) {
    POINT pt;
    RECT rc;
    if (!g_game_hwnd || !IsWindow(g_game_hwnd)) return 0;
    if (g_vw < 64.0f || g_vh < 64.0f) return 0;
    if (!GetCursorPos(&pt)) return 0;
    if (!GetClientRect(g_game_hwnd, &rc)) return 0;
    if (!ScreenToClient(g_game_hwnd, &pt)) return 0;
    const float cw = (float)(rc.right - rc.left);
    const float ch = (float)(rc.bottom - rc.top);
    if (cw < 1.0f || ch < 1.0f) return 0;
    *mx = (float)pt.x * (g_vw / cw);
    *my = (float)pt.y * (g_vh / ch);
    return 1;
}

static int ign_hit(IGN_RECT r, float x, float y) {
    return x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h;
}

/* Input first, so a click lands on the state the painter will show this frame. */
static void ign_tick(float W, float H) {
    const IGN_RECT s = ign_strip(W, H);
    float mx = 0.0f, my = 0.0f;
    const int have = ign_cursor(&mx, &my);
    const int down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const int edge = down && !g_ign_lmb;
    g_ign_lmb = down;

    if (edge && have) {
        for (int k = 0; k < 4; ++k) {
            /* Whole row is the click target, not just the pill: a 46x20 pill is
             * a small thing to hit while the game is running. */
            IGN_RECT row = s;
            row.y = s.y + 30.0f + (float)k * 24.0f;
            row.h = 24.0f;
            if (ign_hit(row, mx, my)) {
                ign_set(k, ign_value(k) ? 0 : 1);
                cfg_apply();
                cfg_save();
                break;
            }
        }
    }

    /* Knob animation, time-based so it looks the same at any frame rate. */
    const DWORD now = GetTickCount();
    float dt = g_ign_last ? (float)(now - g_ign_last) : 16.0f;
    g_ign_last = now;
    if (dt > 100.0f) dt = 100.0f;
    const float step = dt / 120.0f;
    for (int k = 0; k < 4; ++k) {
        const float want = ign_value(k) ? 1.0f : 0.0f;
        if (g_ign_anim[k] < want) {
            g_ign_anim[k] += step;
            if (g_ign_anim[k] > want) g_ign_anim[k] = want;
        } else if (g_ign_anim[k] > want) {
            g_ign_anim[k] -= step;
            if (g_ign_anim[k] < want) g_ign_anim[k] = want;
        }
    }
}

static void ign_draw(float W, float H) {
    if (!g_d2d_ctx || !g_brush) return;
    const IGN_RECT s = ign_strip(W, H);
    float mx = 0.0f, my = 0.0f;
    const int have = ign_cursor(&mx, &my);

    /* Backdrop */
    D2D1_RECT_F bg = D2D1::RectF(s.x, s.y, s.x + s.w, s.y + s.h);
    g_brush->SetColor(col(0xE6101418, 1.0f));
    g_d2d_ctx->FillRectangle(bg, g_brush);
    g_brush->SetColor(col(0xFF2A3138, 1.0f));
    g_d2d_ctx->DrawRectangle(bg, g_brush, 1.0f);

    /* Header */
    if (g_fmt_l) {
        D2D1_RECT_F hr = D2D1::RectF(s.x + 14.0f, s.y + 8.0f,
                                     s.x + s.w - 14.0f, s.y + 28.0f);
        text_in(L"TARGET FILTERS", hr, 0xFF6FE3A8, g_fmt_l);
    }

    for (int k = 0; k < 4; ++k) {
        const IGN_RECT p = ign_pill(s, k);
        const float t = g_ign_anim[k];
        IGN_RECT row = s;
        row.y = s.y + 30.0f + (float)k * 24.0f;
        row.h = 24.0f;
        const int hot = have && ign_hit(row, mx, my);

        /* Hover wash across the whole row */
        if (hot) {
            D2D1_RECT_F hb = D2D1::RectF(s.x + 6.0f, row.y,
                                         s.x + s.w - 6.0f, row.y + row.h);
            g_brush->SetColor(col(0x22FFFFFF, 1.0f));
            g_d2d_ctx->FillRectangle(hb, g_brush);
        }

        /* Label */
        if (g_fmt_l) {
            D2D1_RECT_F lr = D2D1::RectF(s.x + 14.0f, row.y + 1.0f,
                                         p.x - 10.0f, row.y + 21.0f);
            text_in(ign_name[k], lr, hot ? 0xFFFFFFFF : 0xFFC8D0D8, g_fmt_l);
        }

        /* Track: grey when off, green when on, blended by the animation. */
        const float r = (0x33 + (0x39 - 0x33) * t) / 255.0f;
        const float g = (0x3A + (0xD6 - 0x3A) * t) / 255.0f;
        const float b = (0x42 + (0x7F - 0x42) * t) / 255.0f;
        D2D1_COLOR_F tc; tc.r = r; tc.g = g; tc.b = b; tc.a = 1.0f;
        g_brush->SetColor(tc);
        D2D1_ROUNDED_RECT track = D2D1::RoundedRect(
            D2D1::RectF(p.x, p.y, p.x + p.w, p.y + p.h), p.h * 0.5f, p.h * 0.5f);
        g_d2d_ctx->FillRoundedRectangle(track, g_brush);

        /* Knob */
        const float pad = 3.0f;
        const float kd = p.h - pad * 2.0f;
        const float kx = p.x + pad + t * (p.w - kd - pad * 2.0f);
        g_brush->SetColor(col(0xFFF2F6FA, 1.0f));
        g_d2d_ctx->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(kx + kd * 0.5f, p.y + p.h * 0.5f),
                          kd * 0.5f, kd * 0.5f), g_brush);

        /* State word, right of nothing - sits inside the track's left/right */
        if (g_fmt_r) {
            D2D1_RECT_F sr = D2D1::RectF(p.x - 46.0f, row.y + 2.0f,
                                         p.x - 6.0f, row.y + 20.0f);
            text_in(ign_value(k) ? L"ON" : L"OFF", sr,
                    ign_value(k) ? 0xFF6FE3A8 : 0xFF7A848E, g_fmt_r);
        }
    }
}

/* ---- AIM panel -----------------------------------------------------------
 * Replaces the old AIM tab. Four controls only: enable, bone, FOV, smoothing,
 * plus nospread. No calibration: the magnet measures sensitivity itself. */

static float g_aim_anim[2] = { 0.0f, 0.0f };   /* enable, nospread toggles */
static int   g_aim_lmb = 0;
static DWORD g_aim_last = 0;
static int   g_aim_drag = -1;                  /* 0 = fov, 1 = smooth */

static const wchar_t* const bone_name[RR_AIM_GROUP_COUNT] = {
    L"Head", L"Neck", L"Chest", L"Pelvis", L"Closest",
    L"Arms", L"Legs", L"Face", L"Hands", L"Feet"
};

typedef struct { float x, y, w, h; } AIM_RECT;

static AIM_RECT aim_panel(float W, float H) {
    AIM_RECT r;
    r.w = 320.0f; r.h = 236.0f;
    r.x = W - r.w - 24.0f;
    r.y = 24.0f;
    if (r.x < 8.0f) r.x = 8.0f;
    if (r.y + r.h > H - 24.0f) r.y = H - r.h - 24.0f;
    if (r.y < 8.0f) r.y = 8.0f;
    return r;
}

static AIM_RECT aim_row(AIM_RECT s, int k) {
    AIM_RECT r = s;
    r.x = s.x + 12.0f;
    r.w = s.w - 24.0f;
    r.y = s.y + 32.0f + (float)k * 30.0f;
    r.h = 26.0f;
    return r;
}

static int aim_hit(AIM_RECT r, float x, float y) {
    return x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h;
}

/* Slider track occupies the right half of a row. */
static AIM_RECT aim_track(AIM_RECT row) {
    AIM_RECT t;
    t.w = 130.0f; t.h = 6.0f;
    t.x = row.x + row.w - t.w;
    t.y = row.y + row.h * 0.5f - t.h * 0.5f;
    return t;
}

static void aim_slider_draw(AIM_RECT row, float frac, UINT32 accent,
                            const wchar_t* label, int value, const wchar_t* unit) {
    if (g_fmt_l) {
        D2D1_RECT_F lr = D2D1::RectF(row.x, row.y + 3.0f,
                                     row.x + 120.0f, row.y + row.h - 3.0f);
        text_in(label, lr, 0xFFC8D0D8, g_fmt_l);
    }
    const AIM_RECT t = aim_track(row);
    D2D1_ROUNDED_RECT bg = D2D1::RoundedRect(
        D2D1::RectF(t.x, t.y, t.x + t.w, t.y + t.h), 3.0f, 3.0f);
    g_brush->SetColor(col(0xFF2A3138, 1.0f));
    g_d2d_ctx->FillRoundedRectangle(bg, g_brush);
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    if (frac > 0.001f) {
        D2D1_ROUNDED_RECT fg = D2D1::RoundedRect(
            D2D1::RectF(t.x, t.y, t.x + t.w * frac, t.y + t.h), 3.0f, 3.0f);
        g_brush->SetColor(col(accent | 0xFF000000u, 1.0f));
        g_d2d_ctx->FillRoundedRectangle(fg, g_brush);
    }
    const float kx = t.x + t.w * frac;
    g_brush->SetColor(col(0xFFF2F6FA, 1.0f));
    g_d2d_ctx->FillEllipse(
        D2D1::Ellipse(D2D1::Point2F(kx, t.y + t.h * 0.5f), 6.0f, 6.0f), g_brush);
    if (g_fmt_r) {
        wchar_t buf[32];
        _snwprintf_s(buf, 32, _TRUNCATE, L"%d%s", value, unit);
        D2D1_RECT_F vr = D2D1::RectF(t.x - 56.0f, row.y + 3.0f,
                                     t.x - 8.0f, row.y + row.h - 3.0f);
        text_in(buf, vr, 0xFF9AA4AE, g_fmt_r);
    }
}

static void aim_toggle_draw(AIM_RECT row, const wchar_t* label, float t) {
    if (g_fmt_l) {
        D2D1_RECT_F lr = D2D1::RectF(row.x, row.y + 3.0f,
                                     row.x + 160.0f, row.y + row.h - 3.0f);
        text_in(label, lr, 0xFFC8D0D8, g_fmt_l);
    }
    const float pw = 46.0f, ph = 20.0f;
    const float px = row.x + row.w - pw;
    const float py = row.y + (row.h - ph) * 0.5f;
    const float r = (0x33 + (0x39 - 0x33) * t) / 255.0f;
    const float g = (0x3A + (0xD6 - 0x3A) * t) / 255.0f;
    const float b = (0x42 + (0x7F - 0x42) * t) / 255.0f;
    D2D1_COLOR_F tc; tc.r=r; tc.g=g; tc.b=b; tc.a=1.0f;
    g_brush->SetColor(tc);
    g_d2d_ctx->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(px, py, px+pw, py+ph), ph*0.5f, ph*0.5f), g_brush);
    const float kd = ph - 6.0f;
    const float kx = px + 3.0f + t * (pw - kd - 6.0f);
    g_brush->SetColor(col(0xFFF2F6FA, 1.0f));
    g_d2d_ctx->FillEllipse(
        D2D1::Ellipse(D2D1::Point2F(kx + kd*0.5f, py + ph*0.5f), kd*0.5f, kd*0.5f), g_brush);
}

static void aim_tick(float W, float H) {
    const AIM_RECT s = aim_panel(W, H);
    float mx = 0.0f, my = 0.0f;
    const int have = ign_cursor(&mx, &my);
    const int down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const int edge = down && !g_aim_lmb;
    g_aim_lmb = down;

    if (have) {
        /* rows: 0 enable, 1 bone, 2 fov, 3 smooth, 4 nospread */
        if (edge) {
            const AIM_RECT r0 = aim_row(s, 0);
            const AIM_RECT r1 = aim_row(s, 1);
            const AIM_RECT r4 = aim_row(s, 4);
            if (aim_hit(r0, mx, my)) { g_cfg.aim = !g_cfg.aim; cfg_apply(); cfg_save(); }
            else if (aim_hit(r4, mx, my)) { g_cfg.nospread = !g_cfg.nospread; cfg_apply(); cfg_save(); }
            else if (aim_hit(r1, mx, my)) {
                /* left half cycles back, right half forward */
                const int dir = (mx < r1.x + r1.w * 0.5f) ? -1 : 1;
                int b = g_cfg.aim_bone + dir;
                if (b < 0) b = RR_AIM_GROUP_COUNT - 1;
                if (b >= RR_AIM_GROUP_COUNT) b = 0;
                g_cfg.aim_bone = b; cfg_apply(); cfg_save();
            }
            else {
                for (int k = 0; k < 2; ++k) {
                    const AIM_RECT t = aim_track(aim_row(s, 2 + k));
                    AIM_RECT grab = t;
                    grab.y -= 10.0f; grab.h += 20.0f;
                    grab.x -= 8.0f; grab.w += 16.0f;
                    if (aim_hit(grab, mx, my)) { g_aim_drag = k; break; }
                }
            }
        }
        if (g_aim_drag >= 0 && down) {
            const AIM_RECT t = aim_track(aim_row(s, 2 + g_aim_drag));
            float f = (mx - t.x) / t.w;
            if (f < 0.0f) f = 0.0f;
            if (f > 1.0f) f = 1.0f;
            if (g_aim_drag == 0) g_cfg.aim_fov = 1 + (int)(f * 179.0f);
            else                 g_cfg.aim_smooth = 1 + (int)(f * 99.0f);
            cfg_apply();
        }
        if (!down && g_aim_drag >= 0) { g_aim_drag = -1; cfg_save(); }
    } else if (!down) {
        g_aim_drag = -1;
    }

    const DWORD now = GetTickCount();
    float dt = g_aim_last ? (float)(now - g_aim_last) : 16.0f;
    g_aim_last = now;
    if (dt > 100.0f) dt = 100.0f;
    const float step = dt / 120.0f;
    const int want[2] = { g_cfg.aim, g_cfg.nospread };
    for (int k = 0; k < 2; ++k) {
        const float w = want[k] ? 1.0f : 0.0f;
        if (g_aim_anim[k] < w) { g_aim_anim[k] += step; if (g_aim_anim[k] > w) g_aim_anim[k] = w; }
        else if (g_aim_anim[k] > w) { g_aim_anim[k] -= step; if (g_aim_anim[k] < w) g_aim_anim[k] = w; }
    }
}

static void aim_draw(float W, float H) {
    if (!g_d2d_ctx || !g_brush) return;
    const AIM_RECT s = aim_panel(W, H);
    D2D1_RECT_F bg = D2D1::RectF(s.x, s.y, s.x + s.w, s.y + s.h);
    g_brush->SetColor(col(0xE6101418, 1.0f));
    g_d2d_ctx->FillRectangle(bg, g_brush);
    g_brush->SetColor(col(0xFF2A3138, 1.0f));
    g_d2d_ctx->DrawRectangle(bg, g_brush, 1.0f);
    if (g_fmt_l) {
        D2D1_RECT_F hr = D2D1::RectF(s.x + 12.0f, s.y + 8.0f, s.x + s.w - 12.0f, s.y + 28.0f);
        text_in(L"AIM", hr, 0xFF6FE3A8, g_fmt_l);
    }
    aim_toggle_draw(aim_row(s, 0), L"Enabled", g_aim_anim[0]);

    /* bone selector */
    {
        const AIM_RECT r = aim_row(s, 1);
        if (g_fmt_l) {
            D2D1_RECT_F lr = D2D1::RectF(r.x, r.y + 3.0f, r.x + 120.0f, r.y + r.h - 3.0f);
            text_in(L"Target bone", lr, 0xFFC8D0D8, g_fmt_l);
        }
        int b = g_cfg.aim_bone;
        if (b < 0 || b >= RR_AIM_GROUP_COUNT) b = 0;
        if (g_fmt_r) {
            D2D1_RECT_F vr = D2D1::RectF(r.x + 120.0f, r.y + 3.0f, r.x + r.w, r.y + r.h - 3.0f);
            wchar_t buf[64];
            _snwprintf_s(buf, 64, _TRUNCATE, L"<  %s  >", bone_name[b]);
            text_in(buf, vr, 0xFF6FE3A8, g_fmt_r);
        }
    }

    aim_slider_draw(aim_row(s, 2), (float)(g_cfg.aim_fov - 1) / 179.0f,
                    0x39D67F, L"FOV", g_cfg.aim_fov, L"\u00B0");
    aim_slider_draw(aim_row(s, 3), (float)(g_cfg.aim_smooth - 1) / 99.0f,
                    0x39D67F, L"Smoothing", g_cfg.aim_smooth, L"%");
    aim_toggle_draw(aim_row(s, 4), L"No spread", g_aim_anim[1]);

    if (g_fmt_l) {
        D2D1_RECT_F fr = D2D1::RectF(s.x + 12.0f, s.y + s.h - 24.0f,
                                     s.x + s.w - 12.0f, s.y + s.h - 6.0f);
        text_in(L"Sensitivity is measured automatically", fr, 0xFF5A646E, g_fmt_l);
    }
}

static void draw_frame(IDXGISwapChain* swap) {
    static RR_PLAYER players[RR_MAX_PLAYERS];
    static int why_logged = 0;
    RR_CAMERA cam;
    int n, i, shown = 0;
    float W, H;

    if (InterlockedCompareExchange(&g_draw_on, 1, 1) == 0) {
        if (!why_logged) { why_logged = 1; ilog("draw: bailed, drawing switched off"); }
        return;
    }
    /* Polled before the D2D gate: the menu must answer F1 even on a frame where
     * the device is still being built. */
    menu_tick();
    if (!d2d_init(swap)) {
        if (!why_logged) { why_logged = 1; ilog("draw: bailed, d2d_init failed"); }
        return;
    }
    if (!g_target && !make_target()) {
        if (!why_logged) { why_logged = 1; ilog("draw: bailed, make_target failed"); }
        return;
    }
    if (!g_brush) {
        if (!why_logged) { why_logged = 1; ilog("draw: bailed, no brush"); }
        return;
    }

    {
        D2D1_SIZE_F size = g_d2d_ctx->GetSize();
        W = size.width;
        H = size.height;
    }
    if (W < 64 || H < 64) {
        if (!why_logged) { why_logged = 1; ilog("draw: bailed, size %.0fx%.0f", (double)W, (double)H); }
        return;
    }
    /* Hit testing needs the same surface the panel was painted on, and the
     * window the swapchain presents into is what maps a screen cursor
     * position into that surface. */
    g_vw = W;
    g_vh = H;
    if (!g_game_hwnd) {
        DXGI_SWAP_CHAIN_DESC sd;
        if (SUCCEEDED(swap->GetDesc(&sd))) g_game_hwnd = sd.OutputWindow;
    }

    if (g_game_hwnd) rr_gate_install(g_game_hwnd);

    n = game_snapshot(players, RR_MAX_PLAYERS, &cam);
    InterlockedExchange(&g_players_seen, n);

    g_d2d_ctx->BeginDraw();
    g_d2d_ctx->SetTransform(D2D1::Matrix3x2F::Identity());

    /* The mark lands before the camera gate: it is proof that the overlay owns a
     * working D2D target on this swapchain, and it stays visible on menus and
     * loading screens where there is nothing else to draw. */
    {
    }

    /* Aim and anti-aim run on the frame thread with the snapshot this frame
     * already owns: no second reader, no lock, and the camera and size are the
     * ones the projection used. Either feature alone needs the call - aim
     * gating would also silence the anti-aim, which lives in the same worker. */
    if ((g_cfg.aim || g_cfg.trigger || g_cfg.nospread || g_cfg.rage) &&
        cam.valid && n > 0) {
        rr_aim_update(players, n, &cam, (int)W, (int)H);
        /* Instant-lock diagnostics, throttled: what the loop picked, how far off
         * it was, what it asked SendInput for and what it has learned about this
         * game's sensitivity and axis directions. Six lines a second while the
         * aim key is held is enough to reconstruct a divergence. */
                /* Magnet telemetry, throttled: what it locked, the remaining angle,
         * what it asked SendInput for, and the sensitivity it has measured. */
                /* Magnet telemetry, throttled: the locked joint, the remaining angle,
         * what SendInput was asked for, and the sensitivity measured at runtime.
         * Six lines a second while the aim key is held. */
        static unsigned aim_log_at;
        RR_AIM_FRAME af;
        rr_aim_frame_stats(&af);
        const unsigned now_frame = (unsigned)g_frames;
        if (af.picked && (now_frame - aim_log_at) >= 10) {
            aim_log_at = now_frame;
            ilog("aim: id=%d bone=%d yaw=%.3f pitch=%.3f sent=%d,%d dpc=%.5f fov=%.1f",
                 (int)af.id, af.bone,
                 (double)af.yaw, (double)af.pitch,
                 af.sent_x, af.sent_y,
                 (double)af.dpc, (double)af.fov);
        }
        static int nospread_last = -1;
        const int nsw = rr_nospread_writes();
        if (nsw != nospread_last) {
            nospread_last = nsw;
            ilog("nospread: carriers zeroed=%d", nsw);
        }
    } else {
        rr_aim_update(NULL, 0, NULL, 0, 0);
    }

    if (!cam.valid) {
        if (g_menu_open) {
            draw_menu(W, H, shown);
            ign_tick(W, H); ign_draw(W, H);
            aim_tick(W, H); aim_draw(W, H);
        }
        g_d2d_ctx->EndDraw();
        return;
    }

    /* Marker pass first so player boxes read on top of loot dots, and so the
     * interact proximity flag is refreshed on every frame the overlay draws. */
    if (g_cfg.loot || g_cfg.loot_nodes || g_cfg.containers || g_cfg.doors ||
        g_cfg.dropped || g_cfg.vehicle_loot) {
        draw_loot_markers(players, n, &cam, (int)W, (int)H);
    } else {
        g_interact_near = 0;
    }

    /* Player pass. Every body gets a role-coloured box, a health bar with the
     * replicated value clamped to its own scale, an armour bar when the flag is
     * set, a head dot, the UTF-8 nickname and one meta line carrying the role,
     * the weapon and the distance. Skipped: corpses, loot, vehicles, the local
     * body, anything flagged inside a container, and every role the shared
     * category switches have turned off. */
    {
        BR_Config* br = br_get();
        const float maxm = g_cfg.max_dist / 100.0f;

        for (i = 0; i < n && g_cfg.esp; ++i) {
            RR_PLAYER* p = &players[i];
            float hx, hy, fx, fy, boxw, boxh;
            UINT32 rgb;
            const char* role_label;

            if (!p->valid || p->dead || p->is_loot || p->is_vehicle || p->local) continue;
            if (p->hidden_in_object == 1) continue;
            if (!isfinite(p->dist) || p->dist > maxm) continue;
            if (!rr_category_allowed(p->role, br->show_cops, br->show_robbers,
                                     br->show_civilians, br->show_prisoners,
                                     br->show_npcs)) continue;

            if (!game_w2s(&cam, &p->head, (int)W, (int)H, &hx, &hy)) continue;
            if (!game_w2s(&cam, &p->pos, (int)W, (int)H, &fx, &fy)) continue;
            boxh = fy - hy;
            /* A box shorter than its border is noise, and a box wider than a
             * third of the screen means the camera is inside the body. */
            if (!isfinite(boxh) || boxh < 7.0f) continue;
            if (hx < -50.0f || hx > W + 50.0f) continue;
            boxw = boxh * 0.42f;
            if (boxw > W * 0.30f) continue;
            if (boxw < 5.0f) boxw = 5.0f;

            ++shown;
            rgb = color_for(p);
            if (g_cfg.boxes) {
                D2D1_RECT_F r;
                r.left = hx - boxw * 0.5f;
                r.top = hy;
                r.right = hx + boxw * 0.5f;
                r.bottom = fy;
                g_brush->SetColor(col(rgb, 1.0f));
                g_d2d_ctx->DrawRectangle(r, g_brush, 1.6f);
            }
            if (g_cfg.skeleton && p->nbones && rr_esp_labels_visible(p->dist, br->skel_distance))
                draw_skeleton(p, &cam, (int)W, (int)H, rr_skeleton_fade(rgb, p->dist));

            if (g_cfg.health && p->health >= 0) {
                const float frac = rr_health_fraction(p->health);
                D2D1_RECT_F bar;
                bar.left = hx - boxw * 0.5f - 6.0f;
                bar.right = bar.left + 3.5f;
                bar.top = hy;
                bar.bottom = fy;
                g_brush->SetColor(col(0x70000000, 1.0f));
                g_d2d_ctx->FillRectangle(bar, g_brush);
                g_brush->SetColor(col(rr_health_color(frac) | 0xFF000000u, 1.0f));
                bar.top = fy - boxh * frac;
                g_d2d_ctx->FillRectangle(bar, g_brush);
                if (p->armor > 0) {
                    bar.left = bar.right + 1.5f;
                    bar.right = bar.left + 3.0f;
                    bar.top = hy;
                    g_brush->SetColor(col(0xFF5AA0FF, 0.85f));
                    g_d2d_ctx->FillRectangle(bar, g_brush);
                }
            }

            g_brush->SetColor(col(rgb, 0.9f));
            g_d2d_ctx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(hx, hy), 2.4f, 2.4f), g_brush);

            if (g_cfg.names && p->name[0]) {
                wchar_t wname[RR_NAME_MAX];
                narrow_to_wide(p->name, wname, RR_NAME_MAX);
                if (wname[0])
                    hud_push((p->addr << 1), wname, hx, hy - 15.0f, 260.0f,
                             floorf(p->dist) * 2.0f, rgb);
            }

            role_label = rr_esp_role_label(p->role);
            if (g_cfg.distance || role_label || p->weapon[0]) {
                wchar_t meta[192], wrole[24], wweap[64];
                const wchar_t* gap;
                wrole[0] = wweap[0] = 0;
                if (role_label) narrow_to_wide(role_label, wrole, 24);
                if (p->weapon[0]) {
                    char wbuf[96];
                    rr_format_weapon_label(p->weapon, 1, wbuf, sizeof wbuf);
                    narrow_to_wide(wbuf, wweap, 64);
                }
                gap = (wrole[0] && wweap[0]) ? L"  " : L"";
                if (g_cfg.distance && (wrole[0] || wweap[0]))
                    _snwprintf_s(meta, 192, _TRUNCATE, L"%s%s%s  %.0fm",
                                 wrole, gap, wweap, (double)p->dist);
                else if (g_cfg.distance)
                    _snwprintf_s(meta, 192, _TRUNCATE, L"%.0fm", (double)p->dist);
                else
                    _snwprintf_s(meta, 192, _TRUNCATE, L"%s%s%s", wrole, gap, wweap);
                if (meta[0])
                    hud_push((p->addr << 1) | 1u, meta, hx, fy + 2.0f, 300.0f,
                             floorf(p->dist) * 2.0f + 1.0f, 0xFFD8DEE9);
            }
        }
    }

    hud_place_and_draw(players, n, W, H);

    if (g_cfg.radar) draw_radar(players, n, &cam, W, H);

    if (g_menu_open) {
        draw_menu(W, H, shown);
        ign_tick(W, H);
        ign_draw(W, H);
        aim_tick(W, H);
        aim_draw(W, H);
    }

    g_d2d_ctx->EndDraw();
}

/* ---- hooks --------------------------------------------------------------- */

/* Per-frame upkeep, defined with the attach path below. */
static void maint_tick(void);

static void on_present(IDXGISwapChain* swap) {
    __try {
        g_swap = swap;
        draw_frame(swap);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ilog("draw: exception 0x%08lX, overlay disabled", GetExceptionCode());
        InterlockedExchange(&g_draw_on, 0);
    }
    /* Counted here, not in draw_frame: the heartbeat must show that Present is
     * firing even when the overlay bails for a target it cannot draw on. */
    InterlockedIncrement(&g_frames);
    maint_tick();

}

/* Unity's D3D11 backend creates a flip-model swapchain (DXGI_SWAP_EFFECT_FLIP_*),
 * which it presents through IDXGISwapChain1::Present1, not the base Present. Both
 * slots point into dxgi and both are patched; whichever one the game calls lands
 * in the same draw path. */
static HRESULT STDMETHODCALLTYPE present_hook(IDXGISwapChain* swap, UINT sync, UINT flags) {
    on_present(swap);
    return g_present_orig(swap, sync, flags);
}

static HRESULT STDMETHODCALLTYPE present1_hook(IDXGISwapChain* swap, UINT sync, UINT flags,
                                               const DXGI_PRESENT_PARAMETERS* params) {
    on_present(swap);
    return g_present1_orig(swap, sync, flags, params);
}

static HRESULT STDMETHODCALLTYPE resize_hook(IDXGISwapChain* swap, UINT count, UINT w,
                                             UINT h, DXGI_FORMAT fmt, UINT flags) {
    drop_target();
    {
        HRESULT hr = g_resize_orig(swap, count, w, h, fmt, flags);
        make_target();
        return hr;
    }
}

static int in_module(const void* p, HMODULE mod) {
    const unsigned char* b;
    const IMAGE_DOS_HEADER* dos;
    const IMAGE_NT_HEADERS* nt;

    if (!mod || !p) return 0;
    b = (const unsigned char*)mod;
    dos = (const IMAGE_DOS_HEADER*)b;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    nt = (const IMAGE_NT_HEADERS*)(b + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    return (const unsigned char*)p >= b && (const unsigned char*)p < b + nt->OptionalHeader.SizeOfImage;
}

/* Unity caches the swapchain's method pointers when it creates the device, so a
 * straight vtable swap can miss every frame. Walk the process once and redirect
 * every aligned pointer that still equals the real Present/Present1 into the
 * hooks. Our own image is skipped so the pass-through pointers stay original.
 *
 * The scan reads through ReadProcessMemory, never through a dereference. Regions
 * are decommitted under a live game: one that vanishes between the VirtualQuery
 * and the scan faults, and a fault raised inside a hand-mapped image used to
 * dispatch nowhere at all (rr_mmap.c), which cost the whole game process. RPM
 * reports the same event as a short read, and nothing on this path can fault. */
#define SWEEP_CHUNK (256u * 1024u)

static int sweep_present_refs(int verbose) {
    static unsigned char* g_sweep_buf;
    const unsigned char* self = (const unsigned char*)g_hinst;
    unsigned char* cursor = NULL;
    MEMORY_BASIC_INFORMATION mbi;
    uint64_t want[2];
    void* repl[2];
    int patched = 0;

    want[0] = (uint64_t)(uintptr_t)g_present_orig;
    want[1] = (uint64_t)(uintptr_t)g_present1_orig;
    repl[0] = (void*)present_hook;
    repl[1] = (void*)present1_hook;

    if (!g_sweep_buf)
        g_sweep_buf = (unsigned char*)VirtualAlloc(NULL, SWEEP_CHUNK,
                                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_sweep_buf) {
        ilog("sweep: scan buffer refused (%lu)", GetLastError());
        return 0;
    }

    while (VirtualQuery(cursor, &mbi, sizeof mbi) == sizeof mbi) {
        unsigned char* base = (unsigned char*)mbi.BaseAddress;
        unsigned char* next = base + mbi.RegionSize;
        size_t off;

        if (next <= base) break;
        if (mbi.State == MEM_COMMIT &&
            (mbi.Type == MEM_IMAGE || mbi.Type == MEM_PRIVATE) &&
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY)) &&
            !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            !(self && base <= self && self < next)) {
            for (off = 0; off + sizeof(uint64_t) <= mbi.RegionSize; off += SWEEP_CHUNK) {
                size_t chunk = mbi.RegionSize - off;
                size_t got = 0;
                size_t i, n;

                if (chunk > SWEEP_CHUNK) chunk = SWEEP_CHUNK;
                if (!ReadProcessMemory(GetCurrentProcess(), base + off, g_sweep_buf, chunk,
                                       &got) &&
                    !got) {
                    if (!off) ilog("sweep: region %p unreadable, skipped", (void*)base);
                    break;
                }
                n = got / sizeof(uint64_t);
                for (i = 0; i < n; ++i) {
                    uint64_t cand = ((const uint64_t*)g_sweep_buf)[i];
                    uint64_t* q;
                    int k;

                    if (!cand) continue;
                    q = (uint64_t*)(base + off + i * sizeof(uint64_t));
                    for (k = 0; k < 2; ++k) {
                        DWORD oldp = 0;
                        size_t wrote = 0;
                        uint64_t val;

                        if (!want[k] || cand != want[k]) continue;
                        if (!VirtualProtect(q, sizeof(uint64_t), PAGE_READWRITE, &oldp)) continue;
                        val = (uint64_t)(uintptr_t)repl[k];
                        if (WriteProcessMemory(GetCurrentProcess(), q, &val, sizeof val, &wrote) &&
                            wrote == sizeof val)
                            ++patched;
                        VirtualProtect(q, sizeof(uint64_t), oldp, &oldp);
                    }
                }
                if (got < chunk) break;
            }
        }
        cursor = next;
    }
    if (verbose) ilog("sweep: %d Present/Present1 pointers redirected", patched);
    return patched;
}

static int hook_install(void) {
    WNDCLASSEXW wc;
    HWND hwnd;
    DXGI_SWAP_CHAIN_DESC sd;
    IDXGISwapChain* swap = NULL;
    ID3D11Device* dev = NULL;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr;
    DWORD oldp = 0;

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"rr_int_sink";
    RegisterClassExW(&wc);
    hwnd = CreateWindowExW(0, L"rr_int_sink", L"rr_int_sink", WS_POPUP,
                           0, 0, 8, 8, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) { ilog("hook: sink window failed (%lu)", GetLastError()); return 0; }

    memset(&sd, 0, sizeof sd);
    sd.BufferDesc.Width = 8;
    sd.BufferDesc.Height = 8;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 1;
    sd.OutputWindow = hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
                                       D3D11_SDK_VERSION, &sd, &swap, &dev, &fl, NULL);
    if (FAILED(hr) || !swap) {
        ilog("hook: dummy swapchain failed hr=0x%08lX", (unsigned)hr);
        DestroyWindow(hwnd);
        return 0;
    }

    g_vt = *(void***)swap;
    g_present_orig = (PresentFn)g_vt[8];
    g_present1_orig = (Present1Fn)g_vt[22];
    g_resize_orig = (ResizeFn)g_vt[13];
    {
        HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
        int has_present1 = in_module((const void*)g_present1_orig, dxgi);
        if (VirtualProtect(g_vt, 0x200, PAGE_EXECUTE_READWRITE, &oldp)) {
            g_vt[8] = (void*)present_hook;
            g_vt[13] = (void*)resize_hook;
            if (has_present1) g_vt[22] = (void*)present1_hook;
            VirtualProtect(g_vt, 0x200, oldp, &oldp);
        } else {
            ilog("hook: VirtualProtect on the swapchain vtable failed (%lu)", GetLastError());
            swap->Release();
            dev->Release();
            DestroyWindow(hwnd);
            return 0;
        }
        ilog("hook: Present %p => %p, Present1 %p => %p%s (vtable %p)",
             (void*)g_present_orig, (void*)present_hook, (void*)g_present1_orig,
             (void*)present1_hook, has_present1 ? "" : " [skipped: not in dxgi]", (void*)g_vt);
    }

    swap->Release();
    dev->Release();
    DestroyWindow(hwnd);
    return 1;
}

/* ---- worker -------------------------------------------------------------- */

/* A thread the loader did not create gets no DLL_THREAD_ATTACH, so the CRT has
 * no per-thread data for it and the first stdio call faults. The CRT exposes the
 * two halves of that notification directly; calling them is what the loader's
 * thread attach would have done, without the extra entry-point work. */
static void raw_mark(const char* what);

extern "C" int __vcrt_thread_attach(void);
extern "C" int __acrt_thread_attach(void);

static void crt_thread_attach(HINSTANCE mod) {
    (void)mod;
    raw_mark("crt: vcrt ->");
    __vcrt_thread_attach();
    raw_mark("crt: vcrt ok");
    __acrt_thread_attach();
    raw_mark("crt: ucrt ok");
}

/* Progress markers that do not touch the CRT, for the window where the CRT has
 * no per-thread data yet: a missing marker then means the line above it never
 * ran, and the line above it cannot be the logger's fault. */
static void raw_mark(const char* what) {
    HANDLE h = CreateFileA("C:\\Users\\Public\\rr_int.raw", FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD wrote = 0;
    size_t n = strlen(what);
    if (h == INVALID_HANDLE_VALUE) return;
    WriteFile(h, what, (DWORD)n, &wrote, NULL);
    WriteFile(h, "\r\n", 2, &wrote, NULL);
    CloseHandle(h);
}

/* The CRT answers a bad argument with __fastfail, which leaves nothing behind:
 * no exception, no log, just a dead process. The handler runs first, so it turns
 * that silence into a line naming the failing call. */
static void invalid_parameter_handler(const wchar_t* expr, const wchar_t* fn,
                                      const wchar_t* file, unsigned line,
                                      uintptr_t reserved) {
    char msg[1024];
    char e[192] = "", f[192] = "", fl[192] = "";
    (void)reserved;
    if (expr) WideCharToMultiByte(CP_ACP, 0, expr, -1, e, sizeof e, NULL, NULL);
    if (fn)   WideCharToMultiByte(CP_ACP, 0, fn, -1, f, sizeof f, NULL, NULL);
    if (file) WideCharToMultiByte(CP_ACP, 0, file, -1, fl, sizeof fl, NULL, NULL);
    _snprintf_s(msg, sizeof msg, _TRUNCATE,
                "!!! invalid parameter: expr='%s' fn='%s' at %s:%u", e, f, fl, line);
    raw_mark(msg);
}

/* Everything the payload needs at attach time, on the thread the mapper called
 * DllMain from. The anti-cheat terminates the process the moment a thread starts
 * inside the protected game - a DLL that only calls CreateThread is killed with
 * the new thread still unstarted, while the same DLL run inline survives - so
 * this image owns no thread. The per-frame work runs from the Present hook, on
 * the game's own render thread, which costs nothing extra: draw_frame already
 * runs there and the snapshot it draws is the state the loop used to poll. */
static int   g_hooked = 0;
static int   g_beat = 0;
static int   g_toggle_edge = 0;
static int   g_game_inited = 0;
static DWORD g_last_key_poll = 0;
static DWORD g_last_beat = 0;
static DWORD g_last_auto = 0;
static int   g_auto_ok = 0;
static int   g_auto_skip = 0;
static int   g_interact_down = 0;
static DWORD g_interact_at = 0;

/* Attach work. Runs once, inline: the reader layer attaches, the driver device
 * opens, the config lands, the swapchain is hooked. All of it is short - the
 * mapper is still waiting on this call - and the discovery bootstrap, which is
 * not, waits for the first frame. */
static void init_once(void) {
    int i;

    raw_mark("init: entered");
    ilog("==== rr_int attached pid=%u image=%p ====",
         (unsigned)GetCurrentProcessId(), (void*)GetModuleHandleW(NULL));
    /* Every worker in the reader layer runs on this thread from here on. */
    rr_set_no_threads(1);
    rr_force_pid(GetCurrentProcessId());
    rr_attach("ROB RUN.exe");
    raw_mark("init: rr_attach done");
    /* The reader layer opens the device lazily, but nothing in this module calls
     * a read before game_init decides where to look: open it once here so the
     * first discovery pass runs on the kernel transport instead of falling
     * through to a user-mode handle the anti-cheat may refuse. */
    rr_drv_open();
    ilog("transport: %s pid=%u", rr_transport(), (unsigned)g_rr_pid);
    cfg_load();
    raw_mark("init: config loaded");
    ilog("cfg: esp=%d boxes=%d names=%d skel=%d health=%d dist=%d range=%d "
         "aim=%d aimkey=%d fov=%d smooth=%d bone=%d aimdist=%d trig=%d recoil=%d "
         "aa=%d aamode=%d aaspd=%d aarange=%d aakey=%d "
         "menu_key=%d insert_key=%d",
         g_cfg.esp, g_cfg.boxes, g_cfg.names, g_cfg.skeleton, g_cfg.health,
         g_cfg.distance, g_cfg.max_dist, g_cfg.aim, g_cfg.aim_key, g_cfg.aim_fov,
         g_cfg.aim_smooth, g_cfg.aim_bone, g_cfg.aim_dist, g_cfg.trigger,
         g_cfg.norecoil, g_cfg.aa, g_cfg.aa_mode, g_cfg.aa_speed, g_cfg.aa_range,
         g_cfg.aa_key, g_cfg.menu_key, g_cfg.insert_key);
    ilog("cfg: radar=%d range=%d radarloot=%d | loot=%d nodes=%d cont=%d doors=%d "
         "drop=%d veh=%d price=%d | autoloot=%d lootrange=%d key=%d",
         g_cfg.radar, g_cfg.radar_range, g_cfg.radar_loot, g_cfg.loot,
         g_cfg.loot_nodes, g_cfg.containers, g_cfg.doors, g_cfg.dropped,
         g_cfg.vehicle_loot, g_cfg.prices, g_cfg.autoloot,
         g_cfg.autoloot_range, g_cfg.autoloot_key);
    /* Hooking first: game_init walks the heap for seconds, and nothing draws
     * until Present is ours. */
    for (i = 0; i < 20 && !g_hooked; ++i) {
        g_hooked = hook_install();
        if (!g_hooked) Sleep(100);
    }
    if (!g_hooked) ilog("hook: could not install; module stays resident without drawing");
    else sweep_present_refs(1);
    raw_mark(g_hooked ? "init: hooked" : "init: no hook");

    g_last_beat = GetTickCount();
    g_last_key_poll = g_last_beat;
}

/* Called from on_present, once per presented frame: the old loop body, with the
 * 20 ms and 500 ms cadences kept by the clock instead of by Sleep. */
static void maint_tick(void) {
    DWORD now;

    /* First frame: the discovery bootstrap. It belongs here rather than in
     * DllMain because it can hold the caller for seconds, while the mapper waits
     * for the attach call to return. */
    if (!g_game_inited) {
        int init;
        g_game_inited = 1;
        init = game_init();
        raw_mark("init: game_init done");
        ilog("game_init=%d transport=%s", init, rr_transport());
    }
    /* One discovery pass; the pass keeps its own 500/4000 ms cadence. The cost
     * is logged because a pass that runs long here stalls the frame it runs in. */
    {
        static DWORD last_report;
        DWORD t0 = GetTickCount();
        game_pump();
        t0 = GetTickCount() - t0;
        if (t0 >= 100 && GetTickCount() - last_report >= 3000) {
            last_report = GetTickCount();
            ilog("pump: %lu ms", (unsigned long)t0);
        }
    }

    now = GetTickCount();
    rr_gate_set_open(g_menu_open);
    rr_gate_pump();
    if (now - g_last_key_poll >= 20) {
        int key;
        g_last_key_poll = now;
        /* The master switch is a tap: GetAsyncKeyState's bit 0 is set for one
         * poll only, so it is read at most every 20 ms and the edge is tracked. */
        key = GetAsyncKeyState(g_cfg.insert_key) & 1;
        if (key && !g_toggle_edge) {
            LONG v = InterlockedCompareExchange(&g_draw_on, 1, 1) ? 0 : 1;
            InterlockedExchange(&g_draw_on, v);
            ilog("hotkey: drawing %s", v ? "on" : "off");
        }
        g_toggle_edge = key;
    }

    /* Auto-loot. The write pass keeps its own 300 ms clock so it never shares a
     * frame with a discovery pass, and it only writes what is already inside
     * loot range. The interact hold is the second half: containers and bodies
     * are opened by the game itself, so the key that would open them is held
     * while one is in reach and released as soon as the frame stops reporting
     * one. */
    if (g_cfg.autoloot && !g_menu_open) {
        if (now - g_last_auto >= 300) {
            int aok = 0, askip = 0;
            g_last_auto = now;
            rr_loot_auto(0, 1, g_cfg.autoloot_range * 100, &aok, &askip);
            g_auto_ok += aok;
            g_auto_skip += askip;
        }
        if (g_cfg.autoloot_key && g_interact_near) {
            if (!g_interact_down) {
                keybd_event((BYTE)g_cfg.autoloot_key, 0, 0, 0);
                g_interact_down = 1;
                g_interact_at = now;
            } else if (now - g_interact_at >= 400) {
                keybd_event((BYTE)g_cfg.autoloot_key, 0, KEYEVENTF_KEYUP, 0);
                g_interact_down = 0;
                g_interact_at = now;
            }
        } else if (g_interact_down && now - g_interact_at >= 60) {
            keybd_event((BYTE)g_cfg.autoloot_key, 0, KEYEVENTF_KEYUP, 0);
            g_interact_down = 0;
        }
    } else if (g_interact_down) {
        keybd_event((BYTE)g_cfg.autoloot_key, 0, KEYEVENTF_KEYUP, 0);
        g_interact_down = 0;
    }

    if (now - g_last_beat < 500) return;
    g_last_beat = now;
    ++g_beat;
    /* Present may stay quiet for a while when the process caches the method
     * pointers later than the first sweep, so retry a couple of times until
     * frames start moving. */
    if (g_hooked && !g_frames && g_beat > 0 && g_beat < 12 && (g_beat % 4) == 0)
        sweep_present_refs(0);
    if ((g_beat % 10) == 0) {
        char status[256];
        game_status(status, sizeof status);
        ilog("beat frames=%ld players=%ld draw=%ld | %s",
             (long)g_frames, (long)g_players_seen,
             (long)InterlockedCompareExchange(&g_draw_on, 1, 1), status);
        if (g_cfg.autoloot)
            ilog("loot: auto ok=%d skip=%d near=%d range=%dm",
                 g_auto_ok, g_auto_skip, g_interact_near, g_cfg.autoloot_range);
        g_auto_ok = 0;
        g_auto_skip = 0;
    }
}

extern "C" __declspec(dllexport) BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        raw_mark("dll: entered");
        DisableThreadLibraryCalls(mod);
        InitializeCriticalSection(&g_log_cs);
        g_log_ready = 1;
        _set_invalid_parameter_handler(invalid_parameter_handler);
        raw_mark("dll: cs ready");
        g_hinst = mod;
        raw_mark("dll: td init ->");
        rr_td_init();
        rr_mem_ok_provider(rr_td_ok);
        raw_mark("dll: td ready");
        ilog("rr_int: DllMain DLL_PROCESS_ATTACH base=%p", (void*)mod);
        raw_mark("dll: logged");
        init_once();

    } else if (reason == DLL_PROCESS_DETACH) {
        rr_gate_remove();
    }
    return TRUE;
}
