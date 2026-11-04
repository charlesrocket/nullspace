#include "decoration.h"

#include "config.h"
#include "cursor.h"
#include "layout/layout.h"
#include "manager.h"
#include "node/text.h"
#include "seat.h"
#include "server.h"
#include "window.h"

#include <dev/evdev/input-event-codes.h>
#include <libdrm/drm_fourcc.h>
#include <math.h>
#include <scenefx/types/fx/clipped_region.h>
#include <scenefx/types/wlr_scene.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/_clock_id.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_scene.h>

#define DECO_BTN           12
#define DECO_BTN_PAD       8
#define DECO_BTN_HIT_SLOP  2

#define DECO_TAB_MIN_CLOSE (DECO_BTN + 2 * DECO_BTN_PAD)
#define DECO_TAB_MIN_BOTH  (2 * DECO_BTN + 3 * DECO_BTN_PAD)
#define DECO_TAB_CHROME    (2 * DECO_BTN + 4 * DECO_BTN_PAD)

#define DECO_TAB_MIN_W     DECO_TAB_MIN_BOTH
#define DECO_TAB_TEXT_MAX  900
#define DECO_TAB_MAX_W     (DECO_TAB_TEXT_MAX + DECO_TAB_CHROME)

#define DECO_TITLE_FONT    CFG_DECO_FONT

#define DECO_GLYPH_SIZE    10
#define DECO_STROKE        4
#define DECO_DISC_SIZE     10

#define DECO_GLYPH_OFF     ((DECO_BTN - DECO_GLYPH_SIZE) / 2)
#define DECO_DISC_OFF      ((DECO_BTN - DECO_DISC_SIZE) / 2)

#define DECO_X_DOTS        (DECO_GLYPH_SIZE - DECO_STROKE + 1)
#define DECO_X_PARTS       (2 * DECO_X_DOTS)

// Gaussian standard deviation of the pre-rendered shadow.
#define SHADOW_STD         (CFG_WINDOW_SHADOW_SIGMA * 0.5f)

// Anti-aliasing/calibration
#define SHADOW_CUT_GROW    0.0f // .2-.75 test cases

struct DecoStrip {
    int32_t x, y, w, h, r;
};

static const struct DecoStrip DIAMOND_STRIPS[] = {
    {3, 0,  4, 4, 2},
    {2, 1,  6, 4, 2},
    {1, 2,  8, 4, 2},
    {0, 3, 10, 4, 2},
    {1, 4,  8, 4, 2},
    {2, 5,  6, 4, 2},
    {3, 6,  4, 4, 2},
}; // x y w h r

#define DIAMOND_STRIP_COUNT        7
#define DECO_DIAMOND_PARTS         (1 + DIAMOND_STRIP_COUNT)

#define DECO_ANIM_MS               160 // full 0 to 1 transition
#define DECO_ANIM_TICK_MS          16

#define DECO_DBLCLICK_MS           400
#define DECO_DBLCLICK_SLOP         8

#define DECO_INACTIVE_SHADOW_SCALE (2.6f / 3.0f)

enum DecoPart {
    DECO_PART_CLOSE,
    DECO_PART_MIN,
    DECO_PART_TAB,
};

static int32_t window_w(const struct Window *w);
static int32_t window_h(const struct Window *w);
static int32_t tab_corner_radius(int32_t tab_w);

static float clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

static double absd(double v) { return v < 0.0 ? -v : v; }

static bool fapprox(float a, float b) {
    float d = a - b;
    return d > -1e-6f && d < 1e-6f;
}

static int32_t iround(float v) {
    return (int32_t)(v < 0.0f ? v - 0.5f : v + 0.5f);
}

static float ease_out_cubic(float t) {
    float u = 1.0f - t;
    return 1.0f - u * u * u;
}

static void lerp4(float *out, const float *a, const float *b, float t) {
    for (int i = 0; i < 4; i++) out[i] = lerpf(a[i], b[i], t);
}

// `wlr_scene_rect` wants premultiplied values.
static void premul_copy(float *out, const float *in) {
    out[0] = in[0] * in[3];
    out[1] = in[1] * in[3];
    out[2] = in[2] * in[3];
    out[3] = in[3];
}

static void lerp4_premul(float *out, const float *a, const float *b, float t) {
    float pa[4], pb[4];
    premul_copy(pa, a);
    premul_copy(pb, b);
    lerp4(out, pa, pb, t);
}

static uint64_t now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

struct DecoShape {
    int32_t x, y, w, h, r;
};

struct DecoPartSpec {
    struct DecoShape glyph;
    struct DecoShape disc;
};

static const struct DecoShape SHAPE_DISC_FULL = {
    DECO_DISC_OFF, DECO_DISC_OFF, DECO_DISC_SIZE, DECO_DISC_SIZE,
    DECO_DISC_SIZE / 2
};

static const struct DecoShape SHAPE_NONE = {
    DECO_BTN / 2, DECO_BTN / 2, 0, 0, 0
};

static size_t part_count(enum DecoPart kind) {
    switch (kind) {
        case DECO_PART_CLOSE: return DECO_X_PARTS;
        case DECO_PART_MIN: return DECO_DIAMOND_PARTS;
        case DECO_PART_TAB: break;
    }

    return 0;
}

