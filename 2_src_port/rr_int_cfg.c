/* rr_int_cfg.c - the settings object the reader layer expects.
 *
 * rr_game.c is written for the external overlay: its scanning gates read
 * br_get() (menu_bridge.h) for toggles such as show_loot and show_skeleton.
 * Inside the game there is no menu process, so this file provides the same
 * object with the schema defaults and publishes it through br_get().
 *
 * The internal payload owns the values it cares about (max distance, skeleton
 * on/off) and pushes them here; everything the internal build does not draw
 * stays off so the reader layer does not spend time scanning loot, vehicles or
 * containers on the game's own threads.
 */
#include "menu_bridge.h"

#include <string.h>

static BR_Config g_cfg;
static int g_cfg_init = 0;

static void cfg_defaults(void) {
    memset(&g_cfg, 0, sizeof g_cfg);
    g_cfg.magic = BR_MAGIC;
    g_cfg.version = BR_VERSION;
#define BR_INIT(field, initial, minimum, maximum) g_cfg.field = (initial);
    BR_SETTINGS(BR_INIT)
#undef BR_INIT

    /* Internal build: only what the in-process renderer actually uses. */
    g_cfg.enabled = 1;
    g_cfg.show_box = 1;
    g_cfg.show_name = 1;
    g_cfg.show_distance = 1;
    g_cfg.show_health = 1;
    g_cfg.show_skeleton = 0;
    g_cfg.cham_on = 0;
    g_cfg.aim_enabled = 0;
    /* The collision capture modules are linked into this payload and pumped
     * from Present, so the visible-only filter can be on from the start. */
    g_cfg.aa_los_on = 1;
    g_cfg.trigger_los = 1;
    g_cfg.show_self = 0;
    g_cfg.show_loot = 0;
    g_cfg.show_loot_nodes = 0;
    g_cfg.show_containers = 0;
    g_cfg.show_doors = 0;
    g_cfg.show_dropped_loot = 0;
    g_cfg.show_vehicle_loot = 0;
    g_cfg.show_vehicle_markers = 0;
    g_cfg.vehicle_chams = 0;
    g_cfg.radar_on = 0;
    g_cfg.max_distance = 30000;
    g_cfg.esp_text_distance = 30000;
    g_cfg.skel_distance = 15000;
    g_cfg_init = 1;
}

BR_Config* br_get(void) {
    if (!g_cfg_init) cfg_defaults();
    return &g_cfg;
}

int br_config_set(BR_Config* config, const char* key, int value) {
    (void)config;
    (void)key;
    (void)value;
    return 1;
}

int br_config_save(void) { return 1; }
int br_config_load(void) { return 1; }

void br_request_toggle(void) {}
void br_set_menu_visible(int visible) { (void)visible; }
