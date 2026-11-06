#pragma once

#include <nullspace/layouts.h>
#include <stdbool.h>
#include <stdint.h>

struct WindowRuleActions {
    bool has_space;
    int32_t space; // must sync with `wm_switch_space()`

    bool has_blur, blur;
};

void rules_init(void);
void rules_free(void);
bool rules_window_match(
    const char* app_id, const char* title, struct WindowRuleActions* out
);

bool rules_space_layout(int space, enum Layout* out);
