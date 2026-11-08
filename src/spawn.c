#include "spawn.h"

#include "activation.h"
#include "log.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signal.h>
#include <unistd.h>

#define LOG_TOPIC "spawn"

static void restore_signals(void) {
    sigset_t set;
    sigemptyset(&set);
    sigprocmask(SIG_SETMASK, &set, NULL);
    signal(SIGPIPE, SIG_DFL);
}

static int do_spawn(const char* const* argv, const char* shell_cmd) {
    const char* token = activation_mint_spawn_token();

    pid_t pid = fork();
    if (pid < 0) {
        log_warn("fork(): %s", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        restore_signals();
        setsid();

        if (token != NULL) {
            setenv("XDG_ACTIVATION_TOKEN", token, 1);
            setenv("DESKTOP_STARTUP_ID", token, 1);
        }

        if (shell_cmd != NULL) {
            execlp("sh", "sh", "-c", shell_cmd, (char*)NULL);
        } else {
            execvp(argv[0], (char* const*)argv);
        }

        log_warn("Execution failed: %s", strerror(errno));
        _exit(EXIT_FAILURE);
    }

    return 0;
}

int spawn(const char* const* argv) {
    if (argv == NULL || argv[0] == NULL) {
        log_warn("Empty argv");
        return -1;
    }

    return do_spawn(argv, NULL);
}

int spawn_shell(const char* cmdline) {
    if (cmdline == NULL || cmdline[0] == '\0') return -1;
    return do_spawn(NULL, cmdline);
}
