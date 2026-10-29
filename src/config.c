#include "config.h"

#include "actions.h"
#include "input.h"
#include "log.h"
#include "nullspace/layouts.h"

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <ns-util.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/syslimits.h>
#include <sys/types.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

#define LOG_TOPIC "config"

struct Config config;
static bool initialized;

static const char *const default_window_rules[] = {CFG_WINDOW_RULES};

static bool set_string(char **dst, const char *value) {
    char *copy = strdup(value ? value : "");
    if (!copy) return false;
    free(*dst);
    *dst = copy;

    return true;
}

static bool parse_bool(const char *s, bool *out) {
    static const char *const yes[] = {"1", "true", "on", "yes"};
    static const char *const no[] = {"0", "false", "off", "no"};

    for (size_t i = 0; i < sizeof(yes) / sizeof(yes[0]); i++) {
        if (strcasecmp(s, yes[i]) == 0) {
            *out = true;
            return true;
        }

        if (strcasecmp(s, no[i]) == 0) {
            *out = false;
            return true;
        }
    }

    return false;
}

static size_t fmt_long(char *buf, size_t size, long v) {
    if (size < 2) return 0;

    bool neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-(v + 1)) + 1UL : (unsigned long)v;

    char tmp[32];
    size_t len = 0;
    do {
        tmp[len++] = (char)('0' + (u % 10UL));
        u /= 10UL;
    } while (u > 0);

    size_t total = len + (neg ? 1 : 0);
    if (total + 1 > size) return 0;

    size_t i = 0;

    if (neg) buf[i++] = '-';

    while (len > 0) buf[i++] = tmp[--len];

    buf[i] = '\0';
    return i;
}

static size_t fmt_double(char *buf, size_t size, double v, int decimals) {
    if (size < 2) return 0;
    if (decimals < 0) decimals = 0;
    if (decimals > 9) decimals = 9;

    bool neg = v < 0.0;
    if (neg) v = -v;

    double scale = 1.0;
    for (int i = 0; i < decimals; i++) scale *= 10.0;

    unsigned long scaled = (unsigned long)(v * scale + 0.5);
    unsigned long divisor = (unsigned long)scale;

    unsigned long whole = scaled / divisor;
    unsigned long frac = scaled % divisor;

    char rev_whole[32];
    size_t wlen = 0;
    do {
        rev_whole[wlen++] = (char)('0' + (whole % 10UL));
        whole /= 10UL;
    } while (whole > 0);

    char rev_frac[16];
    size_t flen = 0;

    for (int i = 0; i < decimals; i++) {
        rev_frac[flen++] = (char)('0' + (frac % 10UL));
        frac /= 10UL;
    }

    while (flen > 0 && rev_frac[0] == '0') {
        memmove(rev_frac, rev_frac + 1, --flen);
    }

    size_t total = wlen + (flen > 0 ? 1 + flen : 0) + (neg ? 1 : 0);
    if (total + 1 > size) return 0;

    size_t i = 0;

    if (neg) buf[i++] = '-';

    while (wlen > 0) buf[i++] = rev_whole[--wlen];

    if (flen > 0) {
        buf[i++] = '.';
        while (flen > 0) buf[i++] = rev_frac[--flen];
    }

    buf[i] = '\0';
    return i;
}

static bool write_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }

        buf += n;
        len -= (size_t)n;
    }

    return true;
}

static bool w_str(int fd, const char *s) { return write_all(fd, s, strlen(s)); }

static bool w_kv_str(int fd, const char *k, const char *v) {
    return w_str(fd, k) && w_str(fd, "=") && w_str(fd, v ? v : "")
        && w_str(fd, "\n");
}

static bool w_kv_long(int fd, const char *k, long v) {
    char num[32];
    size_t n = fmt_long(num, sizeof(num), v);
    if (n == 0) return false;

    return w_str(fd, k) && w_str(fd, "=") && write_all(fd, num, n)
        && w_str(fd, "\n");
}

