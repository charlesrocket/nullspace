#include "security.h"

#include "config.h"
#include "log.h"
#include "server.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/signal.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/syslimits.h>
#include <sys/types.h>
#include <sys/ucred.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <wayland-util.h>
#include <wlr/types/wlr_security_context_v1.h>
#include <wlr/xwayland.h>
#ifdef XWAYLAND
#include <wlr/xwayland/shell.h>
#endif

#define LOG_TOPIC   "security"
#define MAX_PROMPTS 8
#define PENDING_MAX 64
#define SNOOZE_SECS 60

const char* const perm_names[PERM_COUNT] = {
    [PERM_SCREENCOPY] = "screencopy", [PERM_CLIPBOARD] = "clipboard",
    [PERM_CURSORPOS] = "cursorpos",   [PERM_KEYBOARD] = "keyboard",
    [PERM_POINTER] = "pointer",
};

const char* const perm_actions[PERM_COUNT] = {
    [PERM_SCREENCOPY] = "capture the screen",
    [PERM_CLIPBOARD] = "act as a clipboard manager",
    [PERM_CURSORPOS] = "track the cursor",
    [PERM_KEYBOARD] = "use virtual keyboards",
    [PERM_POINTER] = "control the pointer",
};

const char* const dec_names[DEC_COUNT] = {
    [DEC_ALLOW] = "allow",
    [DEC_DENY] = "deny",
    [DEC_ASK] = "ask",
};

const enum Decision default_decisions[PERM_COUNT] = {
    [PERM_SCREENCOPY] = DEC_ASK, [PERM_CLIPBOARD] = DEC_ASK,
    [PERM_CURSORPOS] = DEC_ASK,  [PERM_KEYBOARD] = DEC_ASK,
    [PERM_POINTER] = DEC_ASK,
};

#define MAX_SANDBOX_GLOBALS 32 // TODO
static struct wl_global* sandbox_globals[MAX_SANDBOX_GLOBALS];
static size_t sandbox_global_count;

struct PendingPrompt {
    struct wl_list link;
    enum Perm perm;
    char exe[];
};

static struct ProtectedGlobal protected_globals[MAX_PROTECTED];
static size_t protected_count;

static struct wl_list rules = {&rules, &rules};
static struct wl_list prompts = {&prompts, &prompts};
static struct wl_list pending_prompts = {&pending_prompts, &pending_prompts};

static size_t prompt_count, pending_count;
static bool draining;

extern char** environ;

static void prompt_free(struct Prompt* p);
static void prompt_drain(void);
static void prompt_start(const char* exe, enum Perm perm);
static int prompt_done(int fd, uint32_t mask, void* data);
static pid_t spawn_perm_dialog(const struct Prompt* p, int stdout_fd);

static const struct ProtectedGlobal* protected_find(const struct wl_global* g) {
    for (size_t i = 0; i < protected_count; i++) {
        if (protected_globals[i].global == g) return &protected_globals[i];
    }
    return NULL;
}

void security_sandbox_add(struct wl_global* global) {
    if (global == NULL) return;
    if (protected_find(global) != NULL) {
        log_err(
            "'%s' is both sandbox-allowed and permission-protected",
            wl_global_get_interface(global)->name
        );

        return;
    }

    if (sandbox_global_count >= MAX_SANDBOX_GLOBALS) {
        log_warn(
            "Sandbox table is full; %s will be hidden from sandboxes",
            wl_global_get_interface(global)->name
        );

        return;
    }

    sandbox_globals[sandbox_global_count++] = global;
}

void security_sandbox_remove(struct wl_global* global) {
    if (global == NULL) return;
    for (size_t i = 0; i < sandbox_global_count; i++) {
        if (sandbox_globals[i] != global) continue;
        sandbox_globals[i] = sandbox_globals[--sandbox_global_count];
        return;
    }
}

static bool sandbox_global_allowed(const struct wl_global* global) {
    if (global == NULL) return false;

    for (size_t i = 0; i < sandbox_global_count; i++) {
        if (sandbox_globals[i] == global) return true;
    }

    return wl_global_get_interface(global) == &wl_shm_interface;
}

static int lookup_name(const char* const names[], int count, const char* name) {
    for (int i = 0; i < count; i++) {
        if (names[i] != NULL && strcmp(names[i], name) == 0) return i;
    }

    return -1;
}

#define ID_PART_MAX 128

