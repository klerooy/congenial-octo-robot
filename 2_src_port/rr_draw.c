/* rr_draw.c - Render ROB RUN snapshots with the gpu.cpp drawing interface.
 *
 * The frame includes player labels, boxes, radar, world markers, vehicle hulls
 * and a skinned reference body. Full-rig poses are filtered in world space
 * before projection. When a rig is unavailable, estimated body points keep
 * basic markers visible. rr_game.c supplies snapshots; this module draws them.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#include "gpu.h"
#include "menu_bridge.h"
#include "rr_game.h"
#include "rr_draw.h"
#include "rr_esp_style.h"
#include "rr_label_layout.h"
#include "rr_cham_model.h"
#include "rr_body_joints.h"
#include "rr_vehicle_cham_model.h"

#define RR_PI 3.14159265358979323846f

static RGBA slot_color(int i) {
    BR_Config* b = br_get();
    if (i < 0 || i >= BR_COLOR_COUNT) return RRGB(255, 255, 255);
    const unsigned char* c = b->color[i].rgb;
    return RARGB(255, c[0], c[1], c[2]);
}

/* Map replicated role names to marker colors for cops, robbers and civilians. */
static RGBA role_color(const char* role) {
    RGBA color = rr_esp_role_color(role);
    return color ? color : 0;
}

static RGBA player_color(const RR_PLAYER* p) {
    if (p->relationships & RR_REL_PARTY) return RRGB(120, 255, 190);
    if (p->relationships & RR_REL_CLAN) return RRGB(218, 153, 255);
    if (p->relationships & RR_REL_POLICE) return RRGB(86, 255, 246);
    if (p->dead) return slot_color(5);
    RGBA color = role_color(p->role);
    return color ? color : slot_color(0);
}

/* Screen visibility for one world marker kind (RR_PLAYER.is_loot). Shared by
 * the label pass and the radar so both obey the same switches. */
static int world_marker_visible(const BR_Config* config, int kind, int rarity) {
    switch (kind) {
        case 1: return config->show_loot && br_loot_rarity_visible(config, rarity);
        case 2: return config->show_loot_nodes && br_loot_rarity_visible(config, rarity);
        case 3: return config->show_containers;
        case 4: return config->show_doors;
        case 5: return config->show_dropped_loot;
        case 6: return config->show_vehicle_loot;
        default: return 0;
    }
}

/* ----------------------------------------------------------- label layout --- */

/* Loot names pile up when several items lie close together. Each drawn label
 * reserves a rectangle (see rr_label_layout.h); the next one steps to the
 * nearest free line and a label that cannot fit is dropped, so the screen stays
 * readable instead of turning into a stack of half-overlapping words. */
static RR_LABEL_LAYOUT g_label_layout;

/* Half-angle in radians to a screen-space radius at the vertical centre.
 * Weapon spread arrives as a small angle, so it converts through the camera
 * FOV; a 30 degree reference half-FOV keeps the circle proportional when the
 * camera is not valid yet. */
static float cone_to_pixels(const RR_CAMERA* cam, float radians, int vh) {
    if (!isfinite(radians) || radians <= 0.0f || vh < 16) return 0.0f;
    if (radians > 1.5f) radians = 1.5f;
    float base = (cam && cam->valid && cam->fov > 1.0f && cam->fov < 170.0f)
                     ? cam->fov * 0.5f * (RR_PI / 180.0f) : (30.0f * RR_PI / 180.0f);
    float radius = (float)vh * 0.5f * tanf(radians) / tanf(base);
    if (!isfinite(radius) || radius < 2.0f) return 0.0f;
    return radius > (float)vh ? (float)vh : radius;
}

static void corner_box(float x, float y, float w, float h, RGBA col, float stroke) {
    float cx = w * 0.28f;
    float cy = h * 0.28f;
    if (cx < 3) cx = 3;
    if (cy < 3) cy = 3;
    float l = x, t = y, r = x + w, b = y + h;
    gpu_pen(col, stroke);
    r_line(l, t, l + cx, t);
    r_line(l, t, l, t + cy);
    r_line(r, t, r - cx, t);
    r_line(r, t, r, t + cy);
    r_line(l, b, l + cx, b);
    r_line(l, b, l, b - cy);
    r_line(r, b, r - cx, b);
    r_line(r, b, r, b - cy);
}

typedef struct {
    uint64_t object, rig;
    int32_t id;
    uint32_t sampled_at;
    uint32_t last_used;
    int crouch, dead, inveh, full_rig;
    RR_V3 root;
    RR_V3 diag_delta, diag_bone;
    int diag_have_delta, diag_have_bone;
    RR_V3 diag_shape[3], diag_shape_older[3];
    int diag_shape_count;
    RR_V3 anchors[RR_MAX_BONES + 2]; /* feet, head, then skeleton points */
    float skin[RR_CHAM_BONES][12];
} CHAM_VISUAL_POSE;
static int g_cham_diag_enabled;
static RR_CHAM_DIAG g_cham_diag;
void cham_diag_enable(void) { memset(&g_cham_diag, 0, sizeof(g_cham_diag)); g_cham_diag_enabled = 1; }
RR_CHAM_DIAG cham_diag_results(void) { return g_cham_diag; }

