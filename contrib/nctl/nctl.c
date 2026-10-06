#ifndef __BSD_VISIBLE
#define __BSD_VISIBLE 1
#endif

#include "keys.h"
#include "layouts.h"

#include <capsicum_helpers.h>
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/capsicum.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCKET_ENV "NULLSPACE_INSTANCE_SIGNATURE"
#define CMD_MAX    4096

struct CtlCommand {
    const char *name;
    const char *desc;
};

static const struct CtlCommand COMMANDS[] = {
    {        "get <key>",              "query a state key"},
    {"set <key> <value>",     "set a runtime config value"},
    { "switch-space <n>",              "switch to space n"},
    {"move-to-space <n>", "move focused window to space n"},
    {     "cycle-layout",       "cycle to the next layout"},
    {            "close",       "close the focused window"},
    {             "ping",    "check the socket connection"},
    {        "subscribe",           "stream states/events"},
    {      "unsubscribe",                 "stop streaming"},
    {     "raw <string>",            "send a raw IPC line"},
    {    "list <string>", "commands | get | set | layouts"},
};

struct LayoutEntry {
    const char *name;
    const char *desc;
};

static const struct LayoutEntry layout_table[] = {
#define LAYOUT_ENTRY(id, label, desc) {(label), (desc)},
    LAYOUTS_TABLE(LAYOUT_ENTRY)
#undef LAYOUT_ENTRY
};

static const size_t layout_table_len =
    sizeof(layout_table) / sizeof(layout_table[0]);

#define RANGE_STR_INNER_(x) #x
#define RANGE_STR_(x)       RANGE_STR_INNER_(x)
#define RANGE_STR(min, max) RANGE_STR_(min) "," RANGE_STR_(max)

static const struct IpcKeyMeta nctl_keys[] = {
#define IPC_KEY_INT(name, access, after, desc, field, min, max)                \
    {name, IPC_KIND_INT, (access), RANGE_STR(min, max), (desc)},
#define IPC_KEY_FLOAT(name, access, after, desc, field, min, max)              \
    {name, IPC_KIND_FLOAT, (access), RANGE_STR(min, max), (desc)},
#define IPC_KEY_BOOL(name, access, after, range, desc, field)                  \
    {name, IPC_KIND_BOOL, (access), (range), (desc)},
#define IPC_KEY_STR(name, access, after, range, desc, field)                   \
    {name, IPC_KIND_STR, (access), (range), (desc)},
#define IPC_KEY_SPECIAL(name, access, range, desc)                             \
    {name, IPC_KIND_SPECIAL, (access), (range), (desc)},

    KEYS_TABLE(
        IPC_KEY_INT, IPC_KEY_FLOAT, IPC_KEY_BOOL, IPC_KEY_STR, IPC_KEY_SPECIAL
    )
        KEYS_TABLE_WALLPAPER(
            IPC_KEY_INT, IPC_KEY_FLOAT, IPC_KEY_BOOL, IPC_KEY_STR,
            IPC_KEY_SPECIAL
        )

#undef IPC_KEY_INT
#undef IPC_KEY_FLOAT
#undef IPC_KEY_BOOL
#undef IPC_KEY_STR
#undef IPC_KEY_SPECIAL
};

static const size_t nctl_keys_len = sizeof(nctl_keys) / sizeof(nctl_keys[0]);

static void usage(FILE *out) {
    fprintf(
        out, "\033[1mnctl\033[0m v" VERSION "\n"
             "┌─────────┐\n"
             "│NULLSPACE│\n"
             "└─────────┘\n"
             "-  -  -\n"
             "A command line interface for Nullspace WM.\n"
             "List all available commands with `nctl list`.\n"
    );
}

static int append_arg(
    char *buf, size_t *off, size_t cap, const char *arg, const char *sep
) {
    int n = snprintf(buf + *off, cap - *off, "%s%s", sep, arg);
    if (n < 0 || (size_t)n >= cap - *off) return -1;

    *off += (size_t)n;
    return 0;
}

static void translate_cmd(char *s) {
    for (; *s != '\0'; s++) {
        int c = (unsigned char)*s;

        if (c == '-') {
            c = '_';
        } else {
            c = toupper(c);
        }

        *s = (char)c;
    }
}

static void list_commands(void) {
    printf("\033[1m\033[4mCOMMANDS\033[0m\n\n");
    for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); i++) {
        printf("  %-22s %s\n", COMMANDS[i].name, COMMANDS[i].desc);
    }

    putchar('\n');
}

static void list_layouts(void) {
    printf("\033[1m\033[4mLAYOUTS\033[0m\n\n");
    for (size_t i = 0; i < layout_table_len; i++) {
        printf("  %-10s %s\n", layout_table[i].name, layout_table[i].desc);
    }

    putchar('\n');
}

static void list_keys(const char *title, enum IpcAccess w, bool show_range) {
    printf("%s\n", title);

    size_t name_w = 0, desc_w = 0;
    for (size_t i = 0; i < nctl_keys_len; i++) {
        if ((nctl_keys[i].access & w) == 0) { continue; }

        size_t n = strlen(nctl_keys[i].name);
        size_t d = nctl_keys[i].desc ? strlen(nctl_keys[i].desc) : 0;
        if (n > name_w) { name_w = n; }
        if (d > desc_w) { desc_w = d; }
    }

    for (size_t i = 0; i < nctl_keys_len; i++) {
        if ((nctl_keys[i].access & w) == 0) { continue; }

        printf(
            "  %-*s  %s ", (int)name_w, nctl_keys[i].name,
            nctl_keys[i].desc ? nctl_keys[i].desc : ""
        );

        if (show_range && nctl_keys[i].range != NULL) {
            printf("[%s]", nctl_keys[i].range);
        }

        putchar('\n');
    }

    putchar('\n');
}

