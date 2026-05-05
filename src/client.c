#include "common.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define OPTY_SESSION_MAX_NAME 96
#define OPTY_SESSION_PORT_BASE 6077ul
#define OPTY_LIST_CONNECT_TIMEOUT_MS 250
#define OPTY_SESSION_MAX_ARGC 128
#define OPTY_CONTROL_TOKEN_BYTES 32u
#define OPTY_STOP_WAIT_MS 7000L

static volatile sig_atomic_t g_sigwinch_pending = 0;
static volatile sig_atomic_t g_terminate_pending = 0;

static int g_raw_mode_enabled = 0;
static int g_raw_mode_fd = -1;
static struct termios g_saved_termios;

struct opty_session_info {
    char name[OPTY_SESSION_MAX_NAME + 1u];
    char host[128];
    char port[16];
    char log[512];
    char cwd[1024];
    char restart_policy[64];
    char control_token[OPTY_MAX_TOKEN + 1u];
    char graceful_input[OPTY_MAX_GRACEFUL_INPUT + 1u];
    char command[1024];
    size_t argc;
    char *argv[OPTY_SESSION_MAX_ARGC + 1u];
};

struct opty_list_entry {
    struct opty_session_info info;
    int alive;
    pthread_t thread;
};

static void restore_terminal(void)
{
    if (g_raw_mode_enabled) {
        (void)tcsetattr(g_raw_mode_fd, TCSANOW, &g_saved_termios);
        g_raw_mode_enabled = 0;
    }
}

static void on_sigwinch(int signo)
{
    (void)signo;
    g_sigwinch_pending = 1;
}

static void on_terminate(int signo)
{
    (void)signo;
    g_terminate_pending = 1;
}

static int install_signal_handler(int signo, void (*handler)(int))
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(signo, &sa, NULL) < 0) {
        return -1;
    }
    return 0;
}

static int setup_signal_handlers(void)
{
    if (install_signal_handler(SIGWINCH, on_sigwinch) < 0) {
        return -1;
    }
    if (install_signal_handler(SIGHUP, on_terminate) < 0) {
        return -1;
    }
    if (install_signal_handler(SIGTERM, on_terminate) < 0) {
        return -1;
    }
    if (install_signal_handler(SIGQUIT, on_terminate) < 0) {
        return -1;
    }
    if (install_signal_handler(SIGINT, on_terminate) < 0) {
        return -1;
    }
    signal(SIGPIPE, SIG_IGN);
    return 0;
}

static int enable_raw_mode(void)
{
    if (!isatty(STDIN_FILENO)) {
        return 0;
    }
    if (tcgetattr(STDIN_FILENO, &g_saved_termios) < 0) {
        return -1;
    }

    g_raw_mode_fd = STDIN_FILENO;
    struct termios raw = g_saved_termios;
    cfmakeraw(&raw);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) < 0) {
        return -1;
    }

    g_raw_mode_enabled = 1;
    atexit(restore_terminal);
    return 0;
}

static void get_terminal_size(uint16_t *cols, uint16_t *rows)
{
    struct winsize ws;

    *cols = 80;
    *rows = 24;

    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) < 0) {
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) < 0) {
            return;
        }
    }
    if (ws.ws_col > 0) {
        *cols = ws.ws_col;
    }
    if (ws.ws_row > 0) {
        *rows = ws.ws_row;
    }
}

static void sleep_retry_interval(void)
{
    struct timespec req = {1, 0};

    while (nanosleep(&req, &req) < 0 && errno == EINTR && !g_terminate_pending) {
    }
}

static int open_scrollback_log(const char *path)
{
    int fd;

    fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    return fd;
}

static int connect_endpoint(const struct opty_endpoint *endpoint)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *it;
    int rc;
    int sock = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG;

    rc = getaddrinfo(endpoint->host, endpoint->port, &hints, &res);
    if (rc != 0) {
        errno = EINVAL;
        return -1;
    }

    for (it = res; it != NULL; it = it->ai_next) {
        sock = socket(it->ai_family, it->ai_socktype | SOCK_CLOEXEC, it->ai_protocol);
        if (sock < 0) {
            continue;
        }
        if (connect(sock, it->ai_addr, it->ai_addrlen) == 0) {
            (void)opty_set_tcp_nodelay(sock);
            break;
        }
        close(sock);
        sock = -1;
    }

    freeaddrinfo(res);
    return sock;
}

static int send_intro(int sock, bool reconnect, uint64_t last_seq, const char *token)
{
    uint16_t cols;
    uint16_t rows;

    get_terminal_size(&cols, &rows);
    if (reconnect) {
        return opty_send_reconnect(sock, last_seq, cols, rows, token);
    }
    return opty_send_hello(sock, cols, rows, token);
}

static int send_resize_now(int sock)
{
    uint16_t cols;
    uint16_t rows;

    get_terminal_size(&cols, &rows);
    return opty_send_resize(sock, cols, rows);
}

static int write_scrollback(int log_fd, const uint8_t *data, size_t len)
{
    if (log_fd < 0 || len == 0) {
        return 0;
    }
    if (opty_write_full(log_fd, data, len) < 0) {
        return -1;
    }
    return 0;
}

static int write_stdout_bytes(const uint8_t *data, size_t len)
{
    if (len == 0) {
        return 0;
    }
    if (opty_write_full(STDOUT_FILENO, data, len) < 0) {
        return -1;
    }
    return 0;
}

static void write_stderr_bytes(const uint8_t *data, size_t len)
{
    if (len > 0) {
        (void)opty_write_full(STDERR_FILENO, data, len);
    }
    (void)opty_write_full(STDERR_FILENO, "\n", 1);
}

static int process_server_frame(int sock, int log_fd, uint64_t *last_seq, const struct opty_frame *frame, int *saw_welcome)
{
    if (frame->type == OPTY_MSG_WELCOME) {
        uint64_t base_seq;
        uint64_t next_seq;

        if (opty_parse_welcome(frame, &base_seq, &next_seq) < 0) {
            return -1;
        }
        (void)base_seq;
        (void)next_seq;
        *saw_welcome = 1;
        return 0;
    }

    if (frame->type == OPTY_MSG_STDOUT || frame->type == OPTY_MSG_REPLAY) {
        uint64_t seq;
        const uint8_t *data;
        size_t len;

        if (opty_parse_stream(frame, &seq, &data, &len) < 0) {
            return -1;
        }
        if (write_stdout_bytes(data, len) < 0) {
            return -1;
        }
        if (write_scrollback(log_fd, data, len) < 0) {
            return -1;
        }
        uint64_t frame_end = seq + (uint64_t)len;
        if (frame_end > *last_seq) {
            *last_seq = frame_end;
        }
        for (;;) {
            if (opty_send_ack(sock, *last_seq) == 0) {
                break;
            }
            if (errno != EINTR || g_terminate_pending) {
                return 1;
            }
        }
        return 0;
    }

    if (frame->type == OPTY_MSG_ERROR) {
        write_stderr_bytes(frame->payload, frame->len);
        return -1;
    }

    return -1;
}

