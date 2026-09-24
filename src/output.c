#include "output.h"

#include "wallpaper.h"

#include <river-layer-shell-v1-client-protocol.h>
#include <river-window-management-v1-client-protocol.h>
#include <river-xkb-bindings-v1-client-protocol.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

void output_maybe_destroy(struct Output *output) {
    if (!output->removed) { return; }
    if (output->wallpaper != NULL) {
        wallpaper_output_destroy(output->wallpaper);
    }

    if (output->layer_shell != NULL) {
        river_layer_shell_output_v1_destroy(output->layer_shell);
    }

    river_output_v1_destroy(output->obj);
    wl_list_remove(&output->link);
    free(output);
}

void output_handle_removed(void *data, struct river_output_v1 *obj) {
    struct Output *output = data;
    output->removed = true;
}

// Ignored events
void output_handle_wl_output(
    void *data, struct river_output_v1 *obj, uint32_t name
) {}

void output_handle_position(
    void *data, struct river_output_v1 *obj, int32_t x, int32_t y
) {
    struct Output *output = data;
    output->pos_x = x;
    output->pos_y = y;

    if (output->wallpaper != NULL) {
        wallpaper_output_set_position(output->wallpaper, x, y);
    }
}

void output_handle_dimensions(
    void *data, struct river_output_v1 *obj, int32_t width, int32_t height
) {
    struct Output *output = data;
    output->width = width;
    output->height = height;

    if (output->wallpaper != NULL) {
        wallpaper_output_set_dimensions(output->wallpaper, width, height);
    }
}