static bool id_part_ok(const char* s) {
    size_t n = strlen(s);
    if (n == 0 || n > ID_PART_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
               || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';

        if (!ok) return false;
    }

    return true;
}

static struct Rule* rule_find(const char* exe) {
    struct Rule* r;
    wl_list_for_each(r, &rules, link) {
        if (strcmp(r->exe, exe) == 0) return r;
    }

    return NULL;
}

static struct Rule* rule_create(const char* exe) {
    size_t n = strlen(exe) + 1;
    struct Rule* r = calloc(1, sizeof(*r) + n);
    if (r == NULL) return NULL;
    memcpy(r->exe, exe, n);
    wl_list_insert(&rules, &r->link);
    return r;
}

static bool rule_set(const char* exe, enum Perm perm, enum Decision dec) {
    if (perm < 0 || perm >= PERM_COUNT) return false;

    struct Rule* r = rule_find(exe);
    if (r == NULL) {
        r = rule_create(exe);
        if (r == NULL) return false;
    }

    r->dec[perm] = dec;
    r->snooze_until[perm] = 0;
    return true;
}

static int64_t now_mono_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec;
}

static void rule_reap_expired_snoozes(void) {
    int64_t now = now_mono_s();
    struct Rule *r, *tmp;
    wl_list_for_each_safe(r, tmp, &rules, link) {
        bool has_decision = false;
        bool has_snooze = false;

        for (int p = 0; p < PERM_COUNT; p++) {
            if (r->dec[p] != DEC_UNSET) has_decision = true;
            if (r->snooze_until[p] > now) has_snooze = true;
        }

        if (has_decision || has_snooze) continue;
        wl_list_remove(&r->link);
        free(r);
    }
}

static void rule_snooze(const char* exe, enum Perm perm) {
    rule_reap_expired_snoozes();
    struct Rule* r = rule_find(exe);
    if (r == NULL) {
        r = rule_create(exe);
        if (r == NULL) return;
    }

    r->snooze_until[perm] = now_mono_s() + SNOOZE_SECS;
}

static bool rule_snoozed(const char* exe, enum Perm perm) {
    const struct Rule* r = rule_find(exe);
    return r != NULL && r->snooze_until[perm] > now_mono_s();
}

bool security_permission_set(
    const char* id, const char* type, const char* mode
) {
    if (id == NULL || type == NULL || mode == NULL) return false;
    if (id[0] == '\0' || type[0] == '\0' || mode[0] == '\0') return false;

    int dec = lookup_name(dec_names, DEC_COUNT, mode);
    if (dec < 0) return false;

    if (strcmp(id, "*") != 0 && id[0] != '/') {
        log_warn("Refusing sandbox identity '%s' (sandboxes are blocked)", id);
        return false;
    }

    if (type[0] == '*' && type[1] == '\0') {
        for (int p = 0; p < PERM_COUNT; p++) {
            if (!rule_set(id, (enum Perm)p, (enum Decision)dec)) return false;
        }

        return true;
    }

    int perm = lookup_name(perm_names, PERM_COUNT, type);
    if (perm < 0) return false;

    return rule_set(id, (enum Perm)perm, (enum Decision)dec);
}

static enum Decision rule_lookup(const char* exe, enum Perm perm) {
    if (perm < 0 || perm >= PERM_COUNT) return DEC_DENY;

    const struct Rule* r = rule_find(exe);
    if (r != NULL && r->dec[perm] != DEC_UNSET) return r->dec[perm];

    if (exe[0] != '/') {
        const char* slash = strchr(exe, '/');
        if (slash != NULL && slash != exe && slash[1] != '\0') {
            size_t len = (size_t)(slash - exe);
            if (len <= ID_PART_MAX) {
                char engine[ID_PART_MAX + 1];
                memcpy(engine, exe, len);
                engine[len] = '\0';

                r = rule_find(engine);
                if (r != NULL && r->dec[perm] != DEC_UNSET) {
                    return r->dec[perm];
                }
            }
        }
    }

    r = rule_find("*");
    if (r != NULL && r->dec[perm] != DEC_UNSET) return r->dec[perm];

    return default_decisions[perm];
}

