#include "help.h"

#include "actions.h"
#include "config.h"
#include "node/text.h"
#include "output.h"
#include "seat.h"
#include "server.h"

#include <scenefx/types/fx/clipped_region.h>
#include <scenefx/types/wlr_scene.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <xkbcommon/xkbcommon.h>

#define LOG_TOPIC "help"
#define HELP_FONT CFG_DECO_FONT
#define HELP_PAD  24

static struct wl_listener help_new_output;

static const float HELP_TEXT_COLOR[4] = {1.0f, 1.0f, 1.0f, 0.9f};
static const float HELP_BG_COLOR[4] = {0.0f, 0.0f, 0.0f, 0.68f};

static struct {
    struct wlr_scene_tree* tree;
    struct wlr_scene_rect* bg;
    struct wlr_scene_blur* blur;
    struct text_node* text;
    bool active;
} help;

struct Buf {
    char* data;
    size_t len, cap;
};

static bool buf_append(struct Buf* b, const char* s) {
    const size_t n = strlen(s);
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 512;
        while (cap < b->len + n + 1) cap *= 2;

        char* p = realloc(b->data, cap);
        if (p == NULL) return false;

        b->data = p;
        b->cap = cap;
    }

    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return true;
}

__attribute__((format(printf, 2, 3))) static bool
buf_printf(struct Buf* b, const char* fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    if (n < 0 || (size_t)n >= sizeof(tmp)) return false;
    return buf_append(b, tmp);
}

static void mods_prefix(uint32_t mods, char* buf, size_t size) {
    static const struct {
        uint32_t bit;
        const char* name;
    } parts[] = {
        { WLR_MODIFIER_LOGO, "Super+"},
        { WLR_MODIFIER_CTRL,  "Ctrl+"},
        {  WLR_MODIFIER_ALT,   "Alt+"},
        {WLR_MODIFIER_SHIFT, "Shift+"},
    };

    buf[0] = '\0';
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        if ((mods & parts[i].bit) == 0) continue;
        if (strlcat(buf, parts[i].name, size) >= size) break;
    }
}

static void bind_combo(const struct UserBind* bind, char* out, size_t size) {
    char mods[32], key[32];

    mods_prefix(bind->mods, mods, sizeof(mods));
    xkb_keysym_get_name(bind->key, key, sizeof(key));
    snprintf(out, size, "%s%s", mods, key);
}

static void
bind_action_desc(const struct UserBind* bind, char* out, size_t size) {
    switch (bind->action) {
        case ACTION_SPAWN:
            {
                const char* const* argv = (const char* const*)bind->arg;
                snprintf(out, size, "spawn %s", argv && argv[0] ? argv[0] : "");
                return;
            }
        case ACTION_SPAWN_SHELL:
            {
                snprintf(
                    out, size, "spawn %s",
                    bind->arg ? (const char*)bind->arg : ""
                );
                return;
            }
        case ACTION_CLOSE: strlcpy(out, "close window", size); return;
        case ACTION_FOCUS_NEXT: strlcpy(out, "focus next window", size); return;

        case ACTION_MOVE: strlcpy(out, "move window", size); return;
        case ACTION_RESIZE: strlcpy(out, "resize mode", size); return;
        case ACTION_MINIMIZE: strlcpy(out, "minimize window", size); return;
        case ACTION_RESTORE_MINIMIZED:
            {
                strlcpy(out, "restore last minimized window", size);
                return;
            }
        case ACTION_FULLSCREEN: strlcpy(out, "toggle fullscreen", size); return;
        case ACTION_CYCLE_LAYOUT: strlcpy(out, "cycle layout", size); return;
        case ACTION_OVERVIEW: strlcpy(out, "overview", size); return;
        case ACTION_EXIT: strlcpy(out, "exit", size); return;
        default: break;
    }

    if (bind->action >= ACTION_SPACE_0 && bind->action <= ACTION_SPACE_9) {
        snprintf(
            out, size, "switch to space %d", bind->action - ACTION_SPACE_0
        );
    } else if (bind->action >= ACTION_MOVE_TO_SPACE_0
               && bind->action <= ACTION_MOVE_TO_SPACE_9) {
        snprintf(
            out, size, "move window to space %d",
            bind->action - ACTION_MOVE_TO_SPACE_0
        );
    } else {
        strlcpy(out, "?", size);
    }
}

