#include "rules.h"

#include "config.h"
#include "log.h"
#include "manager.h"

#include <errno.h>
#include <regex.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TOPIC "rules"

static const char *const rule_specs[] = {NULL, CFG_WINDOW_RULES};

struct Rule {
    regex_t app_id;
    regex_t title;

    int32_t space;
    size_t rule_count;

    bool has_app_id, has_title, has_space;
};

static struct Rule *rules;
static size_t rule_count;
static bool rules_loaded;

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;

    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t')) end--;

    *end = '\0';
    return s;
}

static bool compile(regex_t *re, const char *pattern, const char *spec) {
    int err = regcomp(re, pattern, REG_EXTENDED | REG_ICASE | REG_NOSUB);
    if (err == 0) return true;

    char msg[128];
    regerror(err, re, msg, sizeof(msg));
    log_err("window rule \"%s\": bad regex: %s", spec, msg);
    return false;
}

static void rule_free(struct Rule *r) {
    if (r->has_app_id) regfree(&r->app_id);
    if (r->has_title) regfree(&r->title);
    memset(r, 0, sizeof(*r));
}

static bool parse_field(char *field, const char *spec, struct Rule *r) {
    field = trim(field);
    if (*field == '\0') return true;

    char *colon = strchr(field, ':');
    if (colon == NULL) {
        log_err("window rule \"%s\": expected key:value", spec);
        return false;
    }

    *colon = '\0';
    const char *key = trim(field);
    const char *val = trim(colon + 1);

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

    log_err("window rule \"%s\": unknown/duplicate key \"%s\"", spec, key);
    return false;
}

static bool parse_rule(const char *spec, struct Rule *r) {
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

    if (ok && !r->has_space) {
        log_err("window rule \"%s\": no action (space:<n>)", spec);
        ok = false;
    }

    if (!ok) rule_free(r);
    return ok;
}

void rules_init(void) {
    if (rules_loaded) return;

    size_t spec_count = sizeof(rule_specs) / sizeof(rule_specs[0]) - 1;
    if (spec_count == 0) {
        rules_loaded = true;
        return;
    }

    rules = calloc(spec_count, sizeof(*rules));
    if (rules == NULL) {
        log_err("OOM");
        return;
    }

    rules_loaded = true;

    for (size_t i = 0; i < spec_count; i++) {
        if (parse_rule(rule_specs[i + 1], &rules[rule_count])) { rule_count++; }
    }

    log_info("loaded %zu/%zu", rule_count, spec_count);
}
void rules_free(void) {
    for (size_t i = 0; i < rule_count; i++) rule_free(&rules[i]);

    free(rules);
    rules = NULL;
    rule_count = 0;
    rules_loaded = false;
}

bool rules_match(
    const char *app_id, const char *title, struct WindowRuleActions *out
) {
    if (out == NULL) return false;

    *out = (struct WindowRuleActions){0};
    if (app_id == NULL) app_id = "";
    if (title == NULL) title = "";

    bool hit = false;
    for (size_t i = 0; i < rule_count; i++) {
        const struct Rule *r = &rules[i];

        if (r->has_app_id && regexec(&r->app_id, app_id, 0, NULL, 0) != 0)
            continue;
        if (r->has_title && regexec(&r->title, title, 0, NULL, 0) != 0)
            continue;

        if (r->has_space) {
            out->has_space = true;
            out->space = r->space;
        }

        hit = true;
    }

    return hit;
}