static int run_session(int sock, int log_fd, bool reconnect, const char *token, uint64_t *last_seq)
{
    int stdin_open = 1;
    int saw_welcome = 0;
    int resize_sent = 0;
    int resize_pending = 0;

    for (;;) {
        if (send_intro(sock, reconnect, *last_seq, token) == 0) {
            break;
        }
        if (g_terminate_pending) {
            return 0;
        }
        if (errno != EINTR) {
            return 1;
        }
    }

    for (;;) {
        fd_set readfds;
        int maxfd = sock;
        int rc;

        if (g_terminate_pending) {
            return 0;
        }
        if (g_sigwinch_pending) {
            g_sigwinch_pending = 0;
            resize_pending = 1;
        }

        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);
        if (stdin_open) {
            FD_SET(STDIN_FILENO, &readfds);
            if (STDIN_FILENO > maxfd) {
                maxfd = STDIN_FILENO;
            }
        }

        rc = select(maxfd + 1, &readfds, NULL, NULL, NULL);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return 1;
        }

        if (FD_ISSET(sock, &readfds)) {
            struct opty_frame frame = {0};
            int frame_rc;

            frame_rc = opty_recv_frame(sock, &frame);
            if (frame_rc == 1) {
                opty_frame_free(&frame);
                return 0;
            }
            if (frame_rc < 0) {
                opty_frame_free(&frame);
                if (g_terminate_pending) {
                    return 0;
                }
                if (errno == EINTR) {
                    continue;
                }
                return 1;
            }
            rc = process_server_frame(sock, log_fd, last_seq, &frame, &saw_welcome);
            if (rc < 0) {
                opty_frame_free(&frame);
                if (g_terminate_pending) {
                    return 0;
                }
                return -1;
            }
            if (rc > 0) {
                opty_frame_free(&frame);
                return 1;
            }
            opty_frame_free(&frame);
        }

        if (saw_welcome && (!resize_sent || resize_pending)) {
            for (;;) {
                if (send_resize_now(sock) == 0) {
                    break;
                }
                if (g_terminate_pending) {
                    return 0;
                }
                if (errno != EINTR) {
                    return 1;
                }
            }
            resize_sent = 1;
            resize_pending = 0;
        }

        if (stdin_open && FD_ISSET(STDIN_FILENO, &readfds)) {
            uint8_t buf[4096];
            ssize_t nread;

            nread = read(STDIN_FILENO, buf, sizeof(buf));
            if (nread < 0 && errno == EINTR) {
                continue;
            }
            if (nread <= 0) {
                stdin_open = 0;
                return 0;
            }
            for (;;) {
                if (opty_send_stdin(sock, buf, (size_t)nread) == 0) {
                    break;
                }
                if (g_terminate_pending) {
                    return 0;
                }
                if (errno != EINTR) {
                    return 1;
                }
            }
        }
    }
}

static int opty_is_decimal(const char *value)
{
    if (value == NULL || *value == '\0') {
        return 0;
    }
    for (const char *p = value; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return 0;
        }
    }
    return 1;
}

