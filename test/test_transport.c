/*
 * Tests for the printer transports (src/p1600_net.c, src/p1600_usb.c): device URI
 * to AppSocket address, Bonjour service names, TCP connect/write/read with
 * timeouts against a local stand-in printer, and the USB lookup without a
 * matching printer. No network traffic leaves this Mac.
 */
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "p1600_net.h"
#include "p1600_usb.h"

static double now_seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static void test_service_names(void) {
    char name[256], type[64], domain[256];
    assert(p1600_net_split_service("HP LaserJet Professional P1606dn [A1B2C3]._pdl-datastream._tcp.local.", name,
                                sizeof(name), type, sizeof(type), domain, sizeof(domain)) == 0);
    assert(strcmp(name, "HP LaserJet Professional P1606dn [A1B2C3]") == 0);
    assert(strcmp(type, "_pdl-datastream._tcp") == 0 && strcmp(domain, "local.") == 0);
    assert(p1600_net_split_service("Office.Printer._ipp._tcp.example.com.", name, sizeof(name), type, sizeof(type),
                                domain, sizeof(domain)) == 0);
    assert(strcmp(name, "Office.Printer") == 0 && strcmp(type, "_ipp._tcp") == 0 &&
           strcmp(domain, "example.com.") == 0);
    assert(p1600_net_split_service("Printer._printer._tcp", name, sizeof(name), type, sizeof(type), domain,
                                sizeof(domain)) == 0);
    assert(strcmp(domain, "local.") == 0);
    assert(p1600_net_split_service("Printer._ipp._udp.local.", name, sizeof(name), type, sizeof(type), domain,
                                sizeof(domain)) == 0);
    assert(p1600_net_split_service("no-service", name, sizeof(name), type, sizeof(type), domain, sizeof(domain)) == -1);
    assert(p1600_net_split_service("._ipp._tcp.local.", name, sizeof(name), type, sizeof(type), domain, sizeof(domain)) == -1);
    assert(p1600_net_split_service("Printer._tcp.local.", name, sizeof(name), type, sizeof(type), domain, sizeof(domain)) == -1);
    assert(p1600_net_split_service("Printer._ipp._tcp.local.", name, 4, type, sizeof(type), domain, sizeof(domain)) == -1);
    assert(p1600_net_split_service(NULL, name, sizeof(name), type, sizeof(type), domain, sizeof(domain)) == -1);
    puts("[PASS] Bonjour service names: instance names with dots, types, domains, malformed names");
}

static void test_uris(void) {
    P1600NetTarget target;
    char error[256];
    assert(p1600_net_target_from_uri("socket://192.0.2.7", &target, 100, error, sizeof(error)) == 0);
    assert(strcmp(target.host, "192.0.2.7") == 0 && target.port == P1600_NET_APPSOCKET_PORT);
    assert(p1600_net_target_from_uri("socket://printer.example:9101", &target, 100, error, sizeof(error)) == 0);
    assert(strcmp(target.host, "printer.example") == 0 && target.port == 9101);
    assert(p1600_net_target_from_uri("ipp://192.0.2.7:631/ipp/print", &target, 100, error, sizeof(error)) == 0);
    assert(target.port == P1600_NET_APPSOCKET_PORT);
    assert(p1600_net_target_from_uri("lpd://192.0.2.7/queue", &target, 100, error, sizeof(error)) == 0);
    assert(p1600_net_target_from_uri("ipps://h/ipp/print", &target, 100, error, sizeof(error)) == 0);
    assert(p1600_net_target_from_uri("http://h/", &target, 100, error, sizeof(error)) == 0);
    assert(p1600_net_target_from_uri("usb://HP/LaserJet%20Professional%20P1606dn?serial=X", &target, 100, error,
                                  sizeof(error)) == 1);
    assert(p1600_net_target_from_uri("serial://dev/cu.usb?baud=9600", &target, 100, error, sizeof(error)) == 1);
    assert(p1600_net_target_from_uri("file:///dev/null", &target, 100, error, sizeof(error)) == -1);
    assert(p1600_net_target_from_uri("not a uri", &target, 100, error, sizeof(error)) == -1);
    assert(p1600_net_target_from_uri(NULL, &target, 100, error, sizeof(error)) == -1);

    /* A Bonjour name nobody announces fails within the limit. */
    double start = now_seconds();
    int result = p1600_net_target_from_uri("dnssd://P1600DN%20Test%20No%20Such%20Printer._pdl-datastream._tcp.local./",
                                        &target, 300, error, sizeof(error));
    assert(result == -1 && now_seconds() - start < 5.0);
    assert(p1600_net_target_from_uri("dnssd://not-a-service/", &target, 300, error, sizeof(error)) == -1);
    assert(strcmp(error, "Invalid Bonjour service name") == 0);
    puts("[PASS] Device URIs: AppSocket port, ipp/lpd/http hosts, usb/serial not network, unknown Bonjour name");
}