static const CHAM_VISUAL_POSE* cham_visual_pose(const RR_PLAYER* p) {
    static CHAM_VISUAL_POSE poses[RR_MAX_PLAYERS];
    static uint32_t serial;
    ++serial;
    CHAM_VISUAL_POSE* previous = &poses[0];
    for (int i = 0; i < RR_MAX_PLAYERS; ++i) {
        CHAM_VISUAL_POSE* candidate = &poses[i];
        if (candidate->object == p->addr && candidate->id == p->id && candidate->rig == p->cham_rig) {
            previous = candidate;
            break;
        }
        if (serial - candidate->last_used > serial - previous->last_used) previous = candidate;
    }
    previous->last_used = serial;
    int same = previous->object == p->addr && previous->id == p->id && previous->rig == p->cham_rig &&
               previous->crouch == p->crouch && previous->dead == p->dead && previous->inveh == p->inveh &&
               previous->full_rig == p->full_rig;
    uint32_t elapsed = p->cham_sample_at - previous->sampled_at;
    if (same && elapsed == 0) {
        if (g_cham_diag_enabled) ++g_cham_diag.repeated;
        return previous;
    }

    const float* first_bone = p->cham_skin[0];
    /* A skin matrix includes the inverse bind transform. Its translation
       moves as the torso rotates, even when the character root is stationary. */
    RR_V3 root = p->mesh_bones ? p->pos :
        (RR_V3){first_bone[3], first_bone[7], first_bone[11]};
    float dx = root.x - previous->root.x;
    float dy = root.y - previous->root.y;
    float dz = root.z - previous->root.z;
    int reset = !same || elapsed >= 100 || dx * dx + dy * dy + dz * dz > 0.64f;
    if (g_cham_diag_enabled) {
        ++g_cham_diag.fresh;
        int identity = previous->object == p->addr && previous->id == p->id && previous->rig == p->cham_rig;
        if (!identity) ++g_cham_diag.initial;
        else {
            if (!same) ++g_cham_diag.state;
            if (elapsed >= 100) ++g_cham_diag.gap;
            if (dx * dx + dy * dy + dz * dz > 0.64f) ++g_cham_diag.jump;
            if (elapsed > g_cham_diag.max_gap_ms) g_cham_diag.max_gap_ms = elapsed;
            float jump = sqrtf(dx * dx + dy * dy + dz * dz);
            if (jump > g_cham_diag.max_jump_m) g_cham_diag.max_jump_m = jump;
            RR_V3 bone = { p->cham_skin[RR_RIG_M_HEAD_1_JNT][3] - root.x,
                           p->cham_skin[RR_RIG_M_HEAD_1_JNT][7] - root.y,
                           p->cham_skin[RR_RIG_M_HEAD_1_JNT][11] - root.z };
            if (same && elapsed > 0 && elapsed < 100) {
                if (jump > .01f) {
                    ++g_cham_diag.moving_samples;
                    float prev_len = sqrtf(previous->diag_delta.x * previous->diag_delta.x +
                                           previous->diag_delta.y * previous->diag_delta.y +
                                           previous->diag_delta.z * previous->diag_delta.z);
                    float dot = dx * previous->diag_delta.x + dy * previous->diag_delta.y + dz * previous->diag_delta.z;
                    if (previous->diag_have_delta && prev_len > .01f && dot < -.5f * jump * prev_len)
                        ++g_cham_diag.reversals;
                }
                if (previous->diag_have_bone) {
                    float bx = bone.x - previous->diag_bone.x, by = bone.y - previous->diag_bone.y,
                          bz = bone.z - previous->diag_bone.z;
                    float delta = sqrtf(bx * bx + by * by + bz * bz);
                    if (delta > .15f) ++g_cham_diag.bone_jumps;
                    if (delta > g_cham_diag.max_bone_delta_m) g_cham_diag.max_bone_delta_m = delta;
                }
                const int indices[3] = { RR_RIG_M_HEAD_1_JNT, RR_RIG_L_ARM_2_JNT, RR_RIG_R_ARM_2_JNT };
                for (int q = 0; q < 3; ++q) {
                    const float* mat = p->cham_skin[indices[q]];
                    RR_V3 now_shape = { mat[3] - root.x, mat[7] - root.y, mat[11] - root.z };
                    RR_V3 last = previous->diag_shape[q], older = previous->diag_shape_older[q];
                    float dx1 = now_shape.x - last.x, dy1 = now_shape.y - last.y, dz1 = now_shape.z - last.z;
                    float dx2 = now_shape.x - older.x, dy2 = now_shape.y - older.y, dz2 = now_shape.z - older.z;
                    if (previous->diag_shape_count >= 2 &&
                        dx1 * dx1 + dy1 * dy1 + dz1 * dz1 > .0225f &&
                        dx2 * dx2 + dy2 * dy2 + dz2 * dz2 < .0064f) {
                        if (q == 0) ++g_cham_diag.head_spikes;
                        else if (q == 1) ++g_cham_diag.left_spikes;
                        else ++g_cham_diag.right_spikes;
                    }
                    previous->diag_shape_older[q] = last;
                    previous->diag_shape[q] = now_shape;
                }
                if (previous->diag_shape_count < 2) ++previous->diag_shape_count;
            } else {
                previous->diag_shape_count = 0;
            }
            previous->diag_delta = (RR_V3){ dx, dy, dz };
            previous->diag_have_delta = same && elapsed > 0 && elapsed < 100;
            previous->diag_bone = bone;
            previous->diag_have_bone = 1;
        }
        if (!identity) { previous->diag_have_delta = 0; previous->diag_have_bone = 0; previous->diag_shape_count = 0; }
    }
    /* Keep the filtered rig close to the current pose while damping small
     * animation jitter. A long filter makes moving targets visibly trail. */
    float blend = (float)elapsed / ((float)elapsed + 3.0f);
    if (reset) {
        memcpy(previous->skin, p->cham_skin, sizeof(previous->skin));
    } else {
        /* Carry the entire pose with the live root immediately. Filter only
         * the remaining bone motion so the body stays with the runner. */
        for (int bone = 0; bone < RR_CHAM_BONES; ++bone) {
            float* smoothed = previous->skin[bone];
            const float* live = p->cham_skin[bone];
            if (!isfinite(live[0]) || !isfinite(smoothed[0])) {
                memcpy(smoothed, live, sizeof(previous->skin[bone]));
                continue;
            }
            /* Keep the root and torso responsive; damp arm/hand jitter and
             * damp the legs separately while seated in a moving vehicle. */
            float bone_blend = blend;
            if (bone >= RR_RIG_L_ARM_0_JNT && bone <= RR_RIG_R_ARMTWIST_JNT)
                bone_blend = (float)elapsed / ((float)elapsed + 20.0f);
            else if (p->inveh && bone >= RR_RIG_L_LEG_1_JNT && bone <= RR_RIG_R_FOOT_2_JNT)
                bone_blend = (float)elapsed / ((float)elapsed + 14.0f);
            smoothed[3] += dx;
            smoothed[7] += dy;
            smoothed[11] += dz;
            for (int component = 0; component < 12; ++component)
                smoothed[component] += (live[component] - smoothed[component]) * bone_blend;
        }
    }
    /* Project markers from the same filtered world pose as the mesh. Keep
       camera motion immediate by applying this before screen projection. */
    for (int i = 0; i < RR_MAX_BONES + 2; ++i) {
        if (p->full_rig && i >= 2) {
            int bone = i - 2;
            if (!isfinite(p->bone[bone][0]) || !isfinite(p->bone[bone][1]) || !isfinite(p->bone[bone][2])) {
                previous->anchors[i] = (RR_V3){ NAN, NAN, NAN };
                continue;
            }
            const float* m = previous->skin[bone];
            const float* a = rr_cham_bones[bone].anchor;
            previous->anchors[i] = (RR_V3){
                m[0] * a[0] + m[1] * a[1] + m[2] * a[2] + m[3],
                m[4] * a[0] + m[5] * a[1] + m[6] * a[2] + m[7],
                m[8] * a[0] + m[9] * a[1] + m[10] * a[2] + m[11] };
            continue;
        }
        RR_V3 live;
        if (i == 0) live = p->pos;
        else if (i == 1) live = p->head;
        else memcpy(&live, p->bone[i - 2], sizeof(live));
        RR_V3* anchor = &previous->anchors[i];
        if (reset) *anchor = live;
        else {
            anchor->x += dx + (live.x - anchor->x - dx) * blend;
            anchor->y += dy + (live.y - anchor->y - dy) * blend;
            anchor->z += dz + (live.z - anchor->z - dz) * blend;
        }
    }
    previous->object = p->addr;
    previous->id = p->id;
    previous->rig = p->cham_rig;
    previous->sampled_at = p->cham_sample_at;
    previous->crouch = p->crouch;
    previous->dead = p->dead;
    previous->inveh = p->inveh;
    previous->full_rig = p->full_rig;
    previous->root = root;
    return previous;
}
void cham_diag_sample(const RR_PLAYER* player) {
    if (player && player->valid && !player->local && !player->is_vehicle && !player->is_loot &&
        player->cham_valid && player->cham_rig && player->cham_sample_at) (void)cham_visual_pose(player);
}