static bool w_kv_float(int fd, const char *k, double v, int decimals) {
    char num[64];
    size_t n = fmt_double(num, sizeof(num), v, decimals);
    if (n == 0) return false;

    return w_str(fd, k) && w_str(fd, "=") && write_all(fd, num, n)
        && w_str(fd, "\n");
}

static bool w_kv_bool(int fd, const char *k, bool v) {
    return w_kv_str(fd, k, v ? "true" : "false");
}

static void clear_window_rules(void) {
    for (size_t i = 0; i < config.window_rule_count; i++)
        free(config.window_rules[i]);

    free(config.window_rules);
    config.window_rules = NULL;
    config.window_rule_count = 0;
}

static bool append_window_rule(const char *value) {
    char *copy = strdup(value);
    if (!copy) return false;

    char **rules = realloc(
        config.window_rules, (config.window_rule_count + 1) * sizeof(*rules)
    );

    if (!rules) {
        free(copy);
        return false;
    }

    config.window_rules = rules;
    config.window_rules[config.window_rule_count++] = copy;

    return true;
}

static void set_defaults(void) {
    config.default_layout = CFG_DEFAULT_LAYOUT;
    config.gaps = CFG_GAPS;
    config.smart_gaps = CFG_SMART_GAPS;
    config.nmasters = CFG_NMASTERS;
    config.cursor_size = CFG_CURSOR_SIZE;
    config.cursor_theme = CFG_CURSOR_THEME;
    config.mfact = CFG_MFACT;
    config.center_overspread = CFG_CENTER_OVERSPREAD;
    config.center_when_single_stack = CFG_CENTER_WHEN_SINGLE_STACK;

    set_string(&config.cmd_start, CFG_CMD_START);
    set_string(&config.cmd_terminal, CFG_CMD_TERMINAL);
    set_string(&config.cmd_f1, CFG_CMD_F1);
    set_string(&config.cmd_f2, CFG_CMD_F2);
    set_string(&config.cmd_f3, CFG_CMD_F3);
    set_string(&config.cmd_f4, CFG_CMD_F4);
    set_string(&config.cmd_f5, CFG_CMD_F5);
    set_string(&config.cmd_f6, CFG_CMD_F6);
    set_string(&config.cmd_f7, CFG_CMD_F7);
    set_string(&config.cmd_f8, CFG_CMD_F8);
    set_string(&config.cmd_f9, CFG_CMD_F9);
    set_string(&config.cmd_f10, CFG_CMD_F10);

    for (size_t i = 0;
         i < sizeof(default_window_rules) / sizeof(default_window_rules[0]);
         i++) {
        append_window_rule(default_window_rules[i]);
    }

    config.focus_follow = CFG_FOCUS_FOLLOW;
    config.focus_on_activate = CFG_FOCUS_ON_ACTIVATE;

    set_string(&config.kb_options, CFG_KB_OPTIONS);
    set_string(&config.kb_layouts, CFG_KB_LAYOUTS);

    config.kb_rate_hz = CFG_KB_RATE_HZ;
    config.kb_delay_ms = CFG_KB_DELAY_MS;

    config.libinput_tap_state = CFG_LIBINPUT_TAP_STATE;
    config.libinput_natural_scroll = CFG_LIBINPUT_NATURAL_SCROLL;
    config.libinput_left_handed = CFG_LIBINPUT_LEFT_HANDED;
    config.libinput_middle_emulation = CFG_LIBINPUT_MIDDLE_EMULATION;
    config.libinput_dwt = CFG_LIBINPUT_DWT;
    config.libinput_drag = CFG_LIBINPUT_DRAG;
    config.libinput_drag_lock = CFG_LIBINPUT_DRAG_LOCK;
    config.libinput_three_finger_drag = CFG_LIBINPUT_THREE_FINGER_DRAG;
    config.libinput_accel_profile = CFG_LIBINPUT_ACCEL_PROFILE;
    config.libinput_accel_speed = CFG_LIBINPUT_ACCEL_SPD;
    config.libinput_click_method = CFG_LIBINPUT_CLICK_METHOD;
    config.libinput_scroll_method = CFG_LIBINPUT_SCROLL_METHOD;

#ifdef WALLPAPER
    set_string(&config.wallpaper_path, CFG_WALLPAPER_PATH);
#endif
#ifdef XWAYLAND
    config.xwayland = CFG_XWAYLAND;
#endif
}

