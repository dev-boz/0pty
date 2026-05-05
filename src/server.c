#include "common.h"
#include "protocol.h"
#include "ring.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <pty.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define OPTY_STOP_GRACE_MS 2000L
#define OPTY_STOP_SIGNAL_GRACE_MS 1000L

struct server;

struct client {
    struct server *srv;
    int fd;
    pthread_t thread;
    pthread_mutex_t write_mu;
    struct client *next;
    size_t refs;
    int in_list;
    int dead;
    int authenticated;
    uint64_t live_seq;
};

struct server {
    int listen_fd;
    int master_fd;
    pid_t child_pid;
    pthread_mutex_t clients_mu;
    pthread_mutex_t pty_mu;
    pthread_mutex_t child_mu;
    pthread_mutex_t fds_mu;
    struct client *clients;
    struct opty_ring ring;
    const char *token;
    const char *control_token;
    int child_reaped;
    int child_status;
};

static int server_write_pty(struct server *srv, const uint8_t *data, size_t len);

static void die_perror(const char *what)
{
    perror(what);
}

static void die_msg(const char *what)
{
    fprintf(stderr, "%s\n", what);
}

static void server_sleep_millis(long millis)
{
    struct timespec req;

    req.tv_sec = millis / 1000L;
    req.tv_nsec = (millis % 1000L) * 1000000L;
    while (nanosleep(&req, &req) < 0 && errno == EINTR) {
    }
}

static int server_poll_child_locked(struct server *srv)
{
    int status = 0;
    pid_t done;

    if (srv->child_reaped || srv->child_pid <= 0) {
        return 1;
    }

    for (;;) {
        done = waitpid(srv->child_pid, &status, WNOHANG);
        if (done == srv->child_pid) {
            srv->child_reaped = 1;
            srv->child_status = status;
            return 1;
        }
        if (done == 0) {
            return 0;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == ECHILD) {
            srv->child_reaped = 1;
            return 1;
        }
        return -1;
    }
}

static int server_wait_child_deadline(struct server *srv, long millis)
{
    long waited = 0;

    for (;;) {
        int done;

        if (pthread_mutex_lock(&srv->child_mu) != 0) {
            return -1;
        }
        done = server_poll_child_locked(srv);
        pthread_mutex_unlock(&srv->child_mu);

        if (done != 0) {
            return done > 0 ? 1 : -1;
        }
        if (waited >= millis) {
            return 0;
        }
        server_sleep_millis(50L);
        waited += 50L;
    }
}

static void server_wait_child_blocking(struct server *srv)
{
    if (pthread_mutex_lock(&srv->child_mu) != 0) {
        return;
    }

    while (!srv->child_reaped && srv->child_pid > 0) {
        int status = 0;
        pid_t done = waitpid(srv->child_pid, &status, 0);

        if (done == srv->child_pid) {
            srv->child_reaped = 1;
            srv->child_status = status;
            break;
        }
        if (done < 0 && errno == EINTR) {
            continue;
        }
        if (done < 0 && errno == ECHILD) {
            srv->child_reaped = 1;
            break;
        }
        break;
    }

    pthread_mutex_unlock(&srv->child_mu);
}

static void server_signal_child(struct server *srv, int signo)
{
    if (pthread_mutex_lock(&srv->child_mu) != 0) {
        return;
    }
    if (!srv->child_reaped && srv->child_pid > 0) {
        (void)kill(srv->child_pid, signo);
    }
    pthread_mutex_unlock(&srv->child_mu);
}

static void server_close_listener(struct server *srv)
{
    if (pthread_mutex_lock(&srv->fds_mu) != 0) {
        return;
    }
    if (srv->listen_fd >= 0) {
        int fd = srv->listen_fd;

        srv->listen_fd = -1;
        (void)shutdown(fd, SHUT_RDWR);
        close(fd);
    }
    pthread_mutex_unlock(&srv->fds_mu);
}

static void server_shutdown_clients(struct server *srv)
{
    if (pthread_mutex_lock(&srv->clients_mu) == 0) {
        for (struct client *c = srv->clients; c; c = c->next) {
            (void)shutdown(c->fd, SHUT_RDWR);
        }
        pthread_mutex_unlock(&srv->clients_mu);
    }
}

