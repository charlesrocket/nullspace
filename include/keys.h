#ifndef KEYS_H
#define KEYS_H

enum IpcKind {
    IPC_KIND_INT,
    IPC_KIND_FLOAT,
    IPC_KIND_BOOL,
    IPC_KIND_STR,
    IPC_KIND_SPECIAL,
};

enum IpcAccess {
    IPC_A_R = 1u << 0,
    IPC_A_W = 1u << 1,
    IPC_A_RW = IPC_A_R | IPC_A_W,
};

enum IpcAfter {
    IPC_AFTER_NONE,
    IPC_AFTER_MANAGE,
    IPC_AFTER_LIBINPUT,
    IPC_AFTER_WALLPAPER,
};

struct IpcKeyMeta {
    const char *name;
    enum IpcKind kind;
    enum IpcAccess access;
    const char *range;
    const char *desc;
};

#define KEYS_TABLE(K_INT, K_FLOAT, K_BOOL, K_STR, K_SPECIAL)                   \
    K_SPECIAL(                                                                 \
        "layout", IPC_A_RW, "vtile|vgrid|htile|hrtile|monocle|hgrid|float",    \
        "current layout"                                                       \
    )                                                                          \
    K_SPECIAL("current_space", IPC_A_RW, "0-9", "active space index")         \
    K_SPECIAL("space_count", IPC_A_R, NULL, "number of spaces")                \
    K_SPECIAL(                                                                 \
        "space_window_count", IPC_A_R, NULL,                                   \
        "window count on a space (arg: index)"                                 \
    )                                                                          \
    K_SPECIAL("focused_title", IPC_A_R, NULL, "title of focused window")       \
    K_SPECIAL("focused_app_id", IPC_A_R, NULL, "app_id of focused window")     \
    K_SPECIAL("mfact", IPC_A_RW, "0.0..1", "master area factor")            \
    K_SPECIAL(                                                                 \
        "wallpaper_path", IPC_A_RW, "path to a P6 PPM", "wallpaper image path" \
    )                                                                          \
                                                                               \
    K_INT(                                                                     \
        "nmasters", IPC_A_RW, IPC_AFTER_MANAGE,                                \
        "master window count", wm.nmasters, 1, 1024                            \
    )                                                                          \
    K_INT(                                                                     \
        "gap_outer_h", IPC_A_RW, IPC_AFTER_MANAGE,                             \
        "outer horizontal gap", wm.tiled_gap_outer_h, 0, 500                   \
    )                                                                          \
    K_INT(                                                                     \
        "gap_outer_v", IPC_A_RW, IPC_AFTER_MANAGE,                             \
        "outer vertical gap", wm.tiled_gap_outer_v, 0, 500                     \
    )                                                                          \
    K_INT(                                                                     \
        "gap_inner_h", IPC_A_RW, IPC_AFTER_MANAGE,                             \
        "inner horizontal gap", wm.tiled_gap_inner_h, 0, 500                   \
    )                                                                          \
    K_INT(                                                                     \
        "gap_inner_v", IPC_A_RW, IPC_AFTER_MANAGE,                             \
        "inner vertical gap", wm.tiled_gap_inner_v, 0, 500                     \
    )                                                                          \
    K_BOOL(                                                                    \
        "smart_gaps", IPC_A_RW, IPC_AFTER_MANAGE, "true|false",                \
        "disable gaps when only one window", wm.smart_gaps                     \
    )                                                                          \
    K_BOOL(                                                                    \
        "center_overspread", IPC_A_RW, IPC_AFTER_MANAGE, "true|false",         \
        "masters fill width when n <= nmasters", wm.center_overspread          \
    )                                                                          \
    K_BOOL(                                                                    \
        "center_when_single_stack", IPC_A_RW, IPC_AFTER_MANAGE, "true|false",  \
        "center lone stack window", wm.center_when_single_stack                \
    )                                                                          \
                                                                               \
    K_BOOL(                                                                    \
        "animations_enabled", IPC_A_RW, IPC_AFTER_MANAGE, "true|false",        \
        "enable animations", wm.animations.enabled                             \
    )                                                                          \
    K_BOOL(                                                                    \
        "animations_open_from_top", IPC_A_RW, IPC_AFTER_MANAGE, "true|false",  \
        "open animation origin", wm.animations.open_from_top                   \
    )                                                                          \
    K_INT(                                                                     \
        "animations_duration_move", IPC_A_RW, IPC_AFTER_MANAGE,                \
        "move animation duration (ms)", wm.animations.duration_move, 0, 60000  \
    )                                                                          \
    K_INT(                                                                     \
        "animations_duration_open", IPC_A_RW, IPC_AFTER_MANAGE,                \
        "open animation duration (ms)", wm.animations.duration_open, 0, 60000  \
    )                                                                          \
    K_INT(                                                                     \
        "animations_duration_close", IPC_A_RW, IPC_AFTER_MANAGE,               \
        "close animation duration (ms)", wm.animations.duration_close, 0,      \
        60000                                                                  \
    )                                                                          \
    K_INT(                                                                     \
        "animations_duration_space", IPC_A_RW, IPC_AFTER_MANAGE,               \
        "space slide duration (ms)", wm.animations.duration_space, 0, 60000    \
    )                                                                          \
                                                                               \
    K_STR(                                                                     \
        "kb_layout", IPC_A_R, IPC_AFTER_NONE, NULL, "active keyboard layout",  \
        wm.kb_layout                                                           \
    )                                                                          \
                                                                               \
    K_INT(                                                                     \
        "libinput_tap_state", IPC_A_RW, IPC_AFTER_LIBINPUT,                    \
        "tap-to-click", wm.libinput.tap_state, -1, 1                           \
    )                                                                          \
    K_INT(                                                                     \
        "libinput_natural_scroll", IPC_A_RW, IPC_AFTER_LIBINPUT,               \
        "natural scroll", wm.libinput.natural_scroll, -1, 1                    \
    )                                                                          \
    K_INT(                                                                     \
        "libinput_left_handed", IPC_A_RW, IPC_AFTER_LIBINPUT,                  \
        "left-handed mode", wm.libinput.left_handed, -1, 1                     \
    )                                                                          \
    K_INT(                                                                     \
        "libinput_middle_emulation", IPC_A_RW, IPC_AFTER_LIBINPUT,             \
        "middle button emulation", wm.libinput.middle_emulation, -1, 1         \
    )                                                                          \
    K_INT(                                                                     \
        "libinput_dwt", IPC_A_RW, IPC_AFTER_LIBINPUT,                          \
        "disable-while-typing", wm.libinput.dwt, -1, 1                         \
    )                                                                          \
    K_INT(                                                                     \
        "libinput_drag", IPC_A_RW, IPC_AFTER_LIBINPUT,                         \
        "tap-and-drag", wm.libinput.drag, -1, 1                                \
    )                                                                          \
    K_INT(                                                                     \
        "libinput_drag_lock", IPC_A_RW, IPC_AFTER_LIBINPUT,                    \
        "drag lock", wm.libinput.drag_lock, -1, 2                              \
    )                                                                          \
    K_INT(                                                                     \
        "libinput_three_finger_drag", IPC_A_RW, IPC_AFTER_LIBINPUT,            \
        "3-finger drag", wm.libinput.three_finger_drag, -1, 2                  \
    )                                                                          \
    K_INT(                                                                     \
        "libinput_accel_profile", IPC_A_RW, IPC_AFTER_LIBINPUT,                \
        "accel profile bits (0 none, 1 flat, 2 adaptive, 4 custom)",           \
        wm.libinput.accel_profile, -1, 4                                       \
    )                                                                          \
    K_FLOAT(                                                                   \
        "libinput_accel_speed", IPC_A_RW, IPC_AFTER_LIBINPUT,                  \
        "accel speed (-2 = skip)", wm.libinput.accel_speed, -2.0f, 1.0f        \
    )                                                                          \
    K_INT(                                                                     \
        "libinput_click_method", IPC_A_RW, IPC_AFTER_LIBINPUT,                 \
        "click method bits", wm.libinput.click_method, -1, 2                   \
    )                                                                          \
    K_INT(                                                                     \
        "libinput_scroll_method", IPC_A_RW, IPC_AFTER_LIBINPUT,                \
        "scroll method bits (0 none, 1 two_finger, 2 edge, 4 button)",         \
        wm.libinput.scroll_method, -1, 4                                       \
    )