/* Render the offline shared game mesh, skinned to this character's live rig.
 * Clothing and body variations may differ from this shared reference mesh. */
static int mesh_highlight(const RR_PLAYER* p, const RR_CAMERA* cam, int vw, int vh, RGBA color) {
    if (!p->cham_valid) return 0;
    const float (*skin)[12] = p->cham_skin;
    if (p->cham_rig && p->cham_sample_at) skin = cham_visual_pose(p)->skin;
    float points[RR_CHAM_VERTICES * 2];
    RR_V3 positions[RR_CHAM_VERTICES];
    for (int i = 0; i < RR_CHAM_VERTICES; ++i) {
        const RR_CHAM_VERTEX* v = &rr_cham_vertices[i];
        RR_V3 world = { 0, 0, 0 };
        float total = 0;
        for (int k = 0; k < 4; ++k) {
            float w = v->weight[k];
            int bone = v->bone[k];
            if (w <= 0.0001f || bone >= RR_CHAM_BONES) continue;
            const float* m = skin[bone];
            world.x += w * (m[0] * v->p[0] + m[1] * v->p[1] + m[2] * v->p[2] + m[3]);
            world.y += w * (m[4] * v->p[0] + m[5] * v->p[1] + m[6] * v->p[2] + m[7]);
            world.z += w * (m[8] * v->p[0] + m[9] * v->p[1] + m[10] * v->p[2] + m[11]);
            total += w;
        }
        positions[i].x = positions[i].y = positions[i].z = NAN;
        if (total < 0.9f || total > 1.1f || !isfinite(world.x) || !isfinite(world.y) || !isfinite(world.z)) continue;
        float dx = world.x - p->pos.x, dy = world.y - p->pos.y, dz = world.z - p->pos.z;
        if (dx * dx + dy * dy + dz * dz > 36.0f) continue;
        positions[i] = world;
    }
    game_w2s_many(cam, positions, RR_CHAM_VERTICES, vw, vh, points);
    return r_fill_triangles(points, RR_CHAM_VERTICES, rr_cham_triangles, RR_CHAM_TRIANGLES,
                            (color & 0x00FFFFFFu) | 0x66000000u) > 0;
}

/* Draw anatomical parent links. Attachment helpers can be parked at the
 * character origin and must not appear as body skeleton segments. */
static void draw_full_skeleton(const RR_PLAYER* p, const RR_CAMERA* cam, int vw, int vh) {
    RR_V3 world[RR_CHAM_BONES];
    float xy[RR_CHAM_BONES * 2];
    memcpy(world, p->bone, sizeof(world));
    game_w2s_many(cam, world, RR_CHAM_BONES, vw, vh, xy);
    for (int b = 0; b < RR_CHAM_BONES; ++b) {
        int parent = rr_cham_bones[b].parent;
        if (parent < 0 || parent >= RR_CHAM_BONES) continue;
        if (!rr_rig_is_skeleton_joint(b) || !rr_rig_is_skeleton_joint(parent)) continue;
        float x = xy[b * 2], y = xy[b * 2 + 1], px = xy[parent * 2], py = xy[parent * 2 + 1];
        if (isfinite(x) && isfinite(y) && isfinite(px) && isfinite(py)) r_line(px, py, x, y);
    }
}

