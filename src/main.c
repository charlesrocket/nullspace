#include "input.h"
#include "ipc.h"
#include "keymap.h"
#include "log.h"
#include "manager.h"
#ifdef WALLPAPER
#include "wallpaper.h"
#endif

#include <errno.h>
#include <river-input-management-v1-client-protocol.h>
#include <river-layer-shell-v1-client-protocol.h>
#include <river-libinput-config-v1-client-protocol.h>
#include <river-window-management-v1-client-protocol.h>
#include <river-xkb-bindings-v1-client-protocol.h>
#include <river-xkb-config-v1-client-protocol.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/signal.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

#define LOG_TOPIC ""

static void
input_manager_handle_finished(void *data, struct river_input_manager_v1 *obj) {}

static void input_manager_handle_input_device(
    void *data, struct river_input_manager_v1 *obj,
    struct river_input_device_v1 *device
) {}

static const struct river_input_manager_v1_listener input_manager_listener = {
    .finished = input_manager_handle_finished,
    .input_device = input_manager_handle_input_device,
};

static void handle_global(
    void *data, struct wl_registry *registry, uint32_t name,
    const char *interface, uint32_t version
) {
    if (strcmp(interface, river_window_manager_v1_interface.name) == 0) {
        if (version >= 4) {
            window_manager_v1 = wl_registry_bind(
                registry, name, &river_window_manager_v1_interface, 4
            );
        }
    } else if (strcmp(interface, river_xkb_bindings_v1_interface.name) == 0) {
        xkb_bindings_v1 = wl_registry_bind(
            registry, name, &river_xkb_bindings_v1_interface, 1
        );
    } else if (strcmp(interface, river_layer_shell_v1_interface.name) == 0) {
        layer_shell_v1 = wl_registry_bind(
            registry, name, &river_layer_shell_v1_interface, 1
        );
    } else if (strcmp(interface, wl_compositor_interface.name) == 0) {
        compositor =
            wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, river_xkb_config_v1_interface.name) == 0) {
        keymap_bind(registry, name);
    } else if (strcmp(interface, river_libinput_config_v1_interface.name)
               == 0) {
        libinput_bind(registry, name);
    } else if (strcmp(interface, river_input_manager_v1_interface.name) == 0) {
        input_manager_v1 = wl_registry_bind(
            registry, name, &river_input_manager_v1_interface, 2
        );

        river_input_manager_v1_add_listener(
            input_manager_v1, &input_manager_listener, NULL
        );
    }
}

static void
handle_global_remove(void *data, struct wl_registry *registry, uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
    .global = handle_global,
    .global_remove = handle_global_remove,
};

static const struct river_window_manager_v1_listener wm_listener = {
    .unavailable = wm_handle_unavailable,
    .finished = wm_handle_finished,
    .manage_start = wm_handle_manage_start,
    .render_start = wm_handle_render_start,
    .session_locked = wm_handle_session_locked,
    .session_unlocked = wm_handle_session_unlocked,
    .window = wm_handle_window,
    .output = wm_handle_output,
    .seat = wm_handle_seat,
};

