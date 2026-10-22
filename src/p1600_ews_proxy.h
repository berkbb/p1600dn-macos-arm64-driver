#ifndef P1600_EWS_PROXY_H
#define P1600_EWS_PROXY_H

/*
 * The printer's web pages (EWS) of a USB printer, carried as HTTP over its
 * vendor interface FF/02/10 and served to the browser on the loopback
 * address, starting at /SSI/index.htm; the port is chosen by the system.
 * One request at a time, each answered with Content-Length and
 * "Connection: close"; the proxy ends after `idle_seconds` without a
 * request.
 */

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#define P1600_EWS_START_PAGE "/SSI/index.htm"
#define P1600_EWS_IDLE_SECONDS 180

/* The printer side: the request goes out with `write`, the response comes
 * back through `read` (bytes within `timeout_ms`, 0 at the end, -1 on
 * timeout or error). */
typedef struct {
    void *context;
    int (*write)(void *context, const void *data, size_t length);
    ssize_t (*read)(void *context, void *buffer, size_t length, int timeout_ms);
    void (*close)(void *context);
} P1600EwsTransport;

typedef struct P1600EwsProxy P1600EwsProxy;

/* Starts serving; `url` receives the start page address. The proxy owns
 * the transport and closes it when it stops. NULL with a reason. */
P1600EwsProxy *p1600_ews_proxy_start(P1600EwsTransport transport, int idle_seconds, char *url, size_t url_size,
                               char *error, size_t error_size);

/* The same over the USB printer a usb:// device URI names. */
P1600EwsProxy *p1600_ews_proxy_start_usb(const char *device_uri, int idle_seconds, char *url, size_t url_size,
                                   char *error, size_t error_size);

bool p1600_ews_proxy_running(P1600EwsProxy *proxy);

/* Waits until the proxy ends on its own (idle). Ended, it has closed its
 * listener and its transport (the printer's USB interface is free);
 * p1600_ews_proxy_stop() still frees it. */
void p1600_ews_proxy_wait(P1600EwsProxy *proxy);

/* Stops serving and frees the proxy. */
void p1600_ews_proxy_stop(P1600EwsProxy *proxy);

#endif /* P1600_EWS_PROXY_H */
