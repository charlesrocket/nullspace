#ifndef OUTPUT_H
#define OUTPUT_H

#include "wallpaper.h"

#include <river-layer-shell-v1-client-protocol.h>
#include <river-window-management-v1-client-protocol.h>
#include <stdbool.h>
#include <stdint.h>

struct Output {
    struct river_output_v1 *obj;
    struct river_layer_shell_output_v1 *layer_shell;
    struct wl_list link; // WindowManager.outputs
    struct WallpaperOutput *wallpaper;

    int32_t width;
    int32_t height;
    // Output position in the global coordinate space
    int32_t pos_x;
    int32_t pos_y;
    int32_t area_x;
    int32_t area_y;
    int32_t area_width;
    int32_t area_height;

    bool area_set;
    bool removed;
};

void output_maybe_destroy(struct Output *output);

struct Output *tiling_output(void);

void output_handle_removed(void *data, struct river_output_v1 *obj);

// Ignored events
void output_handle_wl_output(
    void *data, struct river_output_v1 *obj, uint32_t name
);

void output_handle_position(
    void *data, struct river_output_v1 *obj, int32_t x, int32_t y
);

void output_handle_dimensions(
    void *data, struct river_output_v1 *obj, int32_t width, int32_t height
);

#endif // OUTPUT_H
