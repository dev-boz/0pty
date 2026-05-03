#include "common.h"

#include "protocol.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *opty_dup_range(const char *start, size_t len)
{
    char *copy;

    copy = malloc(len + 1u);
    if (copy == NULL) {
        return NULL;
    }
    if (len > 0u) {
        memcpy(copy, start, len);
    }
    copy[len] = '\0';
    return copy;
}

int opty_parse_endpoint(const char *input, const char *default_host, const char *default_port, struct opty_endpoint *out)
{
    const char *host = default_host;
    const char *port = default_port;
    char *host_alloc = NULL;
    char *port_alloc = NULL;

    if (out == NULL || default_host == NULL || default_port == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (input == NULL || *input == '\0') {
        out->host = host;
        out->port = port;
        return 0;
    }

    if (input[0] == '[') {
        const char *close = strchr(input + 1, ']');
        size_t host_len;

        if (close == NULL || close == input + 1) {
            errno = EINVAL;
            return -1;
        }
        host_len = (size_t)(close - (input + 1));
        host_alloc = opty_dup_range(input + 1, host_len);
        if (host_alloc == NULL) {
            errno = ENOMEM;
            return -1;
        }
        host = host_alloc;
        if (close[1] == '\0') {
            out->host = host;
            out->port = port;
            return 0;
        }
        if (close[1] != ':') {
            errno = EINVAL;
            goto fail;
        }
        if (close[2] == '\0') {
            errno = EINVAL;
            goto fail;
        }
        port_alloc = opty_dup_range(close + 2, strlen(close + 2));
        if (port_alloc == NULL) {
            errno = ENOMEM;
            goto fail;
        }
        port = port_alloc;
    } else {
        const char *first_colon = strchr(input, ':');

        if (first_colon == NULL) {
            host = input;
        } else {
            const char *second_colon = strchr(first_colon + 1, ':');
            size_t host_len;

            if (second_colon != NULL) {
                errno = EINVAL;
                return -1;
            }
            if (first_colon == input) {
                host = default_host;
            } else {
                host_len = (size_t)(first_colon - input);
                host_alloc = opty_dup_range(input, host_len);
                if (host_alloc == NULL) {
                    errno = ENOMEM;
                    return -1;
                }
                host = host_alloc;
            }
            if (first_colon[1] == '\0') {
                errno = EINVAL;
                goto fail;
            }
            port_alloc = opty_dup_range(first_colon + 1, strlen(first_colon + 1));
            if (port_alloc == NULL) {
                errno = ENOMEM;
                goto fail;
            }
            port = port_alloc;
        }
    }

    out->host = host;
    out->port = port;
    return 0;

fail:
    free(host_alloc);
    free(port_alloc);
    return -1;
}

void opty_usage_server(const char *argv0)
{
    const char *prog = argv0 != NULL ? argv0 : "0pty-server";

    fprintf(stderr, "Usage: %s [-b host:port] [-r bytes] [-t token] [--] [command args...]\n", prog);
    fprintf(stderr, "Default listen endpoint: %s:%s\n", OPTY_DEFAULT_HOST, OPTY_DEFAULT_PORT);
    fprintf(stderr, "Default command: $SHELL, or /bin/sh when SHELL is unset\n");
}

void opty_usage_client(const char *argv0)
{
    const char *prog = argv0 != NULL ? argv0 : "0pty";

    fprintf(stderr, "Usage: %s NAME start [--] [command args...]\n", prog);
    fprintf(stderr, "       %s start NAME [--] [command args...]\n", prog);
    fprintf(stderr, "       %s connect\n", prog);
    fprintf(stderr, "       %s connect NAME\n", prog);
    fprintf(stderr, "       %s restart NAME\n", prog);
    fprintf(stderr, "       %s NAME\n", prog);
    fprintf(stderr, "       %s list\n", prog);
    fprintf(stderr, "       %s [-t token] [-l scrollback-file] [--retry] [host:port]\n", prog);
    fprintf(stderr, "Session names use letters, digits, dots, dashes, and underscores.\n");
    fprintf(stderr, "Session records live under ~/.0pty/sessions and allocate localhost ports automatically.\n");
    fprintf(stderr, "Default connect endpoint: %s:%s\n", OPTY_DEFAULT_HOST, OPTY_DEFAULT_PORT);
}
