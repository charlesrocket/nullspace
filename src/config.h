#ifndef CONFIG_H
#define CONFIG_H

#define CFG_DEFAULT_LAYOUT                 LAYOUT_TRIMMING

#define CFG_GAP_OUTER_H                    8
#define CFG_GAP_OUTER_V                    8
#define CFG_GAP_INNER_H                    8
#define CFG_GAP_INNER_V                    8
#define CFG_SMART_GAPS                     false

#define CFG_NMASTERS                       1
#define CFG_MFACT                          0.55f
#define CFG_CENTER_OVERSPREAD              false
#define CFG_CENTER_WHEN_SINGLE_STACK       true

#define CFG_TRIMMING_MANUAL_SPLIT          false
#define CFG_TRIMMING_PRESERVE_SPLIT        false
#define CFG_TRIMMING_SMART_SPLIT           false
#define CFG_TRIMMING_HSPLIT                0
#define CFG_TRIMMING_VSPLIT                0
#define CFG_TRIMMING_SPLIT_RATIO           0.5f

#define CFG_ANIM_DURATION_SPACE            200 // ms
#define CFG_ANIM_DURATION_OPEN             200 // ms, grow
#define CFG_ANIM_DURATION_CLOSE            200 // ms, shrink
#define CFG_ANIM_DURATION_TILE             200 // ms, layout transitions

#define CFG_ANIM_DEFAULT_HZ                100
#define CFG_ANIM_MIN_HZ                    60
#define CFG_ANIM_MAX_HZ                    720

#define CFG_WALLPAPER_PATH                 ".local/share/nullspace/wallpaper.ppm"
#define CFG_WALLPAPER_TOPBAR_FADE_H        42

#define CFG_WALLPAPER_PATTERN_BG_R         0x12
#define CFG_WALLPAPER_PATTERN_BG_G         0x12
#define CFG_WALLPAPER_PATTERN_BG_B         0x12
#define CFG_WALLPAPER_PATTERN_DOT_R        0x3a
#define CFG_WALLPAPER_PATTERN_DOT_G        0xb5
#define CFG_WALLPAPER_PATTERN_DOT_B        0x5e
#define CFG_WALLPAPER_PATTERN_GRID_SPACING 24
#define CFG_WALLPAPER_PATTERN_DOT_RADIUS   1

#define CFG_WALLPAPER_BLUR_RADIUS          12
#define CFG_WALLPAPER_BLUR_PASSES          2
#define CFG_WALLPAPER_BLUR_TOP_INSET       8

#define CFG_KB_LAYOUTS                     "us,no" // comma-separated keyboard layouts

// grp:alt_shift_toggle | grp:alt_space_toggle | grp:ctrl_shift_toggle |
// grp:win_space_toggle | grp:alt_caps_toggle
#define CFG_KB_OPTIONS                     "grp:alt_space_toggle" // XKB options

#define CFG_TERMINAL                       {"foot", NULL}

// -1 current | 0 disabled | 1 enabled

#define CFG_LIBINPUT_TAP_STATE             1
#define CFG_LIBINPUT_NATURAL_SCROLL        1
#define CFG_LIBINPUT_LEFT_HANDED           -1
#define CFG_LIBINPUT_MIDDLE_EMULATION      -1
#define CFG_LIBINPUT_DWT                   -1 // disable-while-typing
#define CFG_LIBINPUT_DRAG                  -1
#define CFG_LIBINPUT_DRAG_LOCK             -1 // -1, 0 = disabled, 1 = timeout, 2 = sticky
#define CFG_LIBINPUT_THREE_FINGER_DRAG     -1 // -1, 0 = disabled, 1 = 3fg, 2 = 4fg

// 0 none | 1 flat | 2 adaptive | 4 custom
#define CFG_LIBINPUT_ACCEL_PROFILE         1
// Valid range is -1, 1; -2.0f to "skip"
#define CFG_LIBINPUT_ACCEL_SPEED           (-2.0f)

// 0 none | 1 button_areas | 2 clickfinger
#define CFG_LIBINPUT_CLICK_METHOD          -1
// 0 no_scroll | 1 two_finger | 2 edge | 4 on_button_down
#define CFG_LIBINPUT_SCROLL_METHOD         -1

#endif // CONFIG_H
