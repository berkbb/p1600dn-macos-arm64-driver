#include "p1600_ews_proxy.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "p1600_supplies.h"
#include "p1600_usb.h"

#define REQUEST_HEAD_LIMIT (16U * 1024U)
#define REQUEST_BODY_LIMIT (1024U * 1024U)
#define RESPONSE_HEAD_LIMIT (16U * 1024U)
#define RESPONSE_BODY_LIMIT (8U * 1024U * 1024U)
#define BROWSER_TIMEOUT_MS 5000
#define PRINTER_TIMEOUT_MS 10000
#define DRAIN_QUIET_MS 1000

struct P1600EwsProxy {
    P1600EwsTransport transport;
    int listener;
    int wake[2];
    int idle_seconds;
    unsigned port;
    bool out_of_step;  /* an exchange failed: the printer's late reply may still come */
    pthread_t thread;
    bool joined;
    atomic_bool running;
};

static int fail(char *error, size_t error_size, const char *format, ...) {
    if (error && error_size > 0) {
        va_list arguments;
        va_start(arguments, format);
        vsnprintf(error, error_size, format, arguments);
        va_end(arguments);
    }
    return -1;
}

static int send_all(int fd, const void *data, size_t length) {
    const unsigned char *bytes = data;
    while (length > 0) {
        ssize_t sent = send(fd, bytes, length, 0);
        if (sent < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        bytes += sent;
        length -= (size_t)sent;
    }
    return 0;
}

static ssize_t receive(int fd, void *buffer, size_t length, int timeout_ms) {
    for (;;) {
        struct pollfd channel = {.fd = fd, .events = POLLIN};
        int ready = poll(&channel, 1, timeout_ms);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) return -1;
        ssize_t received = recv(fd, buffer, length, 0);
        if (received < 0 && errno == EINTR) continue;
        return received;
    }
}

/* Reads one browser request: head up to the blank line plus its body. */
static unsigned char *read_request(int fd, size_t *length) {
    size_t capacity = REQUEST_HEAD_LIMIT, used = 0;
    unsigned char *request = malloc(capacity);
    if (!request) return NULL;
    size_t head_end = 0;
    while (head_end == 0) {
        if (used == REQUEST_HEAD_LIMIT) break;
        ssize_t received = receive(fd, request + used, REQUEST_HEAD_LIMIT - used, BROWSER_TIMEOUT_MS);
        if (received <= 0) break;
        used += (size_t)received;
        const unsigned char *end = memmem(request, used, "\r\n\r\n", 4);
        if (end) head_end = (size_t)(end - request) + 4U;
    }
    if (head_end == 0) {
        free(request);
        return NULL;
    }
    /* Content-Length of a request body (POST from the printer's forms). */
    size_t body = 0;
    for (size_t i = 0; i + 15U < head_end; i++) {
        if ((i == 0 || request[i - 1] == '\n') && strncasecmp((const char *)request + i, "Content-Length:", 15) == 0) {
            body = strtoul((const char *)request + i + 15, NULL, 10);
            break;
        }
    }
    if (body > REQUEST_BODY_LIMIT) {
        free(request);
        return NULL;
    }
    size_t total = head_end + body;
    if (total > capacity) {
        unsigned char *grown = realloc(request, total);
        if (!grown) {
            free(request);
            return NULL;
        }
        request = grown;
    }
    while (used < total) {
        ssize_t received = receive(fd, request + used, total - used, BROWSER_TIMEOUT_MS);
        if (received <= 0) {
            free(request);
            return NULL;
        }
        used += (size_t)received;
    }
    *length = total;
    return request;
}

/* The value of header `name` in the request head (without its blanks), or
 * NULL. */
static const char *header_value(const unsigned char *request, size_t head_length, const char *name,
                                size_t *value_length) {
    size_t name_length = strlen(name);
    for (size_t i = 0; i + name_length + 1U < head_length; i++) {
        if ((i > 0 && request[i - 1] != '\n') || strncasecmp((const char *)request + i, name, name_length) != 0 ||
            request[i + name_length] != ':') {
            continue;
        }
        const char *value = (const char *)request + i + name_length + 1U;
        const char *end = (const char *)request + head_length;
        while (value < end && (*value == ' ' || *value == '\t')) value++;
        const char *line_end = value;
        while (line_end < end && *line_end != '\r' && *line_end != '\n') line_end++;
        while (line_end > value && (line_end[-1] == ' ' || line_end[-1] == '\t')) line_end--;
        *value_length = (size_t)(line_end - value);
        return value;
    }
    return NULL;
}

/* "<scheme>127.0.0.1:<port>" or "<scheme>localhost:<port>". */
static bool own_address(const char *value, size_t length, unsigned port, const char *scheme) {
    static const char *const hosts[] = {"127.0.0.1", "localhost"};
    for (size_t i = 0; i < sizeof(hosts) / sizeof(hosts[0]); i++) {
        char expected[64];
        int expected_length = snprintf(expected, sizeof(expected), "%s%s:%u", scheme, hosts[i], port);
        if (expected_length > 0 && length == (size_t)expected_length && strncasecmp(value, expected, length) == 0) {
            return true;
        }
    }
    return false;
}

