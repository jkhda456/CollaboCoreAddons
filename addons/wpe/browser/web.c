/*
 * web: drive the sandbox's web browser (WPE WebKit, the wpe add-on) from the command line — what an
 * agent uses to browse. Each command talks to a running browser (wpe-browser) through its socket,
 * and starts one first if there is none. `web help` lists the commands.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static const char *help_text =
    "web: browse the web from the sandbox (WPE WebKit). A browser starts by itself the first time\n"
    "(it shows on the guest's display as the program NAME, `web` by default: gui list, gui screenshot).\n"
    "\n"
    "usage: web [-n NAME] COMMAND [ARG...]\n"
    "\n"
    "Pages\n"
    "  open URL [TIMEOUT]       load a page (words: a web search); waits until it has loaded\n"
    "  back | forward | reload  history; waits for the page\n"
    "  info                     url, title, loading, history, size, last error or dialog\n"
    "  wait [load | text STR | selector CSS | gone CSS] [TIMEOUT]   wait for the page (seconds)\n"
    "\n"
    "Reading\n"
    "  read [MAX] [--all]       the page as text, every link, button and field numbered [N]\n"
    "  text                     the page's text (innerText)\n"
    "  links                    every link: text and address\n"
    "  html [CSS]               the HTML of the page or of an element\n"
    "  eval JS                  run JavaScript in the page; prints the result (JSON)\n"
    "  screenshot [FILE] [--max-width N]   the page as PNG (default /tmp/NAME.png)\n"
    "\n"
    "Acting (TARGET: a number from `web read`, a CSS selector, or a link's or button's text)\n"
    "  click TARGET | X Y       click (waits for a page it opens); also dblclick, rightclick\n"
    "  type TEXT [TARGET]       type into TARGET (clicked first) or what has the focus\n"
    "  key KEY...               Enter, Tab, Escape, ArrowDown, Backspace, ctrl+a, shift+Tab...\n"
    "  select TARGET VALUE      pick an option of a <select>\n"
    "  hover TARGET | X Y       move the pointer there\n"
    "  scroll [down|up|left|right [N] | top | bottom]\n"
    "\n"
    "Browsers\n"
    "  list                     the running browsers\n"
    "  resize WxH               the page's size\n"
    "  close                    end the browser NAME\n"
    "\n"
    "Exit status: 0 done, 1 failed, 2 usage, 3 timed out, 4 not found, 5 no browser.\n"
    "WEB_START_TIMEOUT (seconds, 300) bounds the start of a browser; WEB_BROWSER_ARGS adds\n"
    "arguments to it (--ephemeral, --ignore-tls-errors, --size WxH); WEB_USER_AGENT replaces its\n"
    "user agent; WPE_BROWSER_DEBUG=1 logs its loads (`gui logs NAME`).\n";

static const char *name = "web";

static void die(int status, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    fputs("web: ", stderr);
    vfprintf(stderr, format, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(status);
}

static const char *socket_dir(void)
{
    const char *dir = getenv("COLLABO_WEB_DIR");
    return dir && *dir ? dir : "/tmp/.collabo-web";
}

static int connect_to(const char *who)
{
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    snprintf(address.sun_path, sizeof address.sun_path, "%s/%s.sock", socket_dir(), who);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    if (connect(fd, (struct sockaddr *)&address, sizeof address) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void write_all(int fd, const void *data, size_t size)
{
    const char *p = data;
    while (size) {
        ssize_t n = write(fd, p, size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            die(1, "lost the browser while sending: %s", strerror(errno));
        p += n;
        size -= n;
    }
}

/* Sends the words, prints the answer; returns its status. */
static int request(int fd, int argc, char **argv, int quiet)
{
    char head[32];
    snprintf(head, sizeof head, "%d\n", argc);
    write_all(fd, head, strlen(head));
    for (int i = 0; i < argc; i++) {
        snprintf(head, sizeof head, "%zu\n", strlen(argv[i]));
        write_all(fd, head, strlen(head));
        write_all(fd, argv[i], strlen(argv[i]));
    }
    /* "STATUS LENGTH\n" then LENGTH bytes */
    char line[64];
    size_t used = 0;
    while (used < sizeof line - 1) {
        ssize_t n = read(fd, line + used, 1);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            die(1, "the browser went away before answering (did it crash? `gui logs %s`)", name);
        if (line[used++] == '\n')
            break;
    }
    line[used] = 0;
    int status = 1;
    size_t length = 0;
    if (sscanf(line, "%d %zu", &status, &length) != 2)
        die(1, "the browser answered something else: %s", line);
    FILE *out = status ? stderr : stdout;
    char buffer[65536];
    while (length) {
        ssize_t n = read(fd, buffer, length < sizeof buffer ? length : sizeof buffer);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        if (!quiet)
            fwrite(buffer, 1, n, out);
        length -= n;
    }
    fflush(out);
    return status;
}

