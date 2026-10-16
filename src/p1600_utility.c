#include "p1600_utility.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "p1600_net.h"
#include "p1600_supplies.h"
#include "p1600_usb.h"

/* Universal Exit Language, written inside printf formats. */
#define UEL "\033%%-12345X"
#define LINK_TIMEOUT_MS 5000
#define INQUIRE_TIMEOUT_MS 2000
#define USB_OPEN_ATTEMPTS 10

const int p1600_sleep_minutes[P1600_SLEEP_CHOICES] = {1, 5, 15, 30, 60, 120};
const int p1600_auto_off_minutes[P1600_AUTO_OFF_CHOICES] = {0, 30, 60, 120, 240, 480, 1440};

static int index_of(const int *values, int count, int value) {
    for (int i = 0; i < count; i++) {
        if (values[i] == value) return i;
    }
    return -1;
}

int p1600_sleep_index(int minutes) {
    return index_of(p1600_sleep_minutes, P1600_SLEEP_CHOICES, minutes);
}

int p1600_auto_off_index(int minutes) {
    return index_of(p1600_auto_off_minutes, P1600_AUTO_OFF_CHOICES, minutes);
}

int p1600_sleep_choice(int index) {
    return index >= 0 && index < P1600_SLEEP_CHOICES ? p1600_sleep_minutes[index] : -1;
}

int p1600_auto_off_choice(int index) {
    return index >= 0 && index < P1600_AUTO_OFF_CHOICES ? p1600_auto_off_minutes[index] : -1;
}

static int fail(char *error, size_t error_size, const char *format, ...) {
    if (error && error_size > 0) {
        va_list arguments;
        va_start(arguments, format);
        vsnprintf(error, error_size, format, arguments);
        va_end(arguments);
    }
    return -1;
}

const char *p1600_utility_pjl_language(const char *localization) {
    static const struct {
        const char *code;
        const char *pjl;
    } languages[] = {
        {"da", "DANISH"}, {"de", "GERMAN"}, {"en", "ENGLISH"}, {"es", "SPANISH"}, {"fi", "FINNISH"},
        {"fr", "FRENCH"}, {"it", "ITALIAN"}, {"ko", "KOREAN"}, {"nl", "DUTCH"}, {"no", "NORWEGIAN"},
        {"nb", "NORWEGIAN"}, {"nn", "NORWEGIAN"}, {"pt", "PORTUGUESE"}, {"sv", "SWEDISH"}, {"tr", "TURKISH"},
    };
    if (!localization || strlen(localization) < 2) return "ENGLISH";
    if (strncasecmp(localization, "zh", 2) == 0) {
        /* zh_TW, zh-Hant, zh_HK: traditional script. */
        const char *rest = localization + 2;
        return strcasestr(rest, "TW") || strcasestr(rest, "HK") || strcasestr(rest, "Hant") ? "TCHINESE" : "CHINESE";
    }
    for (size_t i = 0; i < sizeof(languages) / sizeof(languages[0]); i++) {
        if (strncasecmp(localization, languages[i].code, 2) == 0 &&
            (localization[2] == '\0' || localization[2] == '_' || localization[2] == '-')) {
            return languages[i].pjl;
        }
    }
    return "ENGLISH";
}

static size_t format(char *out, size_t size, const char *pattern, ...) {
    if (!out || size == 0) return 0;
    va_list arguments;
    va_start(arguments, pattern);
    int length = vsnprintf(out, size, pattern, arguments);
    va_end(arguments);
    if (length < 0 || (size_t)length >= size) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)length;
}

size_t p1600_utility_format_sleep(int minutes, char *out, size_t size) {
    /* 0 is accepted as well, although the menu never offers it. */
    if (p1600_sleep_index(minutes) < 0 && minutes != 0) return 0;
    return format(out, size, UEL "@PJL SET POWERSAVETIME=%d\n" UEL, minutes);
}

size_t p1600_utility_format_auto_off(int minutes, char *out, size_t size) {
    if (p1600_auto_off_index(minutes) < 0) return 0;
    return format(out, size, UEL "@PJL SET INACTIVEOFFTIME=%d\n" UEL, minutes);
}

size_t p1600_utility_format_quiet_mode(bool on, char *out, size_t size) {
    return format(out, size, UEL "@PJL SET QUIETMODE=%s\n" UEL, on ? "ON" : "OFF");
}