typedef struct {
    int listener;
    int mode;   /* 0: echo a reply, 1: send nothing, 2: close at once */
} Server;

static void *serve(void *argument) {
    Server *server = argument;
    int client = accept(server->listener, NULL, NULL);
    assert(client >= 0);
    if (server->mode == 2) {
        close(client);
        return NULL;
    }
    char buffer[256];
    ssize_t received = recv(client, buffer, sizeof(buffer), 0);
    assert(received > 0);
    if (server->mode == 0) {
        static const char reply[] = "@PJL INQUIRE QUIETMODE\r\nOFF\r\n\f";
        assert(send(client, reply, sizeof(reply) - 1U, 0) == (ssize_t)(sizeof(reply) - 1U));
    } else {
        struct timespec pause = {0, 500000000L};
        nanosleep(&pause, NULL);
    }
    close(client);
    return NULL;
}

static int listen_local(int *port) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    assert(listener >= 0);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(listen(listener, 1) == 0);
    socklen_t length = sizeof(address);
    assert(getsockname(listener, (struct sockaddr *)&address, &length) == 0);
    *port = ntohs(address.sin_port);
    return listener;
}

static void test_tcp(void) {
    char error[256];
    for (int mode = 0; mode < 3; mode++) {
        int port = 0;
        Server server = {listen_local(&port), mode};
        pthread_t thread;
        assert(pthread_create(&thread, NULL, serve, &server) == 0);
        P1600NetTarget target = {.port = port};
        strcpy(target.host, "127.0.0.1");
        int fd = p1600_net_connect(&target, 2000, error, sizeof(error));
        assert(fd >= 0);
        char buffer[256];
        if (mode == 0) {
            assert(p1600_net_write(fd, "\033%-12345X@PJL INQUIRE QUIETMODE\r\n", 33, 1000) == 0);
            size_t total = 0;
            ssize_t received;
            while ((received = p1600_net_read(fd, buffer + total, sizeof(buffer) - 1U - total, 2000)) > 0) {
                total += (size_t)received;
            }
            buffer[total] = '\0';
            assert(strcmp(buffer, "@PJL INQUIRE QUIETMODE\r\nOFF\r\n\f") == 0);
        } else if (mode == 1) {
            assert(p1600_net_write(fd, "x", 1, 1000) == 0);
            double start = now_seconds();
            assert(p1600_net_read(fd, buffer, sizeof(buffer), 200) == -1);
            assert(now_seconds() - start < 2.0);
        } else {
            assert(p1600_net_read(fd, buffer, sizeof(buffer), 2000) == 0);
        }
        close(fd);
        assert(pthread_join(thread, NULL) == 0);
        close(server.listener);
    }

    /* Nobody listening: refused at once. */
    int port = 0;
    int listener = listen_local(&port);
    close(listener);
    P1600NetTarget closed = {.port = port};
    strcpy(closed.host, "127.0.0.1");
    assert(p1600_net_connect(&closed, 2000, error, sizeof(error)) == -1);
    assert(strstr(error, "127.0.0.1:") != NULL);
    P1600NetTarget unknown = {.port = 9100};
    strcpy(unknown.host, "p1600dn-no-such-host.invalid");
    assert(p1600_net_connect(&unknown, 2000, error, sizeof(error)) == -1);
    P1600NetTarget empty = {0};
    assert(p1600_net_connect(&empty, 100, error, sizeof(error)) == -1);
    assert(p1600_net_connect(NULL, 100, error, sizeof(error)) == -1);
    puts("[PASS] AppSocket: connect, write, reply until close, read timeout, refused and unknown hosts");
}

static void test_usb(void) {
    P1600UsbTarget target;
    assert(p1600_usb_target_from_uri("usb://HP/LaserJet%20Professional%20P1606dn?serial=P1600DN-TEST-NO-DEVICE",
                                  &target) == 0);
    char error[160] = "";
    assert(p1600_usb_open(&target, 7, 1, 2, error, sizeof(error)) == NULL);
    assert(strcmp(error, "USB printer not found") == 0);
    assert(p1600_usb_open(NULL, 7, 1, 2, error, sizeof(error)) == NULL);
    assert(p1600_usb_open(&target, P1600_USB_ANY, P1600_USB_ANY, P1600_USB_ANY, NULL, 0) == NULL);
    assert(p1600_usb_write(NULL, "x", 1, 100) == -1);
    char buffer[4];
    assert(p1600_usb_read(NULL, buffer, sizeof(buffer), 100) == -1);
    p1600_usb_close(NULL);
    puts("[PASS] USB printer interface lookup without a matching printer");
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    test_service_names();
    test_uris();
    test_tcp();
    test_usb();
    puts("[PASS] Printer transport suite");
    return 0;
}
