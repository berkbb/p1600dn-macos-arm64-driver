#include <arpa/inet.h>
#include <cups/cups.h>
#include <cups/http.h>
#include <cups/ipp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "p1600_queues.h"

#define STATUS_TIMEOUT_MS 5000

typedef enum {
    STATUS_LANG_TR = 0,
    STATUS_LANG_EN = 1
} StatusLang;

static StatusLang g_status_lang = STATUS_LANG_TR;

__attribute__((unused))
static void status_set_language(StatusLang lang) {
    g_status_lang = lang;
}

__attribute__((unused))
static StatusLang status_get_language(void) {
    return g_status_lang;
}

static void init_language_from_env(void) {
    const char *lang_env = getenv("P1600DN_LANG");
    if (lang_env) {
        if (strcasecmp(lang_env, "en") == 0 || strcasecmp(lang_env, "english") == 0) {
            g_status_lang = STATUS_LANG_EN;
        } else if (strcasecmp(lang_env, "tr") == 0 || strcasecmp(lang_env, "turkish") == 0) {
            g_status_lang = STATUS_LANG_TR;
        }
    }
}

static const char *connection_type(const char *uri) {
    if (g_status_lang == STATUS_LANG_EN) {
        if (!uri) return "Unknown";
        if (strncasecmp(uri, "usb://", 6) == 0) return "USB";
        if (strncasecmp(uri, "ipp://", 6) == 0 || strncasecmp(uri, "ipps://", 7) == 0) return "Network (IPP)";
        if (strncasecmp(uri, "dnssd://", 8) == 0) return "Network (Bonjour / Wi-Fi or Ethernet)";
        if (strncasecmp(uri, "socket://", 9) == 0) return "Network (AppSocket / Ethernet or Wi-Fi)";
        return "CUPS backend";
    }
    if (!uri) return "Bilinmiyor";
    if (strncasecmp(uri, "usb://", 6) == 0) return "USB";
    if (strncasecmp(uri, "ipp://", 6) == 0 || strncasecmp(uri, "ipps://", 7) == 0) return "Ağ (IPP)";
    if (strncasecmp(uri, "dnssd://", 8) == 0) return "Ağ (Bonjour / Wi-Fi veya Ethernet)";
    if (strncasecmp(uri, "socket://", 9) == 0) return "Ağ (AppSocket / Ethernet veya Wi-Fi)";
    return "CUPS arka ucu";
}

static const char *state_name(int state) {
    if (g_status_lang == STATUS_LANG_EN) {
        switch (state) {
            case IPP_PSTATE_IDLE: return "Idle / Ready";
            case IPP_PSTATE_PROCESSING: return "Printing";
            case IPP_PSTATE_STOPPED: return "Stopped / Attention required";
            default: return "Unknown";
        }
    }
    switch (state) {
        case IPP_PSTATE_IDLE: return "Hazır";
        case IPP_PSTATE_PROCESSING: return "Yazdırıyor";
        case IPP_PSTATE_STOPPED: return "Durduruldu / dikkat gerekli";
        default: return "Bilinmiyor";
    }
}

static void print_text_attribute(const ipp_t *response, const char *name, const char *label) {
    ipp_attribute_t *attr = ippFindAttribute((ipp_t *)response, name, IPP_TAG_ZERO);
    const char *value = attr ? ippGetString(attr, 0, NULL) : NULL;
    if (value && *value) printf("%s: %s\n", label, value);
}

