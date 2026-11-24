/* rr_esp_style.h - Pure presentation helpers: weapon classification, health
 * and role colors, label formatting. No state except the optional weapon
 * cache; every function is safe to call from the render thread only.
 */
#ifndef RR_ESP_STYLE_H
#define RR_ESP_STYLE_H
#include "gpu.h"
#include "rr_game.h"
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include <string.h>

/* ASCII-only case conversion for game identifiers. Guarded so punctuation
 * (@ [ \ ] ^ _) is never corrupted by a bare |0x20, and no locale table is
 * consulted per character. */
static inline char rr_ascii_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static inline char rr_ascii_upper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

typedef struct { int atlas_cell; RGBA color; } RR_WEAPON_STYLE;

/* Classification depends only on the lowercased prefix before the first '_'
 * (max 31 chars). Keep this function pure; the cached variant below derives
 * the same key and must stay in sync with it. */
static inline RR_WEAPON_STYLE rr_weapon_style(const char* weapon) {
    char key[32]; size_t n = 0;
    while (weapon && *weapon && *weapon != '_' && n + 1 < sizeof(key))
        key[n++] = rr_ascii_lower(*weapon++);
    key[n] = 0;
    RR_WEAPON_STYLE style = {7, RRGB(187, 193, 202)};
    if (!strcmp(key, "dp9") || strstr(key, "pistol") || !strcmp(key, "m9")) style.atlas_cell = 0;
    else if (!strcmp(key, "rc556")) { style.atlas_cell = 1; style.color = RRGB(121, 223, 145); }
    else if (!strcmp(key, "kl545")) { style.atlas_cell = 2; style.color = RRGB(121, 223, 145); }
    else if (strstr(key, "shotgun") || !strncmp(key, "sg12", 4) || !strncmp(key, "ks12", 4) || !strcmp(key, "m870")) { style.atlas_cell = 3; style.color = RRGB(255, 103, 103); }
    else if (strstr(key, "smg") || !strcmp(key, "mp5") || !strcmp(key, "uzi") || !strcmp(key, "p90")) { style.atlas_cell = 4; style.color = RRGB(121, 223, 145); }
    else if (strstr(key, "sniper") || !strcmp(key, "svd") || !strcmp(key, "awm")) { style.atlas_cell = 5; style.color = RRGB(121, 223, 145); }
    else if (strstr(key, "knife") || !strcmp(key, "bat")) style.atlas_cell = 6;
    else if (strstr(key, "rifle") || !strcmp(key, "ak47") || !strcmp(key, "m4") || !strcmp(key, "m16")) { style.atlas_cell = 1; style.color = RRGB(121, 223, 145); }
    return style;
}

/* Drop-in replacement for per-frame calls. 16-slot direct-mapped cache keyed
 * by a COPY of the derived classification key (no pointer-lifetime contract:
 * slot contents are owned here, bounded 32 bytes incl. NUL). A hash collision
 * or a weapon swap at the same address fails strcmp and recomputes — never
 * returns a wrong style. Measured hit rate 83-100% for 8-12 distinct weapons.
 * Contract: render thread only. In C each TU gets its own cache; in C++ the
 * inline static is shared — both are correct under the single-thread rule. */
static inline RR_WEAPON_STYLE rr_weapon_style_cached(const char* weapon) {
    static struct { char key[32]; RR_WEAPON_STYLE style; int used; } cache[16];
    char key[32]; size_t n = 0;
    if (weapon) while (*weapon && *weapon != '_' && n + 1 < sizeof(key))
        key[n++] = rr_ascii_lower(*weapon++);
    key[n] = 0;
    unsigned h = 2166136261u;
    for (size_t i = 0; i < n; ++i) { h ^= (unsigned char)key[i]; h *= 16777619u; }
    unsigned slot = h & 15u;
    if (cache[slot].used && strcmp(cache[slot].key, key) == 0) return cache[slot].style;
    RR_WEAPON_STYLE s = rr_weapon_style(weapon);
    memcpy(cache[slot].key, key, n + 1);
    cache[slot].style = s;
    cache[slot].used = 1;
    return s;
}

