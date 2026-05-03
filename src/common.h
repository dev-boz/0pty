#ifndef OPTY_COMMON_H
#define OPTY_COMMON_H

#include <stdint.h>

struct opty_endpoint {
    const char *host;
    const char *port;
};

int opty_parse_endpoint(const char *input, const char *default_host, const char *default_port, struct opty_endpoint *out);
void opty_usage_server(const char *argv0);
void opty_usage_client(const char *argv0);

#endif