static void server_request_shutdown(struct server *srv, const uint8_t *input, size_t input_len)
{
    int done;

    if (input_len > 0u) {
        (void)server_write_pty(srv, input, input_len);
    }

    done = server_wait_child_deadline(srv, OPTY_STOP_GRACE_MS);
    if (done == 0) {
        server_signal_child(srv, SIGHUP);
        done = server_wait_child_deadline(srv, OPTY_STOP_SIGNAL_GRACE_MS);
    }
    if (done == 0) {
        server_signal_child(srv, SIGTERM);
        (void)server_wait_child_deadline(srv, OPTY_STOP_SIGNAL_GRACE_MS);
    }

    server_close_listener(srv);
    server_shutdown_clients(srv);
}

static void client_free(struct client *c)
{
    pthread_mutex_destroy(&c->write_mu);
    close(c->fd);
    free(c);
}

static void client_release_locked(struct client *c)
{
    if (c->refs == 0) {
        return;
    }
    c->refs--;
    if (c->refs == 0) {
        client_free(c);
    }
}

static void server_drop_from_list_locked(struct server *srv, struct client *c)
{
    if (!c->in_list) {
        return;
    }

    struct client **cur = &srv->clients;
    while (*cur && *cur != c) {
        cur = &(*cur)->next;
    }
    if (*cur == c) {
        *cur = c->next;
    }
    c->in_list = 0;
    client_release_locked(c);
}

static void server_mark_dead_locked(struct server *srv, struct client *c)
{
    if (!c->dead) {
        c->dead = 1;
        (void)shutdown(c->fd, SHUT_RDWR);
    }
    server_drop_from_list_locked(srv, c);
}

static struct client **server_snapshot_clients(struct server *srv, size_t *count_out)
{
    *count_out = 0;

    if (pthread_mutex_lock(&srv->clients_mu) != 0) {
        return NULL;
    }

    size_t count = 0;
    for (struct client *c = srv->clients; c; c = c->next) {
        if (c->authenticated && !c->dead) {
            count++;
        }
    }

    if (count == 0) {
        pthread_mutex_unlock(&srv->clients_mu);
        return NULL;
    }

    struct client **snap = calloc(count, sizeof(*snap));
    if (snap == NULL) {
        pthread_mutex_unlock(&srv->clients_mu);
        return NULL;
    }

    size_t idx = 0;
    for (struct client *c = srv->clients; c; c = c->next) {
        if (c->authenticated && !c->dead) {
            c->refs++;
            snap[idx++] = c;
        }
    }

    pthread_mutex_unlock(&srv->clients_mu);
    *count_out = idx;
    return snap;
}

static void server_release_snapshot(struct server *srv, struct client **snap, size_t count)
{
    if (snap == NULL) {
        return;
    }

    if (pthread_mutex_lock(&srv->clients_mu) == 0) {
        for (size_t i = 0; i < count; i++) {
            client_release_locked(snap[i]);
        }
        pthread_mutex_unlock(&srv->clients_mu);
    }
    free(snap);
}

static int server_broadcast_stdout(struct server *srv, uint64_t seq, const uint8_t *data, size_t len)
{
    uint64_t end_seq = seq + (uint64_t)len;
    size_t count = 0;
    struct client **snap = server_snapshot_clients(srv, &count);
    if (count == 0) {
        free(snap);
        return 0;
    }
    if (snap == NULL) {
        return -1;
    }

    for (size_t i = 0; i < count; i++) {
        struct client *c = snap[i];
        if (pthread_mutex_lock(&c->write_mu) != 0) {
            continue;
        }
        int rc = 0;
        if (c->live_seq < end_seq) {
            size_t off = 0;
            uint64_t send_seq = seq;
            if (c->live_seq > seq) {
                off = (size_t)(c->live_seq - seq);
                send_seq = c->live_seq;
            }
            rc = opty_send_stdout(c->fd, send_seq, data + off, len - off);
            if (rc == 0) {
                c->live_seq = end_seq;
            }
        }
        pthread_mutex_unlock(&c->write_mu);
        if (rc < 0) {
            if (pthread_mutex_lock(&srv->clients_mu) == 0) {
                server_mark_dead_locked(srv, c);
                pthread_mutex_unlock(&srv->clients_mu);
            }
        }
    }

    server_release_snapshot(srv, snap, count);
    return 0;
}

