/*
 * LEDM over USB: the printer's vendor-specific interface (class FF,
 * subclass 02, protocol 10) carries HTTP. The request goes to the first bulk
 * OUT pipe and the response is read from the first bulk IN pipe with 1 s
 * timeouts. The device is found by its USB serial number and product name
 * from the queue's usb:// URI.
 */
#include "p1600_supplies.h"

#include <stdio.h>
#include <stdlib.h>

#include "p1600_usb.h"

#define LEDM_BODY_LIMIT (256U * 1024U)
#define LEDM_TIMEOUT_MS 1000U
#define LEDM_WRITE_TIMEOUT_MS 5000U

static ssize_t usb_read(void *context, unsigned char *buffer, size_t length) {
    return p1600_usb_read(context, buffer, length, LEDM_TIMEOUT_MS);
}

int p1600_usb_ledm_level(const P1600UsbTarget *target, char *error, size_t error_size) {
    P1600UsbChannel *channel = p1600_usb_open(target, P1600_LEDM_INTERFACE_CLASS, P1600_LEDM_INTERFACE_SUBCLASS,
                                        P1600_LEDM_INTERFACE_PROTOCOL, error, error_size);
    if (!channel) return -1;

    static const char request[] = P1600_LEDM_REQUEST;
    int level = -1;
    unsigned char *body = NULL;
    if (p1600_usb_write(channel, request, sizeof(request) - 1U, LEDM_WRITE_TIMEOUT_MS) != 0) {
        if (error && error_size > 0) snprintf(error, error_size, "LEDM request failed");
    } else if (!(body = malloc(LEDM_BODY_LIMIT))) {
        if (error && error_size > 0) snprintf(error, error_size, "Out of memory");
    } else {
        size_t length = 0;
        int status = p1600_http_read_response(usb_read, channel, body, LEDM_BODY_LIMIT, &length);
        if (status != 200) {
            if (error && error_size > 0) snprintf(error, error_size, "LEDM request returned HTTP status %d", status);
        } else if ((level = p1600_ledm_level((const char *)body, length)) < 0) {
            if (error && error_size > 0) snprintf(error, error_size, "LEDM document has no toner level");
        }
    }
    free(body);
    p1600_usb_close(channel);
    return level;
}
