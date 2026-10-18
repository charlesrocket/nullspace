#include <capsicum_helpers.h>
#include <err.h>
#include <errno.h>
#include <ns-util.h>
#include <nullspace/keys.h>
#include <nullspace/layouts.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/_param.h>
#include <sys/capsicum.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCKET_ENV     "NULLSPACE_INSTANCE_SIGNATURE"
#define CMD_MAX        4096
#define READ_CHUNK     4096
#define IO_TIMEOUT_SEC 5

struct CtlCommand {
    const char *name;
    const char *usage;
    const char *desc;
};

static const struct CtlCommand COMMANDS[] = {
    {          "get",                     "get <key>",          "query state keys"},
    {          "set",             "set <key> <value>", "set runtime config values"},
    {       "config",                        "config",                "dump state"},
    { "switch-space",              "switch-space <n>",             "switch spaces"},
    {"move-to-space",             "move-to-space <n>",      "move focused windows"},
    { "cycle-layout",                  "cycle-layout",  "cycle to the next layout"},
    {        "close",                         "close",  "close the focused window"},
    {         "dpms", "dpms <on|off|toggle> [output]",       "change output state"},
    {         "ping",                          "ping",   "check socket connection"},
    {    "subscribe",                     "subscribe",      "stream states/events"},
    {          "raw",                  "raw <string>",        "send raw IPC lines"},
    {         "list",                 "list <string>",   "show available commands"},
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

#define RANGE_STR_INNER_(x) #x
#define RANGE_STR_(x)       RANGE_STR_INNER_(x)
#define RANGE_STR(min, max) RANGE_STR_(min) "," RANGE_STR_(max)

static const struct IpcKeyMeta nctl_keys[] = {
#define IPC_KEY_INT(name, access, after, desc, field, min, max)                \
    {name, IPC_TYPE_INT, (access), RANGE_STR(min, max), (desc)},
#define IPC_KEY_FLOAT(name, access, after, desc, field, min, max)              \
    {name, IPC_TYPE_FLOAT, (access), RANGE_STR(min, max), (desc)},
#define IPC_KEY_BOOL(name, access, after, range, desc, field)                  \
    {name, IPC_TYPE_BOOL, (access), (range), (desc)},
#define IPC_KEY_STR(name, access, after, range, desc, field)                   \
    {name, IPC_TYPE_STR, (access), (range), (desc)},
#define IPC_KEY_SPECIAL(name, access, range, desc)                             \
    {name, IPC_TYPE_SPECIAL, (access), (range), (desc)},

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

struct Style {
    const char *bold;
    const char *head;
    const char *reset;
};

static struct Style style_for(int fd) {
    const char *no_color = getenv("NO_COLOR");
    bool on = isatty(fd) && !(no_color != NULL && *no_color != '\0');
    if (on) { return (struct Style){"\033[1m", "\033[1m\033[4m", "\033[0m"}; }

    return (struct Style){"", "", ""};
}

struct Cmd {
    char buf[CMD_MAX];
    size_t len;
};

static bool cmd_append(struct Cmd *c, const char *s) {
    const size_t room = CMD_MAX - 1 - c->len;
    const size_t n = strlcpy(c->buf + c->len, s, room);

    if (n >= room) {
        c->buf[c->len] = '\0';
        return false;
    }

    c->len += n;
    return true;
}

// "switch-space" -> "SWITCH_SPACE"
static bool cmd_append_wire(struct Cmd *c, const char *name) {
    const size_t start = c->len;

    if (!cmd_append(c, name)) { return false; }

    for (size_t i = start; i < c->len; i++) {
        if (c->buf[i] == '-') {
            c->buf[i] = '_';
        } else if (c->buf[i] >= 'a' && c->buf[i] <= 'z') {
            c->buf[i] = (char)(c->buf[i] - 'a' + 'A');
        }
    }

    return true;
}

static void cmd_seal(struct Cmd *c) {
    c->buf[c->len++] = '\n';
    c->buf[c->len] = '\0';
}

static const struct CtlCommand *find_command(const char *name) {
    for (size_t i = 0; i < nitems(COMMANDS); i++) {
        if (strcmp(COMMANDS[i].name, name) == 0) { return &COMMANDS[i]; }
    }

    return NULL;
}

static void usage(FILE *out) {
    const struct Style style = style_for(fileno(out));

    fputs(style.bold, out);
    fputs("nctl", out);
    fputs(style.reset, out);
    fputs(" v", out);
    fputs(VERSION, out);
    fputs(
        "\n"
        "┌─────────┐\n"
        "│NULLSPACE│\n"
        "└─────────┘\n"
        "-  -  -\n"
        "A command line interface for Nullspace compositor.\n"
        "List all available commands with `nctl list`.\n",
        out
    );
}

static void put_col(const char *s, size_t w) {
    size_t n = strlen(s);

    fputs(s, stdout);
    for (; n < w; n++) { putchar(' '); }
    putchar(' ');
}

static void put_heading(const struct Style *st, const char *title) {
    fputs(st->head, stdout);
    fputs(title, stdout);
    fputs(st->reset, stdout);
    fputs("\n\n", stdout);
}

static void list_commands(const struct Style *st) {
    put_heading(st, "COMMANDS");
    for (size_t i = 0; i < nitems(COMMANDS); i++) {
        fputs("  ", stdout);
        put_col(COMMANDS[i].usage, 22);
        fputs(COMMANDS[i].desc, stdout);
        putchar('\n');
    }

    putchar('\n');
}

static void list_layouts(const struct Style *st) {
    put_heading(st, "LAYOUTS");
    for (size_t i = 0; i < nitems(layout_table); i++) {
        fputs("  ", stdout);
        put_col(layout_table[i].name, 10);
        fputs(layout_table[i].desc, stdout);
        putchar('\n');
    }

    putchar('\n');
}

static void list_keys(
    const struct Style *st, const char *title, enum IpcAccess want,
    bool show_range
) {
    size_t name_w = 0, desc_w = 0;

    put_heading(st, title);

    for (size_t i = 0; i < nitems(nctl_keys); i++) {
        const struct IpcKeyMeta *k = &nctl_keys[i];
        size_t n;

        if ((k->access & want) == 0) { continue; }

        n = strlen(k->name);
        if (n > name_w) { name_w = n; }

        if (show_range) {
            n = strlen(k->desc);
            if (n > desc_w) { desc_w = n; }
        }
    }

    for (size_t i = 0; i < nitems(nctl_keys); i++) {
        const struct IpcKeyMeta *k = &nctl_keys[i];

        if ((k->access & want) == 0) { continue; }

        fputs("  ", stdout);
        put_col(k->name, name_w);
        putchar(' ');

        if (show_range && k->range != NULL) {
            put_col(k->desc, desc_w);
            fputs("[", stdout);
            fputs(k->range, stdout);
            fputs("]", stdout);
        } else {
            fputs(k->desc, stdout);
        }

        putchar('\n');
    }

    putchar('\n');
}

static void list_getters(const struct Style *st) {
    list_keys(st, "GETTERS", IPC_A_R, false);
}

static void list_setters(const struct Style *st) {
    list_keys(st, "SETTERS", IPC_A_W, true);
}

static const struct {
    const char *name;
    void (*print)(const struct Style *);
} LIST_TOPICS[] = {
    {"commands", list_commands},
    { "layouts",  list_layouts},
    {     "get",  list_getters},
    {     "set",  list_setters},
};

static int list_all(const char *topic) {
    const struct Style style = style_for(STDOUT_FILENO);
    const bool all = (topic == NULL || *topic == '\0');
    bool matched = false;

    for (size_t i = 0; i < nitems(LIST_TOPICS); i++) {
        if (all || strcmp(topic, LIST_TOPICS[i].name) == 0) {
            LIST_TOPICS[i].print(&style);
            matched = true;
        }
    }

    if (!matched) {
        warnx("unknown list topic '%s'", topic);
        return EXIT_FAILURE;
    }

    if (fflush(stdout) != 0 || ferror(stdout)) {
        warn("stdout");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

static bool is_token_end(char c) {
    return c == '\0' || c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool is_subscribe(const char *line) {
    static const char word[] = "SUBSCRIBE";
    const size_t n = sizeof(word) - 1;

    line += strspn(line, " \t");
    return strncasecmp(line, word, n) == 0 && is_token_end(line[n]);
}

static int
build_command(struct Cmd *c, int argc, char *argv[], bool *streaming) {
    const bool raw = (strcmp(argv[1], "raw") == 0);

    if (raw && argc < 3) {
        warnx("raw requires a line to send");
        return -1;
    }

    for (int i = 2; i < argc; i++) {
        if (strpbrk(argv[i], "\r\n") != NULL) {
            warnx("argument %d contains a line break", i - 1);
            return -1;
        }
    }

    c->len = 0;
    c->buf[0] = '\0';

    if (!raw) {
        const struct CtlCommand *cc = find_command(argv[1]);
        if (cc == NULL) {
            warnx(
                "unknown command '%s' (see `nctl list commands`; "
                "use `nctl raw` for anything not listed)",
                argv[1]
            );

            return -1;
        }

        if (!cmd_append_wire(c, cc->name)) {
            warnx("command is too long");
            return -1;
        }
    }

    bool sep = !raw;
    for (int i = 2; i < argc; i++) {
        if ((sep && !cmd_append(c, " ")) || !cmd_append(c, argv[i])) {
            warnx("command is too long");
            return -1;
        }

        sep = true;
    }

    cmd_seal(c);
    *streaming = is_subscribe(c->buf);
    return 0;
}

static int connect_socket(const char *path, bool streaming) {
    struct sockaddr_un addr = {.sun_family = AF_UNIX};

    if (strlcpy(addr.sun_path, path, sizeof(addr.sun_path))
        >= sizeof(addr.sun_path)) {
        warnx("socket path is too long");
        return -1;
    }

    const int sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock < 0) {
        warn("socket");
        return -1;
    }

    const struct timeval tv = {.tv_sec = IO_TIMEOUT_SEC, .tv_usec = 0};
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0
        || (!streaming
            && setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
        || !set_nosigpipe(sock)) {
        warn("setsockopt");
        close(sock);
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&addr, (socklen_t)SUN_LEN(&addr))
        < 0) {
        warn("connect to '%s'", path);
        close(sock);
        return -1;
    }

    return sock;
}

static int enter_sandbox(int sock) {
    cap_rights_t rights;
    cap_rights_init(&rights, CAP_READ, CAP_WRITE);

    if (caph_rights_limit(sock, &rights) < 0) { return -1; }
    if (caph_limit_stdio() < 0) { return -1; }

    caph_cache_catpages();

    return caph_enter();
}

static int write_all(int fd, const char *p, size_t n) {
    while (n > 0) {
        const ssize_t w = write(fd, p, n);

        if (w < 0) {
            if (errno == EINTR) { continue; }
            return -1;
        }

        if (w == 0) {
            errno = EIO;
            return -1;
        }

        p += w;
        n -= (size_t)w;
    }

    return 0;
}

static bool is_status(const char *line, size_t len, const char *tok) {
    const size_t tl = strlen(tok);

    return len >= tl && memcmp(line, tok, tl) == 0
        && (len == tl || is_token_end(line[tl]));
}

static int line_status(bool streaming, const char *head, size_t n) {
    if (streaming) { return -1; }
    if (is_status(head, n, "OK")) { return EXIT_SUCCESS; }
    if (is_status(head, n, "ERR")) { return EXIT_FAILURE; }

    return -1;
}

static int pump_replies(int sock, bool streaming) {
    char buf[READ_CHUNK];
    char head[4];
    size_t head_len = 0; // bytes of the current line kept (zero at the start)

    for (;;) {
        const ssize_t n = read(sock, buf, sizeof(buf));

        if (n < 0) {
            if (errno == EINTR) { continue; }

            if (errno == EAGAIN) {
                warnx("timed out waiting for a reply");
            } else {
                warn("read");
            }

            return EXIT_FAILURE;
        }

        if (n == 0) {           // server closed the connection
            if (head_len > 0) { // terminate and classify an unfinished line
                if (write_all(STDOUT_FILENO, "\n", 1) < 0) {
                    warn("write");
                    return EXIT_FAILURE;
                }

                const int st = line_status(streaming, head, head_len);
                if (st >= 0) { return st; }
            }

            break;
        }

        for (size_t i = 0; i < (size_t)n; i++) {
            if (buf[i] != '\n') {
                if (head_len < sizeof(head)) { head[head_len++] = buf[i]; }
                continue;
            }

            const int st = line_status(streaming, head, head_len);
            if (st >= 0) {
                if (write_all(STDOUT_FILENO, buf, i + 1) < 0) {
                    warn("write");
                    return EXIT_FAILURE;
                }

                return st;
            }

            head_len = 0;
        }

        if (write_all(STDOUT_FILENO, buf, (size_t)n) < 0) {
            warn("write");
            return EXIT_FAILURE;
        }
    }

    if (streaming) { return EXIT_SUCCESS; }

    warnx("connection closed without a status reply");
    return EXIT_FAILURE;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        usage(stderr);
        return EXIT_FAILURE;
    }

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0
        || strcmp(argv[1], "help") == 0) {
        usage(stdout);
        return EXIT_SUCCESS;
    }

    if (strcmp(argv[1], "list") == 0) {
        if (argc > 3) {
            warnx("list takes a single argument");
            return EXIT_FAILURE;
        }

        return list_all(argc == 3 ? argv[2] : NULL);
    }

    const char *socket_path = getenv(SOCKET_ENV);
    if (socket_path == NULL || *socket_path == '\0') {
        warnx("nullspace socket is not available");
        return EXIT_FAILURE;
    }

    struct Cmd cmd;
    bool streaming = false;
    if (build_command(&cmd, argc, argv, &streaming) < 0) {
        return EXIT_FAILURE;
    }

    const int sock = connect_socket(socket_path, streaming);
    if (sock < 0) { return EXIT_FAILURE; }

    if (enter_sandbox(sock) < 0) {
        warn("capsicum");
        return EXIT_FAILURE;
    }

    if (write_all(sock, cmd.buf, cmd.len) < 0) {
        warn("send");
        return EXIT_FAILURE;
    }

    return pump_replies(sock, streaming);
}