static int print_supplies(const ipp_t *response) {
    ipp_attribute_t *levels = ippFindAttribute((ipp_t *)response, "marker-levels", IPP_TAG_INTEGER);
    ipp_attribute_t *names = ippFindAttribute((ipp_t *)response, "marker-names", IPP_TAG_ZERO);
    ipp_attribute_t *types = ippFindAttribute((ipp_t *)response, "marker-types", IPP_TAG_ZERO);
    int count;

    if (!levels || (count = ippGetCount(levels)) < 1) {
        puts(g_status_lang == STATUS_LANG_EN
             ? "Cartridge / toner: Printer does not report this information or CUPS has not retrieved it yet."
             : "Kartuş / toner: Yazıcı bu bilgiyi paylaşmıyor veya CUPS henüz alamadı.");
        return 0;
    }

    puts(g_status_lang == STATUS_LANG_EN ? "Cartridge / toner:" : "Kartuş / toner:");
    for (int i = 0; i < count; i++) {
        const char *default_supply_name = (g_status_lang == STATUS_LANG_EN) ? "Supply" : "Sarf malzemesi";
        const char *name = names && i < ippGetCount(names) ? ippGetString(names, i, NULL) : default_supply_name;
        const char *type = types && i < ippGetCount(types) ? ippGetString(types, i, NULL) : "";
        int level = ippGetInteger(levels, i);

        if (!name || !*name) name = default_supply_name;
        if (!type) type = "";

        if (type && *type) printf("  - %s (%s): ", name, type);
        else printf("  - %s: ", name);

        if (level >= 0 && level <= 100) printf("%d%%\n", level);
        else if (level == -1) puts(g_status_lang == STATUS_LANG_EN ? "unavailable" : "kullanılamıyor");
        else if (level == -2) puts(g_status_lang == STATUS_LANG_EN ? "unknown" : "bilinmiyor");
        else if (level == -3) puts(g_status_lang == STATUS_LANG_EN ? "level unsupported, cartridge not full" : "seviyesi desteklenmiyor, kartuş dolu değil");
        else puts(g_status_lang == STATUS_LANG_EN ? "unknown" : "bilinmiyor");
    }
    print_text_attribute(response, "marker-message", g_status_lang == STATUS_LANG_EN ? "Marker message" : "Kartuş mesajı");
    return 1;
}

static int print_response(const char *label, const char *uri, ipp_t *response) {
    ipp_attribute_t *state = ippFindAttribute(response, "printer-state", IPP_TAG_ENUM);
    ipp_attribute_t *reasons = ippFindAttribute(response, "printer-state-reasons", IPP_TAG_ZERO);

    printf("\n%s\n", label);
    printf(g_status_lang == STATUS_LANG_EN ? "Connection: %s\n" : "Bağlantı: %s\n", connection_type(uri));
    if (uri) printf("URI: %s\n", uri);
    printf(g_status_lang == STATUS_LANG_EN ? "State: %s\n" : "Durum: %s\n",
           state ? state_name(ippGetInteger(state, 0)) : (g_status_lang == STATUS_LANG_EN ? "Unknown" : "Bilinmiyor"));
    print_text_attribute(response, "printer-state-message", g_status_lang == STATUS_LANG_EN ? "State message" : "Durum mesajı");
    if (reasons) {
        int reason_count = ippGetCount(reasons);
        for (int i = 0; i < reason_count; i++) {
            const char *reason = ippGetString(reasons, i, NULL);
            if (reason && *reason && strcmp(reason, "none") != 0) {
                printf(g_status_lang == STATUS_LANG_EN ? "State reason: %s\n" : "Durum nedeni: %s\n", reason);
            }
        }
    }
    return print_supplies(response);
}

static ipp_t *get_printer_attributes(http_t *http, const char *resource, const char *printer_uri) {
    static const char * const requested[] = {
        "printer-state", "printer-state-message", "printer-state-reasons",
        "marker-levels", "marker-names", "marker-types", "marker-message"
    };
    ipp_t *request = ippNewRequest(IPP_OP_GET_PRINTER_ATTRIBUTES);
    if (!request) return NULL;

    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, printer_uri);
    ippAddStrings(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "requested-attributes",
                  (int)(sizeof(requested) / sizeof(requested[0])), NULL, requested);
    return cupsDoRequest(http, request, resource);
}

static int is_ipp_uri(const char *uri) {
    return uri && (strncasecmp(uri, "ipp://", 6) == 0 || strncasecmp(uri, "ipps://", 7) == 0);
}

