#include "ipc.h"
#include "keymap.h"
#include "log.h"
#include "manager.h"
#include "server.h"
#include "wallpaper.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signal.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/multi.h>
#include <wlr/backend/wayland.h>
#if WLR_HAS_X11_BACKEND
#include <wlr/backend/x11.h>
#endif

#ifndef VERSION
#define VERSION "dirty"
#endif

#define LOG_TOPIC ""

static void create_default_output(struct wlr_backend *backend, void *data) {
    bool *done = data;
    if (*done) { return; }

    if (wlr_backend_is_wl(backend)) {
        if (!wl_list_empty(&server.outputs)) {
            *done = true;
            return;
        }
        wlr_wl_output_create(backend);
        *done = true;
    } else if (wlr_backend_is_headless(backend)) {
        wlr_headless_add_output(backend, 1920, 1080);
        *done = true;
    }
#if WLR_HAS_X11_BACKEND
    else if (wlr_backend_is_x11(backend)) {
        wlr_x11_output_create(backend);
        *done = true;
    }
#endif
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    log_init();
    log_info("Nullspace version %s", VERSION);
    signal(SIGCHLD, SIG_IGN);

    const char *outer_wl = getenv("WAYLAND_DISPLAY");
    char *saved_wl = outer_wl ? strdup(outer_wl) : NULL;

    struct wl_display *display = wl_display_create();
    if (display == NULL) {
        log_err("Failed to create wl_display");
        free(saved_wl);
        return 1;
    }

    setenv("XDG_SESSION_TYPE", "wayland", 1);
    setenv("XDG_CURRENT_DESKTOP", "nullspace", 1);

    wm_init();
    keymap_init();
    keymap_set_layout_callback(wm_set_kb_layout);
#ifdef WALLPAPER
    wallpaper_global_init();
#endif

    if (server_init(display) < 0) {
        log_err("Failed server start");
        wl_display_destroy(display);
        free(saved_wl);
        return 1;
    }

    const char *socket = wl_display_add_socket_auto(display);
    if (socket == NULL) {
        log_err("Failed to create Wayland socket (is XDG_RUNTIME_DIR set?)");
        server_finish();
        wl_display_destroy(display);
        free(saved_wl);
        return 1;
    }

    setenv("WAYLAND_DISPLAY", socket, 1);
    log_info("Display socket is %s", socket);

    ipc_init();
    ipc_attach_event_loop(server.event_loop);

    {
        bool created = false;
        wlr_multi_for_each_backend(
            server.backend, create_default_output, &created
        );

        if (!created) log_info("No default output requested");
    }

#ifdef WALLPAPER
    {
        const char *wp = getenv("NSP_WALLPAPER");
        wm_set_wallpaper_path(wp != NULL ? wp : wm.wallpaper.path);
    }
#endif

    log_info("Entering event loop");
    wl_display_run(display);

    log_info("Shutting down");
    ipc_destroy();
    server_detach_listeners();

    wlr_backend_destroy(server.backend);
    wl_display_destroy_clients(display);

    server_finish();
    keymap_destroy();
    wl_display_destroy(display);

    free(saved_wl);
    return 0;
}
