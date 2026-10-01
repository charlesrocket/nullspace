#include "input.h"
#include "ipc.h"
#include "keymap.h"
#include "layouts/horizontal.h"
#include "layouts/layout.h"
#include "layouts/vertical.h"
#include "manage.h"
#ifdef WALLPAPER
#include "wallpaper.h"
#endif

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
        perror("kqueue");
        return 1;
    }

    struct kevent ev;

    EV_SET(&ev, (uintptr_t)wl_fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    if (kevent(kq, &ev, 1, NULL, 0, NULL) < 0) {
        perror("kevent(wayland)");
        close(kq);
        return 1;
    }

    ipc_kqueue_register(kq);

    bool wl_write_armed = false;

    struct kevent events[256];

    while (true) {
        while (wl_display_prepare_read(display) != 0) {
            if (wl_display_dispatch_pending(display) < 0) {
                fprintf(stderr, "Dispatch failed\n");
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
                        perror("kevent(wayland write)");
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
                perror("kevent(wayland write delete)");
                close(kq);
                return 1;
            }

            wl_write_armed = false;
        }

        ipc_flush_pending();

        struct timespec timeout = {.tv_sec = 0, .tv_nsec = 16 * 1000 * 1000};
        struct timespec *timeoutp = animation_active() ? &timeout : NULL;

        int n = kevent(kq, NULL, 0, events, 256, timeoutp);
        if (n < 0) {
            if (errno == EINTR) {
                wl_display_cancel_read(display);
                continue;
            }

            wl_display_cancel_read(display);
            perror("kevent");
            close(kq);
            return 1;
        }

        // Timeout fired with no events: ask for another manage pass so the
        // animation keeps stepping.
        if (n == 0 && animation_active()) { wm_request_manage(); }

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
                            perror("kevent(wayland write delete)");
                            close(kq);
                            return 1;
                        }
                        wl_write_armed = false;
                    } else if (errno != EAGAIN) {
                        wl_display_cancel_read(display);
                        perror("wl_display_flush");
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
                perror("wl_display_read_events");
                close(kq);
                return 1;
            }
        } else {
            // Nothing to read (e.g. woken by POLLOUT only).
            wl_display_cancel_read(display);
        }

        if (wl_display_dispatch_pending(display) < 0) {
            fprintf(stderr, "Dispatch failed\n");
            close(kq);
            return 1;
        }
    }
}

int main(void) {
    struct wl_display *display = wl_display_connect(NULL);

    if (display == NULL) {
        fprintf(stderr, "Failed to connect to Wayland server\n");
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

    if (wl_display_roundtrip(display) < 0) {
        fprintf(stderr, "Roundtrip failed\n");
        return 1;
    }

    if (window_manager_v1 == NULL || xkb_bindings_v1 == NULL) {
        fprintf(
            stderr, "river_window_manager_v1 or river_xkb_bindings_v1 "
                    "not supported by the Wayland server\n"
        );

        return 1;
    }

    if (layer_shell_v1 == NULL) {
        fprintf(
            stderr, "river_layer_shell_v1 not supported by the Wayland "
                    "server (layer surfaces will be unavailable)\n"
        );
    }

    if (compositor == NULL) {
        fprintf(stderr, "wl_compositor not supported by the Wayland server\n");
        return 1;
    }

    if (shm == NULL) {
        fprintf(stderr, "wl_shm not supported by the Wayland server\n");
    }

#ifdef WALLPAPER
    wallpaper_init(&wp, compositor, shm, window_manager_v1);

    if (compositor != NULL && shm != NULL) {
        const char *wallpaper_path = getenv("NSP_WALLPAPER");
        char default_path[4096];

        if (wallpaper_path == NULL && wm.wallpaper.path != NULL) {
            const char *home = getenv("HOME");

            if (home != NULL) {
                snprintf(
                    default_path, sizeof(default_path), "%s/%s", home,
                    wm.wallpaper.path
                );

                wallpaper_path = default_path;
            }
        }

        if (wallpaper_path != NULL) {
            if (!wallpaper_load_ppm(&wp, wallpaper_path)) {
                fprintf(stderr, "Wallpaper: using default pattern\n");
            }
        }
    }
#endif

    river_window_manager_v1_add_listener(window_manager_v1, &wm_listener, NULL);

    int rc = run_event_loop(display);
    ipc_destroy();
    return rc;
}