static int query_remote_ipp(const char *uri, const char *label) {
    char scheme[32], username[256], host[HTTP_MAX_HOST], resource[HTTP_MAX_URI];
    int port = 0;
    http_t *http;
    ipp_t *response;

    if (!is_ipp_uri(uri)) {
        fprintf(stderr, g_status_lang == STATUS_LANG_EN
                ? "ERROR: Direct query is only supported for ipp:// or ipps:// URIs.\n"
                : "ERROR: Doğrudan sorgu yalnızca ipp:// veya ipps:// URI'leri için kullanılabilir.\n");
        return 0;
    }
    if (httpSeparateURI(HTTP_URI_CODING_ALL, uri, scheme, sizeof(scheme), username, sizeof(username),
                        host, sizeof(host), &port, resource, sizeof(resource)) != HTTP_URI_STATUS_OK) {
        fprintf(stderr, g_status_lang == STATUS_LANG_EN
                ? "ERROR: Invalid IPP URI: %s\n"
                : "ERROR: Geçersiz IPP URI'si: %s\n", uri);
        return 0;
    }
    if (port <= 0) port = ippPort();

    http = httpConnect2(host, port, NULL, AF_UNSPEC,
                        strcasecmp(scheme, "ipps") == 0 ? HTTP_ENCRYPTION_ALWAYS : HTTP_ENCRYPTION_IF_REQUESTED,
                        1, STATUS_TIMEOUT_MS, NULL);
    if (!http) {
        fprintf(stderr, g_status_lang == STATUS_LANG_EN
                ? "ERROR: Unable to connect to printer: %s\n"
                : "ERROR: Yazıcıya bağlanılamadı: %s\n", host);
        return 0;
    }
    response = get_printer_attributes(http, resource, uri);
    if (!response) {
        fprintf(stderr, g_status_lang == STATUS_LANG_EN
                ? "ERROR: IPP query failed: %s\n"
                : "ERROR: IPP sorgusu başarısız: %s\n", cupsLastErrorString());
        httpClose(http);
        return 0;
    }
    if (ippGetStatusCode(response) > IPP_STATUS_OK_CONFLICTING) {
        fprintf(stderr, g_status_lang == STATUS_LANG_EN
                ? "ERROR: IPP query rejected: %s\n"
                : "ERROR: IPP sorgusu reddedildi: %s\n", ippErrorString(ippGetStatusCode(response)));
        ippDelete(response);
        httpClose(http);
        return 0;
    }

    print_response(label, uri, response);
    ippDelete(response);
    httpClose(http);
    return 1;
}

static int query_queue(const char *queue_name) {
    cups_dest_t *dest = cupsGetNamedDest(CUPS_HTTP_DEFAULT, queue_name, NULL);
    const char *device_uri;
    const char *printer_uri;
    char resource[HTTP_MAX_URI];
    ipp_t *response;
    int has_supplies = 0;
    int query_succeeded = 0;

    if (!dest) {
        fprintf(stderr, g_status_lang == STATUS_LANG_EN
                ? "ERROR: CUPS queue not found: %s\n"
                : "ERROR: CUPS kuyruğu bulunamadı: %s\n", queue_name);
        return 0;
    }
    device_uri = cupsGetOption("device-uri", dest->num_options, dest->options);
    printer_uri = cupsGetOption("printer-uri-supported", dest->num_options, dest->options);
    if (!printer_uri) {
        fprintf(stderr, g_status_lang == STATUS_LANG_EN
                ? "ERROR: Unable to get IPP URI for queue: %s\n"
                : "ERROR: Kuyruk için IPP URI'si alınamadı: %s\n", queue_name);
        cupsFreeDests(1, dest);
        return 0;
    }
    if (snprintf(resource, sizeof(resource), "/printers/%s", queue_name) >= (int)sizeof(resource)) {
        fprintf(stderr, g_status_lang == STATUS_LANG_EN
                ? "ERROR: Queue name too long.\n"
                : "ERROR: Kuyruk adı çok uzun.\n");
        cupsFreeDests(1, dest);
        return 0;
    }

    response = get_printer_attributes(CUPS_HTTP_DEFAULT, resource, printer_uri);
    if (!response) {
        fprintf(stderr, g_status_lang == STATUS_LANG_EN
                ? "ERROR: CUPS status query failed: %s\n"
                : "ERROR: CUPS durum sorgusu başarısız: %s\n", cupsLastErrorString());
        cupsFreeDests(1, dest);
        return 0;
    }
    if (ippGetStatusCode(response) <= IPP_STATUS_OK_CONFLICTING) {
        query_succeeded = 1;
        has_supplies = print_response(queue_name, device_uri, response);
    } else {
        fprintf(stderr, g_status_lang == STATUS_LANG_EN
                ? "ERROR: CUPS query rejected: %s\n"
                : "ERROR: CUPS sorgusu reddedildi: %s\n", ippErrorString(ippGetStatusCode(response)));
    }
    ippDelete(response);

    /* Network IPP queues can provide a live response even when CUPS has not cached supplies yet. */
    if (!has_supplies && is_ipp_uri(device_uri)) {
        puts(g_status_lang == STATUS_LANG_EN
             ? "CUPS provided no supply data; querying printer directly via IPP..."
             : "CUPS kartuş bilgisi vermedi; yazıcıya doğrudan IPP sorgusu yapılıyor...");
        const char *remote_label = (g_status_lang == STATUS_LANG_EN)
            ? "Live IPP status from printer"
            : "Yazıcıdan canlı IPP durumu";
        if (query_remote_ipp(device_uri, remote_label)) query_succeeded = 1;
    }
    cupsFreeDests(1, dest);
    return query_succeeded;
}

