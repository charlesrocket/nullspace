#pragma once

#include <stdbool.h>
#include <stdint.h>

struct WindowRuleActions {
    bool has_space;
    int32_t space; // must sync with `wm_switch_space()`
};

void rules_init(void);
void rules_free(void);
bool rules_match(
    const char *app_id, const char *title, struct WindowRuleActions *out
);