// Enumeration-time visibility
//
// NEVER prompts: the client is only calling `get_registry` and may never bind
// or use this global. The use-time prompt happens at `security_check_use()`.
//
// DENY  -> hidden (client cannot see globals, no use-time check)
// ASK   -> visible (client enumerates, use-time hook prompts).
// ALLOW -> visible (client enumerates, use-time hook passes).
static bool permission_visible(const char* exe, enum Perm perm) {
    if (perm < 0 || perm >= PERM_COUNT) return false;
    return rule_lookup(exe, perm) != DEC_DENY;
}

static bool enqueue_prompt(const char* exe, enum Perm perm) {
    struct Prompt* p;
    wl_list_for_each(p, &prompts, link) {
        if (p->perm == perm && strcmp(p->exe, exe) == 0) return true;
    }

    struct PendingPrompt* pp;
    wl_list_for_each(pp, &pending_prompts, link) {
        if (pp->perm == perm && strcmp(pp->exe, exe) == 0) return true;
    }

    if (pending_count >= PENDING_MAX) return false;

    size_t n = strlen(exe) + 1;
    pp = calloc(1, sizeof(*pp) + n);
    if (pp == NULL) return false;

    memcpy(pp->exe, exe, n);
    pp->perm = perm;
    wl_list_insert(pending_prompts.prev, &pp->link);
    pending_count++;
    return true;
}

static void prompt_free(struct Prompt* p) {
    if (p->pid > 0) {
        int status;
        pid_t r;
        do {
            r = waitpid(p->pid, &status, WNOHANG);
        } while (r < 0 && errno == EINTR);

        if (r == 0) {
            kill(-p->pid, SIGKILL);
            do {
                r = waitpid(p->pid, &status, 0);
            } while (r < 0 && errno == EINTR);
        }
    }

    if (p->source != NULL) wl_event_source_remove(p->source);
    close(p->fd);
    wl_list_remove(&p->link);
    prompt_count--;
    free(p);
    prompt_drain();
}

static void prompt_drain(void) {
    if (draining) return;
    if (server.locked) return;

    draining = true;

    while (prompt_count < MAX_PROMPTS && !wl_list_empty(&pending_prompts)) {
        struct PendingPrompt* pp =
            wl_container_of(pending_prompts.next, pp, link);

        wl_list_remove(&pp->link);
        pending_count--;

        if (rule_lookup(pp->exe, pp->perm) == DEC_ASK) {
            prompt_start(pp->exe, pp->perm);
        }

        free(pp);
    }

    draining = false;
}

void security_resume_prompts(void) { prompt_drain(); }

static bool is_bidi_control(const unsigned char* p) {
    if (p[0] != 0xE2 || p[1] == 0x00) return false;
    if (p[1] == 0x80) return p[2] >= 0xAA && p[2] <= 0xAE;
    if (p[1] == 0x81) return p[2] >= 0xA6 && p[2] <= 0xA9;
    return false;
}

static void copy_sanitized(char* dst, size_t dstlen, const char* src) {
    if (dstlen == 0) return;
    size_t i = 0;
    while (*src != '\0' && i + 1 < dstlen) {
        const unsigned char* p = (const unsigned char*)src;
        if (is_bidi_control(p)) {
            src += 3;
            continue;
        }

        unsigned char c = *p;
        dst[i++] = (c < 0x20 || c == 0x7f || c == '<' || c == '>' || c == '&')
                     ? '?'
                     : (char)c;
        src++;
    }

    dst[i] = '\0';
}

static const char prompt_script[] =
    "/usr/bin/timeout 30 /usr/local/bin/gdbus wait --session --activate "
    "org.freedesktop.Notifications >/dev/null 2>&1 </dev/null; "
    "exec /usr/local/bin/notify-send --urgency=critical "
    "--action=approve='Approve' "
    "--action=allow=Allow "
    "--action=deny=Deny "
    "--action=block='Block' "
    "--expire-time=0 --app-name='Permission request' -- \"$1\"";