static struct DecoPartSpec part_spec(enum DecoPart kind, size_t i) {
    struct DecoPartSpec s = {.glyph = SHAPE_NONE, .disc = SHAPE_NONE};

    switch (kind) {
        case DECO_PART_CLOSE:
            {
                if (i == 0) {
                    s.disc = SHAPE_DISC_FULL;
                    break;
                }

                {
                    const int32_t n = DECO_X_DOTS;
                    const int32_t k = (int32_t)i - 1;
                    int32_t a, b;

                    if (k < n) {
                        a = k;
                        b = k;
                    } else {
                        a = k - n;
                        if (a >= n / 2) a++; // skip the shared center dot
                        b = n - 1 - a;
                    }

                    s.glyph = (struct DecoShape){
                        DECO_GLYPH_OFF + a, DECO_GLYPH_OFF + b, DECO_STROKE,
                        DECO_STROKE,        DECO_STROKE / 2,
                    };
                }
            }

            break;

        case DECO_PART_MIN:
            {
                if (i == 0) {
                    s.disc = SHAPE_DISC_FULL;
                } else if (i <= DIAMOND_STRIP_COUNT) {
                    const struct DecoStrip *st = &DIAMOND_STRIPS[i - 1];

                    s.glyph = (struct DecoShape){
                        .x = DECO_GLYPH_OFF + st->x,
                        .y = DECO_GLYPH_OFF + st->y,
                        .w = st->w,
                        .h = st->h,
                        .r = st->r,
                    };
                }
            }

            break;

        case DECO_PART_TAB: break;
    }

    return s;
}

static struct DecoShape
shape_lerp(const struct DecoShape *a, const struct DecoShape *b, float t) {
    return (struct DecoShape){
        .x = iround(lerpf((float)a->x, (float)b->x, t)),
        .y = iround(lerpf((float)a->y, (float)b->y, t)),
        .w = iround(lerpf((float)a->w, (float)b->w, t)),
        .h = iround(lerpf((float)a->h, (float)b->h, t)),
        .r = iround(lerpf((float)a->r, (float)b->r, t)),
    };
}

static void
apply_shape(struct wlr_scene_rect *rect, const struct DecoShape *s) {
    if (rect == NULL) return;

    const int32_t w = s->w > 0 ? s->w : 0;
    const int32_t h = s->h > 0 ? s->h : 0;
    const bool visible = w > 0 && h > 0;

    wlr_scene_node_set_enabled(&rect->node, visible);
    if (!visible) return;

    int32_t r = s->r;
    const int32_t max_r = (w < h ? w : h) / 2;
    if (r > max_r) r = max_r;
    if (r < 0) r = 0;

    wlr_scene_node_set_position(&rect->node, s->x, s->y);
    wlr_scene_rect_set_size(rect, w, h);

    const uint16_t ur = (uint16_t)r;
    struct fx_corner_radii radii = {
        .top_left = ur,
        .top_right = ur,
        .bottom_left = ur,
        .bottom_right = ur,
    };

    wlr_scene_rect_set_corner_radii(rect, radii);
}

static void deco_apply_visuals(struct Decoration *deco) {
    if (deco == NULL) return;

    const float p = clamp01(deco->progress);

    float tab[4], btn[4];
    lerp4_premul(tab, wm.deco_color_inactive, wm.deco_color_active, p);
    lerp4_premul(
        btn, wm.deco_color_button_inactive, wm.deco_color_button_active, p
    );

    if (deco->tab != NULL) wlr_scene_rect_set_color(deco->tab, tab);

    for (int k = 0; k < DECO_BTN_COUNT; k++) {
        struct DecoButton *b = &deco->btn[k];

        for (size_t i = 0; i < b->n_parts && i < DECO_BTN_MAX_PARTS; i++) {
            struct wlr_scene_rect *rect = b->parts[i];
            if (rect == NULL) continue;

            const struct DecoPartSpec s = part_spec((enum DecoPart)k, i);
            const struct DecoShape cur = shape_lerp(&s.disc, &s.glyph, p);

            wlr_scene_rect_set_color(rect, btn);
            apply_shape(rect, &cur);
        }
    }

    if (deco->shadow != NULL) {
        float opacity = CFG_WINDOW_SHADOW_OPACITY
                      * lerpf(DECO_INACTIVE_SHADOW_SCALE, 1.0f, p);

        wlr_scene_buffer_set_opacity(deco->shadow, clamp01(opacity));
    }

    if (deco->title_text != NULL) {
        text_node_set_color(
            deco->title_text, deco->active ? wm.deco_color_font_active
                                           : wm.deco_color_font_inactive
        );
    }
}

void deco_refresh_colors(void) {
    struct Window *w;
    wl_list_for_each(w, &wm.windows, link) {
        if (w->decoration != NULL) deco_apply_visuals(w->decoration);
    }
}

static struct wl_event_source *anim_timer = NULL;
static bool anim_armed = false;

static bool anim_arm(void) {
    if (anim_timer == NULL) return false;
    if (anim_armed) return true;

    if (wl_event_source_timer_update(anim_timer, DECO_ANIM_TICK_MS) != 0) {
        return false;
    }

    anim_armed = true;
    return true;
}

static void anim_snap(struct Decoration *d, float target) {
    d->animating = false;
    d->progress = target;
    d->anim_from = target;
    d->anim_to = target;
    deco_apply_visuals(d);
}

