#pragma once

#include "actions.h"

#include <dev/evdev/input-event-codes.h>
#include <nullspace/layouts.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wlr/types/wlr_keyboard.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

#define CFG_DEFAULT_LAYOUT           LAYOUT_VERTICAL_TILE

#define CFG_GAPS                     8
#define CFG_SMART_GAPS               false
#define CFG_GAP_OUTER_H              CFG_GAPS
#define CFG_GAP_OUTER_V              CFG_GAPS
#define CFG_GAP_INNER_H              CFG_GAPS
#define CFG_GAP_INNER_V              CFG_GAPS

#define CFG_NMASTERS                 2
#define CFG_MFACT                    0.5f

#define CFG_CENTER_OVERSPREAD        false
#define CFG_CENTER_WHEN_SINGLE_STACK true

#define CFG_CMD_START                "desktop-startup" // startup script
#define CFG_CMD_TERMINAL             "foot"
#define CFG_CMD_F1                   "thunar"
#define CFG_CMD_F2                   CFG_CMD_TERMINAL
#define CFG_CMD_F3                   "waterfox"
#define CFG_CMD_F4                   "gram"
#define CFG_CMD_F5                   ""
#define CFG_CMD_F6                   ""
#define CFG_CMD_F7                   ""
#define CFG_CMD_F8                   ""
#define CFG_CMD_F9                   ""
#define CFG_CMD_F10                  ""

#define CFG_WINDOW_RULES                                                       \
    "space:2,title:foot", "space:2,appid:Alacritty",                           \
        "space:2,appid:com.mitchellh.ghostty", "space:3,appid:firefox",        \
        "space:4,appid:emacs", "space:4,appid:app.liten.Gram",                 \
        "space:4,appid:ev.zed.Zed", "space:5,appid:gedit",                     \
        "space:6,appid:thunar", "space:7,appid:Claws-mail",                    \
        "space:7,appid:signal", "space:8,appid:blender",                       \
        "space:8,appid:smplayer", "space:8,appid:mpv", "space:8,appid:vlc",    \
        "space:9,appid:org.nicotine_plus.Nicotine"

#define CFG_MOD_SUPER               WLR_MODIFIER_LOGO  // Super key
#define CFG_MOD_SHIFT               WLR_MODIFIER_SHIFT // Shift key

#define CFG_FOCUS_FOLLOW            true // focus follows the cursor
#define CFG_FOCUS_ON_ACTIVATE       true

#define CFG_BIND(MODS, KEY, ACTION) {MODS, KEY, ACTION, NULL},