static int opty_is_session_name(const char *value)
{
    size_t len;

    if (value == NULL || value[0] == '\0' || value[0] == '-') {
        return 0;
    }

    len = strlen(value);
    if (len > OPTY_SESSION_MAX_NAME || strcmp(value, ".") == 0 || strcmp(value, "..") == 0) {
        return 0;
    }

    for (const char *p = value; *p != '\0'; p++) {
        if ((*p >= 'A' && *p <= 'Z') ||
            (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9') ||
            *p == '_' || *p == '-' || *p == '.') {
            continue;
        }
        return 0;
    }
    return 1;
}

static int opty_numeric_session_endpoint(const char *session, char *port_buf, size_t port_cap, struct opty_endpoint *endpoint)
{
    char *end = NULL;
    unsigned long session_num;
    unsigned long port;

    if (!opty_is_decimal(session) || port_buf == NULL || endpoint == NULL) {
        errno = EINVAL;
        return -1;
    }

    errno = 0;
    session_num = strtoul(session, &end, 10);
    if (errno != 0 || end == session || *end != '\0' || session_num == 0) {
        errno = EINVAL;
        return -1;
    }

    port = OPTY_SESSION_PORT_BASE + session_num - 1ul;
    if (port > 65535ul) {
        errno = EINVAL;
        return -1;
    }
    if (snprintf(port_buf, port_cap, "%lu", port) >= (int)port_cap) {
        errno = ENOSPC;
        return -1;
    }

    endpoint->host = OPTY_DEFAULT_HOST;
    endpoint->port = port_buf;
    return 0;
}

static int opty_session_root(char *buf, size_t cap)
{
    const char *home = getenv("HOME");

    if (home == NULL || home[0] == '\0') {
        errno = ENOENT;
        return -1;
    }
    if (snprintf(buf, cap, "%s/.0pty", home) >= (int)cap) {
        errno = ENOSPC;
        return -1;
    }
    return 0;
}

static int opty_session_dir(char *buf, size_t cap)
{
    const char *home = getenv("HOME");

    if (home == NULL || home[0] == '\0') {
        errno = ENOENT;
        return -1;
    }
    if (snprintf(buf, cap, "%s/.0pty/sessions", home) >= (int)cap) {
        errno = ENOSPC;
        return -1;
    }
    return 0;
}

static int opty_log_dir(char *buf, size_t cap)
{
    const char *home = getenv("HOME");

    if (home == NULL || home[0] == '\0') {
        errno = ENOENT;
        return -1;
    }
    if (snprintf(buf, cap, "%s/.0pty/logs", home) >= (int)cap) {
        errno = ENOSPC;
        return -1;
    }
    return 0;
}

static int opty_ensure_session_dir(void)
{
    char root[512];
    char dir[512];
    char log_dir[512];

    if (opty_session_root(root, sizeof(root)) < 0 ||
        opty_session_dir(dir, sizeof(dir)) < 0 ||
        opty_log_dir(log_dir, sizeof(log_dir)) < 0) {
        return -1;
    }
    if (mkdir(root, 0700) < 0 && errno != EEXIST) {
        return -1;
    }
    if (mkdir(dir, 0700) < 0 && errno != EEXIST) {
        return -1;
    }
    if (mkdir(log_dir, 0700) < 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static int opty_session_path(const char *session, char *buf, size_t cap)
{
    char dir[512];

    if (!opty_is_session_name(session)) {
        errno = EINVAL;
        return -1;
    }
    if (opty_session_dir(dir, sizeof(dir)) < 0) {
        return -1;
    }
    if (snprintf(buf, cap, "%s/%s.session", dir, session) >= (int)cap) {
        errno = ENOSPC;
        return -1;
    }
    return 0;
}

static int opty_session_log_path(const char *session, char *buf, size_t cap)
{
    char dir[512];

    if (!opty_is_session_name(session)) {
        errno = EINVAL;
        return -1;
    }
    if (opty_log_dir(dir, sizeof(dir)) < 0) {
        return -1;
    }
    if (snprintf(buf, cap, "%s/%s.log", dir, session) >= (int)cap) {
        errno = ENOSPC;
        return -1;
    }
    return 0;
}

static void opty_chomp(char *line)
{
    size_t len = strlen(line);

    while (len > 0u && (line[len - 1u] == '\n' || line[len - 1u] == '\r')) {
        line[--len] = '\0';
    }
}

static int opty_generate_control_token(char *buf, size_t cap)
{
    static const char hex[] = "0123456789abcdef";
    uint8_t bytes[OPTY_CONTROL_TOKEN_BYTES];
    int fd;

    if (buf == NULL || cap < OPTY_CONTROL_TOKEN_BYTES * 2u + 1u) {
        errno = ENOSPC;
        return -1;
    }

    fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    if (opty_read_full(fd, bytes, sizeof(bytes)) != (ssize_t)sizeof(bytes)) {
        int saved_errno = errno;

        close(fd);
        errno = saved_errno;
        return -1;
    }
    close(fd);

    for (size_t i = 0; i < sizeof(bytes); i++) {
        buf[i * 2u] = hex[(bytes[i] >> 4) & 0x0fu];
        buf[i * 2u + 1u] = hex[bytes[i] & 0x0fu];
    }
    buf[sizeof(bytes) * 2u] = '\0';
    return 0;
}

static int opty_read_session_endpoint(const char *session, char *port_buf, size_t port_cap, struct opty_endpoint *endpoint)
{
    char path[1024];
    char line[512];
    FILE *fp;
    int found_port = 0;

    if (opty_session_path(session, path, sizeof(path)) < 0) {
        return -1;
    }

    fp = fopen(path, "r");
    if (fp == NULL) {
        return -1;
    }

    endpoint->host = OPTY_DEFAULT_HOST;
    endpoint->port = port_buf;

    while (fgets(line, sizeof(line), fp) != NULL) {
        opty_chomp(line);
        if (strncmp(line, "port=", 5) == 0) {
            const char *port = line + 5;

            if (port[0] == '\0' || strlen(port) >= port_cap) {
                fclose(fp);
                errno = EINVAL;
                return -1;
            }
            for (const char *p = port; *p != '\0'; p++) {
                if (*p < '0' || *p > '9') {
                    fclose(fp);
                    errno = EINVAL;
                    return -1;
                }
            }
            memcpy(port_buf, port, strlen(port) + 1u);
            found_port = 1;
        }
    }

    fclose(fp);
    if (!found_port) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int opty_write_session(const char *session, const struct opty_endpoint *endpoint, const char *log_path,
                              const char *control_token, char **command_argv)
{
    char path[1024];
    char tmp_path[1100];
    char cwd[1024];
    FILE *fp = NULL;
    int fd = -1;
    size_t argc = 0;

    if (opty_ensure_session_dir() < 0 || opty_session_path(session, path, sizeof(path)) < 0) {
        return -1;
    }
    if (control_token == NULL || control_token[0] == '\0' || strlen(control_token) > OPTY_MAX_TOKEN) {
        errno = EINVAL;
        return -1;
    }
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%ld", path, (long)getpid()) >= (int)sizeof(tmp_path)) {
        errno = ENOSPC;
        return -1;
    }

    if (command_argv != NULL) {
        while (command_argv[argc] != NULL) {
            if (argc >= OPTY_SESSION_MAX_ARGC) {
                errno = E2BIG;
                return -1;
            }
            argc++;
        }
    }

    fd = open(tmp_path, O_WRONLY | O_CREAT | O_EXCL | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        return -1;
    }
    fp = fdopen(fd, "w");
    if (fp == NULL) {
        int saved_errno = errno;

        close(fd);
        unlink(tmp_path);
        errno = saved_errno;
        return -1;
    }
    fd = -1;

    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        cwd[0] = '\0';
    }

    fprintf(fp, "name=%s\n", session);
    fprintf(fp, "host=%s\n", endpoint->host);
    fprintf(fp, "port=%s\n", endpoint->port);
    fprintf(fp, "log=%s\n", log_path);
    fprintf(fp, "restart_policy=manual\n");
    fprintf(fp, "control_token=%s\n", control_token);
    fprintf(fp, "graceful_input=/exit\\n\n");
    fprintf(fp, "argc=%zu\n", argc);
    if (argc > 0u) {
        fprintf(fp, "command=");
        for (size_t i = 0; i < argc; i++) {
            fprintf(fp, "%s%s", i == 0u ? "" : " ", command_argv[i]);
        }
        fprintf(fp, "\n");
        for (size_t i = 0; i < argc; i++) {
            fprintf(fp, "argv%zu=%s\n", i, command_argv[i]);
        }
    }
    if (cwd[0] != '\0') {
        fprintf(fp, "cwd=%s\n", cwd);
    }

    if (ferror(fp) || fflush(fp) != 0 || fsync(fileno(fp)) != 0) {
        int saved_errno = errno;

        fclose(fp);
        unlink(tmp_path);
        errno = saved_errno;
        return -1;
    }
    if (fclose(fp) != 0) {
        int saved_errno = errno;

        unlink(tmp_path);
        errno = saved_errno;
        return -1;
    }
    if (rename(tmp_path, path) < 0) {
        int saved_errno = errno;

        unlink(tmp_path);
        errno = saved_errno;
        return -1;
    }

    return 0;
}

static char *opty_strdup(const char *value)
{
    size_t len;
    char *copy;

    if (value == NULL) {
        errno = EINVAL;
        return NULL;
    }
    len = strlen(value);
    copy = malloc(len + 1u);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, value, len + 1u);
    return copy;
}

static char *opty_server_path(const char *client_path)
{
    const char *slash = client_path != NULL ? strrchr(client_path, '/') : NULL;
    const char server_name[] = "0pty-server";
    char *path;
    size_t dir_len;

    if (slash == NULL) {
        return opty_strdup(server_name);
    }

    dir_len = (size_t)(slash - client_path);
    path = malloc(dir_len + 1u + sizeof(server_name));
    if (path == NULL) {
        return NULL;
    }
    memcpy(path, client_path, dir_len);
    path[dir_len] = '/';
    memcpy(path + dir_len + 1u, server_name, sizeof(server_name));
    return path;
}

static int opty_endpoint_accepts(const struct opty_endpoint *endpoint)
{
    int fd = connect_endpoint(endpoint);

    if (fd < 0) {
        return 0;
    }
    close(fd);
    return 1;
}

static int opty_endpoint_bindable(const struct opty_endpoint *endpoint)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *it;
    int bindable = 0;
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG;

    rc = getaddrinfo(endpoint->host, endpoint->port, &hints, &res);
    if (rc != 0) {
        errno = EINVAL;
        return 0;
    }

    for (it = res; it != NULL && !bindable; it = it->ai_next) {
        int fd = socket(it->ai_family, it->ai_socktype | SOCK_CLOEXEC, it->ai_protocol);
        int one = 1;

        if (fd < 0) {
            continue;
        }
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(fd, it->ai_addr, it->ai_addrlen) == 0) {
            bindable = 1;
        }
        close(fd);
    }

    freeaddrinfo(res);
    return bindable;
}

