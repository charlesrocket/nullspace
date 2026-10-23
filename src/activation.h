#pragma once

struct wlr_xdg_activation_v1;

void activation_init(struct wlr_xdg_activation_v1 *activation);
void activation_finish(void);

const char *activation_mint_spawn_token(void);