static bool has_bind(uint32_t mods, xkb_keysym_t key) {
    for (size_t i = 0; i < config.bind_count; i++) {
        if (config.binds[i].mods == mods && config.binds[i].key == key)
            return true;
    }
    return false;
}

// Takes ownership of `arg`
static bool
add_bind(uint32_t mods, xkb_keysym_t key, enum Action action, char *arg) {
    struct UserBind *b =
        realloc(config.binds, (config.bind_count + 1) * sizeof(*b));
    if (!b) {
        free(arg);
        return false;
    }
    config.binds = b;
    config.binds[config.bind_count].mods = mods;
    config.binds[config.bind_count].key = key;
    config.binds[config.bind_count].action = action;
    config.binds[config.bind_count].arg = arg;
    config.bind_count++;
    return true;
}

// MODS,KEY,ACTION[,ARGS...]
// ARGS may contain commas, everything after the 3rd comma is an argument.
static bool parse_bind(const char *value) {
    char *copy = strdup(value);
    if (!copy) return false;

    char *cur = copy;
    char *mods_s = strsep(&cur, ",");
    char *key_s = mods_s ? strsep(&cur, ",") : NULL;
    char *action_s = key_s ? strsep(&cur, ",") : NULL;
    char *args = cur;

    if (!mods_s || !key_s || !action_s) {
        log_warn("Bind: needs MODS,KEY,ACTION[,ARGS]");
        free(copy);
        return false;
    }

    mods_s = ns_trim(mods_s);
    key_s = ns_trim(key_s);
    action_s = ns_trim(action_s);
    if (args) args = ns_trim(args);

    // NONE | SUPER | SHIFT | CTRL | ALT
    // split on `|` or `+`
    uint32_t mods = 0;
    {
        char *mcur = mods_s, *tok;
        while ((tok = strsep(&mcur, "|+")) != NULL) {
            tok = ns_trim(tok);
            if (*tok == '\0') continue;
            if (strcasecmp(tok, "NONE") == 0) { // nothing
            } else if (strcasecmp(tok, "SUPER") == 0
                       || strcasecmp(tok, "LOGO") == 0)
                mods |= WLR_MODIFIER_LOGO;
            else if (strcasecmp(tok, "SHIFT") == 0)
                mods |= WLR_MODIFIER_SHIFT;
            else if (strcasecmp(tok, "CTRL") == 0)
                mods |= WLR_MODIFIER_CTRL;
            else if (strcasecmp(tok, "ALT") == 0)
                mods |= WLR_MODIFIER_ALT;
            else {
                log_warn("Bind: unknown mod '%s'", tok);
                free(copy);
                return false;
            }
        }
    }

    xkb_keysym_t key = xkb_keysym_from_name(key_s, XKB_KEYSYM_CASE_INSENSITIVE);
    if (key == XKB_KEY_NoSymbol) {
        log_warn("Bind: unknown key '%s'", key_s);
        free(copy);
        return false;
    }

    enum Action action = ACTION_NONE;
    bool needs_arg = false;

    if (strcmp(action_s, "spawn") == 0) {
        action = ACTION_SPAWN_SHELL;
        needs_arg = true;
    } else if (strcmp(action_s, "close") == 0) {
        action = ACTION_CLOSE;
    } else if (strcmp(action_s, "focus_next") == 0) {
        action = ACTION_FOCUS_NEXT;
    } else if (strcmp(action_s, "cycle_layout") == 0) {
        action = ACTION_CYCLE_LAYOUT;
    } else if (strcmp(action_s, "exit") == 0) {
        action = ACTION_EXIT;
    } else if (strcmp(action_s, "fullscreen") == 0) {
        action = ACTION_FULLSCREEN;
    } else if (strcmp(action_s, "move") == 0) {
        action = ACTION_MOVE;
    } else if (strcmp(action_s, "resize") == 0) {
        action = ACTION_RESIZE;
    } else if (strncmp(action_s, "space_", 6) == 0) {
        char *end = NULL;
        long n = strtol(action_s + 6, &end, 10);
        if (end != action_s + 6 && *end == '\0' && n >= 0 && n <= 9) {
            action = (enum Action)(ACTION_SPACE_0 + n);
        }
    } else if (strncmp(action_s, "move_to_space_", 14) == 0) {
        char *end = NULL;
        long n = strtol(action_s + 14, &end, 10);
        if (end != action_s + 14 && *end == '\0' && n >= 0 && n <= 9) {
            action = (enum Action)(ACTION_MOVE_TO_SPACE_0 + n);
        }
    }

    if (action == ACTION_NONE) {
        log_warn("Bind: unknown action '%s'", action_s);
        free(copy);
        return false;
    }

    char *arg = NULL;
    if (needs_arg) {
        if (!args || *args == '\0') {
            log_warn("Bind: action '%s' needs an argument", action_s);
            free(copy);
            return false;
        }
        arg = strdup(args);
        if (!arg) {
            free(copy);
            return false;
        }
    } else if (args && *args != '\0') {
        log_warn("Bind: action '%s' ignores extra args", action_s);
    }

    bool ok = add_bind(mods, key, action, arg);
    free(copy);
    return ok;
}