static pid_t spawn_perm_dialog(const struct Prompt* p, int stdout_fd) {
    char text[2048];
    char clean[PATH_MAX];
    const char* action = perm_actions[p->perm];

    copy_sanitized(clean, sizeof(clean), p->exe);

    if (clean[0] == '/') {
        const char* slash = strrchr(clean, '/');
        const char* name =
            (slash != NULL && slash[1] != '\0') ? slash + 1 : clean;
        snprintf(
            text, sizeof(text), "%s (%s) wants to %s.", name, clean, action
        );
    } else {
        const char* slash = strchr(clean, '/');
        if (slash != NULL && slash[1] != '\0') {
            size_t engine_len = (size_t)(slash - clean);
            if (engine_len > (size_t)ID_PART_MAX) engine_len = ID_PART_MAX;
            snprintf(
                text, sizeof(text), "%s (%.*s) wants to %s.", slash + 1,
                (int)engine_len, clean, action
            );
        } else {
            snprintf(text, sizeof(text), "%s wants to %s.", clean, action);
        }
    }

    posix_spawn_file_actions_t fa;
    posix_spawnattr_t attr;
    if (posix_spawn_file_actions_init(&fa) != 0) return -1;
    if (posix_spawnattr_init(&attr) != 0) {
        posix_spawn_file_actions_destroy(&fa);
        return -1;
    }

    sigset_t none, def;
    sigemptyset(&none);
    sigemptyset(&def);
    sigaddset(&def, SIGPIPE);
    sigaddset(&def, SIGCHLD);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &def);
    posix_spawnattr_setpgroup(&attr, 0);
    posix_spawnattr_setflags(
        &attr,
        POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP
    );

    bool ok = true;

    // Never inherit the compositor's stdin.
    ok = ok
      && posix_spawn_file_actions_addopen(
             &fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0
         ) == 0;

    ok = ok
      && posix_spawn_file_actions_adddup2(&fa, stdout_fd, STDOUT_FILENO) == 0;

    ok = ok && posix_spawn_file_actions_addclosefrom_np(&fa, 3) == 0;

    if (!ok) {
        posix_spawn_file_actions_destroy(&fa);
        posix_spawnattr_destroy(&attr);
        return -1;
    }

    char* argv[] = {"sh", "-c", (char*)prompt_script, "sh", text, NULL};
    pid_t pid = -1;
    int err = posix_spawn(&pid, "/bin/sh", &fa, &attr, argv, environ);

    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);

    return err == 0 ? pid : -1;
}

static int prompt_done(int fd, uint32_t mask, void* data) {
    (void)mask;

    struct Prompt* p = data;
    char buf[64] = {0};
    ssize_t n;

    do {
        n = read(fd, buf, sizeof(buf) - 1);
    } while (n == -1 && errno == EINTR);

    enum Decision dec = DEC_UNSET;
    bool persist = false;

    if (n > 0) {
        while (n > 0
               && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' '
               )) {
            buf[--n] = '\0';
        }

        if (strcmp(buf, "allow") == 0) {
            dec = DEC_ALLOW;
        } else if (strcmp(buf, "deny") == 0) {
            dec = DEC_DENY;
        } else if (strcmp(buf, "approve") == 0) {
            dec = DEC_ALLOW;
            persist = true;
        } else if (strcmp(buf, "block") == 0) {
            dec = DEC_DENY;
            persist = true;
        }
    }

    enum Perm perm = p->perm;
    char* exe = strdup(p->exe);

    prompt_free(p);

    if (exe == NULL) {
        log_err("OOM (permission answer for %s lost)", perm_names[perm]);
        return 0;
    }

    if (dec == DEC_UNSET) {
        rule_snooze(exe, perm);
        log_info("Permission: no answer for %s '%s'", perm_names[perm], exe);
    } else if (rule_lookup(exe, perm) != DEC_ASK) {
        log_info("Permission: ignoring late answer for '%s'", exe);
    } else {
        bool applied = rule_set(exe, perm, dec);
        bool saved = false;

        if (persist) {
            saved = config_persist_permission(
                exe, perm_names[perm], dec_names[dec]
            );
        }

        if (!applied && !saved) {
            log_err(
                "OOM: %s %s for '%s' lost", dec_names[dec], perm_names[perm],
                exe
            );
        } else if (saved) {
            log_info(
                "Permission: %s %s for '%s' (persisted)", dec_names[dec],
                perm_names[perm], exe
            );
        } else if (persist) {
            log_warn(
                "Permission: %s %s for '%s' (NOT persisted)", dec_names[dec],
                perm_names[perm], exe
            );
        } else {
            log_info(
                "Permission: %s %s for '%s'", dec_names[dec], perm_names[perm],
                exe
            );
        }
    }

    free(exe);
    return 0;
}

