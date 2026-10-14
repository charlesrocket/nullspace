#ifndef OUTPUT_H
#define OUTPUT_H

#include <scenefx/types/wlr_scene.h>
#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/types/wlr_output.h>

struct Output {
    struct wlr_output *wlr_output;
    struct wlr_scene_output *scene_output;
    struct wl_list link; // NsServer.outputs
    struct wl_listener frame;
    struct wl_listener request_state;
    struct wl_listener destroy;

#ifdef WALLPAPER
    struct WallpaperOutput *wallpaper;
#endif

    int32_t width;
    int32_t height;
    int32_t pos_x;
    int32_t pos_y;

    int32_t area_x;
    int32_t area_y;
    int32_t area_width;
    int32_t area_height;

    bool area_set;
    bool removed;
};

void output_create(struct wlr_output *wlr_output);
void output_destroy(struct Output *out);

bool output_set_enabled(struct wlr_output *wlr_output, bool on);
int output_set_power(const char *name, int mode);

struct Output *output_primary(void);

#endif // OUTPUT_H