/* Only the browser's own pages of this proxy reach the printer: Host names
 * the proxy (no DNS rebinding), and a request another site's page made
 * (Sec-Fetch-Site, Origin, a form post's Referer) is refused (no CSRF
 * against the printer's settings pages). */
static bool from_own_pages(const P1600EwsProxy *proxy, const unsigned char *request, size_t length) {
    const unsigned char *blank = memmem(request, length, "\r\n\r\n", 4);
    size_t head = blank ? (size_t)(blank - request) + 2U : length;
    size_t value_length = 0;
    const char *value = header_value(request, head, "Host", &value_length);
    if (!value || !own_address(value, value_length, proxy->port, "")) return false;
    value = header_value(request, head, "Sec-Fetch-Site", &value_length);
    if (value && !(value_length == 4 && strncasecmp(value, "none", 4) == 0) &&
        !(value_length == 11 && strncasecmp(value, "same-origin", 11) == 0)) {
        return false;
    }
    value = header_value(request, head, "Origin", &value_length);
    if (value) return own_address(value, value_length, proxy->port, "http://");
    bool safe = (length >= 4 && memcmp(request, "GET ", 4) == 0) || (length >= 5 && memcmp(request, "HEAD ", 5) == 0);
    value = safe ? NULL : header_value(request, head, "Referer", &value_length);
    if (value) {
        const char *path = value_length > 7 ? memchr(value + 7, '/', value_length - 7) : NULL;
        return path && own_address(value, (size_t)(path - value), proxy->port, "http://");
    }
    return true;
}

/* A reply that comes after its exchange failed (a printer slower than the
 * timeout, a reply the reader gave up on) would answer the next request:
 * drop what comes until the printer has been quiet for a second. */
static void drain(P1600EwsTransport *transport) {
    unsigned char scratch[4096];
    for (size_t dropped = 0; dropped < RESPONSE_BODY_LIMIT;) {
        ssize_t received = transport->read(transport->context, scratch, sizeof(scratch), DRAIN_QUIET_MS);
        if (received <= 0) break;
        dropped += (size_t)received;
    }
}

typedef struct {
    P1600EwsTransport *transport;
} PrinterReader;

static ssize_t read_printer(void *context, unsigned char *buffer, size_t length) {
    PrinterReader *reader = context;
    return reader->transport->read(reader->transport->context, buffer, length, PRINTER_TIMEOUT_MS);
}

/* The printer's answer, re-framed for the browser: its status line and
 * headers without the transfer framing, then Content-Length and close. */
static void answer(P1600EwsProxy *proxy, int client, const unsigned char *request, size_t request_length) {
    if (!from_own_pages(proxy, request, request_length)) {
        static const char forbidden[] =
            "HTTP/1.1 403 Forbidden\r\nContent-Type: text/plain\r\nContent-Length: 43\r\nConnection: close\r\n\r\n"
            "Only the printer utility's own HTML Config.";
        send_all(client, forbidden, sizeof(forbidden) - 1U);
        return;
    }
    char *head = malloc(RESPONSE_HEAD_LIMIT);
    unsigned char *body = malloc(RESPONSE_BODY_LIMIT);
    size_t body_length = 0;
    int status = -1;
    bool head_request = request_length >= 5 && memcmp(request, "HEAD ", 5) == 0;
    if (proxy->out_of_step) {
        drain(&proxy->transport);
        proxy->out_of_step = false;
    }
    if (head && body && proxy->transport.write(proxy->transport.context, request, request_length) == 0) {
        PrinterReader reader = {&proxy->transport};
        status = p1600_http_read_message(read_printer, &reader, head_request, head, RESPONSE_HEAD_LIMIT, body,
                                      RESPONSE_BODY_LIMIT, &body_length);
    }
    if (status < 0) proxy->out_of_step = true;
    if (status < 0) {
        static const char unavailable[] =
            "HTTP/1.1 502 Bad Gateway\r\nContent-Type: text/plain\r\nContent-Length: 34\r\nConnection: close\r\n\r\n"
            "The printer did not answer (USB).\n";
        send_all(client, unavailable, sizeof(unavailable) - 1U);
    } else {
        static const char *const dropped[] = {"Transfer-Encoding:", "Content-Length:", "Connection:", "Keep-Alive:"};
        char *output = malloc(RESPONSE_HEAD_LIMIT + 64U);
        size_t used = 0;
        for (char *line = head, *next; output && line && *line; line = next) {
            next = strstr(line, "\r\n");
            size_t length = next ? (size_t)(next - line) : strlen(line);
            if (next) next += 2;
            bool keep = true;
            for (size_t i = 0; i < sizeof(dropped) / sizeof(dropped[0]); i++) {
                if (strncasecmp(line, dropped[i], strlen(dropped[i])) == 0) keep = false;
            }
            if (keep && used + length + 2U < RESPONSE_HEAD_LIMIT) {
                memcpy(output + used, line, length);
                memcpy(output + used + length, "\r\n", 2);
                used += length + 2U;
            }
        }
        if (output) {
            used += (size_t)snprintf(output + used, 64U, "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                                     body_length);
            if (send_all(client, output, used) == 0) send_all(client, body, body_length);
        }
        free(output);
    }
    free(head);
    free(body);
}

static void *serve(void *argument) {
    P1600EwsProxy *proxy = argument;
    for (;;) {
        struct pollfd channels[2] = {{.fd = proxy->listener, .events = POLLIN}, {.fd = proxy->wake[0], .events = POLLIN}};
        int ready = poll(channels, 2, proxy->idle_seconds * 1000);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0 || channels[1].revents) break;  /* idle, or stopped */
        int client = accept(proxy->listener, NULL, NULL);
        if (client < 0) continue;
        int on = 1;
        setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
        size_t length = 0;
        unsigned char *request = read_request(client, &length);
        if (request) answer(proxy, client, request, length);
        free(request);
        close(client);
    }
    /* Idle or stopped: the printer's USB interface is free again at once
     * (the toner query and a new HTML Config need it), and the old address
     * refuses connections instead of leaving them unanswered. A stop joins
     * this thread before it frees the proxy. */
    close(proxy->listener);
    proxy->listener = -1;
    if (proxy->transport.close) proxy->transport.close(proxy->transport.context);
    proxy->transport.close = NULL;
    atomic_store(&proxy->running, false);
    return NULL;
}