static int opty_endpoint_accepts_timeout(const struct opty_endpoint *endpoint, int timeout_ms)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *it;
    int alive = 0;
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG;

    rc = getaddrinfo(endpoint->host, endpoint->port, &hints, &res);
    if (rc != 0) {
        errno = EINVAL;
        return 0;
    }

    for (it = res; it != NULL && !alive; it = it->ai_next) {
        int fd = socket(it->ai_family, it->ai_socktype | SOCK_CLOEXEC, it->ai_protocol);
        int flags;
        int connect_rc;

        if (fd < 0) {
            continue;
        }

        flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            close(fd);
            continue;
        }

        connect_rc = connect(fd, it->ai_addr, it->ai_addrlen);
        if (connect_rc == 0) {
            alive = 1;
        } else if (errno == EINPROGRESS) {
            struct pollfd pfd;

            pfd.fd = fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            while ((rc = poll(&pfd, 1u, timeout_ms)) < 0 && errno == EINTR) {
            }
            if (rc > 0 && (pfd.revents & POLLOUT) != 0) {
                int err = 0;
                socklen_t err_len = sizeof(err);

                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &err_len) == 0 && err == 0) {
                    alive = 1;
                }
            }
        }

        close(fd);
    }

    freeaddrinfo(res);
    return alive;
}

static void opty_session_info_init(struct opty_session_info *info, const char *fallback_name)
{
    memset(info, 0, sizeof(*info));
    if (fallback_name != NULL) {
        snprintf(info->name, sizeof(info->name), "%s", fallback_name);
    }
    snprintf(info->host, sizeof(info->host), "%s", OPTY_DEFAULT_HOST);
    snprintf(info->restart_policy, sizeof(info->restart_policy), "%s", "manual");
    snprintf(info->graceful_input, sizeof(info->graceful_input), "%s", "/exit\\n");
}

static int opty_copy_field(char *dst, size_t cap, const char *src)
{
    if (src == NULL || strlen(src) >= cap) {
        errno = EINVAL;
        return -1;
    }
    memcpy(dst, src, strlen(src) + 1u);
    return 0;
}

static void opty_session_info_free(struct opty_session_info *info)
{
    if (info == NULL) {
        return;
    }
    for (size_t i = 0; i < info->argc; i++) {
        free(info->argv[i]);
        info->argv[i] = NULL;
    }
    info->argc = 0;
}

static int opty_parse_argv_key(const char *key, size_t *index_out)
{
    char *end = NULL;
    unsigned long index;

    if (strncmp(key, "argv", 4u) != 0 || key[4] == '\0' || !opty_is_decimal(key + 4)) {
        return 0;
    }

    errno = 0;
    index = strtoul(key + 4, &end, 10);
    if (errno != 0 || end == key + 4 || *end != '\0' || index >= OPTY_SESSION_MAX_ARGC) {
        errno = EINVAL;
        return -1;
    }
    *index_out = (size_t)index;
    return 1;
}