/* Build short labels without variadic formatting in the draw loop. */
static void fmt_num(char* out, size_t n, unsigned v, const char* suffix) {
    char tmp[16];
    int k = 0;
    size_t o = 0;
    if (!n) return;
    do { tmp[k++] = (char)('0' + (v % 10u)); v /= 10u; } while (v && k < 15);
    while (k > 0 && o + 1 < n) out[o++] = tmp[--k];
    while (suffix && *suffix && o + 1 < n) out[o++] = *suffix++;
    out[o] = 0;
}
static void fmt_copy(char* out, size_t n, const char* a, const char* b) {
    size_t o = 0;
    if (!n) return;
    if (a) while (*a && o + 1 < n) out[o++] = *a++;
    if (b) while (*b && o + 1 < n) out[o++] = *b++;
    out[o] = 0;
}

/* Colour for one world marker kind. Rarity drives items and nodes; the rest
 * keep a fixed hue so a container never looks like a legendary rifle. */
static RGBA loot_color(int kind, int rarity) {
    switch (kind) {
        case 2: case 1: {
            static const RGBA rarity_colors[] = {
                RRGB(230, 230, 230), RRGB(230, 230, 230), RRGB(111, 232, 145),
                RRGB(105, 178, 255), RRGB(211, 155, 255), RRGB(255, 190, 92), RRGB(255, 113, 149)
            };
            if (rarity >= 1 && rarity <= 6) return rarity_colors[rarity];
            return RRGB(105, 178, 255);
        }
        case 3: return RRGB(255, 190, 92);
        case 4: return RRGB(180, 200, 225);
        case 5: return RRGB(156, 218, 114);
        case 6: return RRGB(255, 156, 102);
        default: return RRGB(235, 235, 235);
    }
}

static void draw_radar(const RR_PLAYER* arr, int n, const RR_CAMERA* cam,
                       int vw, int vh, const BR_Config* b) {
    const RR_PLAYER* local = NULL;
    for (int i = 0; i < n; ++i) {
        if (arr[i].valid && arr[i].local == RR_LOCAL_CONFIRMED) {
            local = &arr[i];
            break;
        }
    }
    /* Match the centre of ROB RUN's top-left minimap at 1920x1080; scale the
     * HUD coordinates with height so the overlay follows the game client. */
    float width = fminf((float)vh * (210.0f / 1080.0f), (float)vw * .25f);
    float height = fminf((float)vh * (180.0f / 1080.0f), (float)vw * .25f);
    if (!local || width < 70.0f || height < 70.0f || b->radar_range <= 0) return;
    float fx = cam->fwd.x, fz = cam->fwd.z;
    float length = hypotf(fx, fz);
    if (!(length > 0.01f) || !isfinite(length)) {
        fx = sinf(local->rot_yaw);
        fz = cosf(local->rot_yaw);
    } else {
        fx /= length;
        fz /= length;
    }
    float cx = (float)vh * (148.0f / 1080.0f);
    float cy = (float)vh * (130.0f / 1080.0f);
    float left = cx - width * .5f, top = cy - height * .5f;
    float plot_x = width * .5f - 6.0f, plot_y = height * .5f - 6.0f;
    float range = (float)b->radar_range;
    /* The minimap is an anchor: block it so queued loot names never cover the
     * panel the player is reading. */
    rr_label_reserve(&g_label_layout, left, top, width, height);
    /* The transparent centre leaves the game's map and icons visible. */
    r_rect(left, top, width, height, RARGB(180, 230, 224, 245), 1.0f);

    for (int i = 0; i < n; ++i) {
        const RR_PLAYER* p = &arr[i];
        if (!p->valid || p->local) continue;
        if (p->is_vehicle && (!b->radar_world || !b->show_vehicle_markers)) continue;
        if (p->is_loot) {
            if (!b->radar_world || !world_marker_visible(b, p->is_loot, p->loot_rarity)) continue;
        } else if (!p->is_vehicle) {
            if (!rr_relationship_allowed(p->relationships, b->show_clan, b->show_police, b->show_party)) continue;
            if (p->is_npc && !b->show_npcs) continue;
            if (!rr_category_allowed(p->role, b->show_cops, b->show_robbers, b->show_civilians,
                                     b->show_prisoners, b->show_npcs)) continue;
            /* The radar has its own range; the ESP distance limit must not hide
             * contacts the radar is set to show. */
        }
        float dx = p->pos.x - local->pos.x, dz = p->pos.z - local->pos.z;
        float lateral = dx * fz - dz * fx, forward = dx * fx + dz * fz;
        if (!(fabsf(lateral) <= range && fabsf(forward) <= range)) continue;
        float x = cx + lateral * (plot_x / range), y = cy - forward * (plot_y / range);
        if (x < left || x > left + width || y < top || y > top + height) continue;
        if (p->is_vehicle) {
            r_fill_ellipse(x, y, 2.8f, 2.8f, RRGB(255, 214, 110));
            continue;
        }
        if (p->is_loot) {
            r_fill_ellipse(x, y, 2.3f, 2.3f, loot_color(p->is_loot, p->loot_rarity));
            continue;
        }
        RGBA color = player_color(p);
        if (p->dead) r_ellipse(x, y, 3.0f, 3.0f, color, 1.0f);
        else {
            r_fill_ellipse(x, y, 4.5f, 4.5f, RRGB(16, 11, 25));
            r_fill_ellipse(x, y, 3.1f, 3.1f, color);
        }
    }
}

static const RR_VEHICLE_MESH* vehicle_mesh(const RR_PLAYER* p) {
    char key[RR_NAME_MAX];
    size_t length = 0;
    for (const unsigned char* c = (const unsigned char*)p->name; *c && length < sizeof(key) - 1; ++c) {
        if (isalnum(*c)) key[length++] = (char)tolower(*c);
    }
    key[length] = '\0';
    for (size_t i = 0; i < sizeof(rr_vehicle_meshes) / sizeof(rr_vehicle_meshes[0]); ++i) {
        const RR_VEHICLE_MESH* mesh = &rr_vehicle_meshes[i];
        if (strstr(key, mesh->name)) return mesh;
    }
    return NULL;
}