// Spaces key
#define CFG_SPACEKEY(KEY, N)                                                   \
    CFG_BIND(CFG_MOD_SUPER, KEY, ACTION_SPACE_##N)                             \
    CFG_BIND(CFG_MOD_SUPER | CFG_MOD_SHIFT, KEY, ACTION_MOVE_TO_SPACE_##N)

#define CFG_KEYBINDS                                                           \
    CFG_BIND(CFG_MOD_SUPER, XKB_KEY_q, ACTION_CLOSE)                           \
    CFG_BIND(CFG_MOD_SUPER, XKB_KEY_f, ACTION_FOCUS_NEXT)                      \
    CFG_BIND(CFG_MOD_SUPER, XKB_KEY_l, ACTION_CYCLE_LAYOUT)                    \
    CFG_BIND(CFG_MOD_SUPER, XKB_KEY_r, ACTION_EXIT)                            \
    CFG_BIND(CFG_MOD_SUPER | CFG_MOD_SHIFT, XKB_KEY_f, ACTION_FULLSCREEN)      \
    CFG_SPACEKEY(XKB_KEY_1, 1)                                                 \
    CFG_SPACEKEY(XKB_KEY_2, 2)                                                 \
    CFG_SPACEKEY(XKB_KEY_3, 3)                                                 \
    CFG_SPACEKEY(XKB_KEY_4, 4)                                                 \
    CFG_SPACEKEY(XKB_KEY_5, 5)                                                 \
    CFG_SPACEKEY(XKB_KEY_6, 6)                                                 \
    CFG_SPACEKEY(XKB_KEY_7, 7)                                                 \
    CFG_SPACEKEY(XKB_KEY_8, 8)                                                 \
    CFG_SPACEKEY(XKB_KEY_9, 9)                                                 \
    CFG_SPACEKEY(XKB_KEY_0, 0)

#define CFG_POINTER_BINDS                                                      \
    {CFG_MOD_SUPER, BTN_LEFT, ACTION_MOVE, NULL}, {                            \
        CFG_MOD_SUPER, BTN_RIGHT, ACTION_RESIZE, NULL                          \
    }

// grp:alt_shift_toggle | grp:alt_space_toggle | grp:ctrl_shift_toggle |
// grp:win_space_toggle | grp:alt_caps_toggle
#define CFG_KB_OPTIONS                 "grp:alt_space_toggle" // XKB options
#define CFG_KB_LAYOUTS                 "us,no" // comma-separated layouts
#define CFG_KB_RATE_HZ                 26      // repeats/sec
#define CFG_KB_DELAY_MS                600

#define CFG_LIBINPUT_TAP_STATE         1
#define CFG_LIBINPUT_NATURAL_SCROLL    1
#define CFG_LIBINPUT_LEFT_HANDED       -1
#define CFG_LIBINPUT_MIDDLE_EMULATION  -1
#define CFG_LIBINPUT_DWT               -1 // disable-while-typing
#define CFG_LIBINPUT_DRAG              -1
// -1/0 disabled | 1 timeout | 2 sticky
#define CFG_LIBINPUT_DRAG_LOCK         -1
// -1/0 disabled | 1 3fg | 2 4fg
#define CFG_LIBINPUT_THREE_FINGER_DRAG -1
// 0 none | 1 flat | 2 adaptive | 4 custom
#define CFG_LIBINPUT_ACCEL_PROFILE     1
// Valid range is (-1,1), (-2.0f) to skip
#define CFG_LIBINPUT_ACCEL_SPD         (-2.0f)
// 0 none | 1 button_areas | 2 clickfinger
#define CFG_LIBINPUT_CLICK_METHOD      -1
// 0 no_scroll | 1 two_finger | 2 edge | 4 on_button_down
#define CFG_LIBINPUT_SCROLL_METHOD     -1

#define CFG_ROUNDED_CORNERS_ENABLED    true
#define CFG_ROUNDED_CORNERS_RADIUS     8 // minimum is 2

#define CFG_ANIMATIONS_ENABLED         true

#define CFG_ANIM_DURATION_MOVE         250
#define CFG_ANIM_DURATION_OPEN         250
#define CFG_ANIM_DURATION_CLOSE        250
#define CFG_ANIM_DURATION_SPACE        250

// New window's vertical direction
// (`true` slides new windows from above the screen).
#define CFG_ANIM_OPEN_FROM_TOP         false

// Cubic bezier control points (x1, y1, x2, y2).
#define CFG_ANIM_CURVE_MOVE            {0.25, 0.10, 0.25, 1.00}
#define CFG_ANIM_CURVE_OPEN            {0.00, 0.00, 0.20, 1.00}
#define CFG_ANIM_CURVE_CLOSE           {0.40, 0.00, 1.00, 1.00}
#define CFG_ANIM_CURVE_SPACE           {0.25, 0.10, 0.25, 1.00}

#define CFG_BLUR_PASSES                2
#define CFG_BLUR_RADIUS                7
#define CFG_BLUR_NOISE                 0.03f
#define CFG_BLUR_BRIGHTNESS            1.0f
#define CFG_BLUR_CONTRAST              1.0f
#define CFG_BLUR_SATURATION            1.0f
#define CFG_BLUR_STRENGTH              0.9f
#define CFG_BLUR_ALPHA                 1.0f

#define CFG_POPUP_SHADOW_SIGMA         16.2f
#define CFG_POPUP_SHADOW_SPREAD        22
#define CFG_POPUP_SHADOW_OFFSET_X      0
#define CFG_POPUP_SHADOW_OFFSET_Y      2
#define CFG_POPUP_SHADOW_RADIUS        CFG_ROUNDED_CORNERS_RADIUS
#define CFG_POPUP_SHADOW_OPACITY       0.64f

#ifdef WALLPAPER
#define CFG_WALLPAPER_PATH                 ".local/share/nullspace/wallpaper.ppm"

#define CFG_WALLPAPER_PATTERN_BG_R         0x12
#define CFG_WALLPAPER_PATTERN_BG_G         0x12
#define CFG_WALLPAPER_PATTERN_BG_B         0x12
#define CFG_WALLPAPER_PATTERN_DOT_R        0x3a
#define CFG_WALLPAPER_PATTERN_DOT_G        0xb5
#define CFG_WALLPAPER_PATTERN_DOT_B        0x5e
#define CFG_WALLPAPER_PATTERN_GRID_SPACING 24
#define CFG_WALLPAPER_PATTERN_DOT_RADIUS   1
#define CFG_WALLPAPER_BLUR_ENABLED         true
#define CFG_WALLPAPER_BLUR_RADIUS          16
#define CFG_WALLPAPER_BLUR_PASSES          2
#endif

#ifdef XWAYLAND
#define CFG_XWAYLAND true // lazy
#endif

// RUNTIME

struct UserBind {
    uint32_t mods;
    xkb_keysym_t key;
    enum Action action;
    char *arg; // owned, ?NULL (when unused)
};

struct Config {
    struct UserBind *binds;
    enum Layout default_layout;

    int gaps;
    bool smart_gaps;
    int nmasters;
    float mfact;
    bool center_overspread;
    bool center_when_single_stack;

    char *cmd_f1, *cmd_f2, *cmd_f3, *cmd_f4, *cmd_f5, *cmd_f6, *cmd_f7, *cmd_f8,
        *cmd_f9, *cmd_f10, *cmd_start, *cmd_terminal;

    char **window_rules;
    size_t window_rule_count;

    bool focus_follow, focus_on_activate;

    char *kb_options, *kb_layouts;
    int kb_rate_hz, kb_delay_ms;

    int libinput_tap_state;
    int libinput_natural_scroll;
    int libinput_left_handed;
    int libinput_middle_emulation;
    int libinput_dwt;
    int libinput_drag;
    int libinput_drag_lock;
    int libinput_three_finger_drag;
    int libinput_accel_profile;
    float libinput_accel_speed;
    int libinput_click_method;
    int libinput_scroll_method;

    size_t bind_count;

#ifdef WALLPAPER
    char *wallpaper_path;
#endif
#ifdef XWAYLAND
    bool xwayland;
#endif
};

extern struct Config config;

void config_init(void);
void config_finish(void);
