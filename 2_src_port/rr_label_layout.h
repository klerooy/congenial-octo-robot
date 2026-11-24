/* rr_label_layout.h - Screen-space label placement shared by the renderer and
 * the offline checks. Pure arithmetic: every placed label reserves a rectangle,
 * and the next label takes the nearest free line instead of stacking on top of
 * a neighbour. No GPU state, so the host checks can drive it directly.
 */
#ifndef RR_LABEL_LAYOUT_H
#define RR_LABEL_LAYOUT_H

/* The layout holds queued loot labels plus one reserved anchor per player and
 * the radar panel, so the budget is larger than the label queue itself. */
#define RR_LABEL_RECTS 192

typedef struct { float x, y, w, h; } RR_LABEL_RECT;

typedef struct {
    RR_LABEL_RECT rect[RR_LABEL_RECTS];
    int count;
} RR_LABEL_LAYOUT;

static inline void rr_label_reset(RR_LABEL_LAYOUT* layout) {
    if (layout) layout->count = 0;
}

static inline int rr_label_free(const RR_LABEL_LAYOUT* layout,
                                float x, float y, float w, float h) {
    if (!layout) return 1;
    for (int i = 0; i < layout->count; ++i) {
        const RR_LABEL_RECT* r = &layout->rect[i];
        if (x < r->x + r->w + 2.0f && x + w + 2.0f > r->x &&
            y < r->y + r->h + 1.0f && y + h + 1.0f > r->y) return 0;
    }
    return 1;
}

/* Block a rectangle unconditionally: used for anchors that must not move, such
 * as player name columns and the radar panel. Zero-size or an exhausted layout
 * simply keeps the previous reservations instead of failing the caller. */
static inline int rr_label_reserve(RR_LABEL_LAYOUT* layout, float x, float y,
                                   float w, float h) {
    if (!layout || w <= 0.0f || h <= 0.0f) return 0;
    if (layout->count >= RR_LABEL_RECTS) return 0;
    RR_LABEL_RECT* r = &layout->rect[layout->count++];
    r->x = x; r->y = y; r->w = w; r->h = h;
    return 1;
}

/* Reserve the nearest free line around the wanted top-left corner. `step` is
 * one line height; five lines each way is the search limit. */
static inline int rr_label_place(RR_LABEL_LAYOUT* layout, float x, float* y,
                                 float w, float h, float step) {
    static const float shift[] = { 0.0f, 1.0f, -1.0f, 2.0f, -2.0f,
                                   3.0f, -3.0f, 4.0f, -4.0f, 5.0f, -5.0f };
    if (!layout || !y || w <= 0.0f || h <= 0.0f) return 0;
    for (unsigned i = 0; i < sizeof(shift) / sizeof(shift[0]); ++i) {
        float candidate = *y + shift[i] * step;
        if (candidate < 0.0f) continue;
        if (!rr_label_free(layout, x, candidate, w, h)) continue;
        if (layout->count < RR_LABEL_RECTS) {
            RR_LABEL_RECT* r = &layout->rect[layout->count++];
            r->x = x; r->y = candidate; r->w = w; r->h = h;
        }
        *y = candidate;
        return 1;
    }
    return 0;
}

#endif