static void draw_vehicle_cham(const RR_PLAYER* p, const RR_CAMERA* cam, int vw, int vh) {
    const RR_VEHICLE_MESH* mesh = vehicle_mesh(p);
    if (!mesh) return;
    RR_V3 positions[RR_VEHICLE_MESH_MAX_VERTICES];
    float projected[2 * RR_VEHICLE_MESH_MAX_VERTICES];
    const float* pose = p->vehicle_pose;
    for (int i = 0; i < mesh->vertices; ++i) {
        const short* v = mesh->positions + 3 * i;
        float x = v[0] * 0.001f, y = v[1] * 0.001f, z = v[2] * 0.001f;
        positions[i] = (RR_V3){
            pose[0] * x + pose[1] * y + pose[2] * z + pose[3],
            pose[4] * x + pose[5] * y + pose[6] * z + pose[7],
            pose[8] * x + pose[9] * y + pose[10] * z + pose[11] };
    }
    game_w2s_many(cam, positions, mesh->vertices, vw, vh, projected);
    float left = INFINITY, top = INFINITY, right = -INFINITY, bottom = -INFINITY;
    for (int i = 0; i < mesh->vertices; ++i) {
        float x = projected[2 * i], y = projected[2 * i + 1];
        if (!isfinite(x) || !isfinite(y)) continue;
        left = fminf(left, x); right = fmaxf(right, x);
        top = fminf(top, y); bottom = fmaxf(bottom, y);
    }
    if (right < 0 || bottom < 0 || left > vw || top > vh || right - left < 2.0f || bottom - top < 2.0f) return;
    /* A single translucent amber identifies the vehicle independently of the
     * role colors used for its occupants. */
    RGBA color = RARGB(80, 255, 181, 65);
    r_fill_triangles(projected, mesh->vertices, mesh->triangles, mesh->faces, color);
}

/* One on-screen loot marker queued for the label pass. Labels are laid out
 * after the whole snapshot is walked so the closest item keeps its name when
 * two markers collide. */
#define RR_LOOT_TAGS 96
typedef struct {
    float x, y, dist;
    int   weapon_cell;              /* atlas cell, -1 when the item is not a weapon */
    RGBA  color;
    char  title[RR_NAME_MAX + 48];
    char  meta[64];                 /* price line, empty when disabled */
} RR_LOOT_TAG;

static int draw_loot_marker(const RR_PLAYER* p, const RR_CAMERA* cam, int vw, int vh,
                            const BR_Config* b, RR_LOOT_TAG* tag) {
    static const char* const rarity_names[] = { "", "Common", "Uncommon", "Rare", "Epic", "Legendary", "Exotic" };
    float x, y;
    if (!game_w2s(cam, &p->pos, vw, vh, &x, &y)) return 0;
    int rarity = p->loot_rarity >= 1 && p->loot_rarity <= 6 ? p->loot_rarity : 0;
    const char* name = p->name[0] ? p->name : "Loot";
    const char* prefix = p->is_loot == 2 ? "Node: " : "";
    int cell = rr_loot_weapon_cell(name);
    RGBA color = cell >= 0 ? RRGB(255, 226, 138) : loot_color(p->is_loot, p->loot_rarity);

    /* A dropped weapon gets a box around its ground position. The size follows
     * the projection: a close item is not a dot and a distant one stays legible.
     */
    if (cell >= 0) {
        float size = 1100.0f / fmaxf(p->dist, 2.0f);
        if (size < 12.0f) size = 12.0f;
        if (size > 56.0f) size = 56.0f;
        r_rect(x - size * 0.5f, y - size * 0.5f, size, size, color, 1.5f);
        r_line_c(x, y + size * 0.5f, x, y + size * 0.5f + 5.0f, color, 1.0f);
    } else if (p->is_loot == 2) {
        r_ellipse(x, y, 4.0f, 4.0f, color, 1.5f);
    } else {
        r_fill_ellipse(x, y, 4.0f, 4.0f, color);
    }

    if (!tag) return 1;
    memset(tag, 0, sizeof(*tag));
    tag->x = x; tag->y = y; tag->dist = p->dist;
    tag->weapon_cell = cell;
    tag->color = color;
    if (rarity)
        snprintf(tag->title, sizeof(tag->title), "%s%s / %s (%.0f m)", prefix, name, rarity_names[rarity], p->dist);
    else
        snprintf(tag->title, sizeof(tag->title), "%s%s (%.0f m)", prefix, name, p->dist);
    if (b->show_loot_price && (p->is_loot == 1 || p->is_loot == 5 || p->is_loot == 6))
        rr_format_loot_price(b->loot_price_source, p->loot_price_flags,
                             p->loot_base_price, p->loot_sell_price, tag->meta, sizeof(tag->meta));
    return 1;
}

/* Draw the queued loot names closest first, each in the nearest free slot. */
static void draw_loot_tags(RR_LOOT_TAG* tags, int count) {
    for (int i = 1; i < count; ++i) {
        RR_LOOT_TAG item = tags[i];
        int j = i - 1;
        while (j >= 0 && tags[j].dist > item.dist) { tags[j + 1] = tags[j]; --j; }
        tags[j + 1] = item;
    }
    for (int i = 0; i < count; ++i) {
        const RR_LOOT_TAG* t = &tags[i];
        int weapon = t->weapon_cell >= 0;
        float name_px = weapon ? 15.0f : 13.0f;
        float meta_px = weapon ? 12.0f : 11.0f;
        float icon_w = weapon ? 26.0f : 0.0f;
        float icon_h = 13.0f;

        gpu_font(name_px);
        float title_w = r_text_width(t->title);
        float title_h = r_text_height();
        float meta_w = 0.0f, meta_h = 0.0f;
        if (t->meta[0]) {
            gpu_font(meta_px);
            meta_w = r_text_width(t->meta);
            meta_h = r_text_height();
        }
        float box_w = fmaxf(title_w, meta_w) + (weapon ? icon_w + 5.0f : 0.0f);
        float box_h = title_h + (t->meta[0] ? meta_h : 0.0f);
        float left = t->x - box_w * 0.5f;
        float top = t->y + 8.0f;
        if (!rr_label_place(&g_label_layout, left, &top, box_w, box_h, title_h + 2.0f)) continue;

        float text_x = t->x + (weapon ? (icon_w + 5.0f) * 0.5f : 0.0f);
        if (weapon)
            r_weapon_icon(t->weapon_cell, left, top + (title_h - icon_h) * 0.5f, icon_w, icon_h);
        gpu_font(name_px);
        r_text(text_x, top, t->title, t->color, RT_CENTER | RT_OUTLINE);
        if (t->meta[0]) {
            gpu_font(meta_px);
            r_text(text_x, top + title_h, t->meta, slot_color(7), RT_CENTER | RT_OUTLINE);
        }
    }
}

