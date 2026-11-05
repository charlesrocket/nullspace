#include "rules.h"

#include "config.h"
#include "log.h"
#include "output.h"

#include <errno.h>
#include <ns-util.h>
#include <nullspace/layouts.h>
#include <regex.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define LOG_TOPIC "rules"

struct WindowRule {
    regex_t app_id, title;
    int32_t space;

    bool has_app_id, has_title, has_space, has_blur, blur;
};

struct SpaceRule {
    int space;
    enum Layout layout;
};

static struct WindowRule *window_rules;
static struct SpaceRule *space_rules;

static size_t window_rule_count, space_rule_count;

static bool rules_loaded;

static bool compile(regex_t *re, const char *pattern, const char *spec) {
    int err = regcomp(re, pattern, REG_EXTENDED | REG_ICASE | REG_NOSUB);
    if (err == 0) return true;

    char msg[128];
    regerror(err, re, msg, sizeof(msg));
    log_err("window rule \"%s\": bad regex: %s", spec, msg);
    return false;
}

static void window_rule_free(struct WindowRule *r) {
    if (r->has_app_id) regfree(&r->app_id);
    if (r->has_title) regfree(&r->title);
    memset(r, 0, sizeof(*r));
}

static bool parse_bool_value(const char *val, bool *out) {
    if (strcasecmp(val, "1") == 0 || strcasecmp(val, "true") == 0
        || strcasecmp(val, "on") == 0 || strcasecmp(val, "yes") == 0) {
        *out = true;
        return true;
    }

    if (strcasecmp(val, "0") == 0 || strcasecmp(val, "false") == 0
        || strcasecmp(val, "off") == 0 || strcasecmp(val, "no") == 0) {
        *out = false;
        return true;
    }

    return false;
}

static bool parse_field(char *field, const char *spec, struct WindowRule *r) {
    field = ns_trim(field);
    if (*field == '\0') return true;

    char *colon = strchr(field, ':');
    if (colon == NULL) {
        log_err("window rule \"%s\": expected key:value", spec);
        return false;
    }

    *colon = '\0';
    const char *key = ns_trim(field);
    const char *val = ns_trim(colon + 1);

    if (*val == '\0') {
        log_err("window rule \"%s\": empty value for \"%s\"", spec, key);
        return false;
    }

    if (strcmp(key, "appid") == 0 && !r->has_app_id) {
        r->has_app_id = compile(&r->app_id, val, spec);
        return r->has_app_id;
    }

    if (strcmp(key, "title") == 0 && !r->has_title) {
        r->has_title = compile(&r->title, val, spec);
        return r->has_title;
    }

    if (strcmp(key, "space") == 0 && !r->has_space) {
        char *end = NULL;
        errno = 0;
        long n = strtol(val, &end, 10);

        if (errno != 0 || end == val || *end != '\0' || n < 0
            || n >= (long)SPACE_COUNT) {
            log_err(
                "window rule \"%s\": space must be an integer (0-\"%d\")", spec,
                (int)SPACE_COUNT - 1
            );

            return false;
        }

        r->space = (int32_t)n;
        r->has_space = true;
        return true;
    }

    if (strcmp(key, "blur") == 0 && !r->has_blur) {
        bool b;
        if (!parse_bool_value(val, &b)) {
            log_err(
                "window rule \"%s\": blur must be on/off (got \"%s\")", spec,
                val
            );
            return false;
        }

        r->blur = b;
        r->has_blur = true;
        return true;
    }

    log_err("window rule \"%s\": unknown/duplicate key \"%s\"", spec, key);
    return false;
}

static bool parse_space_rule(const char *spec, struct SpaceRule *out) {
    char *copy = strdup(spec);
    if (copy == NULL) {
        log_err("space rule \"%s\": OOM", spec);
        return false;
    }

    int space = -1;
    enum Layout layout = LAYOUT_VERTICAL_TILE;
    bool has_layout = false;
    bool ok = true;

    for (char *cur = copy; ok && cur != NULL;) {
        char *next = strchr(cur, ',');
        if (next != NULL) *next++ = '\0';

        char *colon = strchr(cur, ':');
        if (colon == NULL) {
            log_err("space rule \"%s\": expected key:value", spec);
            ok = false;
            break;
        }

        *colon = '\0';
        const char *key = ns_trim(cur);
        const char *val = ns_trim(colon + 1);

        if (*val == '\0') {
            log_err("space rule \"%s\": empty value for \"%s\"", spec, key);
            ok = false;
            break;
        }

        if (strcmp(key, "id") == 0) {
            long l;
            if (!ns_parse_int(val, &l) || l < 0 || l >= SPACE_COUNT) {
                log_err(
                    "space rule \"%s\": id must be 0..%d", spec, SPACE_COUNT - 1
                );
                ok = false;
                break;
            }
            space = (int)l;
        } else if (strcmp(key, "layout") == 0) {
            if (!ns_parse_layout_name(val, &layout)) {
                log_err("space rule \"%s\": unknown layout \"%s\"", spec, val);
                ok = false;
                break;
            }
            has_layout = true;
        } else {
            log_err("space rule \"%s\": unknown key \"%s\"", spec, key);
            ok = false;
        }

        cur = next;
    }

    free(copy);

    if (!ok) return false;

    if (space < 0) {
        log_err("space rule \"%s\": missing id", spec);
        return false;
    }
    if (!has_layout) {
        log_err("space rule \"%s\": missing layout", spec);
        return false;
    }

    out->space = space;
    out->layout = layout;
    return true;
}

