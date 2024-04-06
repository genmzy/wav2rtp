#ifndef __NETWORK_FILTER_H
#define __NETWORK_FILTER_H

#include "error_types.h"
#include "rtpapi.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>

typedef struct __wr_network_filer_context {
    int socket_fd;                      // local socket descriptor
    struct sockaddr_in *server_addr;    // remote server address
    int sended;                         // judge if the first rtp frame sended
    struct timespec *spec;
} wr_network_filter_context_t;

static inline void wr_network_filter_context_safe_free(wr_network_filter_context_t *ctx)
{
    if (!ctx) {
        return;
    }
    if (ctx->server_addr) {
        free(ctx->server_addr);
    }
    if (ctx->spec) {
        free(ctx->spec);
    }
    if (ctx->socket_fd) {
        close(ctx->socket_fd);
    }
    free(ctx);
}

static inline int has_prefix(const char *str, const char *prefix)
{
    int l = strlen(prefix);
    if (l > strlen(str)) return 0;
    for (int p=0; p<=l; ++p) {
        if (*(prefix+p) != *(str+p)) return 0;
    }
    return 1;
}

static inline int wr_parse_url(char *path, struct sockaddr_in *to)
{
    char *p;
    for (p = (char *)path; p; ++p) {
        if (*p == ':') {
            break;
        }
    }
    if (*p == '\0') {
        return -1;
    }
    *p = '\0';
    int port = atoi(p+1);
    if (!port) {
        return -1;
    }
    to->sin_family = AF_INET;
    to->sin_addr.s_addr = inet_addr(path);
    if (to->sin_addr.s_addr == INADDR_NONE) {
        return -1;
    }
    to->sin_port = ntohs(port);
    *p = ':'; // revert path to original
    return 0;
}

wr_errorcode_t wr_network_filter_notify(wr_rtp_filter_t * filter, wr_event_type_t event, wr_rtp_packet_t * packet);

#endif // !__NETWORK_FILTER_H
