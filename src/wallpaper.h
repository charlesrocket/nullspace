#ifndef WALLPAPER_H
#define WALLPAPER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wayland-util.h>

struct Output;
struct wlr_scene_buffer;
struct wlr_scene_blur;

struct WallpaperConfig {
    int32_t pattern_bg_r, pattern_bg_g, pattern_bg_b;
    int32_t pattern_dot_r, pattern_dot_g, pattern_dot_b;
    int32_t pattern_grid_spacing;
    int32_t pattern_dot_radius;

    int32_t blur_radius, blur_passes;

    char *path;
};

struct WallpaperOutput {
    struct Output *output;
    struct wlr_scene_buffer *scene_buffer;
    struct wlr_scene_blur *blur;

    int32_t width, height;

    uint8_t *cached_sharp;

    bool drawn_valid;
    int32_t drawn_w, drawn_h;
    bool drawn_loaded;
    int32_t drawn_pattern_bg_r, drawn_pattern_bg_g, drawn_pattern_bg_b;
    int32_t drawn_pattern_dot_r, drawn_pattern_dot_g, drawn_pattern_dot_b;
    int32_t drawn_pattern_grid_spacing, drawn_pattern_dot_radius;

    struct wl_list link;
};

bool wallpaper_load_image(const char *path);
void wallpaper_invalidate_all(void);
bool wallpaper_blur_wanted(void);
void wallpaper_finish(void);

struct WallpaperOutput *wallpaper_output_create(struct Output *output);
void wallpaper_output_destroy(struct WallpaperOutput *wpo);

void wallpaper_update_output(struct Output *output);
void wallpaper_update_all(void);

#endif // WALLPAPER_H
