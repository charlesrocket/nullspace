#include "output.h"

#include "manager.h"
#ifdef WALLPAPER
#include "wallpaper.h"
#endif

#include <river-layer-shell-v1-client-protocol.h>
#include <river-window-management-v1-client-protocol.h>
#include <river-xkb-bindings-v1-client-protocol.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <wayland-util.h>

void output_maybe_destroy(struct Output *output) {
    if (!output->removed) { return; }
    if (wm.default_output == output) { wm.default_output = NULL; }

#ifdef WALLPAPER
    if (output->wallpaper != NULL) {
        wallpaper_output_destroy(output->wallpaper);
    }
#endif

    if (output->layer_shell != NULL) {
        river_layer_shell_output_v1_destroy(output->layer_shell);
    }

    river_output_v1_destroy(output->obj);
    wl_list_remove(&output->link);
    free(output);
}

void output_handle_removed(void *data, struct river_output_v1 *obj) {
    ;
    (void)obj;

    struct Output *output = data;
    output->removed = true;
}

// Ignored events
void output_handle_wl_output(
    void *data, struct river_output_v1 *obj, uint32_t name
) {
    (void)data;
    (void)obj;
    (void)name;
}

void output_handle_position(
    void *data, struct river_output_v1 *obj, int32_t x, int32_t y
) {
    ;
    (void)obj;

    struct Output *output = data;
    output->pos_x = x;
    output->pos_y = y;

#ifdef WALLPAPER
    if (output->wallpaper != NULL) {
        wallpaper_output_set_position(output->wallpaper, x, y);
    }
#endif
}

void output_handle_dimensions(
    void *data, struct river_output_v1 *obj, int32_t width, int32_t height
) {
    (void)obj;

    struct Output *output = data;
    output->width = width;
    output->height = height;

#ifdef WALLPAPER
    if (output->wallpaper != NULL) {
        wallpaper_output_set_dimensions(output->wallpaper, width, height);
    }
#endif
}

struct Output *output_primary(void) {
    struct Output *output;
    wl_list_for_each(output, &wm.outputs, link) {
        if (output->removed) { continue; }
        if (output->width <= 0 || output->height <= 0) { continue; }

        return output;
    }

    return NULL;
}