static void anim_retarget(struct Decoration *d, float target) {
    if (d == NULL) return;

    if (d->animating) {
        if (fapprox(d->anim_to, target)) return;
    } else if (fapprox(d->progress, target)) {
        d->anim_to = target;
        return;
    }

    float dist = target - d->progress;
    if (dist < 0.0f) dist = -dist;

    const uint32_t dur = (uint32_t)((float)DECO_ANIM_MS * dist);
    const bool visible = d->tree != NULL && d->tree->node.enabled;

    if (!visible || dur == 0 || anim_timer == NULL) {
        anim_snap(d, target);
        return;
    }

    d->anim_from = d->progress;
    d->anim_to = target;
    d->anim_start_ms = now_ms();
    d->anim_dur_ms = dur;
    d->animating = true;

    if (!anim_arm()) anim_snap(d, target);
}

static bool anim_step(struct Decoration *d, uint64_t now) {
    if (d->tree == NULL) {
        d->animating = false;
        d->progress = d->anim_to;
        return false;
    }

    const uint64_t elapsed =
        now > d->anim_start_ms ? now - d->anim_start_ms : 0;

    if (d->anim_dur_ms == 0 || elapsed >= d->anim_dur_ms) {
        d->progress = d->anim_to;
        d->animating = false;
    } else {
        const float t = (float)elapsed / (float)d->anim_dur_ms;
        d->progress = lerpf(d->anim_from, d->anim_to, ease_out_cubic(t));
    }

    deco_apply_visuals(d);
    return d->animating;
}

static int deco_anim_tick(void *data) {
    (void)data;

    anim_armed = false;

    const uint64_t now = now_ms();
    bool more = false;

    struct Window *w;
    wl_list_for_each(w, &wm.windows, link) {
        struct Decoration *d = w->decoration;
        if (d == NULL || !d->animating) continue;
        if (anim_step(d, now)) more = true;
    }

    if (more && !anim_arm()) {
        wl_list_for_each(w, &wm.windows, link) {
            struct Decoration *d = w->decoration;
            if (d != NULL && d->animating) anim_snap(d, d->anim_to);
        }
    }

    return 0;
}

bool deco_anim_init(struct wl_event_loop *loop) {
    if (anim_timer != NULL) return true;
    if (loop == NULL) return false;

    anim_timer = wl_event_loop_add_timer(loop, deco_anim_tick, NULL);
    anim_armed = false;
    return anim_timer != NULL;
}

void deco_anim_finish(void) {
    if (anim_timer == NULL) return;

    wl_event_source_remove(anim_timer);
    anim_timer = NULL;
    anim_armed = false;
}

static struct Decoration *sliding_deco = NULL;
static uint32_t swallowed_button = 0;

static struct {
    const struct Decoration *deco;
    uint64_t ms;
    double x, y;
} last_click;

static void click_reset(void) {
    last_click.deco = NULL;
    last_click.ms = 0;
}

static bool
click_is_double(const struct Decoration *deco, double lx, double ly) {
    const uint64_t now = now_ms();

    const bool dbl = now != 0 && last_click.deco == deco && now >= last_click.ms
                  && now - last_click.ms <= DECO_DBLCLICK_MS
                  && absd(lx - last_click.x) <= DECO_DBLCLICK_SLOP
                  && absd(ly - last_click.y) <= DECO_DBLCLICK_SLOP;

    if (dbl) {
        click_reset();
    } else {
        last_click.deco = deco;
        last_click.ms = now;
        last_click.x = lx;
        last_click.y = ly;
    }

    return dbl;
}

static void handle_tree_destroy(struct wl_listener *l, void *data) {
    (void)data;

    struct Decoration *deco = wl_container_of(l, deco, tree_destroy);

    wl_list_remove(&deco->tree_destroy.link);
    deco->tree = NULL;
    deco->tab = NULL;

    for (int k = 0; k < DECO_BTN_COUNT; k++)
        deco->btn[k] = (struct DecoButton){0};

    deco->animating = false;
}

struct ShadowBuf {
    struct wlr_buffer base;
    uint32_t *data;
};

static void shadowbuf_destroy(struct wlr_buffer *b) {
    struct ShadowBuf *s = wl_container_of(b, s, base);
    free(s->data);
    free(s);
}

static bool shadowbuf_begin(
    struct wlr_buffer *b, uint32_t flags, void **data, uint32_t *fmt,
    size_t *stride
) {
    (void)flags;

    struct ShadowBuf *s = wl_container_of(b, s, base);
    *data = s->data;
    *fmt = DRM_FORMAT_ARGB8888;
    *stride = (size_t)b->width * 4;
    return true;
}

static void shadowbuf_end(struct wlr_buffer *b) { (void)b; }

static const struct wlr_buffer_impl shadowbuf_impl = {
    .destroy = shadowbuf_destroy,
    .begin_data_ptr_access = shadowbuf_begin,
    .end_data_ptr_access = shadowbuf_end,
};

// Never steal pointer events from windows behind the shadow.
static bool
shadow_no_input(struct wlr_scene_buffer *b, double *sx, double *sy) {
    (void)b;
    (void)sx;
    (void)sy;
    return false;
}

static int shadow_margin(void) {
    int m = (int)ceilf(3.0f * SHADOW_STD);
    return m < 1 ? 1 : m;
}

