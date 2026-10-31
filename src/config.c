#include "config.h"

#include "actions.h"
#include "input.h"
#include "log.h"
#include "nullspace/layouts.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <ns-util.h>
#include <stdbool.h>
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

#define LOG_TOPIC        "config"

#define CONFIG_MAX_BYTES (1024L * 1024L)

#ifndef ARRAY_LEN
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#endif

struct Config config;
static bool initialized;

static const char *const default_window_rules[] = {CFG_WINDOW_RULES};

static const struct {
    const char *key;
    char **field;
    const char *def;
} string_keys[] = {
    {     "cmd_start",      &config.cmd_start,      CFG_CMD_START},
    {  "cursor_theme",   &config.cursor_theme,   CFG_CURSOR_THEME},
    {  "cmd_terminal",   &config.cmd_terminal,   CFG_CMD_TERMINAL},
    {        "cmd_f1",         &config.cmd_f1,         CFG_CMD_F1},
    {        "cmd_f2",         &config.cmd_f2,         CFG_CMD_F2},
    {        "cmd_f3",         &config.cmd_f3,         CFG_CMD_F3},
    {        "cmd_f4",         &config.cmd_f4,         CFG_CMD_F4},
    {        "cmd_f5",         &config.cmd_f5,         CFG_CMD_F5},
    {        "cmd_f6",         &config.cmd_f6,         CFG_CMD_F6},
    {        "cmd_f7",         &config.cmd_f7,         CFG_CMD_F7},
    {        "cmd_f8",         &config.cmd_f8,         CFG_CMD_F8},
    {        "cmd_f9",         &config.cmd_f9,         CFG_CMD_F9},
    {       "cmd_f10",        &config.cmd_f10,        CFG_CMD_F10},
    {    "kb_options",     &config.kb_options,     CFG_KB_OPTIONS},
    {    "kb_layouts",     &config.kb_layouts,     CFG_KB_LAYOUTS},
    {"wallpaper_path", &config.wallpaper_path, CFG_WALLPAPER_PATH},
};

static bool set_string(char **dst, const char *value) {
    char *copy = strdup(value ? value : "");
    if (!copy) return false;
    free(*dst);
    *dst = copy;

    return true;
}

static bool parse_bool(const char *key, const char *value, bool *out) {
    static const char *const yes[] = {"1", "true", "on", "yes"};
    static const char *const no[] = {"0", "false", "off", "no"};

    for (size_t i = 0; i < ARRAY_LEN(yes); i++) {
        if (strcasecmp(value, yes[i]) == 0) {
            *out = true;
            return true;
        }

        if (strcasecmp(value, no[i]) == 0) {
            *out = false;
            return true;
        }
    }

    log_warn("Bad %s '%s'", key, value);
    return false;
}