static int server_resize_pty(struct server *srv, uint16_t cols, uint16_t rows)
{
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = cols;
    ws.ws_row = rows;

    if (pthread_mutex_lock(&srv->pty_mu) != 0) {
        return -1;
    }
    int rc = ioctl(srv->master_fd, TIOCSWINSZ, &ws);
    pthread_mutex_unlock(&srv->pty_mu);
    return rc;
}

static int server_write_pty(struct server *srv, const uint8_t *data, size_t len)
{
    if (len == 0) {
        return 0;
    }
    if (pthread_mutex_lock(&srv->pty_mu) != 0) {
        return -1;
    }
    ssize_t n = opty_write_full(srv->master_fd, data, len);
    pthread_mutex_unlock(&srv->pty_mu);
    return n < 0 ? -1 : 0;
}

static int server_send_error(struct client *c, const char *msg)
{
    if (pthread_mutex_lock(&c->write_mu) != 0) {
        return -1;
    }
    int rc = opty_send_error(c->fd, msg);
    pthread_mutex_unlock(&c->write_mu);
    return rc;
}

static int server_send_welcome_and_replay(struct server *srv, struct client *c, uint64_t requested_seq)
{
    uint64_t base_seq = 0;
    uint64_t next_seq = 0;
    uint64_t from_seq = 0;
    uint64_t replay_next = 0;
    size_t replay_len = 0;
    int rc;

    if (pthread_mutex_lock(&srv->clients_mu) != 0) {
        return -1;
    }
    if (pthread_mutex_lock(&c->write_mu) != 0) {
        pthread_mutex_unlock(&srv->clients_mu);
        return -1;
    }

    /* Lock order for the attach path is clients_mu -> write_mu -> ring.mu. */
    uint8_t *replay = opty_ring_snapshot_from(&srv->ring, requested_seq, &from_seq, &replay_next, &replay_len);
    if (replay_len > 0 && replay == NULL) {
        pthread_mutex_unlock(&c->write_mu);
        pthread_mutex_unlock(&srv->clients_mu);
        return -1;
    }
    opty_ring_bounds(&srv->ring, &base_seq, &next_seq);

    c->authenticated = 1;
    c->live_seq = replay_next;
    pthread_mutex_unlock(&srv->clients_mu);

    rc = opty_send_welcome(c->fd, base_seq, next_seq);
    if (rc == 0) {
        rc = opty_send_replay(c->fd, from_seq, replay, replay_len);
    }

    pthread_mutex_unlock(&c->write_mu);
    free(replay);
    return rc;
}

static void server_finish_client(struct server *srv, struct client *c)
{
    if (pthread_mutex_lock(&srv->clients_mu) == 0) {
        server_mark_dead_locked(srv, c);
        client_release_locked(c);
        pthread_mutex_unlock(&srv->clients_mu);
    }
}

static int server_process_intro(struct server *srv, struct client *c, const struct opty_frame *frame)
{
    uint64_t last_seq = 0;
    uint16_t cols = 0;
    uint16_t rows = 0;
    char token[OPTY_MAX_TOKEN + 1];
    memset(token, 0, sizeof(token));

    if (opty_parse_client_intro(frame, &last_seq, &cols, &rows, token, sizeof(token)) < 0) {
        return -1;
    }
    if (cols == 0 || rows == 0) {
        return -1;
    }
    if (srv->token != NULL && srv->token[0] != '\0' && strcmp(srv->token, token) != 0) {
        return -2;
    }

    if (server_resize_pty(srv, cols, rows) < 0) {
        return -1;
    }

    if (server_send_welcome_and_replay(srv, c, frame->type == OPTY_MSG_RECONNECT ? last_seq : 0) < 0) {
        return -1;
    }

    return 0;
}

