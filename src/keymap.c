#include "keymap.h"

#include "config.h"
#include "log.h"

#include <string.h>
#include <wlr/types/wlr_keyboard.h>
#include <xkbcommon/xkbcommon.h>

#define LOG_TOPIC "keymap"

static struct xkb_context *xkb_context = NULL;
static struct xkb_keymap *xkb_keymap = NULL;
static KeymapLayoutCallback on_layout_change = NULL;

void keymap_set_layout_callback(KeymapLayoutCallback cb) {
    on_layout_change = cb;
}

void keymap_init(void) {
    if (xkb_context != NULL) return;

    xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (xkb_context == NULL) {
        log_err("Failed to create xkb context");
        return;
    }

    struct xkb_rule_names names = {
        .layout = config.kb_layouts,
        .options = config.kb_options,
    };

    xkb_keymap = xkb_keymap_new_from_names(
        xkb_context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS
    );

    if (xkb_keymap == NULL) {
        log_err(
            "Failed to compile layouts '%s' with options '%s'", CFG_KB_LAYOUTS,
            CFG_KB_OPTIONS
        );
    }
}

void keymap_destroy(void) {
    if (xkb_keymap != NULL) {
        xkb_keymap_unref(xkb_keymap);
        xkb_keymap = NULL;
    }

    if (xkb_context != NULL) {
        xkb_context_unref(xkb_context);
        xkb_context = NULL;
    }

    on_layout_change = NULL;
}

void keymap_apply(struct wlr_keyboard *keyboard) {
    if (keyboard == NULL || xkb_keymap == NULL) return;
    wlr_keyboard_set_keymap(keyboard, xkb_keymap);
    wlr_keyboard_set_repeat_info(
        keyboard, config.kb_rate_hz, config.kb_delay_ms
    );

    keymap_notify_layout(keyboard);
}

void keymap_notify_layout(struct wlr_keyboard *keyboard) {
    if (keyboard == NULL || keyboard->xkb_state == NULL) return;
    if (on_layout_change == NULL) return;

    struct xkb_keymap *km =
        keyboard->keymap != NULL ? keyboard->keymap : xkb_keymap;

    if (km == NULL) return;

    xkb_layout_index_t idx = xkb_state_serialize_layout(
        keyboard->xkb_state, XKB_STATE_LAYOUT_EFFECTIVE
    );

    if (idx >= xkb_keymap_num_layouts(km)) return;

    const char *name = xkb_keymap_layout_get_name(km, idx);
    if (name == NULL || name[0] == '\0') return;

    on_layout_change(name);
}
