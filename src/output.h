#pragma once

#include <scenefx/types/wlr_scene.h>
#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/types/wlr_output.h>

#define SPACE_COUNT 10

struct WallpaperOutput;

struct Output {
    struct wlr_output* wlr_output;
    struct wlr_scene_output* scene_output;
    struct wl_list link; // Server.outputs
    struct wl_listener frame, request_state, destroy;
    struct WallpaperOutput* wallpaper;

    int32_t width, height, pos_x, pos_y, area_x, area_y, area_width,
        area_height;

    int current_space;
    bool space_urgent[SPACE_COUNT], area_set, removed, lock_blanked;
};

void output_create(struct wlr_output* wlr_output);
void output_destroy(struct Output* out);

struct Output* output_from_wlr_output(struct wlr_output* wlr_output);
struct Output* output_focused(void);

bool output_set_enabled(struct wlr_output* wlr_output, bool on);
int output_set_power(const char* name, int mode);

struct Output* output_primary(void);