// Coverage of a Gaussian-blurred 1D box (lo, hi) at position p.
static float box_cov(float p, float lo, float hi, float inv) {
    return 0.5f * (erff((p - lo) * inv) - erff((p - hi) * inv));
}

// Antialiased coverage of a rounded rectangle (per-corner radii).
static float rrect_cover(
    float x, float y, float rx, float ry, float rw, float rh, float tl,
    float tr, float br, float bl, float grow
) {
    const float m = fmaxf(fmaxf(tl, tr), fmaxf(br, bl)) + 1.0f;

    if (x < rx - 2 || x > rx + rw + 2 || y < ry - 2 || y > ry + rh + 2) {
        return 0.0f;
    }

    if ((x > rx + m && x < rx + rw - m && y > ry + 1 && y < ry + rh - 1)
        || (x > rx + 1 && x < rx + rw - 1 && y > ry + m && y < ry + rh - m)) {
        return 1.0f;
    }

    const float qx = x - (rx + rw * 0.5f), qy = y - (ry + rh * 0.5f);
    const float r = qx < 0 ? (qy < 0 ? tl : bl) : (qy < 0 ? tr : br);
    const float dx = fabsf(qx) - (rw * 0.5f - r);
    const float dy = fabsf(qy) - (rh * 0.5f - r);
    const float ox = fmaxf(dx, 0.0f), oy = fmaxf(dy, 0.0f);
    const float sd = sqrtf(ox * ox + oy * oy) + fminf(fmaxf(dx, dy), 0.0f) - r;

    return fminf(fmaxf(0.5f + grow - sd, 0.0f), 1.0f);
}

void deco_update_shadow(struct Window *window) {
    struct Decoration *deco = window ? window->decoration : NULL;
    if (deco == NULL || deco->tree == NULL || deco->shadow == NULL) return;

    const int32_t w = window_w(window), h = window_h(window);
    const int32_t tab_x = deco->tab_x, tab_w = deco->tab_w;
    if (tab_w <= 0) return;

    const int tl = window->last_tl < 0 ? 0 : window->last_tl;
    const int tr = window->last_tr < 0 ? 0 : window->last_tr;
    const int bl = window->last_bl < 0 ? 0 : window->last_bl;
    const int br = window->last_br < 0 ? 0 : window->last_br;
    const int tab_r = tab_corner_radius(tab_w);

    const int32_t key[9] = {w, h, tab_x, tab_w, tl, tr, bl, br, tab_r};
    if (deco->shadow_key_valid
        && memcmp(key, deco->shadow_key, sizeof key) == 0) {
        return;
    }

    const int M = shadow_margin();
    const int cw = w + 2 * M, ch = h + DECO_TAB_H + 2 * M;

    struct ShadowBuf *sb = calloc(1, sizeof(*sb));
    if (sb == NULL) return;

    sb->data = calloc((size_t)cw * (size_t)ch, sizeof(uint32_t));
    if (sb->data == NULL) {
        free(sb);
        return;
    }

    float *tbl = malloc(sizeof(float) * (2 * (size_t)cw + 2 * (size_t)ch));
    if (tbl == NULL) {
        free(sb->data);
        free(sb);
        return;
    }

    float *wx = tbl, *tx = wx + cw, *wy = tx + cw, *ty = wy + ch;

    float s = SHADOW_STD;
    if (s < 0.5f) s = 0.5f;
    const float inv = 1.0f / (s * 1.41421356f);

    // Buffer x maps to window x = i - M; buffer y maps to window y =
    // j - M - DECO_TAB_H (the tab lives at y in [-DECO_TAB_H, 0)).
    for (int i = 0; i < cw; i++) {
        const float x = (float)(i - M) + 0.5f;
        wx[i] = box_cov(x, 0.0f, (float)w, inv);
        tx[i] = box_cov(x, (float)tab_x, (float)(tab_x + tab_w), inv);
    }

    for (int j = 0; j < ch; j++) {
        const float y = (float)(j - M - DECO_TAB_H) + 0.5f;
        wy[j] = box_cov(y, 0.0f, (float)h, inv);
        ty[j] = box_cov(y, (float)-DECO_TAB_H, 0.0f, inv);
    }

    for (int j = 0; j < ch; j++) {
        const float y = (float)(j - M - DECO_TAB_H) + 0.5f;
        uint32_t *row = sb->data + (size_t)j * (size_t)cw;

        for (int i = 0; i < cw; i++) {
            const float a = wx[i] * wy[j], b = tx[i] * ty[j];
            float alpha = 1.0f - (1.0f - a) * (1.0f - b);
            if (alpha < 0.002f) continue;

            const float x = (float)(i - M) + 0.5f;

            const float cut = fmaxf(
                rrect_cover(
                    x, y, 0.0f, 0.0f, (float)w, (float)h, (float)tl, (float)tr,
                    (float)br, (float)bl, SHADOW_CUT_GROW
                ),
                rrect_cover(
                    x, y, (float)tab_x, (float)-DECO_TAB_H, (float)tab_w,
                    (float)DECO_TAB_H, (float)tab_r, (float)tab_r, 0.0f, 0.0f,
                    SHADOW_CUT_GROW
                )
            );

            alpha *= 1.0f - cut;
            // Premultiplied black (only the alpha channel is set).
            row[i] = (uint32_t)(alpha * 255.0f + 0.5f) << 24;
        }
    }

    free(tbl);

    wlr_buffer_init(&sb->base, &shadowbuf_impl, cw, ch);
    wlr_scene_buffer_set_buffer(deco->shadow, &sb->base);
    wlr_buffer_drop(&sb->base);
    wlr_scene_buffer_set_dest_size(deco->shadow, cw, ch);
    wlr_scene_node_set_position(&deco->shadow->node, -M, -DECO_TAB_H - M);

    memcpy(deco->shadow_key, key, sizeof key);
    deco->shadow_key_valid = true;
}

