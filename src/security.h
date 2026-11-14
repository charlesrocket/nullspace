#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wayland-util.h>

// Global access control
//
// Two enforcement layers, per wayland display:
//   Layer 0 - sandbox allow-list, underutilized (WIP).
//   Layer 1 - per-permission rules. Per-executable-path (native) or
//             engine/app (sandbox, if any). DEC_DENY hides the global
//             from enumeration; DEC_ASK / DEC_ALLOW expose it, and the
//             use-time hook registered via PROTECT() (a manager whose
//             use hook calls security_check_use()) decides the actual
//             resource creation.
//
// SOCKET PERMISSIONS are the first line of defence.
//
// NATIVE IDENTITY is the absolute path of the peer executable, resolved
// via LOCAL_PEERCRED + KERN_PROC_PATHNAME in `resolve_identity()` (same-uid
// code (LD_PRELOAD, ptrace, an allowed interpreter) can defeat it).
//
// Permissions:
//   screencopy   -- captures pixels (screencopy, image-copy-capture,
//                   output image-copy source)
//   clipboard    -- reads/writes the selection (ext-data-control)
//   cursorpos    -- reads pointer position (relative pointer, pointer
//                   constraints)
//   keyboard     -- creates a virtual keyboard
//   pointer      -- creates a virtual pointer

enum Perm {
    PERM_NONE = -1, // hidden from security-context clients
    PERM_SCREENCOPY,
    PERM_CLIPBOARD,
    PERM_CURSORPOS,
    PERM_KEYBOARD,
    PERM_POINTER,
    PERM_COUNT,
};

enum Decision { DEC_UNSET, DEC_ALLOW, DEC_DENY, DEC_ASK, DEC_COUNT };

extern const char* const perm_names[PERM_COUNT];
extern const char* const perm_actions[PERM_COUNT];
extern const char* const dec_names[DEC_COUNT];
extern const enum Decision default_decisions[PERM_COUNT];

struct Rule {
    struct wl_list link;
    enum Decision dec[PERM_COUNT];
    int64_t snooze_until[PERM_COUNT]; // CLOCK_MONOTONIC seconds
    char exe[];                       // ID (absolute path / sandbox id / "*")
};

struct Prompt {
    struct wl_list link;
    struct wl_event_source* source;
    int fd;    // dialog pipe read end
    pid_t pid; // group leader (POSIX_SPAWN_SETPGROUP)
    enum Perm perm;
    char exe[];
};

struct ProtectedGlobal {
    const struct wl_global* global;
    enum Perm perm;
    bool has_use_hook; // ASK cannot be used if `false`
};

#define MAX_PROTECTED 32

#ifndef PROTECT
#define PROTECT(manager, perm)                                                 \
    do {                                                                       \
        if ((manager) != NULL                                                  \
            && !security_protect_global((manager)->global, (perm))) {          \
            return -1;                                                         \
        }                                                                      \
    } while (0)
#endif

#ifndef PROTECT_GLOBAL
#define PROTECT_GLOBAL(global, perm)                                           \
    do {                                                                       \
        if ((global) != NULL && !security_protect_global((global), (perm))) {  \
            return -1;                                                         \
        }                                                                      \
    } while (0)
#endif

enum Decision security_check_use(struct wl_client* client, enum Perm perm);

bool security_permission_set(
    const char* id, const char* type, const char* mode
);

void security_resume_prompts(void);
void security_permission_finish(void);
bool security_protect_global(const struct wl_global* global, enum Perm perm);

void security_sandbox_add(struct wl_global* global);
void security_sandbox_remove(struct wl_global* global);

bool security_filter_global(
    const struct wl_client* client, const struct wl_global* global, void* data
);