static void build_binds(void) {
    static const struct KeyDef builtin_binds[] = {CFG_KEYBINDS};
    for (size_t i = 0; i < sizeof(builtin_binds) / sizeof(builtin_binds[0]);
         i++) {
        if (has_bind(builtin_binds[i].mods, builtin_binds[i].key)) continue;
        add_bind(
            builtin_binds[i].mods, builtin_binds[i].key,
            builtin_binds[i].action, NULL
        );
    }

    static const struct {
        xkb_keysym_t key;
        char **cmd;
    } spawn_slots[] = {
        {XKB_KEY_Return, &config.cmd_terminal},
        {    XKB_KEY_F1,       &config.cmd_f1},
        {    XKB_KEY_F2,       &config.cmd_f2},
        {    XKB_KEY_F3,       &config.cmd_f3},
        {    XKB_KEY_F4,       &config.cmd_f4},
        {    XKB_KEY_F5,       &config.cmd_f5},
        {    XKB_KEY_F6,       &config.cmd_f6},
        {    XKB_KEY_F7,       &config.cmd_f7},
        {    XKB_KEY_F8,       &config.cmd_f8},
        {    XKB_KEY_F9,       &config.cmd_f9},
        {   XKB_KEY_F10,      &config.cmd_f10},
    };

    for (size_t i = 0; i < sizeof(spawn_slots) / sizeof(spawn_slots[0]); i++) {
        if (has_bind(CFG_MOD_SUPER, spawn_slots[i].key)) continue;
        const char *cmd = *spawn_slots[i].cmd;
        if (!cmd || *cmd == '\0') continue;
        char *dup = strdup(cmd);
        if (!dup) continue;

        add_bind(CFG_MOD_SUPER, spawn_slots[i].key, ACTION_SPAWN_SHELL, dup);
    }
}