static void handle_shadow_destroy(struct wl_listener *l, void *data) {
    (void)data;

    struct Decoration *deco = wl_container_of(l, deco, shadow_destroy);
    wl_list_remove(&deco->shadow_destroy.link);
    deco->shadow = NULL;
}

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunreachable-code"
#endif
static void shadow_node_create(struct Decoration *deco, struct Window *window) {
    if (!(CFG_WINDOW_SHADOW_OPACITY > 0.0f)) return;

    deco->shadow = wlr_scene_buffer_create(window->scene_tree, NULL);
    if (deco->shadow == NULL) return;

    deco->shadow->point_accepts_input = shadow_no_input;
    wlr_scene_node_lower_to_bottom(&deco->shadow->node);

    deco->shadow_destroy.notify = handle_shadow_destroy;
    wl_signal_add(&deco->shadow->node.events.destroy, &deco->shadow_destroy);
    deco->shadow_key_valid = false;
}
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

static void shadow_node_destroy(struct Decoration *deco) {
    if (deco->shadow == NULL) return;

    wl_list_remove(&deco->shadow_destroy.link);
    wlr_scene_node_destroy(&deco->shadow->node);
    deco->shadow = NULL;
}

static void handle_blur_win_destroy(struct wl_listener *l, void *data) {
    (void)data;

    struct Decoration *deco = wl_container_of(l, deco, blur_win_destroy);
    wl_list_remove(&deco->blur_win_destroy.link);
    deco->blur_win = NULL;
}

static void handle_blur_tab_destroy(struct wl_listener *l, void *data) {
    (void)data;

    struct Decoration *deco = wl_container_of(l, deco, blur_tab_destroy);
    wl_list_remove(&deco->blur_tab_destroy.link);
    deco->blur_tab = NULL;
}

static struct wlr_scene_blur *blur_create(
    struct wlr_scene_tree *parent, struct wl_listener *listener,
    wl_notify_func_t notify
) {
    struct wlr_scene_blur *b = wlr_scene_blur_create(parent, 1, 1);
    if (b == NULL) return NULL;

    wlr_scene_blur_set_strength(b, CFG_BLUR_STRENGTH);
    wlr_scene_blur_set_alpha(b, CFG_BLUR_ALPHA);

    wlr_scene_node_lower_to_bottom(&b->node);
    listener->notify = notify;
    wl_signal_add(&b->node.events.destroy, listener);
    return b;
}

static void
blur_destroy(struct wlr_scene_blur **blur, struct wl_listener *listener) {
    if (*blur == NULL) return;

    struct wlr_scene_blur *b = *blur;

    *blur = NULL;
    wl_list_remove(&listener->link);
    wlr_scene_node_destroy(&b->node);
}

void deco_update_blur(struct Window *window) {
    struct Decoration *deco = window ? window->decoration : NULL;
    if (deco == NULL || deco->blur_win == NULL) return;

    if (deco->blur_win != NULL) {
        wlr_scene_blur_set_size(
            deco->blur_win, window_w(window), window_h(window)
        );

        wlr_scene_node_set_position(&deco->blur_win->node, 0, 0);

        struct fx_corner_radii radii = {
            .top_left = (uint16_t)(window->last_tl < 0 ? 0 : window->last_tl),
            .top_right = (uint16_t)(window->last_tr < 0 ? 0 : window->last_tr),
            .bottom_left =
                (uint16_t)(window->last_bl < 0 ? 0 : window->last_bl),
            .bottom_right =
                (uint16_t)(window->last_br < 0 ? 0 : window->last_br),
        };

        wlr_scene_blur_set_corner_radii(deco->blur_win, radii);
    }

    if (deco->blur_tab != NULL && deco->tab_w > 0) {
        const uint16_t tab_r = (uint16_t)tab_corner_radius(deco->tab_w);

        wlr_scene_blur_set_size(deco->blur_tab, deco->tab_w, DECO_TAB_H);
        wlr_scene_node_set_position(
            &deco->blur_tab->node, deco->tab_x, -DECO_TAB_H
        );

        struct fx_corner_radii radii = {
            .top_left = tab_r,
            .top_right = tab_r,
            .bottom_left = 0,
            .bottom_right = 0,
        };

        wlr_scene_blur_set_corner_radii(deco->blur_tab, radii);
    }
}

static int32_t tab_corner_radius(int32_t tab_w) {
    if (!wm.rounded_corners) return 0;

    int32_t r = (int32_t)wm.rounded_corners_radius;
    if (r > tab_w / 2) r = tab_w / 2;
    if (r > DECO_TAB_H) r = DECO_TAB_H;

    return r < 0 ? 0 : r;
}