static void list_all(const char *w) {
    bool all = (w == NULL || *w == '\0');

    if (all || strcmp(w, "commands") == 0) list_commands();
    if (all || strcmp(w, "layouts") == 0) list_layouts();
    if (all || strcmp(w, "get") == 0) {
        list_keys("\033[1m\033[4mGETTERS\033[0m\n", IPC_A_R, false);
    }

    if (all || strcmp(w, "set") == 0) {
        list_keys("\033[1m\033[4mSETTERS\033[0m\n", IPC_A_W, true);
    }
}

int main(int argc, char *argv[]) {
    if (argc >= 2
        && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0
            || strcmp(argv[1], "help") == 0)) {
        usage(stdout);
        return EXIT_SUCCESS;
    }

    if (argc < 2) {
        usage(stderr);
        return EXIT_FAILURE;
    }

    if (strcmp(argv[1], "list") == 0) {
        list_all(argc >= 3 ? argv[2] : NULL);
        return EXIT_SUCCESS;
    }

    const char *socket_path = getenv(SOCKET_ENV);
    if (socket_path == NULL || *socket_path == '\0') {
        fprintf(stderr, "nctl: nullspace socket is not available\n");

        return EXIT_FAILURE;
    }

    const bool raw = (strcmp(argv[1], "raw") == 0);
    bool streaming = false;

    char cmd[CMD_MAX];
    size_t off = 0;

    if (raw) {
        if (argc < 3) {
            fprintf(stderr, "nctl: raw requires a line to send\n");
            return EXIT_FAILURE;
        }

        if (strcmp(argv[2], "SUBSCRIBE") == 0) { streaming = true; }

        for (int i = 2; i < argc; i++) {
            const char *sep = (i == 2) ? "" : " ";
            if (append_arg(cmd, &off, sizeof(cmd), argv[i], sep) < 0) {
                fprintf(stderr, "nctl: command is too long\n");
                return EXIT_FAILURE;
            }
        }
    } else {
        char first[128];
        size_t flen = strlen(argv[1]);
        if (flen >= sizeof(first)) {
            fprintf(stderr, "nctl: command name is too long\n");
            return EXIT_FAILURE;
        }

        memcpy(first, argv[1], flen + 1);

        if (strcmp(first, "subscribe") == 0) {
            memcpy(first, "SUBSCRIBE", sizeof("SUBSCRIBE"));
        } else if (strcmp(first, "unsubscribe") == 0) {
            memcpy(first, "UNSUBSCRIBE", sizeof("UNSUBSCRIBE"));
        } else {
            translate_cmd(first);
        }

        if (strcmp(first, "SUBSCRIBE") == 0) { streaming = true; }

        if (append_arg(cmd, &off, sizeof(cmd), first, "") < 0) {
            fprintf(stderr, "nctl: command is too long\n");
            return EXIT_FAILURE;
        }

        for (int i = 2; i < argc; i++) {
            if (append_arg(cmd, &off, sizeof(cmd), argv[i], " ") < 0) {
                fprintf(stderr, "nctl: command is too long\n");
                return EXIT_FAILURE;
            }
        }
    }

    if (off + 1 >= sizeof(cmd)) {
        fprintf(stderr, "nctl: command is too long\n");
        return EXIT_FAILURE;
    }

    cmd[off++] = '\n';
    cmd[off] = '\0';

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("nctl: socket");
        return EXIT_FAILURE;
    }

    int one = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) < 0) {
        perror("nctl: setsockopt(SO_NOSIGPIPE)");
        close(sock);
        return EXIT_FAILURE;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    if (strlen(socket_path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "nctl: socket path is too long\n");
        close(sock);
        return EXIT_FAILURE;
    }

    memcpy(addr.sun_path, socket_path, strlen(socket_path));

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(
            stderr, "nctl: connect to '%s': %s\n", socket_path, strerror(errno)
        );

        close(sock);
        return EXIT_FAILURE;
    }

    size_t sent = 0;
    while (sent < off) {
        ssize_t n = send(sock, cmd + sent, off - sent, 0);

        if (n < 0) {
            if (errno == EINTR) continue;
            perror("nctl: send");
            close(sock);
            return EXIT_FAILURE;
        }

        if (n == 0) break;
        sent += (size_t)n;
    }

    if (!streaming) { (void)shutdown(sock, SHUT_WR); }

    FILE *stream = fdopen(sock, "r");
    if (stream == NULL) {
        perror("nctl: fdopen");
        close(sock);
        return EXIT_FAILURE;
    }

    char *line = NULL;
    size_t cap = 0;
    int rc = EXIT_SUCCESS;

    while (getline(&line, &cap, stream) != -1) {
        fputs(line, stdout);
        fflush(stdout);

        if (!streaming
            && (strncmp(line, "OK", 2) == 0 || strncmp(line, "ERR", 3) == 0)) {
            break;
        }
    }

    if (ferror(stream)) {
        perror("nctl: recv");
        rc = EXIT_FAILURE;
    }

    free(line);
    fclose(stream);
    return rc;
}