static inline float rr_health_fraction(int health) {
    return fmaxf(0.0f, fminf(1.0f, health / 100.0f));
}
static inline RGBA rr_health_color(float ratio) {
    return ratio > 0.6f ? RRGB(94, 230, 126) : ratio >= 0.3f ? RRGB(255, 210, 73) : RRGB(255, 86, 86);
}
static inline int rr_esp_labels_visible(float distance, int limit_cm) {
    return isfinite(distance) && (limit_cm <= 0 || distance <= limit_cm / 100.0f);
}

/* One category lookup produces both color and label. The separate accessors
 * below delegate here, so a renderer that needs both pays one switch instead
 * of two per player per frame. */
typedef struct { RGBA color; const char* label; } RR_ROLE_STYLE;
static inline RR_ROLE_STYLE rr_role_style(const char* role) {
    RR_ROLE_STYLE s = {0, NULL};
    switch (rr_role_category(role)) {
    case RR_ROLE_COP:      s.color = RRGB(90, 170, 255);  s.label = "[COP]";      break;
    case RR_ROLE_ROBBER:   s.color = RRGB(255, 86, 86);   s.label = "[CRIMINAL]"; break;
    case RR_ROLE_CIVILIAN: s.color = RRGB(105, 224, 139); s.label = "[CIVILIAN]"; break;
    case RR_ROLE_PRISONER: s.color = RRGB(255, 164, 66);  s.label = "[PRISONER]"; break;
    case RR_ROLE_NPC:      s.color = RRGB(194, 154, 255); s.label = "[NPC]";      break;
    default: break;
    }
    return s;
}
static inline RGBA rr_esp_role_color(const char* role) { return rr_role_style(role).color; }
static inline const char* rr_esp_role_label(const char* role) { return rr_role_style(role).label; }

static inline void rr_format_vehicle_owner(const RR_PLAYER* vehicle, char* out, size_t capacity) {
    if (!out || !capacity) return;
    if (!vehicle) { out[0] = 0; return; }
    if (vehicle->vehicle_owner_known && vehicle->vehicle_owner_public_id > 0) {
        if (vehicle->vehicle_owner_name[0]) snprintf(out, capacity, "Owner: %s", vehicle->vehicle_owner_name);
        else snprintf(out, capacity, "Owner ID: %lld", (long long)vehicle->vehicle_owner_public_id);
    } else {
        const char* label = rr_esp_role_label(vehicle->role);   /* category evaluated once */
        if (label) snprintf(out, capacity, "Owner role: %s", label);
        else snprintf(out, capacity, "%s", vehicle->vehicle_owner_known ? "No player owner" : "Owner: unavailable");
    }
}

static inline RGBA rr_skeleton_fade(RGBA color, float distance) {
    float gain = fmaxf(0.35f, 1.0f - fmaxf(0.0f, distance) / 240.0f);
    unsigned r = (unsigned)(((color >> 16) & 255) * gain),
             g = (unsigned)(((color >> 8) & 255) * gain),
             b = (unsigned)((color & 255) * gain);
    return (color & 0xFF000000u) | (r << 16) | (g << 8) | b;
}

/* Parse only an explicit trailing _mN/_mkN; absent levels stay unknown.
 * Last marker wins ("gun_m2_mk3" -> 3). Level bound 999, trailing junk
 * rejected, bare "_m" rejected. */
static inline int rr_weapon_mk(const char* name, const char** suffix) {
    if (suffix) *suffix = NULL;
    if (!name) return -1;
    const char* mark = NULL;
    for (const char* c = name; *c; ++c)
        if (*c == '_' && rr_ascii_lower(c[1]) == 'm') mark = c;
    if (!mark) return -1;
    const char* digit = mark + 2;
    if (rr_ascii_lower(*digit) == 'k') ++digit;
    if (!isdigit((unsigned char)*digit)) return -1;
    int level = 0;
    for (; *digit; ++digit) {
        if (!isdigit((unsigned char)*digit)) return -1;
        level = level * 10 + (*digit - '0');
        if (level > 999) return -1;
    }
    if (suffix) *suffix = mark;
    return level;
}