static int server_process_control_shutdown(struct server *srv, const struct opty_frame *frame)
{
    char token[OPTY_MAX_TOKEN + 1u];
    const uint8_t *input = NULL;
    size_t input_len = 0;

    memset(token, 0, sizeof(token));
    if (opty_parse_control_shutdown(frame, token, sizeof(token), &input, &input_len) < 0) {
        return -1;
    }
    if (srv->control_token == NULL || srv->control_token[0] == '\0' || strcmp(srv->control_token, token) != 0) {
        return -2;
    }

    server_request_shutdown(srv, input, input_len);
    return 0;
}

static void *client_thread_main(void *arg)
{
    struct client *c = arg;
    struct server *srv = c->srv;

    pthread_detach(pthread_self());

    struct opty_frame frame;
    memset(&frame, 0, sizeof(frame));

    if (opty_recv_frame(c->fd, &frame) < 0) {
        server_finish_client(srv, c);
        return NULL;
    }

    if (frame.type == OPTY_MSG_CONTROL_SHUTDOWN) {
        int control_rc = server_process_control_shutdown(srv, &frame);
        if (control_rc == -2) {
            (void)server_send_error(c, "control authentication failed");
        } else if (control_rc < 0) {
            (void)server_send_error(c, "bad control request");
        }
    } else if (frame.type != OPTY_MSG_HELLO && frame.type != OPTY_MSG_RECONNECT) {
        (void)server_send_error(c, "expected hello or reconnect");
    } else {
        int intro_rc = server_process_intro(srv, c, &frame);
        if (intro_rc == -2) {
            (void)server_send_error(c, "authentication failed");
        } else if (intro_rc < 0) {
            (void)server_send_error(c, "bad intro");
        } else {
            for (;;) {
                opty_frame_free(&frame);
                memset(&frame, 0, sizeof(frame));
                if (opty_recv_frame(c->fd, &frame) < 0) {
                    break;
                }

                if (frame.type == OPTY_MSG_STDIN) {
                    if (server_write_pty(srv, frame.payload, frame.len) < 0) {
                        break;
                    }
                } else if (frame.type == OPTY_MSG_RESIZE) {
                    uint16_t cols = 0;
                    uint16_t rows = 0;
                    if (opty_parse_resize(&frame, &cols, &rows) < 0 || cols == 0 || rows == 0) {
                        break;
                    }
                    if (server_resize_pty(srv, cols, rows) < 0) {
                        break;
                    }
                } else if (frame.type == OPTY_MSG_ACK) {
                    uint64_t seq = 0;
                    if (opty_parse_ack(&frame, &seq) < 0) {
                        break;
                    }
                    (void)seq;
                } else {
                    break;
                }
            }
        }
    }

    opty_frame_free(&frame);
    server_finish_client(srv, c);
    return NULL;
}

static void *pty_thread_main(void *arg)
{
    struct server *srv = arg;

    pthread_detach(pthread_self());

    uint8_t buf[4096];
    for (;;) {
        ssize_t n = read(srv->master_fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0) {
            break;
        }

        uint64_t start_seq = 0;
        opty_ring_append(&srv->ring, buf, (size_t)n, &start_seq);
        if (server_broadcast_stdout(srv, start_seq, buf, (size_t)n) < 0) {
            break;
        }
    }

    server_close_listener(srv);
    server_shutdown_clients(srv);
    server_wait_child_blocking(srv);

    return NULL;
}

static int server_add_client(struct server *srv, int fd)
{
    struct client *c = calloc(1, sizeof(*c));

    (void)opty_set_tcp_nodelay(fd);

    if (c == NULL) {
        close(fd);
        return -1;
    }

    c->srv = srv;
    c->fd = fd;
    c->refs = 2;
    c->in_list = 1;
    c->dead = 0;
    c->authenticated = 0;
    c->live_seq = 0;

    if (pthread_mutex_init(&c->write_mu, NULL) != 0) {
        close(fd);
        free(c);
        return -1;
    }

    if (pthread_mutex_lock(&srv->clients_mu) != 0) {
        pthread_mutex_destroy(&c->write_mu);
        close(fd);
        free(c);
        return -1;
    }

    c->next = srv->clients;
    srv->clients = c;
    pthread_mutex_unlock(&srv->clients_mu);

    if (pthread_create(&c->thread, NULL, client_thread_main, c) != 0) {
        if (pthread_mutex_lock(&srv->clients_mu) == 0) {
            server_mark_dead_locked(srv, c);
            client_release_locked(c);
            pthread_mutex_unlock(&srv->clients_mu);
        }
        return -1;
    }

    return 0;
}