static bool parse_int_in_range(
    const char *key, const char *value, long min, long max, long *out
) {
    long l;
    if (ns_parse_int(value, &l) && l >= min && l <= max) {
        *out = l;
        return true;
    }

    log_warn("Bad %s '%s'", key, value);
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
    if (v != v) return 0;

    if (decimals < 0) decimals = 0;
    if (decimals > 9) decimals = 9;

    bool neg = v < 0.0;
    if (neg) v = -v;

    double scale = 1.0;
    for (int i = 0; i < decimals; i++) scale *= 10.0;

    double scaled_d = v * scale + 0.5;
    if (scaled_d >= (double)ULONG_MAX) return 0;

    unsigned long scaled = (unsigned long)scaled_d;
    unsigned long divisor = (unsigned long)scale;

    if (scaled == 0) neg = false;

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

        if (n == 0) {
            errno = EIO;
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
    if (n == 0) {
        errno = EINVAL;
        return false;
    }

    return w_str(fd, k) && w_str(fd, "=") && write_all(fd, num, n)
        && w_str(fd, "\n");
}

static bool w_kv_float(int fd, const char *k, double v, int decimals) {
    char num[64];
    size_t n = fmt_double(num, sizeof(num), v, decimals);
    if (n == 0) {
        errno = EINVAL;
        return false;
    }

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

    char **rules = reallocarray(
        config.window_rules, config.window_rule_count + 1, sizeof(*rules)
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
    config.mfact = CFG_MFACT;
    config.center_overspread = CFG_CENTER_OVERSPREAD;
    config.center_when_single_stack = CFG_CENTER_WHEN_SINGLE_STACK;

    for (size_t i = 0; i < ARRAY_LEN(string_keys); i++)
        set_string(string_keys[i].field, string_keys[i].def);

    for (size_t i = 0; i < ARRAY_LEN(default_window_rules); i++)
        append_window_rule(default_window_rules[i]);

    config.focus_follow = CFG_FOCUS_FOLLOW;
    config.focus_on_activate = CFG_FOCUS_ON_ACTIVATE;

    config.kb_rate_hz = CFG_KB_RATE_HZ;
    config.kb_delay_ms = CFG_KB_DELAY_MS;

    config.input_tap_state = CFG_LIBINPUT_TAP_STATE;
    config.input_natural_scroll = CFG_LIBINPUT_NATURAL_SCROLL;
    config.input_left_handed = CFG_LIBINPUT_LEFT_HANDED;
    config.input_middle_emulation = CFG_LIBINPUT_MIDDLE_EMULATION;
    config.input_dwt = CFG_LIBINPUT_DWT;
    config.input_drag = CFG_LIBINPUT_DRAG;
    config.input_drag_lock = CFG_LIBINPUT_DRAG_LOCK;
    config.input_three_finger_drag = CFG_LIBINPUT_THREE_FINGER_DRAG;
    config.input_accel_profile = CFG_LIBINPUT_ACCEL_PROFILE;
    config.input_accel_speed = CFG_LIBINPUT_ACCEL_SPD;
    config.input_click_method = CFG_LIBINPUT_CLICK_METHOD;
    config.input_scroll_method = CFG_LIBINPUT_SCROLL_METHOD;

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
        reallocarray(config.binds, config.bind_count + 1, sizeof(*b));
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

// NONE | SUPER | SHIFT | CTRL | ALT, split on `|` or `+`.
// Modifies `mods_s` in place.
static bool parse_mods(char *mods_s, uint32_t *out) {
    uint32_t mods = 0;
    char *mcur = mods_s, *tok;

    while ((tok = strsep(&mcur, "|+")) != NULL) {
        tok = ns_trim(tok);
        if (*tok == '\0') continue;

        if (strcasecmp(tok, "NONE") == 0) {
            continue;
        } else if (strcasecmp(tok, "SUPER") == 0
                   || strcasecmp(tok, "LOGO") == 0) {
            mods |= WLR_MODIFIER_LOGO;
        } else if (strcasecmp(tok, "SHIFT") == 0) {
            mods |= WLR_MODIFIER_SHIFT;
        } else if (strcasecmp(tok, "CTRL") == 0) {
            mods |= WLR_MODIFIER_CTRL;
        } else if (strcasecmp(tok, "ALT") == 0) {
            mods |= WLR_MODIFIER_ALT;
        } else {
            log_warn("Bind: unknown mod '%s'", tok);
            return false;
        }
    }

    *out = mods;
    return true;
}

static bool parse_space_index(const char *s, int *out) {
    if (s[0] < '0' || s[0] > '9' || s[1] != '\0') return false;
    *out = s[0] - '0';
    return true;
}

static bool parse_action(const char *s, enum Action *action, bool *needs_arg) {
    static const struct {
        const char *name;
        enum Action action;
        bool needs_arg;
    } actions[] = {
        {       "spawn",  ACTION_SPAWN_SHELL,  true},
        {       "close",        ACTION_CLOSE, false},
        {  "focus_next",   ACTION_FOCUS_NEXT, false},
        {"cycle_layout", ACTION_CYCLE_LAYOUT, false},
        {        "exit",         ACTION_EXIT, false},
        {  "fullscreen",   ACTION_FULLSCREEN, false},
        {        "move",         ACTION_MOVE, false},
        {      "resize",       ACTION_RESIZE, false},
    };

    *needs_arg = false;

    for (size_t i = 0; i < ARRAY_LEN(actions); i++) {
        if (strcmp(s, actions[i].name) == 0) {
            *action = actions[i].action;
            *needs_arg = actions[i].needs_arg;
            return true;
        }
    }

    int n;

    if (strncmp(s, "space_", 6) == 0 && parse_space_index(s + 6, &n)) {
        *action = (enum Action)(ACTION_SPACE_0 + n);
        return true;
    }

    if (strncmp(s, "move_to_space_", 14) == 0
        && parse_space_index(s + 14, &n)) {
        *action = (enum Action)(ACTION_MOVE_TO_SPACE_0 + n);
        return true;
    }

    return false;
}

// MODS,KEY,ACTION[,ARGS...]
// ARGS may contain commas, everything after the 3rd comma is an argument.
static bool parse_bind_fields(char *line) {
    char *cur = line;
    char *mods_s = strsep(&cur, ",");
    char *key_s = strsep(&cur, ",");
    char *action_s = strsep(&cur, ",");
    char *args = cur;

    if (!key_s || !action_s) {
        log_warn("Bind: needs MODS,KEY,ACTION[,ARGS]");
        return false;
    }

    mods_s = ns_trim(mods_s);
    key_s = ns_trim(key_s);
    action_s = ns_trim(action_s);
    if (args) args = ns_trim(args);

    uint32_t mods;
    if (!parse_mods(mods_s, &mods)) return false;

    xkb_keysym_t key = xkb_keysym_from_name(key_s, XKB_KEYSYM_CASE_INSENSITIVE);
    if (key == XKB_KEY_NoSymbol) {
        log_warn("Bind: unknown key '%s'", key_s);
        return false;
    }

    enum Action action;
    bool needs_arg;
    if (!parse_action(action_s, &action, &needs_arg)) {
        log_warn("Bind: unknown action '%s'", action_s);
        return false;
    }

    char *arg = NULL;
    if (needs_arg) {
        if (!args || *args == '\0') {
            log_warn("Bind: action '%s' needs an argument", action_s);
            return false;
        }

        arg = strdup(args);
        if (!arg) return false;
    } else if (args && *args != '\0') {
        log_warn("Bind: action '%s' ignores extra args", action_s);
    }

    return add_bind(mods, key, action, arg);
}

static bool parse_bind(const char *value) {
    char *copy = strdup(value);
    if (!copy) return false;

    bool ok = parse_bind_fields(copy);
    free(copy);
    return ok;
}

static void build_binds(void) {
    static const struct KeyDef builtin_binds[] = {CFG_KEYBINDS};
    for (size_t i = 0; i < ARRAY_LEN(builtin_binds); i++) {
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

    for (size_t i = 0; i < ARRAY_LEN(spawn_slots); i++) {
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
    for (size_t i = 0; i < ARRAY_LEN(string_keys); i++) {
        if (strcmp(key, string_keys[i].key) == 0) {
            set_string(string_keys[i].field, value);
            return;
        }
    }

    long l;
    bool b;
    float f;

    if (strcmp(key, "default_layout") == 0) {
        enum Layout layout;
        if (ns_parse_layout_name(value, &layout))
            config.default_layout = layout;
        else
            log_warn("Bad default_layout '%s'", value);
    } else if (strcmp(key, "gaps") == 0) {
        if (parse_int_in_range(key, value, 0, 1000, &l)) config.gaps = (int)l;
    } else if (strcmp(key, "cursor_size") == 0) {
        if (parse_int_in_range(key, value, 0, 1000, &l))
            config.cursor_size = (int)l;
    } else if (strcmp(key, "nmasters") == 0) {
        if (parse_int_in_range(key, value, 1, 1024, &l))
            config.nmasters = (int)l;
    } else if (strcmp(key, "mfact") == 0) {
        if (ns_parse_float(value, &f) && f > 0.0f && f < 1.0f)
            config.mfact = f;
        else
            log_warn("Bad mfact '%s'", value);
    } else if (strcmp(key, "smart_gaps") == 0) {
        if (parse_bool(key, value, &b)) config.smart_gaps = b;
    } else if (strcmp(key, "center_overspread") == 0) {
        if (parse_bool(key, value, &b)) config.center_overspread = b;
    } else if (strcmp(key, "center_when_single_stack") == 0) {
        if (parse_bool(key, value, &b)) config.center_when_single_stack = b;
    } else if (strcmp(key, "focus_follow") == 0) {
        if (parse_bool(key, value, &b)) config.focus_follow = b;
    } else if (strcmp(key, "focus_on_activate") == 0) {
        if (parse_bool(key, value, &b)) config.focus_on_activate = b;
    } else if (strcmp(key, "window_rule") == 0) {
        if (!*seen_window_rule) {
            clear_window_rules();
            *seen_window_rule = true;
        }

        if (*value != '\0') append_window_rule(value);
    } else if (strcmp(key, "kb_rate_hz") == 0) {
        if (parse_int_in_range(key, value, 1, 1000, &l))
            config.kb_rate_hz = (int)l;
    } else if (strcmp(key, "kb_delay_ms") == 0) {
        if (parse_int_in_range(key, value, 0, 10000, &l))
            config.kb_delay_ms = (int)l;
    } else if (strcmp(key, "input_tap_state") == 0) {
        if (parse_int_in_range(key, value, INT_MIN, INT_MAX, &l))
            config.input_tap_state = (int)l;
    } else if (strcmp(key, "input_natural_scroll") == 0) {
        if (parse_int_in_range(key, value, INT_MIN, INT_MAX, &l))
            config.input_natural_scroll = (int)l;
    } else if (strcmp(key, "input_left_handed") == 0) {
        if (parse_int_in_range(key, value, INT_MIN, INT_MAX, &l))
            config.input_left_handed = (int)l;
    } else if (strcmp(key, "input_middle_emulation") == 0) {
        if (parse_int_in_range(key, value, INT_MIN, INT_MAX, &l))
            config.input_middle_emulation = (int)l;
    } else if (strcmp(key, "input_dwt") == 0) {
        if (parse_int_in_range(key, value, INT_MIN, INT_MAX, &l))
            config.input_dwt = (int)l;
    } else if (strcmp(key, "input_drag") == 0) {
        if (parse_int_in_range(key, value, INT_MIN, INT_MAX, &l))
            config.input_drag = (int)l;
    } else if (strcmp(key, "input_drag_lock") == 0) {
        if (parse_int_in_range(key, value, INT_MIN, INT_MAX, &l))
            config.input_drag_lock = (int)l;
    } else if (strcmp(key, "input_three_finger_drag") == 0) {
        if (parse_int_in_range(key, value, INT_MIN, INT_MAX, &l))
            config.input_three_finger_drag = (int)l;
    } else if (strcmp(key, "input_accel_profile") == 0) {
        if (parse_int_in_range(key, value, INT_MIN, INT_MAX, &l))
            config.input_accel_profile = (int)l;
    } else if (strcmp(key, "input_accel_speed") == 0) {
        if (!ns_parse_float(value, &f)) {
            log_warn("Bad input_accel_speed '%s'", value);
        } else if (f >= -1.0f && f <= 1.0f) {
            config.input_accel_speed = f;
        } else if (f <= -2.0f) {
            config.input_accel_speed = -2.0f;
        } else {
            log_warn("Bad input_accel_speed '%s'", value);
        }
    } else if (strcmp(key, "input_click_method") == 0) {
        if (parse_int_in_range(key, value, INT_MIN, INT_MAX, &l))
            config.input_click_method = (int)l;
    } else if (strcmp(key, "input_scroll_method") == 0) {
        if (parse_int_in_range(key, value, INT_MIN, INT_MAX, &l))
            config.input_scroll_method = (int)l;
#ifdef XWAYLAND
    } else if (strcmp(key, "xwayland") == 0) {
        if (parse_bool(key, value, &b)) config.xwayland = b;
#endif
    } else if (strcmp(key, "bind") == 0) {
        parse_bind(value);
    } else {
        log_warn("Unknown key '%s'", key);
    }
}

static bool config_path(char *buf, size_t size) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0] == '/') {
        if (strlcpy(buf, xdg, size) >= size) return false;
        if (strlcat(buf, "/nullspace/config", size) >= size) return false;

        return true;
    }

    const char *home = getenv("HOME");
    if (!home || home[0] != '/') return false;
    if (strlcpy(buf, home, size) >= size) return false;
    if (strlcat(buf, "/.config/nullspace/config", size) >= size) return false;

    return true;
}

static bool make_dir(const char *p) {
    if (mkdir(p, 0700) == 0) return true;

    int saved = errno;
    struct stat st;
    if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) return true;

    errno = (saved == EEXIST) ? ENOTDIR : saved;
    return false;
}

static bool ensure_parent_dir(const char *path) {
    char tmp[PATH_MAX];
    if (strlcpy(tmp, path, sizeof(tmp)) >= sizeof(tmp)) {
        errno = ENAMETOOLONG;
        return false;
    }

    char *slash = strrchr(tmp, '/');
    if (!slash) {
        errno = EINVAL;
        return false;
    }

    if (slash == tmp) return true;
    *slash = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (!make_dir(tmp)) return false;
        *p = '/';
    }

    return make_dir(tmp);
}