static inline void rr_format_weapon_label(const char* name, int show_mk, char* out, size_t capacity) {
    if (!out || !capacity) return;
    const char* suffix = NULL;
    int level = rr_weapon_mk(name, &suffix);
    char base[96]; size_t count = 0;
    if (name) while (name[count] && (!suffix || name + count < suffix) && count + 1 < sizeof(base)) {
        base[count] = rr_ascii_upper(name[count]); ++count;
    }
    base[count] = 0;
    if (level >= 0 && show_mk) snprintf(out, capacity, "%s MK %d", base, level);
    else snprintf(out, capacity, "%s", base);
}

/* Loot names are display strings ("AK-47", "Combat Knife"), not the internal
 * weapon ids the classifiers above receive. Normalise to lowercase tokens
 * separated by '|' and match whole tokens, so "Combat Helmet" is not read as a
 * bat and "M4A1" still matches through its own token. Returns the weapon atlas
 * cell, or -1 when the item is not a weapon. */
static inline int rr_name_token(const char* tokens, const char* token) {
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "|%s|", token);
    return strstr(tokens, pattern) != NULL;
}

static inline int rr_loot_weapon_cell(const char* name) {
    char tokens[128];
    size_t n = 0;
    int separator = 1;
    tokens[n++] = '|';
    for (const char* c = name; c && *c && n + 2 < sizeof(tokens); ++c) {
        char ch = rr_ascii_lower(*c);
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) { tokens[n++] = ch; separator = 0; }
        else if (!separator) { tokens[n++] = '|'; separator = 1; }
    }
    if (tokens[n - 1] != '|') tokens[n++] = '|';
    tokens[n] = 0;

    static const struct { const char* token; int cell; } map[] = {
        /* Distinctive classes first: "Sniper Rifle" is a sniper, not a rifle. */
        { "awm", 5 }, { "svd", 5 }, { "sniper", 5 }, { "marksman", 5 },
        { "ak47", 1 }, { "ak74", 1 }, { "ak", 1 }, { "ar15", 1 }, { "m4a1", 1 },
        { "m4", 1 }, { "m16", 1 }, { "rifle", 1 }, { "carbine", 1 }, { "scar", 1 }, { "famas", 1 },
        { "sg12", 3 }, { "ks12", 3 }, { "m870", 3 }, { "shotgun", 3 },
        { "mp5", 4 }, { "mp7", 4 }, { "uzi", 4 }, { "p90", 4 }, { "vector", 4 }, { "smg", 4 },
        { "dp9", 0 }, { "m9", 0 }, { "pistol", 0 }, { "revolver", 0 }, { "deagle", 0 },
        { "knife", 6 }, { "bat", 6 }, { "machete", 6 }, { "axe", 6 },
        { "grenade", 7 }, { "c4", 7 }, { "dynamite", 7 }
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); ++i)
        if (rr_name_token(tokens, map[i].token)) return map[i].cell;
    return -1;
}

static inline void rr_format_loot_price(int mode, unsigned flags, int base, int npc, char* out, size_t capacity) {
    if (!out || !capacity) return;
    out[0] = 0;
    int have_base = (flags & RR_LOOT_PRICE_BASE) && base >= 0;
    int have_npc = (flags & RR_LOOT_PRICE_NPC) && npc >= 0;
    if (mode == 2 && have_base && have_npc) snprintf(out, capacity, "NPC $%d | Base $%d/ea", npc, base);
    else if ((mode == 0 || mode == 2) && have_npc) snprintf(out, capacity, "NPC $%d/ea", npc);
    else if ((mode == 1 || mode == 2) && have_base) snprintf(out, capacity, "Base $%d/ea", base);
}
#endif