static void prompt_start(const char* exe, enum Perm perm) {
    struct Prompt* p;
    wl_list_for_each(p, &prompts, link) {
        if (p->perm == perm && strcmp(p->exe, exe) == 0) return;
    }

    if (prompt_count >= MAX_PROMPTS) {
        if (!enqueue_prompt(exe, perm)) {
            log_warn(
                "Prompt queue full, dropping %s '%s'", perm_names[perm], exe
            );
            rule_snooze(exe, perm);
        }

        return;
    }

    size_t n = strlen(exe) + 1;
    p = calloc(1, sizeof(*p) + n);
    if (p == NULL) {
        rule_snooze(exe, perm);
        return;
    }

    memcpy(p->exe, exe, n);
    p->perm = perm;

    int fds[2];
    if (pipe(fds) != 0) {
        free(p);
        rule_snooze(exe, perm);
        return;
    }

    for (int i = 0; i < 2; i++) {
        if (fds[i] <= STDERR_FILENO) {
            int moved = fcntl(fds[i], F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
            if (moved < 0) {
                close(fds[0]);
                close(fds[1]);
                free(p);
                rule_snooze(exe, perm);
                return;
            }

            close(fds[i]);
            fds[i] = moved;
        }
    }

    int fdflags = fcntl(fds[0], F_GETFD);
    if (fdflags >= 0) (void)fcntl(fds[0], F_SETFD, fdflags | FD_CLOEXEC);
    fdflags = fcntl(fds[1], F_GETFD);
    if (fdflags >= 0) (void)fcntl(fds[1], F_SETFD, fdflags | FD_CLOEXEC);

    p->fd = fds[0];
    wl_list_insert(&prompts, &p->link);
    prompt_count++;

    p->source = wl_event_loop_add_fd(
        server.event_loop, p->fd, WL_EVENT_READABLE, prompt_done, p
    );

    p->pid = p->source != NULL ? spawn_perm_dialog(p, fds[1]) : -1;
    close(fds[1]);

    if (p->pid < 0) {
        log_warn("Failed to start permission dialog for '%s'", exe);
        prompt_free(p);
        rule_snooze(exe, perm);
    }
}

static bool protect_global_impl(
    const struct wl_global* global, enum Perm perm, bool has_use_hook
) {
    if (global == NULL) return true;

    if (protected_count == MAX_PROTECTED) {
        log_err(
            "Protected global table is full (%zu/%d)", protected_count,
            MAX_PROTECTED
        );

        return false;
    }

    protected_globals[protected_count++] = (struct ProtectedGlobal
    ){.global = global, .perm = perm, .has_use_hook = has_use_hook};

    return true;
}

bool security_protect_global(const struct wl_global* global, enum Perm perm) {
    return protect_global_impl(global, perm, true);
}

struct ClientId {
    struct wl_listener destroy;
    char id[PATH_MAX];
};

static void client_id_destroy(struct wl_listener* l, void* data) {
    (void)data;

    struct ClientId* c = wl_container_of(l, c, destroy);
    wl_list_remove(&c->destroy.link);
    free(c);
}

static bool sandbox_identifier(
    const struct wlr_security_context_v1_state* ctx, char* buf, size_t size
) {
    if (ctx->sandbox_engine == NULL || !id_part_ok(ctx->sandbox_engine))
        return false;
    const char* app = ctx->app_id;
    int n;
    if (app != NULL && app[0] != '\0') {
        if (!id_part_ok(app)) return false;
        n = snprintf(buf, size, "%s/%s", ctx->sandbox_engine, app);
    } else {
        n = snprintf(buf, size, "%s", ctx->sandbox_engine);
    }

    return n > 0 && (size_t)n < size;
}

static bool
resolve_identity(const struct wl_client* client, char* buf, size_t size) {
    const struct wlr_security_context_v1_state* ctx =
        wlr_security_context_manager_v1_lookup_client(
            server.security_context_manager, client
        );

    if (ctx != NULL) return sandbox_identifier(ctx, buf, size);

#ifdef XWAYLAND
    if (server.xwayland != NULL && server.xwayland->server->client == client) {
        if (strlcpy(buf, "xwayland", size) >= size) return false;
        return true;
    }
#endif

    int fd = wl_client_get_fd((struct wl_client*)client);
    if (fd < 0) {
        log_warn("wl_client_get_fd failed");
        return false;
    }

    pid_t pid = 0;
    struct xucred cred;
    socklen_t cred_len = sizeof(cred);

    if (getsockopt(fd, SOL_LOCAL, LOCAL_PEERCRED, &cred, &cred_len) == 0
        && cred.cr_version == XUCRED_VERSION && cred.cr_pid > 0) {
        pid = cred.cr_pid;
    }

    if (pid <= 0) {
        log_warn("cannot determine peer pid (cr_pid unavailable); "
                 "native client will be treated as unidentifiable");

        return false;
    }

    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, (int)pid};
    size_t out_len = size;

    if (sysctl(mib, 4, buf, &out_len, NULL, 0) != 0 || out_len <= 1
        || buf[0] != '/') {
        log_warn("pid %d: cannot resolve executable path", (int)pid);
        return false;
    }

    return true;
}

