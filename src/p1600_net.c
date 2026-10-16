#include "p1600_net.h"

#include <arpa/inet.h>
#include <dns_sd.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <cups/cups.h>
#include <cups/http.h>

static int fail(char *error, size_t error_size, const char *format, ...) {
    if (error && error_size > 0) {
        va_list arguments;
        va_start(arguments, format);
        vsnprintf(error, error_size, format, arguments);
        va_end(arguments);
    }
    return -1;
}

static int64_t now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

int p1600_net_split_service(const char *service, char *name, size_t name_size, char *type, size_t type_size,
                         char *domain, size_t domain_size) {
    if (!service) return -1;
    /* The type is the last "._xxx._tcp" (instance names may contain dots). */
    const char *protocol = NULL;
    for (const char *p = strstr(service, "._tcp"); p; p = strstr(p + 1, "._tcp")) protocol = p;
    if (!protocol) {
        for (const char *p = strstr(service, "._udp"); p; p = strstr(p + 1, "._udp")) protocol = p;
    }
    if (!protocol || protocol == service) return -1;
    const char *type_start = NULL;
    for (const char *p = service; p < protocol; p++) {
        if (p[0] == '.' && p[1] == '_') type_start = p;
    }
    if (!type_start || type_start == service) return -1;
    const char *after = protocol + 5;
    size_t name_length = (size_t)(type_start - service);
    size_t type_length = (size_t)(after - (type_start + 1));
    const char *domain_start = *after == '.' ? after + 1 : after;
    if (name_length >= name_size || type_length >= type_size || strlen(domain_start) + 7U >= domain_size) return -1;
    memcpy(name, service, name_length);
    name[name_length] = '\0';
    memcpy(type, type_start + 1, type_length);
    type[type_length] = '\0';
    snprintf(domain, domain_size, "%s", *domain_start ? domain_start : "local.");
    return 0;
}

typedef struct {
    bool done;
    DNSServiceErrorType status;
    char host[256];
    int port;
} Resolution;

static void DNSSD_API resolved(DNSServiceRef reference, DNSServiceFlags flags, uint32_t interface,
                               DNSServiceErrorType status, const char *full_name, const char *host_target,
                               uint16_t port, uint16_t text_length, const unsigned char *text, void *context) {
    (void)reference;
    (void)flags;
    (void)interface;
    (void)full_name;
    (void)text_length;
    (void)text;
    Resolution *resolution = context;
    resolution->done = true;
    resolution->status = status;
    if (status == kDNSServiceErr_NoError && host_target) {
        snprintf(resolution->host, sizeof(resolution->host), "%s", host_target);
        resolution->port = ntohs(port);
    }
}

static int resolve_service(const char *service, P1600NetTarget *target, int timeout_ms, char *error, size_t error_size) {
    char name[256], type[64], domain[256];
    if (p1600_net_split_service(service, name, sizeof(name), type, sizeof(type), domain, sizeof(domain)) != 0) {
        return fail(error, error_size, "Invalid Bonjour service name");
    }
    Resolution resolution = {0};
    DNSServiceRef reference = NULL;
    if (DNSServiceResolve(&reference, 0, kDNSServiceInterfaceIndexAny, name, type, domain, resolved, &resolution) !=
        kDNSServiceErr_NoError) {
        return fail(error, error_size, "Bonjour is not available");
    }
    int fd = DNSServiceRefSockFD(reference);
    int64_t deadline = now_ms() + timeout_ms;
    while (!resolution.done) {
        int64_t left = deadline - now_ms();
        struct pollfd channel = {.fd = fd, .events = POLLIN};
        int ready = left > 0 ? poll(&channel, 1, (int)left) : 0;
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0 || DNSServiceProcessResult(reference) != kDNSServiceErr_NoError) break;
    }
    DNSServiceRefDeallocate(reference);
    if (!resolution.done || resolution.status != kDNSServiceErr_NoError || !resolution.host[0]) {
        return fail(error, error_size, "Bonjour printer \"%s\" not found", name);
    }
    snprintf(target->host, sizeof(target->host), "%s", resolution.host);
    /* Only the raw (JetDirect) service carries PJL; others name the host. */
    target->port = strcmp(type, "_pdl-datastream._tcp") == 0 && resolution.port > 0 ? resolution.port
                                                                                    : P1600_NET_APPSOCKET_PORT;
    return 0;
}