static bool write_default_config(const char *path) {
    if (!ensure_parent_dir(path)) {
        log_warn("Cannot create directory for '%s': %s", path, strerror(errno));
        return false;
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        if (errno != EEXIST) log_warn("open('%s'): %s", path, strerror(errno));
        return false;
    }

    errno = 0;
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
    ok = ok && w_kv_long(fd, "input_tap_state", config.input_tap_state);
    ok = ok
      && w_kv_long(fd, "input_natural_scroll", config.input_natural_scroll);

    ok = ok && w_kv_long(fd, "input_left_handed", config.input_left_handed);
    ok = ok
      && w_kv_long(fd, "input_middle_emulation", config.input_middle_emulation);

    ok = ok && w_kv_long(fd, "input_dwt", config.input_dwt);
    ok = ok && w_kv_long(fd, "input_drag", config.input_drag);
    ok = ok && w_kv_long(fd, "input_drag_lock", config.input_drag_lock);
    ok = ok
      && w_kv_long(
             fd, "input_three_finger_drag", config.input_three_finger_drag
      );

    ok = ok && w_kv_long(fd, "input_accel_profile", config.input_accel_profile);
    ok = ok
      && w_kv_float(
             fd, "input_accel_speed", (double)config.input_accel_speed, 3
      );

    ok = ok && w_kv_long(fd, "input_click_method", config.input_click_method);
    ok = ok && w_kv_long(fd, "input_scroll_method", config.input_scroll_method);

    ok = ok && w_str(fd, "\n");
    ok = ok && w_kv_str(fd, "wallpaper_path", config.wallpaper_path);
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

    int err = 0;
    if (!ok) err = errno ? errno : EIO;

    if (close(fd) < 0 && ok) {
        ok = false;
        err = errno;
    }

    if (!ok) {
        unlink(path);
        log_warn("Failed writing '%s': %s", path, strerror(err));
        return false;
    }

    log_info("Default config installed to '%s'", path);
    return true;
}

