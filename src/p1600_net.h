#ifndef P1600_NET_H
#define P1600_NET_H

/*
 * Network access to the printer's AppSocket (JetDirect) port, where PJL
 * commands and their replies travel both ways.
 */

#include <stddef.h>
#include <sys/types.h>

#define P1600_NET_APPSOCKET_PORT 9100

typedef struct {
    char host[256];
    int port;
} P1600NetTarget;

/* The printer a CUPS device URI names: socket://host[:port], lpd, ipp,
 * ipps, http(s)://host (AppSocket port 9100), or a Bonjour service
 * (dnssd://, mdns://), resolved through DNS-SD within `timeout_ms`.
 * Returns 0, 1 when the URI is not a network URI, or -1 with a reason. */
int p1600_net_target_from_uri(const char *uri, P1600NetTarget *target, int timeout_ms, char *error, size_t error_size);

/* Splits a Bonjour service name "Name._type._tcp.domain." into its parts.
 * Returns 0, or -1 when it is not one. */
int p1600_net_split_service(const char *service, char *name, size_t name_size, char *type, size_t type_size,
                         char *domain, size_t domain_size);

/* A connected TCP socket, or -1 with a reason. */
int p1600_net_connect(const P1600NetTarget *target, int timeout_ms, char *error, size_t error_size);

/* Writes everything within `timeout_ms`; 0 or -1. */
int p1600_net_write(int fd, const void *data, size_t length, int timeout_ms);

/* What arrives within `timeout_ms`: the byte count, 0 when the printer
 * closed the connection, or -1 on timeout or error. */
ssize_t p1600_net_read(int fd, void *buffer, size_t length, int timeout_ms);

#endif /* P1600_NET_H */
