#include "spawn.h"

#include "activation.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signal.h>
#include <sys/syslimits.h>
#include <sys/unistd.h>
#include <unistd.h>

#define LOG_TOPIC "spawn"
#define ACT_LEN   (sizeof(ACT_PREFIX) - 1)
#define SID_LEN   (sizeof(SID_PREFIX) - 1)

extern char **environ;

static const char ACT_PREFIX[] = "XDG_ACTIVATION_TOKEN=";
static const char SID_PREFIX[] = "DESKTOP_STARTUP_ID=";

struct SpawnEnv {
    char **envp;
    char *token_activation;
    char *token_startup;
};

static void restore_signals(void) {
    sigset_t set;
    sigemptyset(&set);
    sigprocmask(SIG_SETMASK, &set, NULL);
    signal(SIGPIPE, SIG_DFL);
}

static bool resolve_path(const char *cmd, char *out, size_t out_size) {
    if (cmd == NULL || *cmd == '\0') return false;

    if (strchr(cmd, '/') != NULL) {
        if (strlcpy(out, cmd, out_size) >= out_size) return false;
        return access(out, X_OK) == 0;
    }

    const char *path = getenv("PATH");
    if (path == NULL) path = "/usr/local/bin:/usr/bin:/bin";

    const char *p = path;
    while (*p != '\0') {
        const char *end = strchr(p, ':');
        size_t len = end ? (size_t)(end - p) : strlen(p);

        if (len > 0 && len < out_size) {
            int n = snprintf(out, out_size, "%.*s/%s", (int)len, p, cmd);
            if (n > 0 && (size_t)n < out_size && access(out, X_OK) == 0) {
                return true;
            }
        }

        if (end == NULL) break;
        p = end + 1;
    }

    return false;
}

static bool build_env(const char *token_name, struct SpawnEnv *out) {
    out->envp = NULL;
    out->token_activation = NULL;
    out->token_startup = NULL;

    size_t n = 0;
    while (environ[n] != NULL) n++;

    out->envp = calloc(n + 3, sizeof(*out->envp));
    if (out->envp == NULL) return false;

    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        if (strncmp(environ[i], ACT_PREFIX, ACT_LEN) == 0) continue;
        if (strncmp(environ[i], SID_PREFIX, SID_LEN) == 0) continue;

        out->envp[j++] = environ[i];
    }

    if (token_name != NULL) {
        size_t len = strlen(token_name);

        out->token_activation = malloc(ACT_LEN + len + 1);
        out->token_startup = malloc(SID_LEN + len + 1);

        if (out->token_activation == NULL || out->token_startup == NULL) {
            free(out->token_activation);
            free(out->token_startup);
            free(out->envp);

            out->envp = NULL;
            out->token_activation = NULL;
            out->token_startup = NULL;

            return false;
        }

        memcpy(out->token_activation, ACT_PREFIX, ACT_LEN);
        memcpy(out->token_activation + ACT_LEN, token_name, len + 1);

        memcpy(out->token_startup, SID_PREFIX, SID_LEN);
        memcpy(out->token_startup + SID_LEN, token_name, len + 1);

        out->envp[j++] = out->token_activation;
        out->envp[j++] = out->token_startup;
    }

    out->envp[j] = NULL;
    return true;
}

static void free_env(struct SpawnEnv *env) {
    free(env->token_activation);
    free(env->token_startup);
    free(env->envp);

    env->envp = NULL;
    env->token_activation = NULL;
    env->token_startup = NULL;
}

static char **build_argv(const char *const *argv, const char *shell_cmd) {
    if (shell_cmd != NULL) {
        char **out = calloc(4, sizeof(*out));
        if (out == NULL) return NULL;

        out[0] = (char *)"/bin/sh";
        out[1] = (char *)"-c";
        out[2] = (char *)shell_cmd;
        out[3] = NULL;

        return out;
    }

    size_t n = 0;
    while (argv[n] != NULL) n++;

    char **out = malloc((n + 1) * sizeof(*out));
    if (out == NULL) return NULL;

    for (size_t i = 0; i < n; i++) out[i] = (char *)argv[i];
    out[n] = NULL;
    return out;
}

static int do_spawn(const char *const *argv, const char *shell_cmd) {
    if (argv != NULL) {
        if (argv[0] == NULL) {
            log_warn("Empty argv");
            return -1;
        }
    } else if (shell_cmd != NULL) {
        if (shell_cmd[0] == '\0') return -1;
    } else {
        return -1;
    }

    const char *token_name = activation_mint_spawn_token();

    char exe[PATH_MAX];
    if (shell_cmd != NULL) {
        if (strlcpy(exe, "/bin/sh", sizeof(exe)) >= sizeof(exe)) return -1;
    } else {
        if (!resolve_path(argv[0], exe, sizeof(exe))) {
            log_warn("Command not found: %s", argv[0]);
            return -1;
        }
    }

    char **exec_argv = build_argv(argv, shell_cmd);
    if (exec_argv == NULL) return -1;

    struct SpawnEnv env;
    if (!build_env(token_name, &env)) {
        free(exec_argv);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        log_warn("fork(): %s", strerror(errno));
        free_env(&env);
        free(exec_argv);
        return -1;
    }

    if (pid == 0) {
        restore_signals();
        setsid();

        pid_t pid2 = fork();
        if (pid2 == 0) {
            execve(exe, exec_argv, env.envp);
            _exit(127);
        }

        _exit(pid2 < 0 ? 1 : 0);
    }

    free_env(&env);
    free(exec_argv);
    return 0;
}

int spawn(const char *const *argv) { return do_spawn(argv, NULL); }
int spawn_shell(const char *cmdline) { return do_spawn(NULL, cmdline); }
