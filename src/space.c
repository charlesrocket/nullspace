#include "space.h"

#include "log.h"
#include "manager.h"
#include "output.h"
#include "server.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wayland-protocols/ext-workspace-v1-enum.h>
#include <wayland-server-core.h>
#include <wayland-util.h>
#include <wlr/types/wlr_ext_workspace_v1.h>

#define LOG_TOPIC "ext-workspace"

static struct wlr_ext_workspace_manager_v1 *workspace_manager;
static struct wlr_ext_workspace_group_handle_v1 *workspace_group;
static struct wlr_ext_workspace_handle_v1 *workspace_handles[SPACE_COUNT];
static struct wl_listener workspace_commit_listener;

static int workspace_space(struct wlr_ext_workspace_handle_v1 *handle) {
    return (int)(intptr_t)handle->data;
}

void ext_workspace_update_states(void) {
    for (int i = 0; i < SPACE_COUNT; i++) {
        struct wlr_ext_workspace_handle_v1 *handle = workspace_handles[i];
        if (handle == NULL) { continue; }

        bool active = (i == wm.current_space);
        wlr_ext_workspace_handle_v1_set_active(handle, active);
        // All nullspace workspaces are visible
        wlr_ext_workspace_handle_v1_set_hidden(handle, false);
        wlr_ext_workspace_handle_v1_set_urgent(handle, wm.space_urgent[i]);
    }
}

static void handle_commit(struct wl_listener *l, void *data) {
    (void)l;

    struct wlr_ext_workspace_v1_commit_event *ev = data;

    struct wlr_ext_workspace_v1_request *req, *tmp;
    wl_list_for_each_safe(req, tmp, ev->requests, link) {
        switch (req->type) {
            case WLR_EXT_WORKSPACE_V1_REQUEST_ACTIVATE:
                if (req->activate.workspace != NULL) {
                    int space = workspace_space(req->activate.workspace);
                    if (space >= 0 && space < SPACE_COUNT) {
                        wm_switch_space(space);
                    }
                }

                break;

            case WLR_EXT_WORKSPACE_V1_REQUEST_DEACTIVATE:
                // Nullspace always keeps exactly one active workspace
                break;

            case WLR_EXT_WORKSPACE_V1_REQUEST_CREATE_WORKSPACE:
            case WLR_EXT_WORKSPACE_V1_REQUEST_ASSIGN:
            case WLR_EXT_WORKSPACE_V1_REQUEST_REMOVE:
                // Not advertised
                break;
        }
    }
}

bool ext_workspace_init(struct wl_display *display) {
    if (workspace_manager != NULL) { return true; }

    workspace_manager = wlr_ext_workspace_manager_v1_create(display, 1);
    if (workspace_manager == NULL) {
        log_err("Failed to create ext-workspace manager");
        return false;
    }

    workspace_group =
        wlr_ext_workspace_group_handle_v1_create(workspace_manager, 0);
    if (workspace_group == NULL) {
        log_err("Failed to create ext-workspace group");
        workspace_manager = NULL;
        return false;
    }

    const uint32_t caps =
        EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE
        | EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_DEACTIVATE;

    for (int i = 0; i < SPACE_COUNT; i++) {
        char id[16];
        (void)snprintf(id, sizeof(id), "%d", i);

        struct wlr_ext_workspace_handle_v1 *handle =
            wlr_ext_workspace_handle_v1_create(workspace_manager, id, caps);

        if (handle == NULL) {
            log_err("Failed to create ext-workspace handle %d", i);
            goto fail;
        }

        handle->data = (void *)(intptr_t)i;
        workspace_handles[i] = handle;

        wlr_ext_workspace_handle_v1_set_group(handle, workspace_group);

        char name[16];
        (void)snprintf(name, sizeof(name), "%d", i);
        wlr_ext_workspace_handle_v1_set_name(handle, name);

        uint32_t coord = (uint32_t)i;
        wlr_ext_workspace_handle_v1_set_coordinates(handle, &coord, 1);
    }

    struct Output *out;
    wl_list_for_each(out, &server.outputs, link) {
        if (!out->removed && out->wlr_output != NULL) {
            wlr_ext_workspace_group_handle_v1_output_enter(
                workspace_group, out->wlr_output
            );
        }
    }

    workspace_commit_listener.notify = handle_commit;
    wl_signal_add(
        &workspace_manager->events.commit, &workspace_commit_listener
    );

    ext_workspace_update_states();
    return true;

fail:
    for (int i = 0; i < SPACE_COUNT; i++) {
        if (workspace_handles[i] != NULL) {
            wlr_ext_workspace_handle_v1_destroy(workspace_handles[i]);
            workspace_handles[i] = NULL;
        }
    }

    wlr_ext_workspace_group_handle_v1_destroy(workspace_group);
    workspace_group = NULL;
    workspace_manager = NULL;
    return false;
}

void ext_workspace_output_enter(struct Output *out) {
    if (workspace_group == NULL || out == NULL || out->wlr_output == NULL) {
        return;
    }

    wlr_ext_workspace_group_handle_v1_output_enter(
        workspace_group, out->wlr_output
    );
}

void ext_workspace_output_leave(struct Output *out) {
    if (workspace_group == NULL || out == NULL || out->wlr_output == NULL) {
        return;
    }

    wlr_ext_workspace_group_handle_v1_output_leave(
        workspace_group, out->wlr_output
    );
}

void ext_workspace_finish(void) {
    if (workspace_manager == NULL) { return; }

    wl_list_remove(&workspace_commit_listener.link);

    for (int i = 0; i < SPACE_COUNT; i++) {
        if (workspace_handles[i] != NULL) {
            wlr_ext_workspace_handle_v1_destroy(workspace_handles[i]);
            workspace_handles[i] = NULL;
        }
    }

    if (workspace_group != NULL) {
        wlr_ext_workspace_group_handle_v1_destroy(workspace_group);
        workspace_group = NULL;
    }

    workspace_manager = NULL;
}
