/* menu_bridge.h - Shared integer settings, limits, colors and menu telemetry.
 * BR_SETTINGS is the schema used for declarations, defaults and validation.
 */
#ifndef MENU_BRIDGE_H
#define MENU_BRIDGE_H
#ifdef __cplusplus
extern "C" {
#endif

#define BR_MAGIC 0x57444D31u
#define BR_VERSION 27u
#define BR_COLOR_COUNT 15

/* Shared schema for the WebView, defaults and on-disk settings.
 * Each entry is a field, default, minimum and maximum.
 * Every line MUST end with a backslash: without continuations the macro
 * expands to nothing and BR_Config loses all setting fields. */
#define BR_SETTINGS(X) \
    X(enabled, 1, 0, 1) \
    X(show_box, 1, 0, 1) \
    X(show_skeleton, 1, 0, 1) \
    X(show_name, 1, 0, 1) \
    X(show_armor, 1, 0, 1) \
    X(show_distance, 1, 0, 1) \
    X(show_weapon, 1, 0, 1) \
    X(show_head, 1, 0, 1) \
    X(show_self, 0, 0, 1) \
    X(show_clan, 1, 0, 1) \
    X(show_party, 1, 0, 1) \
    X(show_police, 1, 0, 1) \
    X(show_cops, 1, 0, 1) \
    X(show_robbers, 1, 0, 1) \
    X(show_civilians, 1, 0, 1) \
    X(show_prisoners, 1, 0, 1) \
    X(show_npcs, 1, 0, 1) \
    X(esp_weapon_icon, 1, 0, 1) \
    X(esp_weapon_mk, 1, 0, 1) \
    X(esp_role_label, 1, 0, 1) \
    X(esp_text_distance, 15000, 0, 200000) \
    X(cham_on, 0, 0, 1) \
    X(max_distance, 30000, 0, 200000) \
    X(skel_distance, 15000, 0, 200000) \
    X(line_w, 2, 1, 4) \
    X(radar_on, 1, 0, 1) \
    X(radar_range, 150, 25, 500) \
    X(aim_enabled, 0, 0, 1) \
    X(rage, 0, 0, 1) \
    X(aa_show_fov, 1, 0, 1) \
    X(aim_fov, 100, 10, 300) \
    X(aim_key, 0x12, 1, 254) \
    X(aim_smooth, 30, 1, 100) \
    X(aa_aim_cal, 100, 10, 2000) \
    X(aa_dist, 0, 0, 1000) \
    X(aim_targets, 0, 0, 7) \
    X(trigger_enabled, 0, 0, 1) \
    X(trigger_key, 0, 0, 254) \
    X(trigger_delay, 80, 0, 300) \
    X(trigger_targets, 0, 0, 7) \
    X(show_spread_visualizer, 0, 0, 1) \
    X(aa_bone, 0, 0, 9) \
    X(aim_humanize, 0, 0, 1) \
    X(aim_humanize_strength, 35, 0, 100) \
    X(aim_humanize_speed, 50, 1, 100) \
    X(aim_bone_random, 0, 0, 1) \
    X(aim_bone_interval, 350, 50, 2000) \
    X(aa_los_on, 1, 0, 1) \
    X(aa_closest_first, 0, 0, 1) \
    X(aim_require_fire, 0, 0, 1) \
    X(trigger_head_only, 0, 0, 1) \
    X(trigger_fov, 10, 1, 80) \
    X(trigger_los, 1, 0, 1) \
    X(nospread, 1, 0, 1) \
    X(norecoil_enabled, 0, 0, 1) \
    X(norecoil_mode, 0, 0, 2) \
    X(norecoil_strength, 100, 0, 200) \
    X(norecoil_horizontal, 25, 0, 100) \
    X(antiaim_enabled, 0, 0, 1) \
    X(antiaim_mode, 0, 0, 3) \
    X(antiaim_speed, 50, 1, 100) \
    X(antiaim_range, 6, 1, 60) \
    X(antiaim_key, 0, 0, 254) \
    X(antiaim_model, 0, 0, 1) \
    X(antiaim_model_mode, 0, 0, 3) \
    X(antiaim_model_speed, 60, 1, 100) \
    X(antiaim_model_range, 90, 1, 180) \
    X(antiaim_flick, 0, 0, 1) \
    X(show_loot, 0, 0, 1) \
    X(show_loot_price, 1, 0, 1) \
    X(loot_price_source, 0, 0, 2) \
    X(show_loot_nodes, 0, 0, 1) \
    X(loot_tier_none, 1, 0, 1) \
    X(loot_tier_common, 1, 0, 1) \
    X(loot_tier_uncommon, 1, 0, 1) \
    X(loot_tier_rare, 1, 0, 1) \
    X(loot_tier_epic, 1, 0, 1) \
    X(loot_tier_legendary, 1, 0, 1) \
    X(loot_tier_exotic, 1, 0, 1) \
    X(vehicle_chams, 0, 0, 1) \
    X(show_containers, 0, 0, 1) \
    X(show_doors, 0, 0, 1) \
    X(show_dropped_loot, 0, 0, 1) \
    X(show_vehicle_loot, 0, 0, 1) \
    X(radar_world, 0, 0, 1) \
    X(show_crouch, 0, 0, 1) \
    X(show_health, 0, 0, 1) \
    X(show_vehicle_markers, 0, 0, 1) \
    X(show_vehicle_occupants, 0, 0, 1) \
    X(show_vehicle_owner, 1, 0, 1) \
    X(aim_ignore_dead, 1, 0, 1) \
    X(aim_ignore_prone, 0, 0, 1) \
    X(aim_ignore_party, 1, 0, 1) \
    X(aim_ignore_clan, 1, 0, 1) \
    X(aim_ignore_civilian, 0, 0, 1) \
    X(aim_legit, 1, 0, 1)

typedef struct BR_Config {
    unsigned int magic, version;
#define BR_DECLARE(field, initial, minimum, maximum) int field;
    BR_SETTINGS(BR_DECLARE)
#undef BR_DECLARE
    struct { unsigned char rgb[3]; } color[BR_COLOR_COUNT];
    int menu_open;
    int telem_targets;
    int telem_game_pid;
    int telem_cham_n;
    int telem_los_state; /* 0 off, 1 loading/stale, 2 ready */
#ifdef RR_PORT_DIAGNOSTICS
    int show_vehicles;
#endif
} BR_Config;

/* Unrecognized values use the same no-rarity bucket as the loot renderer. */
static int br_loot_rarity_visible(const BR_Config* config, int rarity) {
    switch (rarity) {
    case 1: return config->loot_tier_common;
    case 2: return config->loot_tier_uncommon;
    case 3: return config->loot_tier_rare;
    case 4: return config->loot_tier_epic;
    case 5: return config->loot_tier_legendary;
    case 6: return config->loot_tier_exotic;
    default: return config->loot_tier_none;
    }
}

BR_Config* br_get(void);
int br_config_set(BR_Config* config, const char* key, int value);
int br_config_save(void);
int br_config_load(void);
int br_start_menu(void);
void br_stop_menu(void);
void br_set_menu_visible(int visible);
void br_request_toggle(void);

#ifdef __cplusplus
}
#endif
#endif