void draw_esp(const RR_PLAYER* arr, int n, const RR_CAMERA* cam, int vw, int vh) {
    BR_Config* b = br_get();
    b->telem_cham_n = 0;
    if (!b->enabled) return;
    if (!arr || !cam || !cam->valid || n < 0) n = 0;

    float line_w = (float)(b->line_w < 1 ? 1 : (b->line_w > 4 ? 4 : b->line_w));
    if (vw < 16 || vh < 16) return;
    if (n > RR_MAX_PLAYERS) n = RR_MAX_PLAYERS;

    /* Loot names are queued and drawn last: the layout pass needs every marker
     * on screen before it can pick a non-overlapping line for each one. */
    static RR_LOOT_TAG loot_tags[RR_LOOT_TAGS];
    int loot_tag_n = 0;
    rr_label_reset(&g_label_layout);

    if ((b->cham_on || b->vehicle_chams) && n > 0) {
        /* Always put vehicles behind player chams, regardless of roster order. */
        if (b->vehicle_chams) for (int i = 0; i < n; ++i) {
            const RR_PLAYER* p = &arr[i];
            if (!p->valid || !p->is_vehicle || !p->vehicle_cham_valid ||
                (b->max_distance > 0 && p->dist > (float)b->max_distance / 100.0f)) continue;
            draw_vehicle_cham(p, cam, vw, vh);
        }
        gpu_mesh_flush();
        for (int i = 0; i < n; ++i) {
            const RR_PLAYER* p = &arr[i];
            if (!p->valid || (p->local && !b->show_self) || p->is_loot || p->is_vehicle ||
                (b->max_distance > 0 && p->dist > (float)b->max_distance / 100.0f)) continue;
            if (!b->cham_on || !p->cham_valid) continue;
            if (!rr_relationship_allowed(p->relationships, b->show_clan, b->show_police, b->show_party)) continue;
            if (!rr_category_allowed(p->role, b->show_cops, b->show_robbers, b->show_civilians,
                                     b->show_prisoners, b->show_npcs)) continue;
            float fx, fy, hx, hy;
            if (!game_w2s(cam, &p->pos, vw, vh, &fx, &fy) && !game_w2s(cam, &p->head, vw, vh, &hx, &hy)) continue;
            if (mesh_highlight(p, cam, vw, vh, player_color(p))) {
                ++b->telem_cham_n;
            }
        }
        gpu_mesh_flush();
    }

    if (b->show_self) {
        const RR_PLAYER* self = NULL;
        for (int i = 0; i < n; ++i) if (arr[i].valid && arr[i].local) { self = &arr[i]; break; }
        gpu_font(12.0f);
        r_text(12.0f, 12.0f, !self ? "SELF: unavailable" :
               self->hidden_in_object ? "SELF: hiding object detected" : "SELF: no hiding object detected",
               RRGB(227, 215, 252), RT_LEFT | RT_OUTLINE);
    }
    int drawn = 0;
    for (int i = 0; i < n; i++) {
        /* Apply the player drawing budget while walking the mixed snapshot. */
        if (drawn >= RR_MAX_PLAYERS) continue;
        const RR_PLAYER* p = &arr[i];
        if (!p->valid || (p->local && !b->show_self)) continue;
        if (!rr_relationship_allowed(p->relationships, b->show_clan, b->show_police, b->show_party)) continue;
        if (!rr_category_allowed(p->role, b->show_cops, b->show_robbers, b->show_civilians,
                                 b->show_prisoners, b->show_npcs)) continue;
        /* Menu settings are stored in centimetres; positions use metres. */
        if (b->max_distance > 0 && p->dist > (float)b->max_distance / 100.0f) continue;

        if (p->is_loot) {
            if (world_marker_visible(b, p->is_loot, p->loot_rarity) && loot_tag_n < RR_LOOT_TAGS &&
                draw_loot_marker(p, cam, vw, vh, b, &loot_tags[loot_tag_n]))
                ++loot_tag_n;
            continue;
        }
        if (p->is_vehicle) {
            if (b->show_vehicle_markers) {
                float x, y;
                if (game_w2s(cam, &p->pos, vw, vh, &x, &y)) {
                    char label[RR_NAME_MAX + 32];
                    const char* name = p->name[0] ? p->name : (p->is_vehicle == 2 ? "Helicopter" : "Car");
                    if (b->show_vehicle_occupants && p->occupants_known)
                        snprintf(label, sizeof label, "%s [%d] (%.0f m)", name, p->occupants, p->dist);
                    else snprintf(label, sizeof label, "%s (%.0f m)", name, p->dist);
                    RGBA color = RRGB(255, 214, 110);
                    r_fill_ellipse(x, y, 4.0f, 4.0f, color);
                    gpu_font(12.0f);
                    r_text(x, y + 6.0f, label, color, RT_CENTER | RT_OUTLINE);
                    if (b->show_vehicle_owner) {
                        char owner[RR_NAME_MAX + 32];
                        rr_format_vehicle_owner(p, owner, sizeof(owner));
                        if (owner[0]) {
                            gpu_font(11.0f);
                            r_text(x, y + 20.0f, owner, slot_color(8), RT_CENTER | RT_OUTLINE);
                        }
                    }
                }
            }
            continue;
        }

        RR_PLAYER visual;
        if (p->cham_valid && p->cham_rig && p->cham_sample_at && (!p->inveh || p->full_rig)) {
            const CHAM_VISUAL_POSE* pose = cham_visual_pose(p);
            visual = *p;
            visual.pos = pose->anchors[0];
            visual.head = pose->anchors[1];
            memcpy(visual.bone, &pose->anchors[2], sizeof(visual.bone));
            p = &visual;
        }

        float fx = NAN, fy = NAN, hx = NAN, hy = NAN;
        /* Without a usable rig, estimate a seated body at 1.45 m and widen the
         * box to match the vehicle marker.
         */
        RR_V3 top = p->head;
        /* Use the resolved head joint for seated players when available; the
         * fixed-height fallback can place the marker above the vehicle roof.
         */
        if (p->inveh && !p->mesh_bones) { top.x = p->pos.x; top.y = p->pos.y + 1.45f; top.z = p->pos.z; }
        int feet_ok = game_w2s(cam, &p->pos, vw, vh, &fx, &fy);
        int head_ok = game_w2s(cam, &top, vw, vh, &hx, &hy);
        if ((!feet_ok && !head_ok) || !isfinite(fx) || !isfinite(fy) || !isfinite(hx) || !isfinite(hy)) continue;

        float bh = fabsf(fy - hy);
        if (bh < 4.0f) bh = 4.0f;
        if (p->inveh && bh > 420.0f) bh = 420.0f;
        float bw = bh * (p->inveh ? 1.30f : 0.52f) + fabsf(hx - fx);
        float bx = (fx + hx) * 0.5f - bw * 0.5f;
        float by = fminf(hy, fy);
        if (bx < -200 || bx > vw + 200 || by < -200 || by > vh + 200) continue;

        RGBA col = player_color(p);
        if (p->dead) {
            /* downed: dim, but still visible */
            int a = 255 * 60 / 100;
            col = (col & 0x00FFFFFFu) | ((uint32_t)a << 24);
        }

        if (b->show_box) {
            corner_box(bx, by, bw, bh, col, line_w);
        }
        if (b->show_head) {
            /* Clamp the head-dot radius so nearby players do not produce oversized
             * markers.
             */
            float r = bw * 0.11f;
            if (r < 2.0f) r = 2.0f;
            if (r > 9.0f) r = 9.0f;
            r_fill_ellipse(hx, hy, r, r, col);
        }
        if (b->show_health && p->health >= 0) {
            /* Vertical bar hugging the left edge of the box. */
            float ratio = rr_health_fraction(p->health);
            float bar_x = bx - 5.0f, bar_w = 3.0f;
            r_fill(bar_x, by, bar_w, bh, RARGB(150, 18, 20, 26));
            float fill = bh * ratio;
            if (fill > 0.5f) r_fill(bar_x, by + bh - fill, bar_w, fill, rr_health_color(ratio));
        }
        if (b->show_skeleton && (!p->inveh || p->full_rig) && !p->hidden_in_object &&
            (b->skel_distance <= 0 || p->dist <= (float)b->skel_distance / 100.0f)) {
            gpu_pen(rr_skeleton_fade(col, p->dist), line_w);
            if (p->full_rig && p->nbones == RR_CHAM_BONES) {
                draw_full_skeleton(p, cam, vw, vh);
            } else if (p->nbones > 1) {
                /* spine chain and head from the animated SkeletonModel rig */
                for (int k = 0; k + 1 < p->nbones; k++) {
                    float ax, ay, bx2, by2;
                    if (!game_w2s(cam, (const RR_V3*)p->bone[k], vw, vh, &ax, &ay)) continue;
                    if (!game_w2s(cam, (const RR_V3*)p->bone[k + 1], vw, vh, &bx2, &by2)) continue;
                    r_line(ax, ay, bx2, by2);
                }
                float hxx, hyy;
                if (game_w2s(cam, (const RR_V3*)p->bone[p->nbones - 1], vw, vh, &hxx, &hyy)) {
                    float r = fabsf(fy - hyy) * 0.075f;
                    if (r < 2.5f) r = 2.5f;
                    if (r > 11.0f) r = 11.0f;
                    r_fill_ellipse(hxx, hyy - r * 0.5f, r, r, col);
                }
            } else {
                /* no rig data yet: two-segment torso fallback */
                float mid = by + bh * 0.55f;
                r_line(fx, by + bh * 0.10f, fx, mid);
                r_line(fx, mid, fx, by + bh);
                r_line(fx - bw * 0.30f, mid, fx + bw * 0.30f, mid);
            }
        }
        int text_ok = rr_esp_labels_visible(p->dist, b->esp_text_distance);
        if (text_ok && b->esp_role_label) {
            const char* role_label = rr_esp_role_label(p->role);
            if (p->relationships == (RR_REL_CLAN | RR_REL_POLICE)) role_label = "CLAN / POLICE";
            else if (p->relationships & RR_REL_PARTY) role_label = "PARTY";
            else if (p->relationships & RR_REL_CLAN) role_label = "CLAN MEMBER";
            else if (p->relationships & RR_REL_POLICE) role_label = "POLICE COWORKER";
            if (role_label) {
                /* Filled role badge above the head, tinted by category color. */
                gpu_font(12.0f);
                float tw = r_text_width(role_label) + 12.0f;
                float th = 16.0f;
                float cyb = by - 34.0f;
                RGBA badge = role_color(p->role);
                if (!badge) badge = col;
                RGBA bg = (badge & 0x00FFFFFFu) | (170u << 24);
                r_fill_ellipse(fx, cyb, tw * 0.5f, th * 0.5f, bg);
                r_text(fx, cyb - 7.0f, role_label, RRGB(10, 12, 16), RT_CENTER);
            }
        }
        if (text_ok && b->show_name) {
            char label[RR_NAME_MAX];
            if (p->local) fmt_copy(label, sizeof(label), "YOU", NULL);
            else if (p->name[0]) fmt_copy(label, sizeof(label), p->name, NULL);
            else if (p->id) { label[0] = 0x23; fmt_num(label + 1, sizeof(label) - 1, (unsigned)p->id, NULL); }
            else label[0] = 0;
            if (label[0]) {
                gpu_font(12.0f);
                r_text(fx, by - 15.0f, label, slot_color(6), RT_CENTER | RT_OUTLINE);
            }
        }
        if (text_ok && p->hidden_in_object) {
            gpu_font(12.0f);
            r_text(fx, by - 49.0f, p->hidden_in_object == 2 ? "POSSIBLY HIDING" : "HIDING", col, RT_CENTER | RT_OUTLINE);
        }
        if (text_ok && p->inveh) {
            char vbuf[32];
            /* A hiding player already owns the line above the role badge; the
             * vehicle tag steps up instead of printing on top of it. */
            float tag_y = p->hidden_in_object ? by - 64.0f : by - 49.0f;
            if (p->veh[0]) fmt_copy(vbuf, sizeof(vbuf), "IN ", p->veh);
            else fmt_copy(vbuf, sizeof(vbuf), "IN VEHICLE", NULL);
            gpu_font(12.0f);
            r_text(fx, tag_y, vbuf, col, RT_CENTER | RT_OUTLINE);
        }
        float label_y = by + bh + 2.0f;
        if (text_ok && b->show_distance) {
            char buf[32];
            fmt_num(buf, sizeof(buf), (unsigned)(p->dist + 0.5f), " m");
            gpu_font(11.0f);
            r_text(fx, label_y, buf, slot_color(8), RT_CENTER | RT_OUTLINE);
            label_y += r_text_height();
        }
        if (text_ok && b->show_weapon && p->weapon[0]) {
            char wbuf[48];
            rr_format_weapon_label(p->weapon, b->esp_weapon_mk, wbuf, sizeof(wbuf));
            RGBA wcol = rr_weapon_style_cached(p->weapon).color;
            gpu_font(10.0f);
            float tw = r_text_width(wbuf);
            if (b->esp_weapon_icon) {
                RR_WEAPON_STYLE style = rr_weapon_style_cached(p->weapon);
                float iw = 22.0f, ih = 11.0f;
                r_weapon_icon(style.atlas_cell, fx - tw * 0.5f - iw - 3.0f, label_y - 1.0f, iw, ih);
            }
            r_text(fx, label_y, wbuf, wcol, RT_CENTER | RT_OUTLINE);
            label_y += r_text_height();
        }
        if (text_ok && b->show_health && p->health >= 0) {
            char health[32];
            snprintf(health, sizeof health, "HP: %d", p->health);
            gpu_font(10.0f);
            r_text(fx, label_y, health, slot_color(6), RT_CENTER | RT_OUTLINE);
            label_y += r_text_height();
        }
        if (text_ok && b->show_crouch && p->crouch == 1) {
            gpu_font(10.0f);
            r_text(fx, label_y, "CROUCHING", slot_color(6), RT_CENTER | RT_OUTLINE);
            label_y += r_text_height();
        }
        if (text_ok && b->show_armor && p->armor == 1) {
            gpu_font(9.0f);
            r_text(fx + bw * 0.5f + 3.0f, by + 2.0f, "ARMOR", slot_color(9), RT_LEFT | RT_OUTLINE);
        }
        if (text_ok && p->dead) {
            gpu_font(10.0f);
            r_text(fx, by + bh * 0.5f, "DOWN", slot_color(5), RT_CENTER | RT_OUTLINE);
        }
        if (text_ok) {
            /* Block the name/badge band above the head so the loot pass never
             * prints a price over a player's name. The stat column under the box
             * stays free: it is far more useful to keep a nearby item labelled. */
            float top_y = by - (p->hidden_in_object && p->inveh ? 67.0f : 52.0f);
            float bottom_y = by + 2.0f;
            float col_w = fmaxf(bw, 64.0f) + 10.0f;
            rr_label_reserve(&g_label_layout, fx - col_w * 0.5f, top_y,
                             col_w, bottom_y - top_y);
        }
        drawn++;
    }

    /* Aim-assist FOV circle: aim_fov is a pixel radius in the menu and in the
     * picker, so the same number draws the same capture zone. */
    if (b->aim_enabled && b->aa_show_fov) {
        float radius = (float)b->aim_fov;
        if (radius > (float)vh) radius = (float)vh;
        if (radius >= 4.0f)
            r_ellipse((float)vw * 0.5f, (float)vh * 0.5f, radius, radius, slot_color(14), 1.0f);
    }
    /* Weapon spread: the game's own cone, drawn from the local spread value. */
    if (b->show_spread_visualizer) {
        const RR_PLAYER* self = NULL;
        for (int i = 0; i < n; ++i) if (arr[i].valid && arr[i].local) { self = &arr[i]; break; }
        if (self && self->has_spread_data) {
            float radius = cone_to_pixels(cam, self->spread_value, vh);
            if (radius >= 1.0f) {
                r_ellipse((float)vw * 0.5f, (float)vh * 0.5f, radius, radius, slot_color(12), 1.0f);
                /* Movement and airborne penalties add to the current cone; a
                 * dimmer outer ring shows what the next shot would cover. */
                float extra = self->spread_value + self->spread_move + self->spread_air;
                float outer = cone_to_pixels(cam, extra, vh);
                if (outer > radius + 1.5f)
                    r_ellipse((float)vw * 0.5f, (float)vh * 0.5f, outer, outer,
                              (slot_color(12) & 0x00FFFFFFu) | (120u << 24), 1.0f);
                gpu_font(10.0f);
                char spread[64];
                if (self->has_fake_spread_data)
                    snprintf(spread, sizeof(spread), "SPREAD %.3f  AIR %.3f  MOVE %.3f",
                             self->spread_value, self->spread_air, self->spread_move);
                else
                    snprintf(spread, sizeof(spread), "SPREAD %.3f", self->spread_value);
                r_text((float)vw * 0.5f, (float)vh * 0.5f + 6.0f, spread,
                       slot_color(12), RT_CENTER | RT_OUTLINE);
            }
        }
    }
    if (b->radar_on && n > 0) draw_radar(arr, n, cam, vw, vh, b);
    if (loot_tag_n > 0) draw_loot_tags(loot_tags, loot_tag_n);

    b->telem_targets = drawn;
}
