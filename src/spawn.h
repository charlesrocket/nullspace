#pragma once

int spawn(const char *const *argv);
int spawn_shell(const char *cmdline);

void spawn_init(void);
void spawn_finish(void);
