#ifndef P1600_UTILITY_H
#define P1600_UTILITY_H

/*
 * Printer utility for the P1600dn series: the PJL it sends (EcoSMART sleep
 * and auto-off, Quiet Mode, information pages, cleaning page), the settings
 * it reads back with PJL INQUIRE, and the printer connection (USB printer
 * interface; the AppSocket port for network printers).
 * See reference/zjstream/ZJS_STREAM_SPEC.md, section 8.
 */

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* The USB IDs of the P1600dn series (P1600dn, P1606dn ... P1609dn). */
#define P1600_UTILITY_VENDOR_ID 0x03F0
#define P1600_UTILITY_PRODUCT_ID 0x0A2A

/* The menus, in display order. Sleep after (POWERSAVETIME) and Auto Off
 * after (INACTIVEOFFTIME, 0 = never), in minutes. */
#define P1600_SLEEP_CHOICES 6
#define P1600_AUTO_OFF_CHOICES 7
extern const int p1600_sleep_minutes[P1600_SLEEP_CHOICES];
extern const int p1600_auto_off_minutes[P1600_AUTO_OFF_CHOICES];

/* Menu position of a value, or -1 when the menu does not offer it. */
int p1600_sleep_index(int minutes);
int p1600_auto_off_index(int minutes);

/* The value at a menu position, or -1 (for Swift, which sees the arrays as
 * tuples). */
int p1600_sleep_choice(int index);
int p1600_auto_off_choice(int index);

typedef enum {
    P1600_INFO_DEMO = 0,           /* TESTPAGE=DEMOPAGE */
    P1600_INFO_CONFIGURATION,      /* TESTPAGE=SELFTEST */
    P1600_INFO_SUPPLY_STATUS       /* TESTPAGE=SUPPLIES */
} P1600InfoPage;

/* PJL LANG for an information page, from a localization code ("de",
 * "zh_TW", "nb-NO", ...): every language gets its PJL name (ENGLISH when
 * unknown). */
const char *p1600_utility_pjl_language(const char *localization);

/* The exact bytes: ESC %-12345X, the PJL line(s) ending in LF, ESC %-12345X.
 * Each returns the length written into `out` (0 if it does not fit or an
 * argument is invalid). */
size_t p1600_utility_format_sleep(int minutes, char *out, size_t size);
size_t p1600_utility_format_auto_off(int minutes, char *out, size_t size);
size_t p1600_utility_format_quiet_mode(bool on, char *out, size_t size);
size_t p1600_utility_format_info_page(P1600InfoPage page, const char *pjl_language, char *out, size_t size);
size_t p1600_utility_format_cleaning(char *out, size_t size);
size_t p1600_utility_format_inquire(const char *variable, char *out, size_t size);

/* Finds "@PJL INQUIRE <variable>" and its value in printer output. Returns
 * 1 with the value, 0 when more data is needed, -1 when the reply is
 * malformed. A value of "?" means the printer does not know the variable. */
int p1600_utility_parse_inquire(const char *data, size_t length, const char *variable, char *value, size_t size);

/* A connection to the printer a CUPS device URI names. */
typedef struct P1600PrinterLink P1600PrinterLink;

/* usb:// opens the printer interface (class 7, subclass 1) and tries for
 * up to 10 s while another program (a print job) holds it; other URIs
 * connect to the AppSocket port. NULL with a reason on failure. */
P1600PrinterLink *p1600_printer_link_open(const char *device_uri, char *error, size_t error_size);
bool p1600_printer_link_is_usb(const P1600PrinterLink *link);
int p1600_printer_link_send(P1600PrinterLink *link, const void *data, size_t length);
/* Sends INQUIRE and waits up to 2 s for the reply. 0 with the value, or -1. */
int p1600_printer_link_inquire(P1600PrinterLink *link, const char *variable, char *value, size_t size);
void p1600_printer_link_close(P1600PrinterLink *link);

/* -1 for values that could not be read. */
typedef struct {
    int sleep_minutes;
    int auto_off_minutes;
    int quiet_mode;         /* 1 on, 0 off */
} P1600PrinterSettings;

/* Reads POWERSAVETIME, INACTIVEOFFTIME, then QUIETMODE, each in its
 * own session. Returns how many values were read, or -1 with a reason when
 * the printer could not be reached or answered none of them. */
int p1600_utility_read_settings(const char *device_uri, P1600PrinterSettings *settings, char *error, size_t error_size);

/* Sends one action in its own session. 0, or -1 with a reason. */
int p1600_utility_send(const char *device_uri, const char *data, size_t length, char *error, size_t error_size);

/* http:// address of the printer's web pages for a network device URI;
 * 1 for USB printers (their pages need p1600_ews_proxy), -1 on failure. */
int p1600_utility_web_address(const char *device_uri, char *url, size_t size, char *error, size_t error_size);

#endif /* P1600_UTILITY_H */