static void update_tab_corners(struct Decoration *deco, int32_t tab_w) {
    if (deco == NULL || deco->tab == NULL) return;

    const uint16_t r = (uint16_t)tab_corner_radius(tab_w);
    struct fx_corner_radii radii = {
        .top_left = r,
        .top_right = r,
        .bottom_left = 0,
        .bottom_right = 0,
    };

    wlr_scene_rect_set_corner_radii(deco->tab, radii);
}

static int32_t window_w(const struct Window *w) {
    return w->width > 0 ? w->width : 1;
}

static int32_t window_h(const struct Window *w) {
    return w->height > 0 ? w->height : 1;
}

static int32_t tab_width_for(const struct Window *w) {
    const int32_t ww = window_w(w);

    int32_t natural = 0;
    if (w->decoration != NULL && w->decoration->title_text != NULL) {
        natural = text_node_natural_width(w->decoration->title_text);
    }

    int32_t wanted = natural > 0 ? natural + DECO_TAB_CHROME : DECO_TAB_MIN_W;
    if (wanted < DECO_TAB_MIN_W) wanted = DECO_TAB_MIN_W;
    if (wanted > DECO_TAB_MAX_W) wanted = DECO_TAB_MAX_W;

    return wanted < ww ? wanted : ww;
}

static int32_t tab_x_max(const struct Window *w, int32_t tab_w) {
    const int32_t m = window_w(w) - tab_w;
    return m > 0 ? m : 0;
}

static void
layout_button(struct DecoButton *b, int32_t x, int32_t y, bool enabled) {
    if (b == NULL || b->tree == NULL) return;

    wlr_scene_node_set_enabled(&b->tree->node, enabled);
    wlr_scene_node_set_position(&b->tree->node, x, y);

    if (b->hit != NULL) {
        wlr_scene_node_set_position(&b->hit->node, -DECO_BTN_HIT_SLOP, -y);
        wlr_scene_rect_set_size(
            b->hit, DECO_BTN + 2 * DECO_BTN_HIT_SLOP, DECO_TAB_H
        );
    }
}

static void layout_tab(struct Decoration *deco, int32_t tab_w) {
    if (deco == NULL || deco->tree == NULL || deco->tab == NULL) return;

    deco->tab_w = tab_w;

    const int32_t bx = deco->tab_x;
    const int32_t by = (DECO_TAB_H - DECO_BTN) / 2;

    wlr_scene_node_set_position(&deco->tab->node, bx, 0);
    wlr_scene_rect_set_size(deco->tab, tab_w, DECO_TAB_H);

    update_tab_corners(deco, tab_w);

    layout_button(
        &deco->btn[DECO_PART_CLOSE], bx + DECO_BTN_PAD, by,
        tab_w >= DECO_TAB_MIN_CLOSE
    );

    layout_button(
        &deco->btn[DECO_PART_MIN], bx + 2 * DECO_BTN_PAD + DECO_BTN - 1, by,
        tab_w >= DECO_TAB_MIN_BOTH
    );

    if (deco->title_text != NULL && deco->title_text->buffer_node != NULL) {
        int32_t avail = tab_w - DECO_TAB_CHROME;
        if (avail < 0) avail = 0;

        text_node_set_max_width(deco->title_text, avail);
        wlr_scene_node_set_enabled(
            &deco->title_text->buffer_node->node, avail > 0
        );

        if (avail > 0) {
            int th = 0;
            text_node_get_size(deco->title_text, NULL, &th);
            if (th <= 0) th = DECO_BTN;

            const int32_t text_x = bx + 3 * DECO_BTN_PAD + 2 * DECO_BTN - 1;
            int32_t text_y = (DECO_TAB_H - th) / 2;
            if (text_y < 0) text_y = 0;

            wlr_scene_node_set_position(
                &deco->title_text->buffer_node->node, text_x, text_y
            );
        }
    }
}

static bool button_create(
    struct Decoration *deco, struct Window *window, enum DecoPart kind
) {
    static const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    struct DecoButton *b = &deco->btn[kind];

    b->tree = wlr_scene_tree_create(deco->tree);
    if (b->tree == NULL) return false;

    b->tree->node.data = &window->node_tag;

    float initial[4];
    premul_copy(initial, wm.deco_color_button_inactive);

    const size_t count = part_count(kind);
    for (size_t i = 0; i < count && i < DECO_BTN_MAX_PARTS; i++) {
        b->parts[i] = wlr_scene_rect_create(b->tree, 1, 1, initial);

        if (b->parts[i] == NULL) return false;
        b->n_parts = i + 1;
    }

    b->hit = wlr_scene_rect_create(
        b->tree, DECO_BTN + 2 * DECO_BTN_HIT_SLOP, DECO_TAB_H, clear
    );

    return b->hit != NULL;
}