static const char* client_id_cached(struct wl_client* client) {
    struct wl_listener* l =
        wl_client_get_destroy_listener(client, client_id_destroy);
    struct ClientId* c;
    if (l != NULL) {
        c = wl_container_of(l, c, destroy);
        return c->id[0] ? c->id : NULL;
    }
    c = calloc(1, sizeof(*c));
    if (c == NULL) return NULL;
    c->destroy.notify = client_id_destroy;
    wl_client_add_destroy_listener(client, &c->destroy);
    if (!resolve_identity(client, c->id, sizeof(c->id))) c->id[0] = '\0';
    return c->id[0] ? c->id : NULL;
}

static bool client_is_sandboxed(struct wl_client* client) {
    if (server.security_context_manager == NULL) return false;
    return wlr_security_context_manager_v1_lookup_client(
               server.security_context_manager, client
           )
        != NULL;
}

bool security_filter_global(
    const struct wl_client* client, const struct wl_global* global, void* data
) {
    (void)data;

    const bool sandboxed = client_is_sandboxed((struct wl_client*)client);

    // Layer 0 hard policy
    // Fail closed (any global not on the sandbox allow-list is invisible).
    if (sandboxed && !sandbox_global_allowed(global)) return false;

#ifdef XWAYLAND
    // xwayland-shell-v1 may only be bound by the Xwayland server.
    if (server.xwayland != NULL && server.xwayland->shell_v1 != NULL
        && global == server.xwayland->shell_v1->global) {
        return server.xwayland->server != NULL
            && client == server.xwayland->server->client;
    }
#endif

    // Layer 1 per-permission rules
    // Enumeration-only: no prompts here.
    const struct ProtectedGlobal* pg = protected_find(global);
    if (pg == NULL) return true;

    if (pg->perm == PERM_NONE) return !sandboxed;

    if (!config.security_permissions && !sandboxed) return true;

    const char* id = client_id_cached((struct wl_client*)client);
    if (id == NULL) {
        log_warn("Cannot identify client (denying %s)", perm_names[pg->perm]);
        return false;
    }

    // No use-time hook exists for this global, so ASK cannot be utilized.
    // Expose it only on an explicit ALLOW.
    if (!pg->has_use_hook) return rule_lookup(id, pg->perm) == DEC_ALLOW;

    return permission_visible(id, pg->perm);
}

void security_permission_finish(void) {
    struct PendingPrompt *pp, *pptmp;
    wl_list_for_each_safe(pp, pptmp, &pending_prompts, link) {
        wl_list_remove(&pp->link);
        free(pp);
    }

    pending_count = 0;

    struct Prompt *p, *ptmp;
    wl_list_for_each_safe(p, ptmp, &prompts, link) { prompt_free(p); }

    struct Rule *r, *rtmp;
    wl_list_for_each_safe(r, rtmp, &rules, link) {
        wl_list_remove(&r->link);
        free(r);
    }
}

enum Decision security_check_use(struct wl_client* client, enum Perm perm) {
    if (perm < 0 || perm >= PERM_COUNT) return DEC_DENY;

    // The two enforcement paths must agree: when the feature is off, native
    // clients are not prompted and are not denied (potential sandboxed clients
    // keep the normal rules (they are already filtered out of these globals).
    if (!config.security_permissions && !client_is_sandboxed(client))
        return DEC_ALLOW;

    const char* id = client_id_cached(client);
    if (id == NULL) {
        log_warn("Cannot identify client (denying %s)", perm_names[perm]);
        return DEC_DENY;
    }

    enum Decision d = rule_lookup(id, perm);
    if (d == DEC_ASK && !rule_snoozed(id, perm) && !server.locked)
        prompt_start(id, perm);

    return d;
}