size_t p1600_utility_format_info_page(P1600InfoPage page, const char *pjl_language, char *out, size_t size) {
    static const char *const pages[] = {"DEMOPAGE", "SELFTEST", "SUPPLIES"};
    if ((unsigned)page > P1600_INFO_SUPPLY_STATUS || !pjl_language || !*pjl_language) return 0;
    for (const char *p = pjl_language; *p; p++) {
        if (!isupper((unsigned char)*p)) return 0;  /* PJL names only */
    }
    return format(out, size, UEL "@PJL SET LANG=%s\n@PJL SET TESTPAGE=%s\n" UEL, pjl_language, pages[page]);
}

size_t p1600_utility_format_cleaning(char *out, size_t size) {
    return format(out, size, UEL "@PJL CLEANPRINTER\n" UEL);
}

size_t p1600_utility_format_inquire(const char *variable, char *out, size_t size) {
    if (!variable || !*variable) return 0;
    for (const char *p = variable; *p; p++) {
        if (!isupper((unsigned char)*p)) return 0;
    }
    return format(out, size, UEL "@PJL INQUIRE %s\n" UEL, variable);
}

static bool is_blank(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

int p1600_utility_parse_inquire(const char *data, size_t length, const char *variable, char *value, size_t size) {
    if (!data || !variable || !value || size == 0) return -1;
    char prefix[64];
    int prefix_length = snprintf(prefix, sizeof(prefix), "@PJL INQUIRE %s", variable);
    if (prefix_length <= 0 || (size_t)prefix_length >= sizeof(prefix)) return -1;
    const char *start = memmem(data, length, prefix, (size_t)prefix_length);
    if (!start) return 0;
    /* Blanks separate the fields; a form feed ends the PJL message. */
    const char *end = data + length;
    const char *p = start + prefix_length;
    if (p < end && !is_blank(*p) && *p != '\f') return -1;  /* a longer variable name */
    while (p < end && is_blank(*p)) p++;
    const char *token = p;
    while (p < end && !is_blank(*p) && *p != '\f') p++;
    if (p == end) return 0;          /* the value may continue */
    if (p == token) return -1;       /* no value before the end of the message */
    size_t token_length = (size_t)(p - token);
    if (token_length >= size) return -1;
    memcpy(value, token, token_length);
    value[token_length] = '\0';
    return 1;
}

struct P1600PrinterLink {
    P1600UsbChannel *usb;
    int socket;
};

P1600PrinterLink *p1600_printer_link_open(const char *device_uri, char *error, size_t error_size) {
    P1600PrinterLink *link = calloc(1, sizeof(*link));
    if (!link) {
        fail(error, error_size, "Out of memory");
        return NULL;
    }
    link->socket = -1;
    P1600UsbTarget usb;
    int kind = p1600_usb_target_from_uri(device_uri, &usb);
    if (kind == 0) {
        /* Exclusive open of the printer interface, up to 10 tries 1 s apart. */
        char reason[160] = "";
        for (int attempt = 0; attempt < USB_OPEN_ATTEMPTS && !link->usb; attempt++) {
            if (attempt > 0) sleep(1);
            link->usb = p1600_usb_open(&usb, 7, 1, P1600_USB_ANY, reason, sizeof(reason));
            if (!link->usb && !strstr(reason, "busy")) break;
        }
        if (!link->usb) {
            fail(error, error_size, "%s", reason);
            free(link);
            return NULL;
        }
        return link;
    }
    P1600NetTarget network;
    int found = p1600_net_target_from_uri(device_uri, &network, LINK_TIMEOUT_MS, error, error_size);
    if (found == 0) link->socket = p1600_net_connect(&network, LINK_TIMEOUT_MS, error, error_size);
    if (link->socket < 0) {
        if (found == 1) fail(error, error_size, "The printer's connection cannot carry PJL");
        free(link);
        return NULL;
    }
    return link;
}

bool p1600_printer_link_is_usb(const P1600PrinterLink *link) {
    return link && link->usb;
}

int p1600_printer_link_send(P1600PrinterLink *link, const void *data, size_t length) {
    if (!link || !data) return -1;
    return link->usb ? p1600_usb_write(link->usb, data, length, LINK_TIMEOUT_MS)
                     : p1600_net_write(link->socket, data, length, LINK_TIMEOUT_MS);
}

static int64_t now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

int p1600_printer_link_inquire(P1600PrinterLink *link, const char *variable, char *value, size_t size) {
    char request[96];
    size_t request_length = p1600_utility_format_inquire(variable, request, sizeof(request));
    if (!link || !value || request_length == 0 || p1600_printer_link_send(link, request, request_length) != 0) return -1;

    /* The reply may come in pieces, after other output; reading goes on
     * until it is complete or the time is up. */
    char reply[4096];
    size_t used = 0;
    int64_t deadline = now_ms() + INQUIRE_TIMEOUT_MS;
    while (used < sizeof(reply)) {
        int64_t left = deadline - now_ms();
        if (left <= 0) break;
        ssize_t received = link->usb ? p1600_usb_read(link->usb, reply + used, sizeof(reply) - used, (unsigned)left)
                                     : p1600_net_read(link->socket, reply + used, sizeof(reply) - used, (int)left);
        if (received <= 0) break;
        used += (size_t)received;
        int parsed = p1600_utility_parse_inquire(reply, used, variable, value, size);
        if (parsed != 0) return parsed > 0 ? 0 : -1;
    }
    return -1;
}

void p1600_printer_link_close(P1600PrinterLink *link) {
    if (!link) return;
    p1600_usb_close(link->usb);
    if (link->socket >= 0) close(link->socket);
    free(link);
}

static int inquire_number(const char *device_uri, const char *variable, int *number, char *error, size_t error_size) {
    P1600PrinterLink *link = p1600_printer_link_open(device_uri, error, error_size);
    if (!link) return -1;
    char value[32];
    int status = p1600_printer_link_inquire(link, variable, value, sizeof(value));
    p1600_printer_link_close(link);
    if (status != 0) return 0;
    if (strcasecmp(variable, "QUIETMODE") == 0) {
        if (strcasecmp(value, "ON") == 0) *number = 1;
        else if (strcasecmp(value, "OFF") == 0) *number = 0;
        else return 0;
        return 1;
    }
    char *end = NULL;
    errno = 0;
    long parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0' || errno != 0 || parsed < 0 || parsed > 100000) return 0;
    *number = (int)parsed;
    return 1;
}

int p1600_utility_read_settings(const char *device_uri, P1600PrinterSettings *settings, char *error, size_t error_size) {
    if (!settings) return -1;
    *settings = (P1600PrinterSettings){-1, -1, -1};
    int read = 0, status;
    if ((status = inquire_number(device_uri, "POWERSAVETIME", &settings->sleep_minutes, error, error_size)) < 0) {
        return -1;
    }
    read += status;
    if ((status = inquire_number(device_uri, "INACTIVEOFFTIME", &settings->auto_off_minutes, error, error_size)) < 0) {
        return -1;
    }
    read += status;
    if ((status = inquire_number(device_uri, "QUIETMODE", &settings->quiet_mode, error, error_size)) < 0) return -1;
    return read + status;
}

int p1600_utility_send(const char *device_uri, const char *data, size_t length, char *error, size_t error_size) {
    if (!data || length == 0) return fail(error, error_size, "Nothing to send");
    P1600PrinterLink *link = p1600_printer_link_open(device_uri, error, error_size);
    if (!link) return -1;
    int status = p1600_printer_link_send(link, data, length);
    p1600_printer_link_close(link);
    return status == 0 ? 0 : fail(error, error_size, "The printer did not take the command");
}

int p1600_utility_web_address(const char *device_uri, char *url, size_t size, char *error, size_t error_size) {
    P1600UsbTarget usb;
    if (p1600_usb_target_from_uri(device_uri, &usb) == 0) return 1;
    P1600NetTarget network;
    int found = p1600_net_target_from_uri(device_uri, &network, LINK_TIMEOUT_MS, error, error_size);
    if (found != 0) return found > 0 ? fail(error, error_size, "The printer has no web pages on this connection") : -1;
    bool ipv6 = strchr(network.host, ':') != NULL;
    size_t host_length = strlen(network.host);
    if (!ipv6 && host_length > 0 && network.host[host_length - 1] == '.') network.host[host_length - 1] = '\0';
    int length = snprintf(url, size, ipv6 ? "http://[%s]/" : "http://%s/", network.host);
    return length > 0 && (size_t)length < size ? 0 : fail(error, error_size, "Address too long");
}