P1600EwsProxy *p1600_ews_proxy_start(P1600EwsTransport transport, int idle_seconds, char *url, size_t url_size,
                               char *error, size_t error_size) {
    if (!transport.write || !transport.read || idle_seconds <= 0 || !url || url_size == 0) {
        if (transport.close) transport.close(transport.context);
        fail(error, error_size, "Invalid proxy arguments");
        return NULL;
    }
    P1600EwsProxy *proxy = calloc(1, sizeof(*proxy));
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t length = sizeof(address);
    if (!proxy || listener < 0 || bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listener, 16) != 0 || getsockname(listener, (struct sockaddr *)&address, &length) != 0 ||
        pipe(proxy->wake) != 0) {
        fail(error, error_size, "Unable to listen on the loopback address: %s", strerror(errno));
        if (listener >= 0) close(listener);
        free(proxy);
        if (transport.close) transport.close(transport.context);
        return NULL;
    }
    fcntl(listener, F_SETFD, FD_CLOEXEC);
    proxy->transport = transport;
    proxy->listener = listener;
    proxy->idle_seconds = idle_seconds;
    proxy->port = (unsigned)ntohs(address.sin_port);
    atomic_init(&proxy->running, true);
    snprintf(url, url_size, "http://127.0.0.1:%u" P1600_EWS_START_PAGE, (unsigned)ntohs(address.sin_port));
    if (pthread_create(&proxy->thread, NULL, serve, proxy) != 0) {
        fail(error, error_size, "Unable to start the proxy thread");
        close(listener);
        close(proxy->wake[0]);
        close(proxy->wake[1]);
        free(proxy);
        if (transport.close) transport.close(transport.context);
        return NULL;
    }
    return proxy;
}

static int usb_write(void *context, const void *data, size_t length) {
    return p1600_usb_write(context, data, length, 5000);
}

static ssize_t usb_read(void *context, void *buffer, size_t length, int timeout_ms) {
    return p1600_usb_read(context, buffer, length, timeout_ms > 0 ? (unsigned)timeout_ms : 1U);
}

static void usb_close(void *context) {
    p1600_usb_close(context);
}

P1600EwsProxy *p1600_ews_proxy_start_usb(const char *device_uri, int idle_seconds, char *url, size_t url_size,
                                   char *error, size_t error_size) {
    P1600UsbTarget target;
    if (p1600_usb_target_from_uri(device_uri, &target) != 0) {
        fail(error, error_size, "Not a USB printer");
        return NULL;
    }
    P1600UsbChannel *channel = p1600_usb_open(&target, P1600_LEDM_INTERFACE_CLASS, P1600_LEDM_INTERFACE_SUBCLASS,
                                        P1600_LEDM_INTERFACE_PROTOCOL, error, error_size);
    if (!channel) return NULL;
    P1600EwsTransport transport = {channel, usb_write, usb_read, usb_close};
    return p1600_ews_proxy_start(transport, idle_seconds, url, url_size, error, error_size);
}

bool p1600_ews_proxy_running(P1600EwsProxy *proxy) {
    return proxy && atomic_load(&proxy->running);
}

void p1600_ews_proxy_wait(P1600EwsProxy *proxy) {
    if (!proxy || proxy->joined) return;
    pthread_join(proxy->thread, NULL);
    proxy->joined = true;
}

void p1600_ews_proxy_stop(P1600EwsProxy *proxy) {
    if (!proxy) return;
    static const char stop = 1;
    while (write(proxy->wake[1], &stop, 1) < 0 && errno == EINTR) {
    }
    if (!proxy->joined) pthread_join(proxy->thread, NULL);
    /* serve() has closed the listener and the transport. */
    close(proxy->wake[0]);
    close(proxy->wake[1]);
    free(proxy);
}
