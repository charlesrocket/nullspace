#ifndef __BSD_VISIBLE
#define __BSD_VISIBLE 1
#endif

#include "keymap.h"

#include <fcntl.h>
#include <river-input-management-v1-client-protocol.h>
#include <river-xkb-config-v1-client-protocol.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

struct Keyboard {
    struct river_xkb_keyboard_v1 *obj;
    struct wl_list link;
};

static const char *layout_name = "us";
static struct xkb_context *xkb_context;
static struct river_xkb_config_v1 *xkb_config;
static struct river_xkb_keymap_v1 *xkb_keymap;
static bool keymap_ready;        // success event received
static struct wl_list keyboards; // Keyboard

void keymap_set_layout(const char *layout) {
    if (layout != NULL && layout[0] != '\0') { layout_name = layout; }
}

void keymap_init(void) {
    wl_list_init(&keyboards);
    xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
}

static void
keyboard_handle_removed(void *data, struct river_xkb_keyboard_v1 *obj) {
    struct Keyboard *kb = data;
    river_xkb_keyboard_v1_destroy(kb->obj);
    wl_list_remove(&kb->link);
    free(kb);
}

// Ignored events
static void keyboard_handle_input_device(
    void *data, struct river_xkb_keyboard_v1 *obj,
    struct river_input_device_v1 *device
) {}

static void keyboard_handle_layout(
    void *data, struct river_xkb_keyboard_v1 *obj, uint32_t index,
    const char *name // may be NULL
) {}

static void keyboard_handle_capslock_enabled(
    void *data, struct river_xkb_keyboard_v1 *obj
) {}

static void keyboard_handle_capslock_disabled(
    void *data, struct river_xkb_keyboard_v1 *obj
) {}

static void
keyboard_handle_numlock_enabled(void *data, struct river_xkb_keyboard_v1 *obj) {
}

static void keyboard_handle_numlock_disabled(
    void *data, struct river_xkb_keyboard_v1 *obj
) {}

static const struct river_xkb_keyboard_v1_listener keyboard_listener = {
    .removed = keyboard_handle_removed,
    .input_device = keyboard_handle_input_device,
    .layout = keyboard_handle_layout,
    .capslock_enabled = keyboard_handle_capslock_enabled,
    .capslock_disabled = keyboard_handle_capslock_disabled,
    .numlock_enabled = keyboard_handle_numlock_enabled,
    .numlock_disabled = keyboard_handle_numlock_disabled,
};

static void keymap_handle_success(void *data, struct river_xkb_keymap_v1 *obj) {
    keymap_ready = true;

    struct Keyboard *kb;
    wl_list_for_each(kb, &keyboards, link) {
        river_xkb_keyboard_v1_set_keymap(kb->obj, xkb_keymap);
    }
}

static void keymap_handle_failure(
    void *data, struct river_xkb_keymap_v1 *obj, const char *error_msg
) {
    fprintf(stderr, "Keymap: compositor rejected keymap: %s\n", error_msg);

    keymap_ready = false;
    river_xkb_keymap_v1_destroy(obj);
    xkb_keymap = NULL;
}

static const struct river_xkb_keymap_v1_listener keymap_listener = {
    .success = keymap_handle_success,
    .failure = keymap_handle_failure,
};

static int keymap_shm_create(size_t len) {
    int fd = shm_open(SHM_ANON, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) { return -1; }

    if (ftruncate(fd, (off_t)len) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static void keymap_shm_seal(int fd) {
    (void)fcntl(
        fd, F_ADD_SEALS,
        F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL
    );
}

static struct river_xkb_keymap_v1 *keymap_create(void) {
    struct xkb_rule_names names = {0};
    names.layout = layout_name;

    struct xkb_keymap *keymap = xkb_keymap_new_from_names2(
        xkb_context, &names, XKB_KEYMAP_FORMAT_TEXT_V2,
        XKB_KEYMAP_COMPILE_NO_FLAGS
    );

    if (keymap == NULL) {
        fprintf(
            stderr, "Keymap: failed to compile layout \"%s\"\n", layout_name
        );
        return NULL;
    }

    char *str = xkb_keymap_get_as_string2(
        keymap, XKB_KEYMAP_FORMAT_TEXT_V2, XKB_KEYMAP_SERIALIZE_NO_FLAGS
    );

    xkb_keymap_unref(keymap);

    if (str == NULL) {
        fprintf(stderr, "Keymap: failed to serialize keymap\n");
        return NULL;
    }

    size_t len = strlen(str) + 1;
    int fd = keymap_shm_create(len);
    if (fd < 0) {
        fprintf(stderr, "Keymap: failed to create shm fd\n");
        free(str);
        return NULL;
    }

    void *map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        fprintf(stderr, "Keymap: mmap failed\n");
        close(fd);
        free(str);
        return NULL;
    }

    memcpy(map, str, len);
    free(str);

    munmap(map, len);

    keymap_shm_seal(fd);

    struct river_xkb_keymap_v1 *obj = river_xkb_config_v1_create_keymap(
        xkb_config, fd, RIVER_XKB_CONFIG_V1_KEYMAP_FORMAT_TEXT_V2
    );

    close(fd);
    return obj;
}

static void
config_handle_finished(void *data, struct river_xkb_config_v1 *obj) {
    river_xkb_config_v1_destroy(obj);
    xkb_config = NULL;
}

static void config_handle_xkb_keyboard(
    void *data, struct river_xkb_config_v1 *obj,
    struct river_xkb_keyboard_v1 *id
) {
    struct Keyboard *kb = calloc(1, sizeof(struct Keyboard));
    kb->obj = id;
    wl_list_insert(keyboards.prev, &kb->link);

    river_xkb_keyboard_v1_add_listener(id, &keyboard_listener, kb);

    if (keymap_ready) { river_xkb_keyboard_v1_set_keymap(id, xkb_keymap); }
}

static const struct river_xkb_config_v1_listener config_listener = {
    .finished = config_handle_finished,
    .xkb_keyboard = config_handle_xkb_keyboard,
};

void keymap_bind(struct wl_registry *registry, uint32_t name) {
    if (xkb_context == NULL) {
        fprintf(stderr, "Keymap: no xkb context\n");
        return;
    }

    xkb_config =
        wl_registry_bind(registry, name, &river_xkb_config_v1_interface, 1);
    river_xkb_config_v1_add_listener(xkb_config, &config_listener, NULL);

    xkb_keymap = keymap_create();
    if (xkb_keymap != NULL) {
        river_xkb_keymap_v1_add_listener(xkb_keymap, &keymap_listener, NULL);
    }
}

void keymap_destroy(void) {
    if (xkb_config != NULL) { river_xkb_config_v1_stop(xkb_config); }
    if (xkb_context != NULL) { xkb_context_unref(xkb_context); }
}
