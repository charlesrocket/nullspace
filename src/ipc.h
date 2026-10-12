#ifndef IPC_H
#define IPC_H

#include <stdbool.h>
#include <wayland-server-core.h>

struct Window;

void ipc_init(void);
void ipc_destroy(void);
void ipc_attach_event_loop(struct wl_event_loop *loop);
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

#endif // IPC_H