/* Runs argv; quiet: its stdout to /dev/null (its errors still show) */
static int run_quietly(char *const argv[], int wait_for_it, int quiet)
{
    pid_t pid;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (quiet)
        posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    int error = posix_spawnp(&pid, argv[0], &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (error)
        return -1;
    if (!wait_for_it)
        return 0;
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

static int run(char *const argv[], int wait_for_it)
{
    return run_quietly(argv, wait_for_it, 0);
}

static int have(const char *program)
{
    const char *path = getenv("PATH");
    if (!path)
        path = "/usr/bin:/bin";
    char *copy = strdup(path), *save = NULL;
    for (char *dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        char file[4096];
        snprintf(file, sizeof file, "%s/%s", dir, program);
        if (!access(file, X_OK)) {
            free(copy);
            return 1;
        }
    }
    free(copy);
    return 0;
}

/* Starts a browser named `name`: on the display (gui run) when there is one, else on its own. */
static void start_browser(void)
{
    char *args[32];
    int n = 0;
    char *extra = getenv("WEB_BROWSER_ARGS") ? strdup(getenv("WEB_BROWSER_ARGS")) : NULL;
    if (have("gui")) {
        args[n++] = "gui";
        args[n++] = "run";
        args[n++] = "--name";
        args[n++] = (char *)name;
        args[n++] = "--";
    }
    args[n++] = "wpe-browser";
    args[n++] = "--name";
    args[n++] = (char *)name;
    if (extra) {
        char *save = NULL;
        for (char *word = strtok_r(extra, " ", &save); word && n < 30; word = strtok_r(NULL, " ", &save))
            args[n++] = word;
    }
    args[n] = NULL;
    if (!strcmp(args[0], "gui")) {
        int status = run_quietly(args, 1, 1);
        if (status == 0)
            return;
        fprintf(stderr, "web: gui run failed (%d); starting the browser without a display\n", status);
        memmove(args, args + 5, (n - 4) * sizeof *args);
    }
    /* on its own: in the background, its output in a log beside the socket */
    char log[4096];
    snprintf(log, sizeof log, "%s/%s.log", socket_dir(), name);
    mkdir(socket_dir(), 0700);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 1, log, O_WRONLY | O_CREAT | O_APPEND, 0600);
    posix_spawn_file_actions_adddup2(&actions, 1, 2);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
    pid_t pid;
    if (posix_spawnp(&pid, "wpe-browser", &actions, &attributes, args, environ))
        die(5, "cannot start wpe-browser: %s (is the wpe add-on on?)", strerror(errno));
}

static int connect_or_start(void)
{
    int fd = connect_to(name);
    if (fd >= 0)
        return fd;
    start_browser();
    const char *limit = getenv("WEB_START_TIMEOUT");
    double timeout = limit ? atof(limit) : 300;
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int said = 0;
    for (;;) {
        fd = connect_to(name);
        if (fd >= 0)
            return fd;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) / 1e9;
        if (elapsed > timeout)
            die(5, "the browser did not start within %.0f s (gui logs %s)", timeout, name);
        if (!said && elapsed > 3) {
            fprintf(stderr, "web: starting the browser \"%s\" (its first start on this computer compiles it: about 20 s)...\n", name);
            said = 1;
        }
        usleep(200000);
    }
}

static int cmd_list(void)
{
    DIR *dir = opendir(socket_dir());
    int found = 0;
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            size_t length = strlen(entry->d_name);
            if (length < 6 || strcmp(entry->d_name + length - 5, ".sock"))
                continue;
            char who[256];
            snprintf(who, sizeof who, "%.*s", (int)(length - 5), entry->d_name);
            int fd = connect_to(who);
            if (fd < 0)
                continue;
            char *words[] = { "url" };
            printf("%s\t", who);
            fflush(stdout);
            request(fd, 1, words, 0);
            close(fd);
            found++;
        }
        closedir(dir);
    }
    if (!found)
        printf("no browser is running (web open URL starts one)\n");
    return 0;
}

static int cmd_screenshot(int argc, char **argv)
{
    char file[4096];
    snprintf(file, sizeof file, "/tmp/%s.png", name);
    char *args[16];
    int n = 0;
    args[n++] = "gui";
    args[n++] = "screenshot";
    args[n++] = (char *)name;
    args[n++] = "--idle";
    args[n++] = "300";
    for (int i = 1; i < argc && n < 12; i++) {
        if (argv[i][0] != '-' && (i == 1 || argv[i - 1][0] != '-'))
            snprintf(file, sizeof file, "%s", argv[i]);
        else
            args[n++] = argv[i];
    }
    args[n++] = "-o";
    args[n++] = file;
    args[n] = NULL;
    if (!have("gui"))
        die(1, "no display (the gui command of the tools image) to take a screenshot from; `web read` reads the page as text");
    int fd = connect_or_start(); /* the browser must be there */
    close(fd);
    int status = run(args, 1);
    if (status == 0)
        printf("%s\n", file);
    return status;
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if ((!strcmp(argv[i], "-n") || !strcmp(argv[i], "--name")) && i + 1 < argc)
            name = argv[++i];
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            fputs(help_text, stdout);
            return 0;
        } else
            die(2, "unknown option %s (web help)", argv[i]);
    }
    if (i >= argc || !strcmp(argv[i], "help")) {
        fputs(help_text, i >= argc ? stderr : stdout);
        return i >= argc ? 2 : 0;
    }
    if (!getenv("COLLABO_WEB_NAME") && strchr(name, '/'))
        die(2, "a browser's name has no '/'");
    char **words = argv + i;
    int count = argc - i;
    const char *command = words[0];
    signal(SIGPIPE, SIG_IGN);

    if (!strcmp(command, "list"))
        return cmd_list();
    if (!strcmp(command, "screenshot"))
        return cmd_screenshot(count, words);
    if (!strcmp(command, "view") || !strcmp(command, "logs")) {
        char *args[] = { "gui", (char *)command, (char *)name, NULL };
        return run(args, 1);
    }
    if (!strcmp(command, "close") || !strcmp(command, "quit") || !strcmp(command, "stop")) {
        int fd = connect_to(name);
        if (fd < 0) {
            printf("no browser \"%s\" is running\n", name);
            return 0;
        }
        char *close_words[] = { "close" };
        return request(fd, 1, close_words, 0);
    }
    int fd = connect_or_start();
    int status = request(fd, count, words, 0);
    close(fd);
    return status;
}