static void space_rules_init(void) {
    if (config.space_rule_count == 0) return;

    space_rules = calloc(config.space_rule_count, sizeof(*space_rules));
    if (space_rules == NULL) {
        log_err("space rules: OOM");
        return;
    }

    for (size_t i = 0; i < config.space_rule_count; i++) {
        struct SpaceRule r;
        if (parse_space_rule(config.space_rules[i], &r))
            space_rules[space_rule_count++] = r;
    }

    if (space_rule_count > 0) {
        log_info("Space: %zu/%zu", space_rule_count, config.space_rule_count);
    }
}

bool rules_space_layout(int space, enum Layout *out) {
    // Later rules override earlier ones.
    for (size_t i = space_rule_count; i > 0; i--) {
        if (space_rules[i - 1].space == space) {
            *out = space_rules[i - 1].layout;
            return true;
        }
    }

    return false;
}

static bool parse_rule(const char *spec, struct WindowRule *r) {
    memset(r, 0, sizeof(*r));

    char *copy = strdup(spec);
    if (copy == NULL) {
        log_err("window rule \"%s\": OOM", spec);
        return false;
    }

    bool ok = true;
    for (char *cur = copy; ok && cur != NULL;) {
        char *next = strchr(cur, ',');
        if (next != NULL) *next++ = '\0';

        ok = parse_field(cur, spec, r);
        cur = next;
    }

    free(copy);

    if (ok && !r->has_app_id && !r->has_title) {
        log_err("window rule \"%s\": needs App ID/Title", spec);
        ok = false;
    }

    if (ok && !r->has_space && !r->has_blur) {
        log_err(
            "window rule \"%s\": no action (space:<n> and/or blur:<on|off>)",
            spec
        );

        ok = false;
    }

    if (!ok) window_rule_free(r);
    return ok;
}

void rules_init(void) {
    if (rules_loaded) return;
    rules_loaded = true;

    space_rules_init();

    size_t spec_count = config.window_rule_count;
    if (spec_count == 0) return;

    window_rules = calloc(spec_count, sizeof(*window_rules));
    if (window_rules == NULL) {
        log_err("OOM");
        return;
    }

    for (size_t i = 0; i < spec_count; i++) {
        if (parse_rule(
                config.window_rules[i], &window_rules[window_rule_count]
            ))
            window_rule_count++;
    }

    log_info("Window: %zu/%zu", window_rule_count, spec_count);
}

void rules_free(void) {
    for (size_t i = 0; i < window_rule_count; i++)
        window_rule_free(&window_rules[i]);

    free(window_rules);
    window_rules = NULL;
    window_rule_count = 0;

    free(space_rules);
    space_rules = NULL;
    space_rule_count = 0;

    rules_loaded = false;
}

bool rules_window_match(
    const char *app_id, const char *title, struct WindowRuleActions *out
) {
    if (out == NULL) return false;

    *out = (struct WindowRuleActions){0};
    if (app_id == NULL) app_id = "";
    if (title == NULL) title = "";

    bool hit = false;
    for (size_t i = 0; i < window_rule_count; i++) {
        const struct WindowRule *r = &window_rules[i];

        if (r->has_app_id && regexec(&r->app_id, app_id, 0, NULL, 0) != 0)
            continue;
        if (r->has_title && regexec(&r->title, title, 0, NULL, 0) != 0)
            continue;

        if (r->has_space) {
            out->has_space = true;
            out->space = r->space;
        }

        if (r->has_blur) {
            out->has_blur = true;
            out->blur = r->blur;
        }

        hit = true;
    }

    return hit;
}