static void
parse_key(const char *key, const char *value, bool *seen_window_rule) {
    long l;
    bool b;
    float f;
    enum Layout layout;

    if (strcmp(key, "default_layout") == 0) {
        if (ns_parse_layout_name(value, &layout))
            config.default_layout = layout;
        else
            log_warn("Bad default_layout '%s'", value);
        return;
    }
    if (strcmp(key, "gaps") == 0) {
        if (ns_parse_int(value, &l) && l >= 0 && l <= 1000)
            config.gaps = (int)l;
        else
            log_warn("Bad gaps '%s'", value);
        return;
    }
    if (strcmp(key, "cursor_size") == 0) {
        if (ns_parse_int(value, &l) && l >= 0 && l <= 1000)
            config.cursor_size = (int)l;
        else
            log_warn("Bad cursor size '%s'", value);
        return;
    }
    if (strcmp(key, "smart_gaps") == 0) {
        if (parse_bool(value, &b))
            config.smart_gaps = b;
        else
            log_warn("Bad smart_gaps '%s'", value);
        return;
    }
    if (strcmp(key, "nmasters") == 0) {
        if (ns_parse_int(value, &l) && l >= 1 && l <= 1024)
            config.nmasters = (int)l;
        else
            log_warn("Bad nmasters '%s'", value);
        return;
    }
    if (strcmp(key, "mfact") == 0) {
        if (ns_parse_float(value, &f) && f > 0.0f && f < 1.0f)
            config.mfact = f;
        else
            log_warn("Bad mfact '%s'", value);
        return;
    }
    if (strcmp(key, "center_overspread") == 0) {
        if (parse_bool(value, &b))
            config.center_overspread = b;
        else
            log_warn("Bad center_overspread '%s'", value);
        return;
    }
    if (strcmp(key, "center_when_single_stack") == 0) {
        if (parse_bool(value, &b))
            config.center_when_single_stack = b;
        else
            log_warn("Bad center_when_single_stack '%s'", value);
        return;
    }

    if (strcmp(key, "cmd_start") == 0) {
        set_string(&config.cmd_start, value);
        return;
    }
    if (strcmp(key, "cursor_theme") == 0) {
        set_string(&config.cursor_theme, value);
        return;
    }
    if (strcmp(key, "cmd_terminal") == 0) {
        set_string(&config.cmd_terminal, value);
        return;
    }
    if (strcmp(key, "cmd_f1") == 0) {
        set_string(&config.cmd_f1, value);
        return;
    }
    if (strcmp(key, "cmd_f2") == 0) {
        set_string(&config.cmd_f2, value);
        return;
    }
    if (strcmp(key, "cmd_f3") == 0) {
        set_string(&config.cmd_f3, value);
        return;
    }
    if (strcmp(key, "cmd_f4") == 0) {
        set_string(&config.cmd_f4, value);
        return;
    }
    if (strcmp(key, "cmd_f5") == 0) {
        set_string(&config.cmd_f5, value);
        return;
    }
    if (strcmp(key, "cmd_f6") == 0) {
        set_string(&config.cmd_f6, value);
        return;
    }
    if (strcmp(key, "cmd_f7") == 0) {
        set_string(&config.cmd_f7, value);
        return;
    }
    if (strcmp(key, "cmd_f8") == 0) {
        set_string(&config.cmd_f8, value);
        return;
    }
    if (strcmp(key, "cmd_f9") == 0) {
        set_string(&config.cmd_f9, value);
        return;
    }
    if (strcmp(key, "cmd_f10") == 0) {
        set_string(&config.cmd_f10, value);
        return;
    }

    if (strcmp(key, "window_rule") == 0) {
        if (!*seen_window_rule) {
            clear_window_rules();
            *seen_window_rule = true;
        }

        if (*value != '\0') append_window_rule(value);

        return;
    }

    if (strcmp(key, "focus_follow") == 0) {
        if (parse_bool(value, &b))
            config.focus_follow = b;
        else
            log_warn("Bad focus_follow '%s'", value);
        return;
    }

    if (strcmp(key, "focus_on_activate") == 0) {
        if (parse_bool(value, &b))
            config.focus_on_activate = b;
        else
            log_warn("Bad focus_on_activate '%s'", value);
        return;
    }

    if (strcmp(key, "kb_options") == 0) {
        set_string(&config.kb_options, value);
        return;
    }

    if (strcmp(key, "kb_layouts") == 0) {
        set_string(&config.kb_layouts, value);
        return;
    }

    if (strcmp(key, "kb_rate_hz") == 0) {
        if (ns_parse_int(value, &l) && l > 0 && l <= 1000)
            config.kb_rate_hz = (int)l;
        else
            log_warn("Bad kb_rate_hz '%s'", value);
        return;
    }

    if (strcmp(key, "kb_delay_ms") == 0) {
        if (ns_parse_int(value, &l) && l >= 0 && l <= 10000)
            config.kb_delay_ms = (int)l;
        else
            log_warn("Bad kb_delay_ms '%s'", value);
        return;
    }

    if (strcmp(key, "libinput_tap_state") == 0) {
        if (ns_parse_int(value, &l))
            config.libinput_tap_state = (int)l;
        else
            log_warn("Bad libinput_tap_state '%s'", value);
        return;
    }
    if (strcmp(key, "libinput_natural_scroll") == 0) {
        if (ns_parse_int(value, &l))
            config.libinput_natural_scroll = (int)l;
        else
            log_warn("Bad libinput_natural_scroll '%s'", value);
        return;
    }

    if (strcmp(key, "libinput_left_handed") == 0) {
        if (ns_parse_int(value, &l))
            config.libinput_left_handed = (int)l;
        else
            log_warn("Bad libinput_left_handed '%s'", value);
        return;
    }

    if (strcmp(key, "libinput_middle_emulation") == 0) {
        if (ns_parse_int(value, &l))
            config.libinput_middle_emulation = (int)l;
        else
            log_warn("Bad libinput_middle_emulation '%s'", value);
        return;
    }
    if (strcmp(key, "libinput_dwt") == 0) {
        if (ns_parse_int(value, &l))
            config.libinput_dwt = (int)l;
        else
            log_warn("Bad libinput_dwt '%s'", value);
        return;
    }

    if (strcmp(key, "libinput_drag") == 0) {
        if (ns_parse_int(value, &l))
            config.libinput_drag = (int)l;
        else
            log_warn("Bad libinput_drag '%s'", value);
        return;
    }

    if (strcmp(key, "libinput_drag_lock") == 0) {
        if (ns_parse_int(value, &l))
            config.libinput_drag_lock = (int)l;
        else
            log_warn("Bad libinput_drag_lock '%s'", value);
        return;
    }

    if (strcmp(key, "libinput_three_finger_drag") == 0) {
        if (ns_parse_int(value, &l))
            config.libinput_three_finger_drag = (int)l;
        else
            log_warn("Bad libinput_three_finger_drag '%s'", value);
        return;
    }

    if (strcmp(key, "libinput_accel_profile") == 0) {
        if (ns_parse_int(value, &l))
            config.libinput_accel_profile = (int)l;
        else
            log_warn("Bad libinput_accel_profile '%s'", value);
        return;
    }

    if (strcmp(key, "libinput_accel_speed") == 0) {
        if (ns_parse_float(value, &f))
            config.libinput_accel_speed = f;
        else
            log_warn("Bad libinput_accel_speed '%s'", value);
        return;
    }

    if (strcmp(key, "libinput_click_method") == 0) {
        if (ns_parse_int(value, &l))
            config.libinput_click_method = (int)l;
        else
            log_warn("Bad libinput_click_method '%s'", value);
        return;
    }

    if (strcmp(key, "libinput_scroll_method") == 0) {
        if (ns_parse_int(value, &l))
            config.libinput_scroll_method = (int)l;
        else
            log_warn("Bad libinput_scroll_method '%s'", value);
        return;
    }

#ifdef WALLPAPER
    if (strcmp(key, "wallpaper_path") == 0) {
        set_string(&config.wallpaper_path, value);
        return;
    }
#endif
#ifdef XWAYLAND
    if (strcmp(key, "xwayland") == 0) {
        if (parse_bool(value, &b))
            config.xwayland = b;
        else
            log_warn("Bad xwayland '%s'", value);
        return;
    }
#endif

    if (strcmp(key, "bind") == 0) {
        parse_bind(value);
        return;
    }

    log_warn("Unknown key '%s'", key);
}