static char* build_text(void) {
    struct Buf b = {0};
    bool ok = buf_append(&b, "NULLSPACE key bindings\n\n");

    for (size_t i = 0; ok && i < config.bind_count; i++) {
        const struct UserBind* bind = &config.binds[i];
        char combo[64], desc[256];

        bind_combo(bind, combo, sizeof(combo));
        bind_action_desc(bind, desc, sizeof(desc));

        ok = buf_printf(&b, "%-26s %s\n", combo, desc);
    }

    if (!ok) {
        free(b.data);
        return NULL;
    }

    return b.data;
}

static void help_destroy(void) {
    help.active = false;

    if (help.text != NULL) {
        text_node_destroy(help.text);
        help.text = NULL;
    }

    help.bg = NULL;
    help.blur = NULL;

    if (help.tree != NULL) {
        wlr_scene_node_destroy(&help.tree->node);
        help.tree = NULL;
    }
}

void help_show(void) {
    if (help.active) return;
    if (server.scene == NULL || server.overlay_tree == NULL) return;

    struct Output* out = output_primary();
    if (out == NULL || out->width <= 0 || out->height <= 0) return;

    char* text = build_text();
    if (text == NULL) return;

    help.tree = wlr_scene_tree_create(server.overlay_tree);
    if (help.tree == NULL) {
        free(text);
        return;
    }

    wlr_scene_node_lower_to_bottom(&help.tree->node);

    help.blur = wlr_scene_blur_create(help.tree, 1, 1);
    help.bg = wlr_scene_rect_create(help.tree, 1, 1, HELP_BG_COLOR);
    help.text = text_node_create(help.tree, HELP_FONT, HELP_TEXT_COLOR, 1);

    if (help.blur == NULL || help.bg == NULL || help.text == NULL) {
        free(text);
        help_destroy();
        return;
    }

    wlr_scene_blur_set_strength(help.blur, CFG_BLUR_STRENGTH);
    wlr_scene_blur_set_alpha(help.blur, CFG_BLUR_ALPHA);

    text_node_set_text(help.text, text);
    free(text);

    int tw = 0, th = 0;
    text_node_get_size(help.text, &tw, &th);
    if (tw <= 0 || th <= 0) {
        help_destroy();
        return;
    }

    int bw = tw + 2 * HELP_PAD;
    int bh = th + 2 * HELP_PAD;

    const int max_w = out->width - 40;
    const int max_h = out->height - 40;
    if (bw > max_w) bw = max_w;
    if (bh > max_h) bh = max_h;
    if (bw <= 0 || bh <= 0) {
        help_destroy();
        return;
    }

    const int bx = out->pos_x + (out->width - bw) / 2;
    const int by = out->pos_y + (out->height - bh) / 2;

    uint16_t r = 0;
    if (CFG_ROUNDED_CORNERS_RADIUS && CFG_ROUNDED_CORNERS_RADIUS > 0) {
        const int lim = (bw < bh ? bw : bh) / 2;
        r = (uint16_t)((int)CFG_ROUNDED_CORNERS_RADIUS > lim
                           ? lim
                           : CFG_ROUNDED_CORNERS_RADIUS);
    }

    if (r > 0) {
        const struct fx_corner_radii radii = {
            .top_left = r,
            .top_right = r,
            .bottom_left = r,
            .bottom_right = r,
        };

        wlr_scene_rect_set_corner_radii(help.bg, radii);
        wlr_scene_blur_set_corner_radii(help.blur, radii);
    }

    wlr_scene_node_set_position(&help.tree->node, bx, by);
    wlr_scene_blur_set_size(help.blur, bw, bh);
    wlr_scene_rect_set_size(help.bg, bw, bh);
    wlr_scene_node_set_position(
        &help.text->buffer_node->node, HELP_PAD, HELP_PAD
    );

    help.active = true;
    seat_set_help_capture(true);
}

void help_hide(void) {
    if (!help.active) return;
    seat_set_help_capture(false);
    help_destroy();
}

bool help_active(void) { return help.active; }

static bool help_waiting;

static void help_stop_waiting(void) {
    if (!help_waiting) return;
    wl_list_remove(&help_new_output.link);
    help_waiting = false;
}

static void help_on_new_output(struct wl_listener* l, void* data) {
    (void)l;
    (void)data;

    help_show();
    if (help.active) help_stop_waiting();
}

void help_schedule(void) {
    if (output_primary() != NULL) {
        help_show();
        if (help.active) return;
    }

    if (help_waiting) return;
    help_new_output.notify = help_on_new_output;
    wl_signal_add(&server.backend->events.new_output, &help_new_output);
    help_waiting = true;
}

void help_finish(void) {
    help_stop_waiting();
    help_destroy();
}