/* A P1600dn series queue of this Mac: not an instance, not a printer
 * cupsGetDests() found over Bonjour (this Mac's own AirPrint advertisement
 * among them) and not a driverless queue (p1600_queues.h). */
static int is_p1600dn_queue(const cups_dest_t *dest) {
    const char *model = cupsGetOption("printer-make-and-model", dest->num_options, dest->options);
    if (dest->instance || !model || p1600_queue_is_driverless(model) ||
        p1600_queue_is_discovered(cupsGetOption("printer-type", dest->num_options, dest->options))) {
        return 0;
    }
    return strcasestr(model, "P1606dn") || strcasestr(model, "Professional_P1606dn") || strcasestr(model, "P1600") ||
           strcasestr(model, "P1607") || strcasestr(model, "P1608") || strcasestr(model, "P1609");
}

static int query_all_queues(void) {
    cups_dest_t *dests = NULL;
    int count = cupsGetDests(&dests);
    int found = 0;
    int queried = 0;

    for (int i = 0; i < count; i++) {
        if (is_p1600dn_queue(&dests[i])) {
            found++;
            queried += query_queue(dests[i].name);
        }
    }
    cupsFreeDests(count, dests);
    if (!found) {
        fprintf(stderr, g_status_lang == STATUS_LANG_EN
                ? "HP LaserJet Pro P1606dn CUPS queue not found. Retry with a queue name or --uri.\n"
                : "HP LaserJet Pro P1606dn CUPS kuyruğu bulunamadı. Bir kuyruk adı veya --uri ile tekrar deneyin.\n");
    } else if (!queried) {
        fprintf(stderr, g_status_lang == STATUS_LANG_EN
                ? "HP LaserJet Pro P1606dn queue found but status query could not complete.\n"
                : "HP LaserJet Pro P1606dn kuyruğu bulundu ancak durum sorgusu tamamlanamadı.\n");
    }
    return queried;
}

static void print_usage(const char *program) {
    if (g_status_lang == STATUS_LANG_EN) {
        fprintf(stderr, "Usage: %s [--en|--tr] [CUPS_QUEUE_NAME | --uri ipp(s)://PRINTER/ipp/print]\n", program);
    } else {
        fprintf(stderr, "Kullanım: %s [--en|--tr] [CUPS_KUYRUK_ADI | --uri ipp(s)://YAZICI/ipp/print]\n", program);
    }
}

int main(int argc, char *argv[]) {
    init_language_from_env();
    int arg_idx = 1;
    if (arg_idx < argc) {
        if (strcmp(argv[arg_idx], "--en") == 0 || strcmp(argv[arg_idx], "--english") == 0) {
            status_set_language(STATUS_LANG_EN);
            arg_idx++;
        } else if (strcmp(argv[arg_idx], "--tr") == 0 || strcmp(argv[arg_idx], "--turkish") == 0) {
            status_set_language(STATUS_LANG_TR);
            arg_idx++;
        } else if (strcmp(argv[arg_idx], "--lang") == 0 && arg_idx + 1 < argc) {
            if (strcmp(argv[arg_idx + 1], "en") == 0 || strcmp(argv[arg_idx + 1], "english") == 0) {
                status_set_language(STATUS_LANG_EN);
                arg_idx += 2;
            } else if (strcmp(argv[arg_idx + 1], "tr") == 0 || strcmp(argv[arg_idx + 1], "turkish") == 0) {
                status_set_language(STATUS_LANG_TR);
                arg_idx += 2;
            }
        }
    }
    int remaining = argc - arg_idx;
    if (remaining == 0) return query_all_queues() ? 0 : 1;
    if (remaining == 1) {
        if (strcmp(argv[arg_idx], "--uri") == 0) {
            print_usage(argv[0]);
            return 1;
        }
        return query_queue(argv[arg_idx]) ? 0 : 1;
    }
    if (remaining == 2 && strcmp(argv[arg_idx], "--uri") == 0) {
        const char *remote_label = (g_status_lang == STATUS_LANG_EN)
            ? "Live IPP status from printer"
            : "Yazıcıdan canlı IPP durumu";
        return query_remote_ipp(argv[arg_idx + 1], remote_label) ? 0 : 1;
    }
    print_usage(argv[0]);
    return 1;
}
