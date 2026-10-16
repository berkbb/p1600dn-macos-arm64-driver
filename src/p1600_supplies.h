#ifndef P1600_SUPPLIES_H
#define P1600_SUPPLIES_H

/*
 * Toner level reporting for the CUPS command filter
 * (reference/zjstream/ZJS_STREAM_SPEC.md, section 7):
 *   usb://   HTTP GET of the LEDM document /DevMgmt/ConsumableConfigDyn.xml
 *            over the printer's vendor USB interface (class FF/02/10) and
 *            the first <dd:ConsumablePercentageLevelRemaining>;
 *   others   Apple's snmpInk for the queue and the first
 *            prtMarkerSuppliesLevel of its Printer MIB supply table.
 * The level is reported to CUPS as one `ATTR: marker-...` line.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* The LEDM request, byte for byte (49 bytes, no Host header). */
#define P1600_LEDM_REQUEST "GET /DevMgmt/ConsumableConfigDyn.xml HTTP/1.1\r\n\r\n"
#define P1600_LEDM_INTERFACE_CLASS 0xFF
#define P1600_LEDM_INTERFACE_SUBCLASS 0x02
#define P1600_LEDM_INTERFACE_PROTOCOL 0x10
#define P1600_USB_VENDOR_ID 0x03F0

#ifndef P1600_SNMPINK_PATH
#define P1600_SNMPINK_PATH "/System/Library/Printers/Libraries/snmpInk"
#endif

typedef struct {
    char make[64];      /* URI host: "HP" */
    char product[256];  /* URI path without "/": "LaserJet Professional P1606dn" */
    char serial[128];   /* "serial" query value */
} P1600UsbTarget;

/* 0 for a usb:// URI naming a product or serial, 1 for another scheme,
 * -1 for an invalid URI. */
int p1600_usb_target_from_uri(const char *uri, P1600UsbTarget *target);

/* An exact comparison of the URI product with the USB product string fails
 * for macOS URIs ("usb://HP/LaserJet..." against "HP LaserJet...");
 * the make prefix and letter case are accepted here, and a matching serial
 * number identifies an HP device on its own. */
bool p1600_usb_device_matches(const P1600UsbTarget *target, const char *usb_product,
                           const char *usb_serial, uint32_t vendor_id);

/* Reads an HTTP/1.x response (Content-Length, chunked, or until the reader
 * stops) through `read`, which returns the number of bytes read, 0 at the
 * end, or -1 on error or timeout. Returns the status code, or -1. */
typedef ssize_t (*P1600ByteReader)(void *context, unsigned char *buffer, size_t length);
int p1600_http_read_response(P1600ByteReader read, void *context, unsigned char *body,
                          size_t capacity, size_t *body_length);

/* p1600_http_read_response() that also returns the status line and header
 * lines, each ending in CR LF, in `head` (the body is de-chunked). */
int p1600_http_read_message(P1600ByteReader read, void *context, char *head, size_t head_size, unsigned char *body,
                         size_t capacity, size_t *body_length);

/* First <dd:ConsumablePercentageLevelRemaining> (0-100), or -1. */
int p1600_ledm_level(const char *xml, size_t length);

/* First supply of snmpInk's plist: prtMarkerSuppliesLevel as a percentage
 * of prtMarkerSuppliesMaxCapacity, or -1. */
int p1600_snmpink_level(const char *plist, size_t length);

/* `ATTR: marker-colors=#000000 marker-levels=N marker-names=Black
 * marker-types=toner` plus a newline. Returns the length, or 0. */
size_t p1600_supply_attr_line(int level, char *out, size_t size);

/* USB transport (p1600_usb_ledm.c): queries the LEDM document of the device
 * `target` names. Returns 0-100, or -1 with a reason in `error`. */
int p1600_usb_ledm_level(const P1600UsbTarget *target, char *error, size_t error_size);

#endif /* P1600_SUPPLIES_H */