static int server_spawn_pty(struct server *srv, char **child_argv)
{
    int master_fd = -1;
    int slave_fd = -1;
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = 80;
    ws.ws_row = 24;

    if (openpty(&master_fd, &slave_fd, NULL, NULL, &ws) < 0) {
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(master_fd);
        close(slave_fd);
        return -1;
    }

    if (pid == 0) {
        if (srv->listen_fd >= 0) {
            close(srv->listen_fd);
        }
        close(master_fd);

        if (setsid() < 0) {
            _exit(127);
        }
        if (ioctl(slave_fd, TIOCSCTTY, 0) < 0) {
            _exit(127);
        }

        if (dup2(slave_fd, STDIN_FILENO) < 0 ||
            dup2(slave_fd, STDOUT_FILENO) < 0 ||
            dup2(slave_fd, STDERR_FILENO) < 0) {
            _exit(127);
        }
        if (slave_fd > STDERR_FILENO) {
            close(slave_fd);
        }

        execvp(child_argv[0], child_argv);
        _exit(127);
    }

    close(slave_fd);
    srv->master_fd = master_fd;
    srv->child_pid = pid;
    return 0;
}

static int server_create_listener(const char *host, const char *port)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo(%s:%s): %s\n", host, port, gai_strerror(rc));
        return -1;
    }

    int listen_fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }

        int one = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && listen(fd, 16) == 0) {
            listen_fd = fd;
            break;
        }
        close(fd);
    }

    freeaddrinfo(res);
    return listen_fd;
}