int p1600_net_target_from_uri(const char *uri, P1600NetTarget *target, int timeout_ms, char *error, size_t error_size) {
    if (!uri || !target) return fail(error, error_size, "No device URI");
    memset(target, 0, sizeof(*target));
    char scheme[32], userpass[256], host[256], resource[1024];
    int port = 0;
    if (httpSeparateURI(HTTP_URI_CODING_ALL, uri, scheme, sizeof(scheme), userpass, sizeof(userpass), host,
                        sizeof(host), &port, resource, sizeof(resource)) < HTTP_URI_STATUS_OK || !host[0]) {
        return strncasecmp(uri, "usb:", 4) == 0 ? 1 : fail(error, error_size, "Invalid device URI");
    }
    if (strcasecmp(scheme, "dnssd") == 0 || strcasecmp(scheme, "mdns") == 0) {
        return resolve_service(host, target, timeout_ms, error, error_size);
    }
    static const char *const schemes[] = {"socket", "lpd", "ipp", "ipps", "http", "https"};
    bool network = false;
    for (size_t i = 0; i < sizeof(schemes) / sizeof(schemes[0]); i++) {
        if (strcasecmp(scheme, schemes[i]) == 0) network = true;
    }
    if (!network) return 1;
    snprintf(target->host, sizeof(target->host), "%s", host);
    target->port = strcasecmp(scheme, "socket") == 0 && port > 0 ? port : P1600_NET_APPSOCKET_PORT;
    return 0;
}

int p1600_net_connect(const P1600NetTarget *target, int timeout_ms, char *error, size_t error_size) {
    if (!target || !target->host[0]) return fail(error, error_size, "No printer address");
    char service[16];
    snprintf(service, sizeof(service), "%d", target->port);
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
    struct addrinfo *addresses = NULL;
    int resolved_status = getaddrinfo(target->host, service, &hints, &addresses);
    if (resolved_status != 0) return fail(error, error_size, "%s: %s", target->host, gai_strerror(resolved_status));

    int result = -1;
    int last_error = ETIMEDOUT;
    int64_t deadline = now_ms() + timeout_ms;
    for (struct addrinfo *address = addresses; address && result < 0; address = address->ai_next) {
        int fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0) continue;
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
        int flags = fcntl(fd, F_GETFL);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        if (connect(fd, address->ai_addr, address->ai_addrlen) == 0) {
            result = fd;
        } else if (errno == EINPROGRESS) {
            struct pollfd channel = {.fd = fd, .events = POLLOUT};
            int64_t left = deadline - now_ms();
            int socket_error = 0;
            socklen_t length = sizeof(socket_error);
            if (left > 0 && poll(&channel, 1, (int)left) == 1 &&
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) == 0 && socket_error == 0) {
                result = fd;
            } else {
                last_error = socket_error ? socket_error : ETIMEDOUT;
            }
        } else {
            last_error = errno;
        }
        if (result < 0) close(fd);
    }
    freeaddrinfo(addresses);
    if (result < 0) return fail(error, error_size, "%s:%d: %s", target->host, target->port, strerror(last_error));
    return result;
}

int p1600_net_write(int fd, const void *data, size_t length, int timeout_ms) {
    const unsigned char *bytes = data;
    int64_t deadline = now_ms() + timeout_ms;
    while (length > 0) {
        struct pollfd channel = {.fd = fd, .events = POLLOUT};
        int64_t left = deadline - now_ms();
        int ready = left > 0 ? poll(&channel, 1, (int)left) : 0;
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) return -1;
        ssize_t written = send(fd, bytes, length, 0);
        if (written < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return -1;
        }
        bytes += written;
        length -= (size_t)written;
    }
    return 0;
}

ssize_t p1600_net_read(int fd, void *buffer, size_t length, int timeout_ms) {
    int64_t deadline = now_ms() + timeout_ms;
    for (;;) {
        struct pollfd channel = {.fd = fd, .events = POLLIN};
        int64_t left = deadline - now_ms();
        int ready = left > 0 ? poll(&channel, 1, (int)left) : 0;
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) return -1;
        ssize_t received = recv(fd, buffer, length, 0);
        if (received < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        return received;
    }
}