void deco_create(struct Window *window) {
    if (window == NULL || window->decoration != NULL) return;
    if (window->closed || window->fullscreen) return;
    if (layouts_is_tiled(wm.layout)) return;

    struct Decoration *deco = calloc(1, sizeof(*deco));
    if (deco == NULL) return;

    deco->window = window;
    deco->tree = wlr_scene_tree_create(window->scene_tree);
    if (deco->tree == NULL) {
        free(deco);
        return;
    }

    shadow_node_create(deco, window);

    if (CFG_BLUR_WINDOW) {
        deco->blur_win = blur_create(
            window->scene_tree, &deco->blur_win_destroy, handle_blur_win_destroy
        );
    }

    deco->blur_tab = blur_create(
        window->scene_tree, &deco->blur_tab_destroy, handle_blur_tab_destroy
    );

    float tab_initial[4];
    premul_copy(tab_initial, wm.deco_color_inactive);

    deco->tab = wlr_scene_rect_create(
        deco->tree, DECO_TAB_MAX_W, DECO_TAB_H, tab_initial
    );

    bool ok = deco->tab != NULL;
    for (int k = 0; ok && k < DECO_BTN_COUNT; k++) {
        ok = button_create(deco, window, (enum DecoPart)k);
    }

    if (!ok) {
        shadow_node_destroy(deco);
        blur_destroy(&deco->blur_win, &deco->blur_win_destroy);
        blur_destroy(&deco->blur_tab, &deco->blur_tab_destroy);

        wlr_scene_node_destroy(&deco->tree->node);
        free(deco);
        return;
    }

    deco->tree_destroy.notify = handle_tree_destroy;
    wl_signal_add(&deco->tree->node.events.destroy, &deco->tree_destroy);

    deco->tree->node.data = &window->node_tag;
    wlr_scene_node_set_enabled(&deco->tree->node, window->mapped);

    window->decoration = deco;

    deco->active = (window == wm_focus_stack_top());

    const float *initial_color =
        deco->active ? wm.deco_color_font_active : wm.deco_color_font_inactive;

    deco->title_text =
        text_node_create(deco->tree, DECO_TITLE_FONT, initial_color, 1);
    if (deco->title_text != NULL) {
        text_node_set_text(deco->title_text, window->title);
    }

    deco->progress = deco->active ? 1.0f : 0.0f;
    deco->anim_from = deco->progress;
    deco->anim_to = deco->progress;
    deco->animating = false;

    deco_update_position(window);
    deco_apply_visuals(deco);
}

void deco_destroy(struct Window *window) {
    if (window == NULL || window->decoration == NULL) return;

    struct Decoration *deco = window->decoration;
    window->decoration = NULL;

    if (sliding_deco == deco) sliding_deco = NULL;
    if (last_click.deco == deco) click_reset();

    deco->animating = false;

    if (deco->title_text != NULL) {
        text_node_destroy(deco->title_text);
        deco->title_text = NULL;
    }

    shadow_node_destroy(deco);
    blur_destroy(&deco->blur_win, &deco->blur_win_destroy);
    blur_destroy(&deco->blur_tab, &deco->blur_tab_destroy);

    if (deco->tree != NULL) {
        wl_list_remove(&deco->tree_destroy.link);
        wlr_scene_node_destroy(&deco->tree->node);
        deco->tree = NULL;
    }

    free(deco);
    window_update_corners(window);
}

void deco_update_position(struct Window *window) {
    struct Decoration *deco = window ? window->decoration : NULL;
    if (deco == NULL || deco->tree == NULL || deco->tab == NULL) return;

    const int32_t tab_w = tab_width_for(window);
    const int32_t max_x = tab_x_max(window, tab_w);
    if (deco->tab_x > max_x) deco->tab_x = max_x;
    if (deco->tab_x < 0) deco->tab_x = 0;

    wlr_scene_node_set_position(&deco->tree->node, 0, -DECO_TAB_H);
    layout_tab(deco, tab_w);

    window_update_corners(window);

    deco_update_blur(window);
    deco_update_shadow(window);
}

void deco_update_active(struct Window *window, bool active) {
    struct Decoration *deco = window ? window->decoration : NULL;
    if (deco == NULL || deco->active == active) return;

    deco->active = active;

    if (deco->title_text != NULL) {
        text_node_set_color(
            deco->title_text,
            active ? wm.deco_color_font_active : wm.deco_color_font_inactive
        );
    }

    anim_retarget(deco, active ? 1.0f : 0.0f);
}

void deco_update_for_layout(struct Window *window) {
    if (window == NULL) return;

    if (layouts_is_tiled(wm.layout) || window->fullscreen) {
        deco_destroy(window);
        return;
    }

    if (window->decoration == NULL) deco_create(window);

    struct Decoration *deco = window->decoration;
    if (deco != NULL && deco->tree != NULL) {
        // Also re-enables the shadow and blur nodes that unmap disabled.
        deco_set_visible(window, window->mapped);
        deco_update_position(window);
    }
}

void deco_update_title(struct Window *window) {
    if (window == NULL || window->decoration == NULL) return;

    struct Decoration *deco = window->decoration;
    if (deco->title_text == NULL) return;

    text_node_set_text(deco->title_text, window->title);
    deco_update_position(window);
}

void deco_set_visible(struct Window *window, bool visible) {
    struct Decoration *deco = window ? window->decoration : NULL;
    if (deco == NULL || deco->tree == NULL) return;

    if (!visible && sliding_deco == deco) sliding_deco = NULL;
    wlr_scene_node_set_enabled(&deco->tree->node, visible);

    if (deco->shadow != NULL) {
        wlr_scene_node_set_enabled(&deco->shadow->node, visible);
    }

    if (deco->blur_win != NULL) {
        wlr_scene_node_set_enabled(&deco->blur_win->node, visible);
    }

    if (deco->blur_tab != NULL) {
        wlr_scene_node_set_enabled(&deco->blur_tab->node, visible);
    }

    if (!visible && deco->animating) anim_snap(deco, deco->anim_to);
}

