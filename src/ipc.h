#ifndef NULLSPACE_IPC_H
#define NULLSPACE_IPC_H

#include <stdbool.h>
#include <sys/event.h>

struct Window;

void ipc_init(void);
void ipc_destroy(void);

void ipc_kqueue_register(int kq);
void ipc_kqueue_handle(const struct kevent *ev);
void ipc_flush_pending(void);
void ipc_process_pending_commands(void);

void ipc_notify_layout(void);
void ipc_notify_space(void);
void ipc_notify_focus(void);
void ipc_notify_kb_layout(void);
void ipc_notify_window_opened(struct Window *w);
void ipc_notify_window_closed(struct Window *w);
void ipc_notify_window_meta(struct Window *w);
void ipc_notify_all_state(void);

#endif /* NULLSPACE_IPC_H */