static bool config_path(char *buf, size_t size) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        if (strlcpy(buf, xdg, size) >= size) return false;
        if (strlcat(buf, "/nullspace/config", size) >= size) return false;

        return true;
    }

    const char *home = getenv("HOME");
    if (!home || !*home) return false;
    if (strlcpy(buf, home, size) >= size) return false;
    if (strlcat(buf, "/.config/nullspace/config", size) >= size) return false;

    return true;
}

static bool ensure_parent_dir(const char *path) {
    char tmp[PATH_MAX];
    if (strlcpy(tmp, path, sizeof(tmp)) >= sizeof(tmp)) return false;

    char *slash = strrchr(tmp, '/');
    if (!slash || slash == tmp) return false;
    *slash = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(tmp, 0700) < 0 && errno != EEXIST) return false;
        *p = '/';
    }

    if (mkdir(tmp, 0700) < 0 && errno != EEXIST) return false;
    return true;
}

static bool write_default_config(const char *path) {
    if (!ensure_parent_dir(path)) {
        log_warn("Cannot create directory for '%s'", path);
        return false;
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        if (errno != EEXIST) log_warn("open('%s')", path);
        return false;
    }

    bool ok = true;

    ok = ok && w_str(fd, "# nullspace configuration\n\n");

    ok = ok
      && w_kv_str(fd, "default_layout", ns_layout_name(config.default_layout));
    ok = ok && w_kv_long(fd, "gaps", config.gaps);
    ok = ok && w_kv_long(fd, "cursor_size", config.cursor_size);
    ok = ok && w_kv_bool(fd, "smart_gaps", config.smart_gaps);
    ok = ok && w_kv_long(fd, "nmasters", config.nmasters);
    ok = ok && w_kv_float(fd, "mfact", (double)config.mfact, 3);
    ok = ok && w_kv_bool(fd, "center_overspread", config.center_overspread);
    ok = ok
      && w_kv_bool(
             fd, "center_when_single_stack", config.center_when_single_stack
      );

    ok = ok && w_str(fd, "\n");
    ok = ok && w_kv_str(fd, "cmd_start", config.cmd_start);
    ok = ok && w_kv_str(fd, "cursor_theme", config.cursor_theme);
    ok = ok && w_kv_str(fd, "cmd_terminal", config.cmd_terminal);
    ok = ok && w_kv_str(fd, "cmd_f1", config.cmd_f1);
    ok = ok && w_kv_str(fd, "cmd_f2", config.cmd_f2);
    ok = ok && w_kv_str(fd, "cmd_f3", config.cmd_f3);
    ok = ok && w_kv_str(fd, "cmd_f4", config.cmd_f4);
    ok = ok && w_kv_str(fd, "cmd_f5", config.cmd_f5);
    ok = ok && w_kv_str(fd, "cmd_f6", config.cmd_f6);
    ok = ok && w_kv_str(fd, "cmd_f7", config.cmd_f7);
    ok = ok && w_kv_str(fd, "cmd_f8", config.cmd_f8);
    ok = ok && w_kv_str(fd, "cmd_f9", config.cmd_f9);
    ok = ok && w_kv_str(fd, "cmd_f10", config.cmd_f10);

    ok = ok && w_str(fd, "\n");
    for (size_t i = 0; ok && i < config.window_rule_count; i++)
        ok = w_kv_str(fd, "window_rule", config.window_rules[i]);

    ok = ok && w_str(fd, "\n");
    ok = ok && w_kv_bool(fd, "focus_follow", config.focus_follow);
    ok = ok && w_kv_bool(fd, "focus_on_activate", config.focus_on_activate);

    ok = ok && w_str(fd, "\n");
    ok = ok && w_kv_str(fd, "kb_options", config.kb_options);
    ok = ok && w_kv_str(fd, "kb_layouts", config.kb_layouts);
    ok = ok && w_kv_long(fd, "kb_rate_hz", config.kb_rate_hz);
    ok = ok && w_kv_long(fd, "kb_delay_ms", config.kb_delay_ms);

    ok = ok && w_str(fd, "\n");
    ok = ok && w_kv_long(fd, "libinput_tap_state", config.libinput_tap_state);
    ok = ok
      && w_kv_long(
             fd, "libinput_natural_scroll", config.libinput_natural_scroll
      );
    ok = ok
      && w_kv_long(fd, "libinput_left_handed", config.libinput_left_handed);
    ok = ok
      && w_kv_long(
             fd, "libinput_middle_emulation", config.libinput_middle_emulation
      );
    ok = ok && w_kv_long(fd, "libinput_dwt", config.libinput_dwt);
    ok = ok && w_kv_long(fd, "libinput_drag", config.libinput_drag);
    ok = ok && w_kv_long(fd, "libinput_drag_lock", config.libinput_drag_lock);
    ok = ok
      && w_kv_long(
             fd, "libinput_three_finger_drag", config.libinput_three_finger_drag
      );
    ok = ok
      && w_kv_long(fd, "libinput_accel_profile", config.libinput_accel_profile);
    ok = ok
      && w_kv_float(
             fd, "libinput_accel_speed", (double)config.libinput_accel_speed, 3
      );
    ok = ok
      && w_kv_long(fd, "libinput_click_method", config.libinput_click_method);
    ok = ok
      && w_kv_long(fd, "libinput_scroll_method", config.libinput_scroll_method);

    ok = ok && w_str(fd, "\n");
#ifdef WALLPAPER
    ok = ok && w_kv_str(fd, "wallpaper_path", config.wallpaper_path);
#endif
#ifdef XWAYLAND
    ok = ok && w_kv_bool(fd, "xwayland", config.xwayland);
#endif

    static const char footer[] =
        "\n"
        "# bind = MODS,KEY,ACTION[,ARGS...]\n"
        "# MODS: NONE SUPER SHIFT CTRL ALT, combined with | or +\n"
        "# ACTION: spawn (takes shell command), close, focus_next,\n"
        "#         cycle_layout, exit, fullscreen, move, resize,\n"
        "#         space_0..space_9, move_to_space_0..move_to_space_9\n";

    ok = ok && write_all(fd, footer, sizeof(footer) - 1);

    if (close(fd) < 0) ok = false;

    if (!ok) {
        unlink(path);
        log_warn("Failed writing '%s'", path);
        return false;
    }

    log_info("Default config installed to '%s'", path);
    return true;
}

