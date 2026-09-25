#ifndef WALLPAPER_H
#define WALLPAPER_H

#include "config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wayland-client-core.h>
#include <wayland-util.h>

struct wl_compositor;
struct wl_shm;
struct river_window_manager_v1;
struct river_node_v1;
struct river_shell_surface_v1;

#define WALLPAPER_TOPBAR_FADE_H CFG_WALLPAPER_TOPBAR_FADE_H

struct WallpaperOutput {
    struct river_shell_surface_v1 *shell_surface;
    struct river_node_v1 *node;
    struct wl_surface *surface;
    struct wl_buffer *buffer;
    struct Wallpaper *wp;

    int32_t width, height; // output dimensions, in wm logical space
    int32_t pos_x, pos_y;  // output position, in wm logical space

    int32_t drawn_w, drawn_h, drawn_top_h, drawn_top_fade_h,
        drawn_fade_h; // current

    uint8_t *cached_sharp, *cached_blurred;
    uint8_t *data;
    size_t size;

    bool drawn_blur_all, drawn_loaded, drawn_valid;

    bool busy;
    bool deferred;
    bool pos_valid;
    bool position_sent;
    bool placed;

    struct wl_list link; // Wallpaper.outputs
};

struct Wallpaper {
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct river_window_manager_v1 *wm;

    // Decoded source image (RGBA8, straight alpha, owned).
    uint8_t *image_pixels;

    int32_t blur_top_h;
    int32_t blur_top_fade_h;
    int32_t blur_fade_h;

    int image_width, image_height;

    bool loaded;
    bool blur_all;

    struct wl_list outputs; // WallpaperOutput
};

void wallpaper_init(
    struct Wallpaper *wp, struct wl_compositor *compositor, struct wl_shm *shm,
    struct river_window_manager_v1 *wm
);

bool wallpaper_load_ppm(struct Wallpaper *wp, const char *path);

struct WallpaperOutput *wallpaper_output_create(struct Wallpaper *wp);

void wallpaper_output_set_dimensions(
    struct WallpaperOutput *wpo, int32_t width, int32_t height
);

void wallpaper_output_set_position(
    struct WallpaperOutput *wpo, int32_t x, int32_t y
);

void wallpaper_output_destroy(struct WallpaperOutput *wpo);

void wallpaper_manage(
    struct Wallpaper *wp, bool blur_all, int32_t blur_top_h,
    int32_t blur_top_fade_h, int32_t blur_fade_h
);

#endif // WALLPAPER_H