static FILE *open_config(const char *path, bool *missing) {
    *missing = false;

    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
    if (fd < 0) {
        if (errno == ENOENT)
            *missing = true;
        else
            log_warn("open('%s'): %s", path, strerror(errno));
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        log_warn("fstat('%s'): %s", path, strerror(errno));
        close(fd);
        return NULL;
    }

    const char *why = NULL;
    if (!S_ISREG(st.st_mode))
        why = "not a regular file";
    else if (st.st_size > CONFIG_MAX_BYTES)
        why = "exceeds maximum allowed size";

    if (why) {
        log_warn("Ignoring '%s': %s", path, why);
        close(fd);
        return NULL;
    }

    FILE *f = fdopen(fd, "r");
    if (!f) {
        log_warn("fdopen('%s'): %s", path, strerror(errno));
        close(fd);
        return NULL;
    }

    return f;
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

static void load_user_config(const char *path) {
    bool missing;
    FILE *f = open_config(path, &missing);

    if (f) {
        parse_config_file(f);
        if (ferror(f)) log_warn("Read error on '%s'", path);
        fclose(f);

        log_info("Loaded '%s'", path);
        return;
    }

    if (missing && write_default_config(path)) return;

    log_warn("Using built-in defaults");
}

void config_init(void) {
    if (initialized) return;

    memset(&config, 0, sizeof(config));
    set_defaults();
    initialized = true;

    char path[PATH_MAX];
    if (config_path(path, sizeof(path)))
        load_user_config(path);
    else
        log_warn("Cannot determine config path (using built-in defaults)");

    build_binds();
}

void config_finish(void) {
    if (!initialized) return;

    clear_window_rules();

    for (size_t i = 0; i < ARRAY_LEN(string_keys); i++)
        free(*string_keys[i].field);

    for (size_t i = 0; i < config.bind_count; i++) free(config.binds[i].arg);
    free(config.binds);

    memset(&config, 0, sizeof(config));
    initialized = false;
}