static void parse_config_file(FILE *f) {
    char *line = NULL;
    size_t cap = 0;
    bool seen_window_rule = false;

    while (getline(&line, &cap, f) >= 0) {
        line[strcspn(line, "\r\n")] = '\0';

        char *s = ns_trim(line);
        if (*s == '\0' || *s == '#') continue;

        char *eq = strchr(s, '=');
        if (!eq) {
            log_warn("Ignoring line without '=': %s", s);
            continue;
        }

        *eq = '\0';

        char *key = ns_trim(s);
        char *value = ns_trim(eq + 1);
        if (*key == '\0') continue;

        parse_key(key, value, &seen_window_rule);
    }

    free(line);
}

void config_init(void) {
    if (initialized) return;

    memset(&config, 0, sizeof(config));
    set_defaults();
    initialized = true;

    char path[PATH_MAX];
    if (!config_path(path, sizeof(path))) {
        log_warn("Cannot determine path (using defaults)");
        build_binds();
        return;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT) {
            if (!write_default_config(path))
                log_warn("Using built-in defaults");
        } else {
            log_warn("open('%s'); using built-in defaults", path);
        }

        build_binds();
        return;
    }

    parse_config_file(f);
    if (ferror(f)) log_warn("Read error on '%s'", path);
    fclose(f);

    log_info("Loaded '%s'", path);

    build_binds();
}

void config_finish(void) {
    if (!initialized) return;

    clear_window_rules();

#define FREE_CFG(field)                                                        \
    do {                                                                       \
        free(config.field);                                                    \
        config.field = NULL;                                                   \
    } while (0)

    FREE_CFG(cmd_start);
    FREE_CFG(cursor_theme);
    FREE_CFG(cmd_terminal);
    FREE_CFG(cmd_f1);
    FREE_CFG(cmd_f2);
    FREE_CFG(cmd_f3);
    FREE_CFG(cmd_f4);
    FREE_CFG(cmd_f5);
    FREE_CFG(cmd_f6);
    FREE_CFG(cmd_f7);
    FREE_CFG(cmd_f8);
    FREE_CFG(cmd_f9);
    FREE_CFG(cmd_f10);
    FREE_CFG(kb_options);
    FREE_CFG(kb_layouts);
#ifdef WALLPAPER
    FREE_CFG(wallpaper_path);
#endif
#undef FREE_CFG

    for (size_t i = 0; i < config.bind_count; i++) free(config.binds[i].arg);
    free(config.binds);
    config.binds = NULL;
    config.bind_count = 0;

    initialized = false;
}