void deco_raise(struct Window *window) {
    struct Decoration *deco = window ? window->decoration : NULL;
    if (deco == NULL || deco->tree == NULL || window->scene_tree == NULL) {
        return;
    }

    if (deco->tree->node.parent != window->scene_tree->node.parent) return;

    wlr_scene_node_place_above(&deco->tree->node, &window->scene_tree->node);
}

static bool
deco_pick(double lx, double ly, struct Decoration **out, enum DecoPart *part) {
    double nx, ny;
    struct wlr_scene_node *node =
        wlr_scene_node_at(&server.scene->tree.node, lx, ly, &nx, &ny);

    if (node == NULL) return false;

    struct Window *w = window_from_scene_node(node);
    if (w == NULL || w->closed || w->decoration == NULL) return false;

    struct Decoration *deco = w->decoration;

    if (node->type == WLR_SCENE_NODE_RECT) {
        if (deco->tab == NULL) return false;

        if (node == &deco->tab->node) {
            *part = DECO_PART_TAB;
        } else {
            bool found = false;

            for (int k = 0; k < DECO_BTN_COUNT; k++) {
                struct wlr_scene_rect *hit = deco->btn[k].hit;
                if (hit != NULL && node == &hit->node) {
                    *part = (enum DecoPart)k;
                    found = true;
                    break;
                }
            }

            if (!found) return false;
        }

        *out = deco;
        return true;
    }

    if (node->type == WLR_SCENE_NODE_BUFFER && deco->title_text != NULL
        && deco->title_text->buffer_node != NULL
        && node == &deco->title_text->buffer_node->node) {
        *part = DECO_PART_TAB;
        *out = deco;
        return true;
    }

    return false;
}

static void focus_and_raise(struct Window *w) {
    if (w->closed || !w->mapped || w->space_hidden) return;
    seat_focus(w);
    window_raise(w);
}

bool deco_handle_press(double lx, double ly, uint32_t button, uint32_t mods) {
    swallowed_button = 0;
    sliding_deco = NULL;

    struct Decoration *deco = NULL;
    enum DecoPart part = DECO_PART_TAB;

    if (!deco_pick(lx, ly, &deco, &part)) {
        click_reset();
        return false;
    }

    struct Window *w = deco->window;
    swallowed_button = button;

    if (button != BTN_LEFT) {
        click_reset();
        return true;
    }

    switch (part) {
        case DECO_PART_CLOSE:
            click_reset();
            window_close(w);
            return true;

        case DECO_PART_MIN:
            click_reset();
            wm_minimize_window(w);
            return true;

        case DECO_PART_TAB:
            focus_and_raise(w);

            if (mods & WLR_MODIFIER_SHIFT) {
                click_reset();
                sliding_deco = deco;
                deco->slide_start_x = lx;
                deco->slide_start_tab_x = deco->tab_x;
            } else if (click_is_double(deco, lx, ly)) {
                window_toggle_maximized(w);
            } else {
                cursor_begin_interactive(w, SEAT_OP_MOVE, 0);
            }

            return true;
    }

    return true;
}

bool deco_handle_release(uint32_t button) {
    if (swallowed_button == 0 || swallowed_button != button) return false;

    swallowed_button = 0;
    sliding_deco = NULL;
    return true;
}

bool deco_handle_motion(double lx) {
    struct Decoration *deco = sliding_deco;
    if (deco == NULL) return false;

    struct Window *w = deco->window;
    if (w == NULL || w->closed || !w->mapped || w->space_hidden
        || w->decoration != deco || deco->tree == NULL) {
        sliding_deco = NULL;
        return false;
    }

    const int32_t tab_w = tab_width_for(w);
    const double max_x = (double)tab_x_max(w, tab_w);

    double nx = (double)deco->slide_start_tab_x + (lx - deco->slide_start_x);
    if (nx < 0.0) nx = 0.0;
    if (nx > max_x) nx = max_x;

    const int32_t new_x = (int32_t)nx;
    if (new_x != deco->tab_x) {
        deco->tab_x = new_x;
        deco_update_position(w);
    }

    return true;
}

_Static_assert(
    DECO_GLYPH_OFF + DECO_GLYPH_SIZE <= DECO_BTN,
    "diamond must fit inside the button cell"
);

_Static_assert(
    (int)DECO_PART_TAB == DECO_BTN_COUNT,
    "DECO_BTN_COUNT (header) should equal the number of buttons"
);

_Static_assert(
    DECO_X_PARTS <= DECO_BTN_MAX_PARTS, "DECO_BTN_MAX_PARTS too small for X"
);

_Static_assert(
    DECO_DIAMOND_PARTS <= DECO_BTN_MAX_PARTS,
    "DECO_BTN_MAX_PARTS too small for diamond"
);

_Static_assert(
    DECO_X_DOTS % 2 == 1, "X needs an odd dot count (shared center)"
);

_Static_assert(
    DECO_STROKE % 2 == 0, "stroke must be even (round corners, centring)"
);

_Static_assert((DECO_BTN - DECO_GLYPH_SIZE) % 2 == 0, "glyph must be centered");
_Static_assert((DECO_BTN - DECO_DISC_SIZE) % 2 == 0, "disc must be centered");