static int run_event_loop(struct wl_display *display) {
    int wl_fd = wl_display_get_fd(display);
    int kq = kqueue();
    if (kq < 0) {
        log_err("kqueue: %s", strerror(errno));
        return 1;
    }

    struct kevent ev;

    EV_SET(&ev, (uintptr_t)wl_fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    if (kevent(kq, &ev, 1, NULL, 0, NULL) < 0) {
        log_err("kevent(wayland): %s", strerror(errno));

        close(kq);
        return 1;
    }

    ipc_kqueue_register(kq);
    bool wl_write_armed = false;

    struct kevent events[256];

    while (true) {
        while (wl_display_prepare_read(display) != 0) {
            if (wl_display_dispatch_pending(display) < 0) {
                log_err("dispatch failed");
                close(kq);
                return 1;
            }
        }

        if (wl_display_flush(display) < 0) {
            if (errno == EAGAIN) {
                if (!wl_write_armed) {
                    EV_SET(
                        &ev, (uintptr_t)wl_fd, EVFILT_WRITE, EV_ADD, 0, 0, NULL
                    );

                    if (kevent(kq, &ev, 1, NULL, 0, NULL) < 0) {
                        wl_display_cancel_read(display);
                        log_err("kevent(wayland write): %s", strerror(errno));
                        close(kq);
                        return 1;
                    }

                    wl_write_armed = true;
                }
            } else {
                wl_display_cancel_read(display);
                perror("wl_display_flush");
                close(kq);
                return 1;
            }
        } else if (wl_write_armed) {
            EV_SET(&ev, (uintptr_t)wl_fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
            if (kevent(kq, &ev, 1, NULL, 0, NULL) < 0 && errno != ENOENT) {
                wl_display_cancel_read(display);
                log_err("kevent(wayland write delete): %s", strerror(errno));
                close(kq);
                return 1;
            }

            wl_write_armed = false;
        }

        ipc_flush_pending();

        struct timespec timeout = {.tv_sec = 0, .tv_nsec = 16 * 1000 * 1000};
        struct timespec *timeoutp = wm_animation_active() ? &timeout : NULL;

        int n = kevent(kq, NULL, 0, events, 256, timeoutp);
        if (n < 0) {
            if (errno == EINTR) {
                wl_display_cancel_read(display);
                continue;
            }

            wl_display_cancel_read(display);
            log_err("kevent: %s", strerror(errno));
            close(kq);
            return 1;
        }

        // Timeout fired with no events: ask for another manage pass so the
        // animation keeps stepping.
        if (n == 0 && wm_animation_active()) { wm_request_manage(); }

        bool wl_readable = false;

        for (int i = 0; i < n; i++) {
            const struct kevent *e = &events[i];
            int fd = (int)e->ident;

            if (fd == wl_fd) {
                if (e->filter == EVFILT_READ) {
                    wl_readable = true;
                } else if (e->filter == EVFILT_WRITE) {
                    if (wl_display_flush(display) == 0) {
                        EV_SET(
                            &ev, (uintptr_t)wl_fd, EVFILT_WRITE, EV_DELETE, 0,
                            0, NULL
                        );

                        if (kevent(kq, &ev, 1, NULL, 0, NULL) < 0
                            && errno != ENOENT) {
                            wl_display_cancel_read(display);
                            log_err(
                                "kevent(wayland write delete): %s",
                                strerror(errno)
                            );
                            close(kq);
                            return 1;
                        }

                        wl_write_armed = false;
                    } else if (errno != EAGAIN) {
                        wl_display_cancel_read(display);
                        log_err("wl_display_flush: %s", strerror(errno));
                        close(kq);
                        return 1;
                    }
                }

                continue;
            }

            ipc_kqueue_handle(e);
        }

        if (wl_readable) {
            if (wl_display_read_events(display) < 0) {
                log_err("wl_display_read_events: %s", strerror(errno));
                close(kq);
                return 1;
            }
        } else {
            // Nothing to read (e.g. woken by POLLOUT only).
            wl_display_cancel_read(display);
        }

        if (wl_display_dispatch_pending(display) < 0) {
            log_err("dispatch failed: %s", strerror(errno));
            close(kq);
            return 1;
        }
    }
}

int main(void) {
    log_init();
    struct wl_display *display = wl_display_connect(NULL);

    if (display == NULL) {
        log_err("failed to connect to Wayland server");
        return 1;
    }

    // Avoid passing WAYLAND_DEBUG on to our children.
    // It only matters if it's set when the display is created.
    unsetenv("WAYLAND_DEBUG");
    // Ensure children are automatically reaped.
    signal(SIGCHLD, SIG_IGN);

    wm_init();

    ipc_init();
    keymap_init();
    libinput_init();

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);

    if (wl_display_roundtrip(display) < 0) { // main thread now
        log_err("roundtrip failed: %s", strerror(errno));
        return 1;
    }

    if (window_manager_v1 == NULL || xkb_bindings_v1 == NULL) {
        log_err("river_window_manager_v1/river_xkb_bindings_v1 not "
                "supported by the Wayland server");

        return 1;
    }

    if (layer_shell_v1 == NULL) {
        log_warn("river_layer_shell_v1 not supported by the Wayland "
                 "server (layer surfaces will be unavailable)");
    }

    if (compositor == NULL) {
        log_err("wl_compositor not supported by the Wayland server");
        return 1;
    }

    if (shm == NULL) { log_warn("wl_shm not supported by the Wayland server"); }

#ifdef WALLPAPER
    wallpaper_init(&wp, compositor, shm, window_manager_v1);

    const char *wallpaper_path = getenv("NSP_WALLPAPER");
    wm_set_wallpaper_path(
        wallpaper_path != NULL ? wallpaper_path : wm.wallpaper.path
    );
#endif

    river_window_manager_v1_add_listener(window_manager_v1, &wm_listener, NULL);

    int rc = run_event_loop(display);
    ipc_destroy();
    return rc;
}
