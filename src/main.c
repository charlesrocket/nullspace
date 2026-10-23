#include "config.h"
#include "ipc.h"
#include "keymap.h"
#include "log.h"
#include "manager.h"
#include "seat.h"
#include "server.h"
#include "spawn.h"
#ifdef WALLPAPER
#include "wallpaper.h"
#endif
#ifdef XWAYLAND
#include "xwayland.h"
#endif

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

static void spawn_startup(void *d) {
    // TODO
    (void)d;

    seat_spawn((const char *const[]){CFG_CMD_START});
}

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
    setenv("XDG_SESSION_DESKTOP", "Nullspace", 1);
    setenv("XDG_CURRENT_DESKTOP", "Nullspace", 1);

    setenv("QT_WAYLAND_DISABLE_WINDOWDECORATION", "1", 1);
    setenv("QT_QPA_PLATFORMTHEME", "qt6ct", 0);
    setenv("QT_QPA_PLATFORM", "wayland;xcb", 0);
    setenv("GDK_BACKEND", "wayland,x11,*", 0);

    wm_init();
    keymap_init();
    keymap_set_layout_callback(wm_set_kb_layout);

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

#ifdef XWAYLAND
    if (!xwayland_init(display)) { log_warn("XWayland is not ready"); }
#endif

    ipc_init();
    ipc_attach_event_loop(server.event_loop);

#ifdef WALLPAPER
    wallpaper_worker_init();
    wm_set_wallpaper_path(wm.wallpaper.path);
#endif

    {
        bool created = false;
        wlr_multi_for_each_backend(
            server.backend, create_default_output, &created
        );

        if (!created) log_info("No default output requested");
    }

    wl_event_loop_add_idle(server.event_loop, spawn_startup, NULL);
    log_info("Entering event loop");
    wl_display_run(display);

    log_info("Shutting down");

    wl_display_destroy_clients(display);
    ipc_destroy();

#ifdef XWAYLAND
    xwayland_finish();
#endif

    server_detach_listeners();
    wlr_backend_destroy(server.backend);

    server_finish();
    keymap_destroy();
    wl_display_destroy(display);

    free(saved_wl);
    return 0;
}