#ifdef WALLPAPER
#define KEYS_TABLE_WALLPAPER(K_INT, K_FLOAT, K_BOOL, K_STR, K_SPECIAL)         \
    K_INT(                                                                     \
        "wallpaper_topbar_fade_h", IPC_A_RW, IPC_AFTER_NONE,                   \
        "wallpaper topbar fade height", wm.wallpaper.topbar_fade_h, 0, 1000    \
    )                                                                          \
    K_INT(                                                                     \
        "wallpaper_pattern_bg_r", IPC_A_RW, IPC_AFTER_WALLPAPER,               \
        "pattern background red", wm.wallpaper.pattern_bg_r, 0, 255            \
    )                                                                          \
    K_INT(                                                                     \
        "wallpaper_pattern_bg_g", IPC_A_RW, IPC_AFTER_WALLPAPER,               \
        "pattern background green", wm.wallpaper.pattern_bg_g, 0, 255          \
    )                                                                          \
    K_INT(                                                                     \
        "wallpaper_pattern_bg_b", IPC_A_RW, IPC_AFTER_WALLPAPER,               \
        "pattern background blue", wm.wallpaper.pattern_bg_b, 0, 255           \
    )                                                                          \
    K_INT(                                                                     \
        "wallpaper_pattern_dot_r", IPC_A_RW, IPC_AFTER_WALLPAPER,              \
        "pattern dot red", wm.wallpaper.pattern_dot_r, 0, 255                  \
    )                                                                          \
    K_INT(                                                                     \
        "wallpaper_pattern_dot_g", IPC_A_RW, IPC_AFTER_WALLPAPER,              \
        "pattern dot green", wm.wallpaper.pattern_dot_g, 0, 255                \
    )                                                                          \
    K_INT(                                                                     \
        "wallpaper_pattern_dot_b", IPC_A_RW, IPC_AFTER_WALLPAPER,              \
        "pattern dot blue", wm.wallpaper.pattern_dot_b, 0, 255                 \
    )                                                                          \
    K_INT(                                                                     \
        "wallpaper_pattern_grid_spacing", IPC_A_RW, IPC_AFTER_WALLPAPER,       \
        "pattern grid spacing", wm.wallpaper.pattern_grid_spacing, 1, 512      \
    )                                                                          \
    K_INT(                                                                     \
        "wallpaper_pattern_dot_radius", IPC_A_RW, IPC_AFTER_WALLPAPER,         \
        "pattern dot radius", wm.wallpaper.pattern_dot_radius, 0, 128          \
    )                                                                          \
    K_INT(                                                                     \
        "wallpaper_blur_radius", IPC_A_RW, IPC_AFTER_WALLPAPER,                \
        "wallpaper blur radius", wm.wallpaper.blur_radius, 0, 256              \
    )                                                                          \
    K_INT(                                                                     \
        "wallpaper_blur_passes", IPC_A_RW, IPC_AFTER_WALLPAPER,                \
        "wallpaper blur passes", wm.wallpaper.blur_passes, 0, 16               \
    )                                                                          \
    K_INT(                                                                     \
        "wallpaper_blur_top_inset", IPC_A_RW, IPC_AFTER_WALLPAPER,             \
        "wallpaper blur top inset", wm.wallpaper.blur_top_inset, 0, 512        \
    )
#else
#define KEYS_TABLE_WALLPAPER(K_INT, K_FLOAT, K_BOOL, K_STR, K_SPECIAL)
#endif

#endif // KEYS_H