static int opty_read_session_info(const char *session, struct opty_session_info *info)
{
    char path[1024];
    char line[2048];
    FILE *fp;

    if (info == NULL || opty_session_path(session, path, sizeof(path)) < 0) {
        return -1;
    }

    fp = fopen(path, "r");
    if (fp == NULL) {
        return -1;
    }

    opty_session_info_init(info, session);
    while (fgets(line, sizeof(line), fp) != NULL) {
        char *value;
        size_t argv_index = 0;
        int argv_key;

        opty_chomp(line);
        value = strchr(line, '=');
        if (value == NULL) {
            continue;
        }
        *value++ = '\0';

        if (strcmp(line, "name") == 0) {
            if (opty_copy_field(info->name, sizeof(info->name), value) < 0) {
                fclose(fp);
                return -1;
            }
        } else if (strcmp(line, "host") == 0) {
            if (opty_copy_field(info->host, sizeof(info->host), value) < 0) {
                fclose(fp);
                return -1;
            }
        } else if (strcmp(line, "port") == 0) {
            if (opty_copy_field(info->port, sizeof(info->port), value) < 0) {
                fclose(fp);
                return -1;
            }
        } else if (strcmp(line, "log") == 0) {
            if (opty_copy_field(info->log, sizeof(info->log), value) < 0) {
                fclose(fp);
                return -1;
            }
        } else if (strcmp(line, "cwd") == 0) {
            if (opty_copy_field(info->cwd, sizeof(info->cwd), value) < 0) {
                fclose(fp);
                return -1;
            }
        } else if (strcmp(line, "restart_policy") == 0) {
            if (opty_copy_field(info->restart_policy, sizeof(info->restart_policy), value) < 0) {
                fclose(fp);
                opty_session_info_free(info);
                return -1;
            }
        } else if (strcmp(line, "control_token") == 0) {
            if (opty_copy_field(info->control_token, sizeof(info->control_token), value) < 0) {
                fclose(fp);
                opty_session_info_free(info);
                return -1;
            }
        } else if (strcmp(line, "graceful_input") == 0) {
            if (opty_copy_field(info->graceful_input, sizeof(info->graceful_input), value) < 0) {
                fclose(fp);
                opty_session_info_free(info);
                return -1;
            }
        } else if (strcmp(line, "command") == 0) {
            if (opty_copy_field(info->command, sizeof(info->command), value) < 0) {
                fclose(fp);
                opty_session_info_free(info);
                return -1;
            }
        } else if (strcmp(line, "argc") == 0) {
            char *end = NULL;
            unsigned long argc;

            errno = 0;
            argc = strtoul(value, &end, 10);
            if (errno != 0 || end == value || *end != '\0' || argc > OPTY_SESSION_MAX_ARGC) {
                fclose(fp);
                opty_session_info_free(info);
                errno = EINVAL;
                return -1;
            }
            info->argc = (size_t)argc;
        } else if ((argv_key = opty_parse_argv_key(line, &argv_index)) != 0) {
            if (argv_key < 0) {
                fclose(fp);
                opty_session_info_free(info);
                return -1;
            }
            free(info->argv[argv_index]);
            info->argv[argv_index] = opty_strdup(value);
            if (info->argv[argv_index] == NULL) {
                fclose(fp);
                opty_session_info_free(info);
                return -1;
            }
        }
    }

    fclose(fp);
    if (info->port[0] == '\0') {
        errno = EINVAL;
        opty_session_info_free(info);
        return -1;
    }
    for (size_t i = 0; i < info->argc; i++) {
        if (info->argv[i] == NULL) {
            errno = EINVAL;
            opty_session_info_free(info);
            return -1;
        }
    }
    info->argv[info->argc] = NULL;
    return 0;
}

static void *opty_list_probe_thread(void *arg)
{
    struct opty_list_entry *entry = arg;
    struct opty_endpoint endpoint;

    endpoint.host = entry->info.host;
    endpoint.port = entry->info.port;
    entry->alive = opty_endpoint_accepts_timeout(&endpoint, OPTY_LIST_CONNECT_TIMEOUT_MS);
    return NULL;
}

static int opty_pick_session_endpoint(char *port_buf, size_t port_cap, struct opty_endpoint *endpoint)
{
    for (unsigned long port = OPTY_SESSION_PORT_BASE; port <= 65535ul; port++) {
        if (snprintf(port_buf, port_cap, "%lu", port) >= (int)port_cap) {
            errno = ENOSPC;
            return -1;
        }
        endpoint->host = OPTY_DEFAULT_HOST;
        endpoint->port = port_buf;
        if (opty_endpoint_bindable(endpoint)) {
            return 0;
        }
    }

    errno = EADDRINUSE;
    return -1;
}

static void opty_sleep_millis(long millis)
{
    struct timespec req;

    req.tv_sec = millis / 1000;
    req.tv_nsec = (millis % 1000) * 1000000L;
    while (nanosleep(&req, &req) < 0 && errno == EINTR) {
    }
}

static int opty_wait_for_server(pid_t pid, const struct opty_endpoint *endpoint, const char *log_path)
{
    for (int i = 0; i < 60; i++) {
        int status;
        pid_t done;

        if (opty_endpoint_accepts(endpoint)) {
            return 0;
        }

        done = waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            fprintf(stderr, "0pty server exited before it was ready; see %s\n", log_path);
            return -1;
        }
        if (done < 0 && errno != EINTR) {
            perror("waitpid");
            return -1;
        }

        opty_sleep_millis(50);
    }

    fprintf(stderr, "timed out waiting for 0pty server; see %s\n", log_path);
    return -1;
}

