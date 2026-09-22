#ifndef WALLPAPER_H
#define WALLPAPER_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-client-core.h>

struct wl_compositor;
struct wl_shm;
struct river_window_manager_v1;
struct river_output_v1;
struct river_node_v1;

#define WALLPAPER_TOPBAR_FADE_H 42

struct wpo_pool {
    struct wl_shm_pool *handle;
    int fd;
    uint8_t *data;
    size_t size;
    int32_t width, height;
    bool valid;
};

struct WallpaperOutput {
    struct river_shell_surface_v1 *shell_surface;
    struct river_node_v1 *node;
    struct wl_surface *surface;
    struct wl_buffer *buffers[2];

    struct river_output_v1 *output; // not owned

    uint8_t *cached_sharp;
    uint8_t *cached_blurred;

    int32_t width, height; // output dimensions, in wm logical space

    struct wpo_pool pool;

    int next_buffer;

    bool configured; // node placed + first buffer committed
    bool needs_redraw;

    struct wl_list link; // Wallpaper.outputs
};

struct Wallpaper {
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct river_window_manager_v1 *wm;

    // Decoded source image (RGBA8, straight alpha), owned.
    uint8_t *image_pixels;

    int32_t blur_top_h;
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

struct WallpaperOutput *
wallpaper_output_create(struct Wallpaper *wp, struct river_output_v1 *output);

void wallpaper_output_set_dimensions(
    struct WallpaperOutput *wpo, int32_t width, int32_t height
);

void wallpaper_output_destroy(struct WallpaperOutput *wpo);

void wallpaper_manage(
    struct Wallpaper *wp, bool blur_all, int32_t blur_top_h, int32_t blur_fade_h
);

#endif // WALLPAPER_H
