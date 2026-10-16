#ifndef P1600_USB_H
#define P1600_USB_H

/*
 * IOKit access to one interface of the printer's USB device, shared by the
 * toner level filter (LEDM, interface FF/02/10) and the printer utility (PJL
 * on the printer interface 07/01/02). The device is the one the queue's
 * usb:// URI names (p1600_usb_device_matches()).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "p1600_supplies.h"

#define P1600_USB_ANY (-1)

typedef struct P1600UsbChannel P1600UsbChannel;

/* Opens the first interface of the device with the given class, subclass
 * and protocol (P1600_USB_ANY matches all) and its first bulk OUT and IN pipes.
 * Returns NULL with a reason in `error` ("USB printer not found", "... is
 * busy" while another program, such as the CUPS backend, holds it). */
P1600UsbChannel *p1600_usb_open(const P1600UsbTarget *target, int interface_class, int subclass, int protocol,
                          char *error, size_t error_size);

/* Writes everything; 0, or -1 after a failure or timeout. */
int p1600_usb_write(P1600UsbChannel *channel, const void *data, size_t length, unsigned timeout_ms);

/* Reads what the printer sends within `timeout_ms` (whole packets; the rest
 * of a packet is kept for the next call). Returns the byte count, or -1 on
 * timeout or error. */
ssize_t p1600_usb_read(P1600UsbChannel *channel, void *buffer, size_t length, unsigned timeout_ms);

void p1600_usb_close(P1600UsbChannel *channel);

#endif /* P1600_USB_H */