int main(int argc, char **argv)
{
    struct opty_endpoint bind_ep = {
        .host = OPTY_DEFAULT_HOST,
        .port = OPTY_DEFAULT_PORT,
    };
    const char *token = NULL;
    const char *control_token = NULL;
    char control_token_buf[OPTY_MAX_TOKEN + 1u];
    size_t ring_size = OPTY_DEFAULT_RING_SIZE;

    char **child_argv = NULL;

    control_token_buf[0] = '\0';

    opterr = 0;
    optind = 1;

    int opt;
    while ((opt = getopt(argc, argv, "b:r:t:c:h")) != -1) {
        switch (opt) {
        case 'b':
            if (opty_parse_endpoint(optarg, OPTY_DEFAULT_HOST, OPTY_DEFAULT_PORT, &bind_ep) < 0) {
                opty_usage_server(argv[0]);
                return 2;
            }
            break;
        case 'r': {
            errno = 0;
            char *end = NULL;
            unsigned long long v = strtoull(optarg, &end, 10);
            if (errno != 0 || end == optarg || *end != '\0' || v == 0 || v > SIZE_MAX) {
                opty_usage_server(argv[0]);
                return 2;
            }
            ring_size = (size_t)v;
            break;
        }
        case 't':
            if (strlen(optarg) > OPTY_MAX_TOKEN) {
                opty_usage_server(argv[0]);
                return 2;
            }
            token = optarg;
            break;
        case 'c':
            if (strlen(optarg) > OPTY_MAX_TOKEN) {
                opty_usage_server(argv[0]);
                return 2;
            }
            snprintf(control_token_buf, sizeof(control_token_buf), "%s", optarg);
            control_token = control_token_buf;
            break;
        case 'h':
            opty_usage_server(argv[0]);
            return 0;
        default:
            opty_usage_server(argv[0]);
            return 2;
        }
    }

    if (control_token == NULL) {
        const char *env_token = getenv("OPTY_CONTROL_TOKEN");

        if (env_token != NULL && env_token[0] != '\0') {
            if (strlen(env_token) > OPTY_MAX_TOKEN) {
                opty_usage_server(argv[0]);
                return 2;
            }
            snprintf(control_token_buf, sizeof(control_token_buf), "%s", env_token);
            control_token = control_token_buf;
        }
    }
    unsetenv("OPTY_CONTROL_TOKEN");

    if (optind < argc && strcmp(argv[optind], "--") == 0) {
        optind++;
    }
    if (optind < argc) {
        child_argv = &argv[optind];
    } else {
        const char *shell = getenv("SHELL");
        if (shell == NULL || shell[0] == '\0') {
            shell = "/bin/sh";
        }
        static char *default_argv[2];
        default_argv[0] = (char *)shell;
        default_argv[1] = NULL;
        child_argv = default_argv;
    }

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        die_perror("signal");
        return 1;
    }

    struct server srv;
    memset(&srv, 0, sizeof(srv));
    srv.listen_fd = -1;
    srv.master_fd = -1;
    srv.child_pid = -1;
    srv.token = token;
    srv.control_token = control_token;

    if (pthread_mutex_init(&srv.clients_mu, NULL) != 0) {
        die_msg("pthread_mutex_init");
        return 1;
    }
    if (pthread_mutex_init(&srv.pty_mu, NULL) != 0) {
        die_msg("pthread_mutex_init");
        pthread_mutex_destroy(&srv.clients_mu);
        return 1;
    }
    if (pthread_mutex_init(&srv.child_mu, NULL) != 0) {
        die_msg("pthread_mutex_init");
        pthread_mutex_destroy(&srv.pty_mu);
        pthread_mutex_destroy(&srv.clients_mu);
        return 1;
    }
    if (pthread_mutex_init(&srv.fds_mu, NULL) != 0) {
        die_msg("pthread_mutex_init");
        pthread_mutex_destroy(&srv.child_mu);
        pthread_mutex_destroy(&srv.pty_mu);
        pthread_mutex_destroy(&srv.clients_mu);
        return 1;
    }
    if (opty_ring_init(&srv.ring, ring_size) < 0) {
        die_msg("ring init failed");
        pthread_mutex_destroy(&srv.fds_mu);
        pthread_mutex_destroy(&srv.child_mu);
        pthread_mutex_destroy(&srv.pty_mu);
        pthread_mutex_destroy(&srv.clients_mu);
        return 1;
    }

    srv.listen_fd = server_create_listener(bind_ep.host, bind_ep.port);
    if (srv.listen_fd < 0) {
        die_perror("bind/listen");
        opty_ring_destroy(&srv.ring);
        pthread_mutex_destroy(&srv.fds_mu);
        pthread_mutex_destroy(&srv.child_mu);
        pthread_mutex_destroy(&srv.pty_mu);
        pthread_mutex_destroy(&srv.clients_mu);
        return 1;
    }

    if (server_spawn_pty(&srv, child_argv) < 0) {
        die_perror("spawn pty");
        close(srv.listen_fd);
        opty_ring_destroy(&srv.ring);
        pthread_mutex_destroy(&srv.fds_mu);
        pthread_mutex_destroy(&srv.child_mu);
        pthread_mutex_destroy(&srv.pty_mu);
        pthread_mutex_destroy(&srv.clients_mu);
        return 1;
    }

    pthread_t pty_thread;
    if (pthread_create(&pty_thread, NULL, pty_thread_main, &srv) != 0) {
        die_msg("pthread_create");
        close(srv.listen_fd);
        if (srv.child_pid > 0) {
            kill(srv.child_pid, SIGTERM);
            (void)waitpid(srv.child_pid, NULL, 0);
        }
        close(srv.master_fd);
        opty_ring_destroy(&srv.ring);
        pthread_mutex_destroy(&srv.fds_mu);
        pthread_mutex_destroy(&srv.child_mu);
        pthread_mutex_destroy(&srv.pty_mu);
        pthread_mutex_destroy(&srv.clients_mu);
        return 1;
    }

    for (;;) {
        struct sockaddr_storage ss;
        socklen_t slen = sizeof(ss);
        int fd = accept(srv.listen_fd, (struct sockaddr *)&ss, &slen);
        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EBADF || errno == EINVAL) {
                break;
            }
            if (errno == EMFILE || errno == ENFILE || errno == ENOMEM || errno == ENOBUFS) {
                usleep(100000);
                continue;
            }
            die_perror("accept");
            break;
        }

        (void)server_add_client(&srv, fd);
    }

    return 0;
}