static int opty_decode_graceful_input(const char *value, uint8_t *out, size_t cap, size_t *len_out)
{
    size_t len = 0;

    if (value == NULL || out == NULL || len_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    for (const char *p = value; *p != '\0'; p++) {
        unsigned char ch = (unsigned char)*p;

        if (ch == '\\' && p[1] != '\0') {
            p++;
            switch (*p) {
            case 'n':
                ch = '\n';
                break;
            case 'r':
                ch = '\r';
                break;
            case 't':
                ch = '\t';
                break;
            case '\\':
                ch = '\\';
                break;
            default:
                ch = (unsigned char)*p;
                break;
            }
        }

        if (len == cap) {
            errno = EMSGSIZE;
            return -1;
        }
        out[len++] = ch;
    }

    *len_out = len;
    return 0;
}

static int opty_wait_endpoint_dead(const struct opty_endpoint *endpoint, long timeout_ms)
{
    long waited = 0;

    while (waited <= timeout_ms) {
        if (!opty_endpoint_accepts_timeout(endpoint, 100)) {
            return 0;
        }
        opty_sleep_millis(100L);
        waited += 100L;
    }

    errno = ETIMEDOUT;
    return -1;
}

static int opty_spawn_server(const char *argv0, const char *session, const struct opty_endpoint *endpoint,
                             const char *log_path, const char *control_token, char **command_argv)
{
    char endpoint_arg[128];
    char *server_path;
    char **exec_argv;
    size_t command_count = 0;
    size_t exec_index = 0;
    pid_t pid;

    if (snprintf(endpoint_arg, sizeof(endpoint_arg), "%s:%s", endpoint->host, endpoint->port) >= (int)sizeof(endpoint_arg)) {
        errno = ENOSPC;
        return -1;
    }

    server_path = opty_server_path(argv0);
    if (server_path == NULL) {
        return -1;
    }

    if (command_argv != NULL) {
        while (command_argv[command_count] != NULL) {
            command_count++;
        }
    }

    exec_argv = calloc(command_count + 6u, sizeof(*exec_argv));
    if (exec_argv == NULL) {
        free(server_path);
        return -1;
    }
    exec_argv[exec_index++] = server_path;
    exec_argv[exec_index++] = "-b";
    exec_argv[exec_index++] = endpoint_arg;
    exec_argv[exec_index++] = "--";
    for (size_t i = 0; i < command_count; i++) {
        exec_argv[exec_index++] = command_argv[i];
    }
    exec_argv[exec_index] = NULL;

    pid = fork();
    if (pid < 0) {
        free(exec_argv);
        free(server_path);
        return -1;
    }

    if (pid == 0) {
        int log_fd;
        int null_fd;

        if (control_token != NULL && control_token[0] != '\0' &&
            setenv("OPTY_CONTROL_TOKEN", control_token, 1) < 0) {
            perror("set OPTY_CONTROL_TOKEN");
            _exit(127);
        }

        (void)setsid();
        null_fd = open("/dev/null", O_RDONLY);
        if (null_fd >= 0) {
            (void)dup2(null_fd, STDIN_FILENO);
            if (null_fd > STDERR_FILENO) {
                close(null_fd);
            }
        }

        log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (log_fd >= 0) {
            (void)dup2(log_fd, STDOUT_FILENO);
            (void)dup2(log_fd, STDERR_FILENO);
            if (log_fd > STDERR_FILENO) {
                close(log_fd);
            }
        }

        if (strchr(server_path, '/') != NULL) {
            execv(server_path, exec_argv);
        } else {
            execvp(server_path, exec_argv);
        }
        perror("exec 0pty-server");
        _exit(127);
    }

    free(exec_argv);
    free(server_path);

    if (opty_wait_for_server(pid, endpoint, log_path) < 0) {
        return -1;
    }
    fprintf(stderr, "started 0pty session %s on %s:%s; log: %s\n", session, endpoint->host, endpoint->port, log_path);
    return 0;
}

static int run_client_endpoint(const struct opty_endpoint *endpoint, const char *token, const char *log_path, bool retry)
{
    int log_fd = -1;
    int status = 1;
    int have_connected_once = 0;
    uint64_t last_seq = 0;

    if (token != NULL && strlen(token) > OPTY_MAX_TOKEN) {
        fprintf(stderr, "token exceeds maximum length of %u bytes\n", (unsigned)OPTY_MAX_TOKEN);
        return 1;
    }

    if (setup_signal_handlers() < 0) {
        perror("sigaction");
        return 1;
    }
    if (enable_raw_mode() < 0) {
        perror("terminal raw mode");
        return 1;
    }

    if (log_path != NULL) {
        log_fd = open_scrollback_log(log_path);
        if (log_fd < 0) {
            perror(log_path);
            return 1;
        }
    }

    for (;;) {
        int sock;
        int session_rc;
        bool reconnect = have_connected_once != 0;

        if (g_terminate_pending) {
            status = 0;
            break;
        }

        sock = connect_endpoint(endpoint);
        if (sock < 0) {
            if (!retry) {
                perror("connect");
                status = 1;
                break;
            }
            sleep_retry_interval();
            continue;
        }

        session_rc = run_session(sock, log_fd, reconnect, token, &last_seq);
        have_connected_once = 1;
        close(sock);

        if (session_rc == 0) {
            status = 0;
            break;
        }

        if (session_rc > 0) {
            if (!retry) {
                status = 1;
                break;
            }
            sleep_retry_interval();
            continue;
        }

        status = 1;
        break;
    }

    if (log_fd >= 0) {
        close(log_fd);
    }
    restore_terminal();
    return status;
}

static int run_named_connect(const char *session)
{
    char port[16];
    struct opty_endpoint endpoint;
    struct opty_session_info info;
    int rc;

    if (!opty_is_session_name(session)) {
        fprintf(stderr, "invalid session name: %s\n", session);
        return 1;
    }

    if (opty_read_session_info(session, &info) == 0) {
        endpoint.host = info.host;
        endpoint.port = info.port;
        rc = run_client_endpoint(&endpoint, NULL, NULL, false);
        opty_session_info_free(&info);
        return rc;
    } else if (opty_read_session_endpoint(session, port, sizeof(port), &endpoint) < 0) {
        if (opty_is_decimal(session) && opty_numeric_session_endpoint(session, port, sizeof(port), &endpoint) == 0) {
            return run_client_endpoint(&endpoint, NULL, NULL, false);
        }
        fprintf(stderr, "no such 0pty session: %s\n", session);
        fprintf(stderr, "start it with: 0pty %s start <command>\n", session);
        return 1;
    }

    return run_client_endpoint(&endpoint, NULL, NULL, false);
}

static int run_named_stop(const char *session)
{
    struct opty_session_info info;
    struct opty_endpoint endpoint;
    uint8_t graceful_input[OPTY_MAX_GRACEFUL_INPUT];
    size_t graceful_len = 0;
    int sock;
    int status = 0;
    long waited = 0;

    if (!opty_is_session_name(session)) {
        fprintf(stderr, "invalid session name: %s\n", session);
        return 1;
    }
    if (opty_read_session_info(session, &info) < 0) {
        fprintf(stderr, "no such 0pty session: %s\n", session);
        return 1;
    }
    if (info.control_token[0] == '\0') {
        fprintf(stderr, "session %s has no control token; restart it with the current 0pty before stopping\n", session);
        opty_session_info_free(&info);
        return 1;
    }

    endpoint.host = info.host;
    endpoint.port = info.port;
    if (!opty_endpoint_accepts_timeout(&endpoint, OPTY_LIST_CONNECT_TIMEOUT_MS)) {
        fprintf(stderr, "0pty session %s is already dead\n", session);
        opty_session_info_free(&info);
        return 0;
    }

    if (opty_decode_graceful_input(info.graceful_input, graceful_input, sizeof(graceful_input), &graceful_len) < 0) {
        perror("decode graceful_input");
        opty_session_info_free(&info);
        return 1;
    }

    sock = connect_endpoint(&endpoint);
    if (sock < 0) {
        perror("connect");
        opty_session_info_free(&info);
        return 1;
    }
    if (opty_send_control_shutdown(sock, info.control_token, graceful_input, graceful_len) < 0) {
        perror("send stop");
        close(sock);
        opty_session_info_free(&info);
        return 1;
    }
    (void)shutdown(sock, SHUT_WR);

    while (waited < OPTY_STOP_WAIT_MS) {
        struct pollfd pfd;
        int rc;

        pfd.fd = sock;
        pfd.events = POLLIN;
        pfd.revents = 0;
        rc = poll(&pfd, 1u, 100);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            status = 1;
            break;
        }
        if (rc == 0) {
            waited += 100L;
            continue;
        }
        if ((pfd.revents & POLLIN) != 0) {
            struct opty_frame frame = {0};
            int frame_rc = opty_recv_frame(sock, &frame);

            if (frame_rc == 0 && frame.type == OPTY_MSG_ERROR) {
                write_stderr_bytes(frame.payload, frame.len);
                status = 1;
            } else if (frame_rc < 0) {
                status = 1;
            }
            opty_frame_free(&frame);
            break;
        }
        if ((pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
            break;
        }
    }

    close(sock);

    if (status == 0 && opty_wait_endpoint_dead(&endpoint, OPTY_STOP_WAIT_MS) < 0) {
        fprintf(stderr, "sent stop request for %s, but it is still listening on %s:%s\n",
                session, endpoint.host, endpoint.port);
        status = 1;
    }
    if (status == 0) {
        fprintf(stderr, "stopped 0pty session %s\n", session);
    }

    opty_session_info_free(&info);
    return status;
}

static void opty_print_session_list(const struct opty_list_entry *entries, size_t count)
{
    printf("%-24s %-21s %-7s %-8s %s\n", "NAME", "ENDPOINT", "STATE", "POLICY", "CWD");
    for (size_t i = 0; i < count; i++) {
        char endpoint_text[160];

        if (snprintf(endpoint_text, sizeof(endpoint_text), "%s:%s", entries[i].info.host, entries[i].info.port) >= (int)sizeof(endpoint_text)) {
            continue;
        }
        printf("%-24s %-21s %-7s %-8s %s\n",
               entries[i].info.name,
               endpoint_text,
               entries[i].alive ? "alive" : "dead",
               entries[i].info.restart_policy[0] != '\0' ? entries[i].info.restart_policy : "manual",
               entries[i].info.cwd[0] != '\0' ? entries[i].info.cwd : "-");
    }
}

static void opty_free_session_entries(struct opty_list_entry *entries, size_t count)
{
    if (entries == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        opty_session_info_free(&entries[i].info);
    }
    free(entries);
}

static int opty_load_session_entries(struct opty_list_entry **entries_out, size_t *count_out)
{
    char dir[512];
    DIR *dp;
    struct dirent *ent;
    struct opty_list_entry *entries = NULL;
    size_t count = 0;
    size_t cap = 0;
    int status = 0;

    *entries_out = NULL;
    *count_out = 0;

    if (opty_session_dir(dir, sizeof(dir)) < 0) {
        perror("session directory");
        return 1;
    }

    dp = opendir(dir);
    if (dp == NULL) {
        if (errno == ENOENT) {
            return 0;
        }
        perror(dir);
        return 1;
    }

    while ((ent = readdir(dp)) != NULL) {
        size_t len = strlen(ent->d_name);
        char session[OPTY_SESSION_MAX_NAME + 1u];

        if (len <= 8u || strcmp(ent->d_name + len - 8u, ".session") != 0) {
            continue;
        }
        if (len - 8u > OPTY_SESSION_MAX_NAME) {
            continue;
        }
        memcpy(session, ent->d_name, len - 8u);
        session[len - 8u] = '\0';
        if (!opty_is_session_name(session)) {
            continue;
        }

        if (count == cap) {
            size_t new_cap = cap == 0u ? 8u : cap * 2u;
            struct opty_list_entry *new_entries = realloc(entries, new_cap * sizeof(*new_entries));
            if (new_entries == NULL) {
                perror("list sessions");
                status = 1;
                break;
            }
            entries = new_entries;
            cap = new_cap;
        }

        memset(&entries[count], 0, sizeof(entries[count]));
        if (opty_read_session_info(session, &entries[count].info) < 0) {
            continue;
        }
        count++;
    }

    closedir(dp);

    for (size_t i = 0; i < count && status == 0; i++) {
        if (pthread_create(&entries[i].thread, NULL, opty_list_probe_thread, &entries[i]) != 0) {
            status = 1;
            count = i;
            break;
        }
    }
    for (size_t i = 0; i < count; i++) {
        (void)pthread_join(entries[i].thread, NULL);
    }

    if (status != 0) {
        opty_free_session_entries(entries, count);
        return status;
    }

    *entries_out = entries;
    *count_out = count;
    return 0;
}

static int run_named_list(void)
{
    struct opty_list_entry *entries = NULL;
    size_t count = 0;
    int status = opty_load_session_entries(&entries, &count);

    if (status != 0) {
        return status;
    }
    opty_print_session_list(entries, count);
    opty_free_session_entries(entries, count);
    return status;
}

static int run_connect_auto(void)
{
    struct opty_list_entry *entries = NULL;
    size_t count = 0;
    int status = opty_load_session_entries(&entries, &count);

    if (status != 0) {
        return status;
    }

    if (count == 0u) {
        fprintf(stderr, "No 0pty sessions found. Start one with: 0pty NAME start <command>\n");
        opty_free_session_entries(entries, count);
        return 1;
    }

    if (count > 1u) {
        opty_print_session_list(entries, count);
        fprintf(stderr, "Multiple 0pty sessions found. Choose one with: 0pty connect NAME\n");
        opty_free_session_entries(entries, count);
        return 1;
    }

    if (!entries[0].alive) {
        fprintf(stderr, "Session %s is dead. Run '0pty restart %s' to bring it back.\n",
                entries[0].info.name, entries[0].info.name);
        opty_free_session_entries(entries, count);
        return 1;
    }

    status = run_named_connect(entries[0].info.name);
    opty_free_session_entries(entries, count);
    return status;
}

static int run_named_start(const char *argv0, const char *session, char **command_argv)
{
    char port[16];
    char log_path[512];
    char control_token[OPTY_CONTROL_TOKEN_BYTES * 2u + 1u];
    struct opty_endpoint endpoint;

    if (!opty_is_session_name(session)) {
        fprintf(stderr, "invalid session name: %s\n", session);
        return 1;
    }

    if (opty_read_session_endpoint(session, port, sizeof(port), &endpoint) == 0 && opty_endpoint_accepts(&endpoint)) {
        fprintf(stderr, "0pty session %s is already listening on %s:%s; connecting\n", session, endpoint.host, endpoint.port);
    } else {
        if (opty_pick_session_endpoint(port, sizeof(port), &endpoint) < 0 ||
            opty_session_log_path(session, log_path, sizeof(log_path)) < 0 ||
            opty_generate_control_token(control_token, sizeof(control_token)) < 0) {
            perror("allocate 0pty session");
            return 1;
        }
        if (opty_write_session(session, &endpoint, log_path, control_token, command_argv) < 0) {
            perror("write 0pty session");
            return 1;
        }
        if (opty_spawn_server(argv0, session, &endpoint, log_path, control_token, command_argv) < 0) {
            perror("start 0pty session");
            return 1;
        }
    }

    return run_client_endpoint(&endpoint, NULL, NULL, false);
}

static int run_named_restart(const char *argv0, const char *session)
{
    struct opty_session_info info;
    struct opty_endpoint endpoint;
    char host[128];
    char port[16];
    char current_cwd[1024];
    int have_current_cwd = 0;

    if (!opty_is_session_name(session)) {
        fprintf(stderr, "invalid session name: %s\n", session);
        return 1;
    }
    if (opty_read_session_info(session, &info) < 0) {
        fprintf(stderr, "no such 0pty session: %s\n", session);
        return 1;
    }
    if (info.argc == 0u) {
        fprintf(stderr, "session %s has no stored argv; start it again with: 0pty %s start <command>\n", session, session);
        opty_session_info_free(&info);
        return 1;
    }

    snprintf(host, sizeof(host), "%s", info.host);
    snprintf(port, sizeof(port), "%s", info.port);
    endpoint.host = host;
    endpoint.port = port;
    if (opty_endpoint_accepts(&endpoint)) {
        fprintf(stderr, "0pty session %s is already alive on %s:%s; use: 0pty connect %s\n",
                session, endpoint.host, endpoint.port, session);
        opty_session_info_free(&info);
        return 1;
    }

    if (getcwd(current_cwd, sizeof(current_cwd)) != NULL) {
        have_current_cwd = 1;
    }
    if (info.cwd[0] != '\0' && chdir(info.cwd) < 0) {
        perror(info.cwd);
        opty_session_info_free(&info);
        return 1;
    }

    if (opty_spawn_server(argv0, session, &endpoint, info.log[0] != '\0' ? info.log : "/tmp/0pty-restart.log",
                          info.control_token[0] != '\0' ? info.control_token : NULL, info.argv) < 0) {
        perror("restart 0pty session");
        if (have_current_cwd) {
            (void)chdir(current_cwd);
        }
        opty_session_info_free(&info);
        return 1;
    }

    if (have_current_cwd) {
        (void)chdir(current_cwd);
    }

    opty_session_info_free(&info);
    return run_client_endpoint(&endpoint, NULL, NULL, false);
}

static void usage(const char *argv0)
{
    opty_usage_client(argv0);
}

int main(int argc, char **argv)
{
    const char *token = NULL;
    const char *log_path = NULL;
    const char *endpoint_arg = NULL;
    bool retry = false;
    struct opty_endpoint endpoint;
    int c;

    static const struct option long_opts[] = {
        {"retry", no_argument, NULL, 1},
        {"help", no_argument, NULL, 2},
        {0, 0, 0, 0}
    };

    if (argc == 2 && strcmp(argv[1], "list") == 0) {
        return run_named_list();
    }
    if (argc == 2 && strcmp(argv[1], "connect") == 0) {
        return run_connect_auto();
    }
    if (argc >= 3 && strcmp(argv[1], "connect") == 0) {
        return run_named_connect(argv[2]);
    }
    if (argc >= 3 && strcmp(argv[1], "stop") == 0) {
        return run_named_stop(argv[2]);
    }
    if (argc >= 3 && strcmp(argv[1], "restart") == 0) {
        return run_named_restart(argv[0], argv[2]);
    }
    if (argc == 2 && opty_is_session_name(argv[1])) {
        return run_named_connect(argv[1]);
    }
    if (argc >= 3 && opty_is_session_name(argv[1]) && strcmp(argv[2], "connect") == 0) {
        return run_named_connect(argv[1]);
    }
    if (argc >= 3 && opty_is_session_name(argv[1]) && strcmp(argv[2], "stop") == 0) {
        return run_named_stop(argv[1]);
    }
    if (argc >= 3 && opty_is_session_name(argv[1]) && strcmp(argv[2], "restart") == 0) {
        return run_named_restart(argv[0], argv[1]);
    }
    if (argc >= 3 && opty_is_session_name(argv[1]) && strcmp(argv[2], "start") == 0) {
        char **command_argv = &argv[3];
        if (command_argv[0] != NULL && strcmp(command_argv[0], "--") == 0) {
            command_argv++;
        }
        return run_named_start(argv[0], argv[1], command_argv);
    }
    if (argc >= 3 && strcmp(argv[1], "start") == 0 && opty_is_session_name(argv[2])) {
        char **command_argv = &argv[3];
        if (command_argv[0] != NULL && strcmp(command_argv[0], "--") == 0) {
            command_argv++;
        }
        return run_named_start(argv[0], argv[2], command_argv);
    }

    while ((c = getopt_long(argc, argv, "t:l:", long_opts, NULL)) != -1) {
        switch (c) {
        case 't':
            token = optarg;
            break;
        case 'l':
            log_path = optarg;
            break;
        case 1:
            retry = true;
            break;
        case 2:
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    if (optind < argc) {
        endpoint_arg = argv[optind++];
    }
    if (optind < argc) {
        usage(argv[0]);
        return 1;
    }

    if (token != NULL && strlen(token) > OPTY_MAX_TOKEN) {
        fprintf(stderr, "token exceeds maximum length of %u bytes\n", (unsigned)OPTY_MAX_TOKEN);
        return 1;
    }

    if (endpoint_arg != NULL) {
        if (opty_parse_endpoint(endpoint_arg, OPTY_DEFAULT_HOST, OPTY_DEFAULT_PORT, &endpoint) < 0) {
            fprintf(stderr, "invalid endpoint: %s\n", endpoint_arg);
            return 1;
        }
    } else {
        endpoint.host = OPTY_DEFAULT_HOST;
        endpoint.port = OPTY_DEFAULT_PORT;
    }

    return run_client_endpoint(&endpoint, token, log_path, retry);
}
