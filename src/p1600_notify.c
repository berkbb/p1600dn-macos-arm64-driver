#include "p1600_notify.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* Base reasons that raise an attention notice. */
static const char *const interesting_reasons[] = {
    "media-empty", "media-jam", "media-needed", "cover-open", "door-open",
    "toner-empty", "marker-supply-empty", "connecting-to-device", "offline", "other",
};

/* Copies at most size - 1 bytes without splitting a UTF-8 sequence and
 * turns control characters into spaces (notices are one line each). */
static void copy_line(char *destination, size_t size, const char *source) {
    if (!destination || size == 0) return;
    if (!source) source = "";
    size_t length = strlen(source);
    if (length >= size) {
        length = size - 1U;
        while (length > 0 && ((unsigned char)source[length] & 0xC0U) == 0x80U) length--;
    }
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)source[i];
        destination[i] = (char)(c < 0x20U || c == 0x7FU ? ' ' : c);
    }
    destination[length] = '\0';
}

static bool ends_with(const char *text, size_t length, const char *suffix) {
    size_t suffix_length = strlen(suffix);
    return length > suffix_length && memcmp(text + length - suffix_length, suffix, suffix_length) == 0;
}

void p1600_notify_reason_base(const char *keyword, char *base, size_t size) {
    if (!base || size == 0) return;
    base[0] = '\0';
    if (!keyword) return;
    size_t length = strlen(keyword);
    static const char *const suffixes[] = {"-error", "-warning", "-report"};
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        if (ends_with(keyword, length, suffixes[i])) {
            length -= strlen(suffixes[i]);
            break;
        }
    }
    if (length >= size) return;
    memcpy(base, keyword, length);
    base[length] = '\0';
}

bool p1600_notify_reason_of_interest(const char *base) {
    if (!base) return false;
    for (size_t i = 0; i < sizeof(interesting_reasons) / sizeof(interesting_reasons[0]); i++) {
        if (strcmp(base, interesting_reasons[i]) == 0) return true;
    }
    return false;
}

/* True when a reason with this base is present; `error_form` tells whether
 * one of them carries the -error suffix. */
static bool has_reason(const P1600PrinterSnapshot *snapshot, const char *base, bool *error_form) {
    bool found = false;
    if (error_form) *error_form = false;
    for (int i = 0; i < snapshot->reason_count && i < P1600_NOTIFY_MAX_REASONS; i++) {
        char reason_base[P1600_NOTIFY_REASON_SIZE];
        p1600_notify_reason_base(snapshot->reasons[i], reason_base, sizeof(reason_base));
        if (strcmp(reason_base, base) != 0) continue;
        found = true;
        if (error_form && ends_with(snapshot->reasons[i], strlen(snapshot->reasons[i]), "-error")) *error_form = true;
    }
    return found;
}

bool p1600_notify_attention_active(const P1600PrinterSnapshot *snapshot, const char *base) {
    if (!snapshot || !snapshot->has_data || !base) return false;
    bool error_form = false;
    if (!has_reason(snapshot, base, &error_form)) return false;
    if (strcmp(base, "connecting-to-device") == 0) return error_form || snapshot->message[0] != '\0';
    return true;
}

P1600TonerBucket p1600_notify_toner_bucket(const P1600PrinterSnapshot *snapshot) {
    if (!snapshot || !snapshot->has_data) return P1600_TONER_UNKNOWN;
    if (has_reason(snapshot, "toner-empty", NULL) || has_reason(snapshot, "marker-supply-empty", NULL)) {
        return P1600_TONER_EMPTY;
    }
    if (snapshot->toner < 0 || snapshot->toner > 100) return P1600_TONER_UNKNOWN;
    if (snapshot->toner == 0) return P1600_TONER_EMPTY;
    if (snapshot->toner <= 5) return P1600_TONER_VERY_LOW;
    if (snapshot->toner <= 15) return P1600_TONER_LOW;
    return P1600_TONER_OK;
}

static P1600Notice *add_notice(P1600Notice *notices, int *count, P1600NoticeKind kind, int level) {
    P1600Notice *notice = &notices[(*count)++];
    memset(notice, 0, sizeof(*notice));
    notice->kind = kind;
    notice->level = level;
    return notice;
}

int p1600_notify_compare(const P1600PrinterSnapshot *before, const P1600PrinterSnapshot *after, P1600Notice *notices, int max) {
    if (!after || !after->has_data || !notices || max <= 0) return 0;
    const bool primed = before && before->has_data;
    int count = 0;
    int message_target = -1;
    bool toner_attention = false;

    for (int i = 0; i < after->reason_count && i < P1600_NOTIFY_MAX_REASONS && count < max; i++) {
        char base[P1600_NOTIFY_REASON_SIZE];
        p1600_notify_reason_base(after->reasons[i], base, sizeof(base));
        if (!p1600_notify_reason_of_interest(base) || !p1600_notify_attention_active(after, base)) continue;
        if (primed && p1600_notify_attention_active(before, base)) continue;
        bool listed = false;
        for (int n = 0; n < count && !listed; n++) listed = strcmp(notices[n].reason, base) == 0;
        if (listed) continue;

        bool error_form = false;
        has_reason(after, base, &error_form);
        P1600Notice *notice = add_notice(notices, &count, P1600_NOTICE_ATTENTION, after->toner);
        copy_line(notice->reason, sizeof(notice->reason), base);
        notice->tentative = strcmp(base, "connecting-to-device") == 0 && !error_form;
        /* The driver reports its reasons in the -error form together with
         * the PPD's text, so the message belongs to such a reason. */
        if (message_target < 0 && error_form) message_target = count - 1;
        if (strcmp(base, "toner-empty") == 0 || strcmp(base, "marker-supply-empty") == 0) toner_attention = true;
    }
    if (count > 0 && after->message[0]) {
        if (message_target < 0) message_target = 0;
        copy_line(notices[message_target].message, sizeof(notices[message_target].message), after->message);
    }

    if (primed && count < max) {
        P1600TonerBucket was = p1600_notify_toner_bucket(before);
        P1600TonerBucket now = p1600_notify_toner_bucket(after);
        if (was == P1600_TONER_UNKNOWN) was = P1600_TONER_OK;
        if (now != P1600_TONER_UNKNOWN && now > was) {
            static const P1600NoticeKind kinds[] = {P1600_NOTICE_TONER_LOW, P1600_NOTICE_TONER_LOW, P1600_NOTICE_TONER_VERY_LOW,
                                                 P1600_NOTICE_TONER_EMPTY};
            if (!(now == P1600_TONER_EMPTY && toner_attention)) add_notice(notices, &count, kinds[now], after->toner);
        } else if (before->toner >= 0 && before->toner <= 15 && after->toner > 50 && after->toner <= 100) {
            add_notice(notices, &count, P1600_NOTICE_CARTRIDGE_REPLACED, after->toner);
        }
    }
    return count;
}

static P1600TonerBucket valid_bucket(P1600TonerBucket bucket) {
    return bucket >= P1600_TONER_OK && bucket <= P1600_TONER_EMPTY ? bucket : P1600_TONER_UNKNOWN;
}

int p1600_notify_prime_toner(P1600TonerBucket remembered, const P1600PrinterSnapshot *after, P1600Notice *notice) {
    if (!after || !after->has_data || !notice) return 0;
    P1600TonerBucket was = valid_bucket(remembered);
    P1600TonerBucket now = p1600_notify_toner_bucket(after);
    if (was == P1600_TONER_UNKNOWN) was = P1600_TONER_OK;
    if (now == P1600_TONER_UNKNOWN || now <= was) return 0;
    if (now == P1600_TONER_EMPTY && (p1600_notify_attention_active(after, "toner-empty") ||
                                  p1600_notify_attention_active(after, "marker-supply-empty"))) {
        return 0;
    }
    static const P1600NoticeKind kinds[] = {P1600_NOTICE_TONER_LOW, P1600_NOTICE_TONER_LOW, P1600_NOTICE_TONER_VERY_LOW,
                                         P1600_NOTICE_TONER_EMPTY};
    int count = 0;
    add_notice(notice, &count, kinds[now], after->toner);
    return 1;
}

bool p1600_notify_event_wants_levels(const P1600NotifyEvent *event, int own_job) {
    return event && event->kind == P1600_EVENT_JOB_COMPLETED && !event->levels_job &&
           (own_job <= 0 || event->job_id != own_job);
}

double p1600_notify_levels_time(double wanted, double asked, double interval) {
    if (asked < 0.0 || interval <= 0.0) return wanted;
    return wanted > asked + interval ? wanted : asked + interval;
}

bool p1600_notify_should_post(const P1600Notice *notice, P1600TonerBucket remembered) {
    if (!notice) return false;
    P1600TonerBucket bucket;
    switch (notice->kind) {
    case P1600_NOTICE_TONER_LOW:
        bucket = P1600_TONER_LOW;
        break;
    case P1600_NOTICE_TONER_VERY_LOW:
        bucket = P1600_TONER_VERY_LOW;
        break;
    case P1600_NOTICE_TONER_EMPTY:
        bucket = P1600_TONER_EMPTY;
        break;
    case P1600_NOTICE_ATTENTION:
    case P1600_NOTICE_CARTRIDGE_REPLACED:
    default:
        return true;
    }
    remembered = valid_bucket(remembered);
    if (remembered == P1600_TONER_UNKNOWN) remembered = P1600_TONER_OK;
    return bucket > remembered;
}

P1600TonerBucket p1600_notify_remember(P1600TonerBucket remembered, const P1600PrinterSnapshot *after) {
    P1600TonerBucket bucket = p1600_notify_toner_bucket(after);
    return bucket != P1600_TONER_UNKNOWN ? bucket : valid_bucket(remembered);
}

size_t p1600_notify_active_reasons(const P1600PrinterSnapshot *snapshot, char *out, size_t size) {
    if (!out || size == 0) return 0;
    out[0] = '\0';
    size_t length = 0;
    for (size_t i = 0; i < sizeof(interesting_reasons) / sizeof(interesting_reasons[0]); i++) {
        if (!p1600_notify_attention_active(snapshot, interesting_reasons[i])) continue;
        size_t needed = strlen(interesting_reasons[i]) + (length > 0 ? 1U : 0U);
        if (length + needed >= size) {
            out[0] = '\0';
            return 0;
        }
        if (length > 0) out[length++] = ',';
        strcpy(out + length, interesting_reasons[i]);
        length += strlen(interesting_reasons[i]);
    }
    return length;
}

bool p1600_notify_reason_listed(const char *list, const char *base) {
    if (!list || !base || !*base) return false;
    size_t length = strlen(base);
    for (const char *entry = list; *entry;) {
        size_t entry_length = strcspn(entry, ",");
        if (entry_length == length && strncmp(entry, base, length) == 0) return true;
        entry += entry_length;
        if (*entry == ',') entry++;
    }
    return false;
}

void p1600_notify_carry_toner(P1600PrinterSnapshot *after, const P1600PrinterSnapshot *before) {
    if (!after || !before || !after->has_data || !before->has_data) return;
    if (after->toner < 0 && before->toner >= 0 && before->toner <= 100) after->toner = before->toner;
}

size_t p1600_notice_name(const P1600Notice *notice, char *out, size_t size) {
    if (!out || size == 0) return 0;
    out[0] = '\0';
    if (!notice) return 0;
    int length;
    switch (notice->kind) {
    case P1600_NOTICE_ATTENTION:
        length = snprintf(out, size, "attention.%s", notice->reason);
        break;
    case P1600_NOTICE_TONER_LOW:
        length = snprintf(out, size, "toner.low");
        break;
    case P1600_NOTICE_TONER_VERY_LOW:
        length = snprintf(out, size, "toner.very-low");
        break;
    case P1600_NOTICE_TONER_EMPTY:
        length = snprintf(out, size, "toner.empty");
        break;
    case P1600_NOTICE_CARTRIDGE_REPLACED:
        length = snprintf(out, size, "toner.replaced");
        break;
    default:
        length = -1;
        break;
    }
    if (length < 0 || (size_t)length >= size) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)length;
}

/* --- IPP ------------------------------------------------------------------ */

#define REQUEST_TIMEOUT_SECONDS 30.0

void p1600_notify_set_server(const char *server) {
    if (!server || !*server) return;
    /* libcups keeps the server per thread; threads read CUPS_SERVER first. */
    setenv("CUPS_SERVER", server, 1);
    cupsSetServer(server);
}

http_t *p1600_notify_connect(int timeout_ms) {
    http_t *http = httpConnect2(cupsServer(), ippPort(), NULL, AF_UNSPEC, cupsEncryption(), 1,
                                timeout_ms > 0 ? timeout_ms : 30000, NULL);
    if (http) httpSetTimeout(http, REQUEST_TIMEOUT_SECONDS, NULL, NULL);
    return http;
}

static bool valid_queue(const char *queue) {
    if (!queue || !*queue) return false;
    size_t length = strlen(queue);
    if (length >= P1600_NOTIFY_QUEUE_SIZE) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)queue[i];
        if (c <= 0x20U || c == 0x7FU || c == '/' || c == '\\' || c == '?' || c == '#' || c == '"' || c == '\'') {
            return false;
        }
    }
    return true;
}

/* A request addressed to ipp://localhost/printers/<queue>, or to the
 * server (ipp://localhost/) for a NULL queue. */
static ipp_t *new_request(ipp_op_t operation, const char *queue, char *resource, size_t resource_size) {
    ipp_t *request = ippNewRequest(operation);
    if (!request) return NULL;
    char uri[HTTP_MAX_URI];
    if (queue) {
        httpAssembleURIf(HTTP_URI_CODING_ALL, uri, sizeof(uri), "ipp", NULL, "localhost", 0, "/printers/%s", queue);
        snprintf(resource, resource_size, "/printers/%s", queue);
    } else {
        snprintf(uri, sizeof(uri), "ipp://localhost/");
        snprintf(resource, resource_size, "/");
    }
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, uri);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsUser());
    return request;
}

/* P1600NotifyStatus of a request without an IPP response. */
static int failure_status(void) {
    ipp_status_t status = cupsLastError();
    if (status == IPP_STATUS_ERROR_NOT_FOUND) return P1600_NOTIFY_NOT_FOUND;
    /* Client errors (refused, not authorized) and CUPS's authentication
     * errors are answers; server errors and lost connections are not. */
    if ((status >= IPP_STATUS_ERROR_BAD_REQUEST && status < IPP_STATUS_ERROR_INTERNAL) ||
        status >= IPP_STATUS_ERROR_CUPS_AUTHENTICATION_CANCELED) {
        return P1600_NOTIFY_FAILED;
    }
    return P1600_NOTIFY_UNREACHABLE;
}

static int response_status(ipp_t *response) {
    if (!response) return failure_status();
    ipp_status_t status = ippGetStatusCode(response);
    if (status == IPP_STATUS_ERROR_NOT_FOUND) return P1600_NOTIFY_NOT_FOUND;
    if (status > IPP_STATUS_OK_EVENTS_COMPLETE) return P1600_NOTIFY_FAILED;
    return P1600_NOTIFY_OK;
}

static ipp_t *send_request(http_t *http, ipp_t *request, const char *resource, double timeout) {
    httpSetTimeout(http, timeout, NULL, NULL);
    return cupsDoRequest(http, request, resource);
}

static const char *string_value(ipp_t *response, const char *name, int index) {
    ipp_attribute_t *attribute = ippFindAttribute(response, name, IPP_TAG_ZERO);
    if (!attribute || index >= ippGetCount(attribute)) return NULL;
    return ippGetString(attribute, index, NULL);
}

static void read_reasons(ipp_t *response, P1600PrinterSnapshot *snapshot) {
    ipp_attribute_t *reasons = ippFindAttribute(response, "printer-state-reasons", IPP_TAG_ZERO);
    int count = reasons ? ippGetCount(reasons) : 0;
    for (int i = 0; i < count && snapshot->reason_count < P1600_NOTIFY_MAX_REASONS; i++) {
        const char *value = ippGetString(reasons, i, NULL);
        if (!value || !*value || strlen(value) >= P1600_NOTIFY_REASON_SIZE) continue;
        char keyword[P1600_NOTIFY_REASON_SIZE];
        size_t length = 0;
        for (; value[length]; length++) keyword[length] = (char)tolower((unsigned char)value[length]);
        keyword[length] = '\0';
        if (strcmp(keyword, "none") == 0) continue;
        bool listed = false;
        for (int n = 0; n < snapshot->reason_count && !listed; n++) listed = strcmp(snapshot->reasons[n], keyword) == 0;
        if (!listed) memcpy(snapshot->reasons[snapshot->reason_count++], keyword, length + 1U);
    }
}

/* The toner marker: the first of type toner or ink, else the only one,
 * else the one named black, else the first. */
static int read_toner(ipp_t *response) {
    ipp_attribute_t *levels = ippFindAttribute(response, "marker-levels", IPP_TAG_INTEGER);
    int count = levels ? ippGetCount(levels) : 0;
    if (count < 1) return -1;
    int chosen = -1;
    for (int i = 0; i < count && chosen < 0; i++) {
        const char *type = string_value(response, "marker-types", i);
        if (type && (strcmp(type, "toner") == 0 || strcmp(type, "toner-cartridge") == 0 || strcmp(type, "ink") == 0 ||
                     strcmp(type, "ink-cartridge") == 0)) {
            chosen = i;
        }
    }
    for (int i = 0; i < count && chosen < 0 && count > 1; i++) {
        const char *name = string_value(response, "marker-names", i);
        if (name && strcasestr(name, "black")) chosen = i;
    }
    if (chosen < 0) chosen = 0;
    int level = ippGetInteger(levels, chosen);
    return level >= 0 && level <= 100 ? level : -1;
}

int p1600_notify_snapshot(http_t *http, const char *queue, P1600PrinterSnapshot *snapshot) {
    if (!snapshot) return P1600_NOTIFY_FAILED;
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->toner = -1;
    if (!http || !valid_queue(queue)) return P1600_NOTIFY_FAILED;
    copy_line(snapshot->queue, sizeof(snapshot->queue), queue);

    static const char *const attributes[] = {
        "marker-levels", "marker-names", "marker-types", "printer-info",
        "printer-state", "printer-state-message", "printer-state-reasons",
    };
    char resource[256];
    ipp_t *request = new_request(IPP_OP_GET_PRINTER_ATTRIBUTES, queue, resource, sizeof(resource));
    if (!request) return P1600_NOTIFY_FAILED;
    ippAddStrings(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "requested-attributes",
                  (int)(sizeof(attributes) / sizeof(attributes[0])), NULL, attributes);
    ipp_t *response = send_request(http, request, resource, REQUEST_TIMEOUT_SECONDS);
    int status = response_status(response);
    if (status == P1600_NOTIFY_OK) {
        ipp_attribute_t *state = ippFindAttribute(response, "printer-state", IPP_TAG_ENUM);
        snapshot->state = state ? ippGetInteger(state, 0) : 0;
        read_reasons(response, snapshot);
        copy_line(snapshot->message, sizeof(snapshot->message), string_value(response, "printer-state-message", 0));
        copy_line(snapshot->info, sizeof(snapshot->info), string_value(response, "printer-info", 0));
        snapshot->toner = read_toner(response);
        snapshot->has_data = true;
    }
    ippDelete(response);
    return status;
}

/* notify-lease-duration of a response, or `requested` when absent. */
static int granted_lease(ipp_t *response, int requested) {
    ipp_attribute_t *lease = ippFindAttribute(response, "notify-lease-duration", IPP_TAG_INTEGER);
    int value = lease ? ippGetInteger(lease, 0) : requested;
    return value >= 0 ? value : 0;
}

int p1600_notify_subscribe(http_t *http, const char *queue, int lease_seconds, int *subscription_id) {
    if (subscription_id) *subscription_id = 0;
    if (!http || !valid_queue(queue) || !subscription_id || lease_seconds < 0) return P1600_NOTIFY_FAILED;

    static const char *const events[] = {"printer-state-changed", "printer-stopped", "job-completed"};
    char resource[256];
    ipp_t *request = new_request(IPP_OP_CREATE_PRINTER_SUBSCRIPTIONS, queue, resource, sizeof(resource));
    if (!request) return P1600_NOTIFY_FAILED;
    ippAddStrings(request, IPP_TAG_SUBSCRIPTION, IPP_TAG_KEYWORD, "notify-events",
                  (int)(sizeof(events) / sizeof(events[0])), NULL, events);
    ippAddString(request, IPP_TAG_SUBSCRIPTION, IPP_TAG_KEYWORD, "notify-pull-method", NULL, "ippget");
    ippAddInteger(request, IPP_TAG_SUBSCRIPTION, IPP_TAG_INTEGER, "notify-lease-duration", lease_seconds);
    ipp_t *response = send_request(http, request, resource, REQUEST_TIMEOUT_SECONDS);
    int status = response_status(response);
    if (status == P1600_NOTIFY_OK) {
        ipp_attribute_t *id = ippFindAttribute(response, "notify-subscription-id", IPP_TAG_INTEGER);
        if (id && ippGetInteger(id, 0) > 0) {
            *subscription_id = ippGetInteger(id, 0);
            status = granted_lease(response, lease_seconds);
        } else {
            status = P1600_NOTIFY_FAILED;  /* the subscription was refused (notify-status-code) */
        }
    }
    ippDelete(response);
    return status;
}

static ipp_t *subscription_request(ipp_op_t operation, int subscription_id, char *resource, size_t resource_size) {
    ipp_t *request = new_request(operation, NULL, resource, resource_size);
    if (request) {
        ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "notify-subscription-id", subscription_id);
    }
    return request;
}

int p1600_notify_renew(http_t *http, int subscription_id, int lease_seconds) {
    if (!http || subscription_id <= 0 || lease_seconds < 0) return P1600_NOTIFY_FAILED;
    char resource[16];
    ipp_t *request = subscription_request(IPP_OP_RENEW_SUBSCRIPTION, subscription_id, resource, sizeof(resource));
    if (!request) return P1600_NOTIFY_FAILED;
    ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "notify-lease-duration", lease_seconds);
    ipp_t *response = send_request(http, request, resource, REQUEST_TIMEOUT_SECONDS);
    int status = response_status(response);
    if (status == P1600_NOTIFY_OK) status = granted_lease(response, lease_seconds);
    ippDelete(response);
    return status;
}

int p1600_notify_cancel(http_t *http, int subscription_id) {
    if (!http || subscription_id <= 0) return P1600_NOTIFY_FAILED;
    char resource[16];
    ipp_t *request = subscription_request(IPP_OP_CANCEL_SUBSCRIPTION, subscription_id, resource, sizeof(resource));
    if (!request) return P1600_NOTIFY_FAILED;
    ipp_t *response = send_request(http, request, resource, REQUEST_TIMEOUT_SECONDS);
    int status = response_status(response);
    ippDelete(response);
    return status;
}

static double monotonic_seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static void sleep_seconds(double seconds) {
    if (seconds <= 0.0) return;
    struct timespec pause = {(time_t)seconds, (long)((seconds - (double)(time_t)seconds) * 1e9)};
    while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {
    }
}

/* "ipp://localhost/printers/Office%20P1606dn" -> "Office P1606dn". */
static void queue_from_uri(const char *uri, char *queue, size_t size) {
    queue[0] = '\0';
    const char *path = uri ? strstr(uri, "/printers/") : NULL;
    if (!path) path = uri ? strstr(uri, "/classes/") : NULL;
    if (!path) return;
    path = strchr(path + 1, '/') + 1;
    size_t length = 0;
    for (const char *p = path; *p && *p != '?' && *p != '/' && length + 1U < size; p++) {
        if (p[0] == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char hex[3] = {p[1], p[2], '\0'};
            queue[length++] = (char)strtol(hex, NULL, 16);
            p += 2;
        } else {
            queue[length++] = *p;
        }
    }
    queue[length] = '\0';
}

typedef struct {
    bool open;
    unsigned int seen;  /* attributes read for this event (EVENT_* bits) */
    int sequence;
    int subscription;
    int job_id;
    bool levels_job;
    P1600NotifyEventKind kind;
    char queue[128];
    char uri_queue[128];
} PendingEvent;

static void finish_event(PendingEvent *pending, int subscription_id, int first, P1600NotifyEvent *events, int max,
                         int *count, int *last) {
    if (!pending->open) return;
    pending->open = false;
    if (pending->sequence < first || (pending->subscription != 0 && pending->subscription != subscription_id)) return;
    if (*count >= max) return;
    P1600NotifyEvent *event = &events[(*count)++];
    memset(event, 0, sizeof(*event));
    event->kind = pending->kind;
    event->sequence = pending->sequence;
    event->job_id = pending->job_id;
    event->levels_job = pending->levels_job;
    copy_line(event->queue, sizeof(event->queue), pending->queue[0] ? pending->queue : pending->uri_queue);
    if (pending->sequence > *last) *last = pending->sequence;
}

/* Event notification groups of a Get-Notifications response, from
 * sequence number `first` on. Returns the count; `*last` is the highest
 * sequence number stored. */
static int read_events(ipp_t *response, int subscription_id, int first, P1600NotifyEvent *events, int max, int *last) {
    static const char *const names[] = {
        "notify-sequence-number", "notify-subscription-id", "notify-job-id", "notify-subscribed-event",
        "printer-name", "notify-printer-uri", "job-printer-uri", "job-name",
    };
    int count = 0;
    PendingEvent pending = {0};
    for (ipp_attribute_t *attribute = ippFirstAttribute(response); attribute;
         attribute = ippNextAttribute(response)) {
        const char *name = ippGetName(attribute);
        if (!name || ippGetGroupTag(attribute) != IPP_TAG_EVENT_NOTIFICATION) {
            finish_event(&pending, subscription_id, first, events, max, &count, last);
            continue;
        }
        unsigned int bit = 0;
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && !bit; i++) {
            if (strcmp(name, names[i]) == 0) bit = 1U << i;
        }
        /* libcups separates event groups; a repeated attribute also starts
         * the next event. */
        if (pending.open && (pending.seen & bit)) finish_event(&pending, subscription_id, first, events, max, &count, last);
        if (!pending.open) {
            memset(&pending, 0, sizeof(pending));
            pending.open = true;
            pending.sequence = -1;
        }
        pending.seen |= bit;
        ipp_tag_t tag = ippGetValueTag(attribute);
        if (strcmp(name, "notify-sequence-number") == 0 && tag == IPP_TAG_INTEGER) {
            pending.sequence = ippGetInteger(attribute, 0);
        } else if (strcmp(name, "notify-subscription-id") == 0 && tag == IPP_TAG_INTEGER) {
            pending.subscription = ippGetInteger(attribute, 0);
        } else if (strcmp(name, "notify-job-id") == 0 && tag == IPP_TAG_INTEGER) {
            pending.job_id = ippGetInteger(attribute, 0);
        } else if (strcmp(name, "notify-subscribed-event") == 0) {
            const char *event = ippGetString(attribute, 0, NULL);
            if (!event) event = "";
            pending.kind = strcmp(event, "printer-state-changed") == 0 ? P1600_EVENT_PRINTER_STATE_CHANGED
                           : strcmp(event, "printer-stopped") == 0     ? P1600_EVENT_PRINTER_STOPPED
                           : strcmp(event, "job-completed") == 0       ? P1600_EVENT_JOB_COMPLETED
                                                                       : P1600_EVENT_OTHER;
        } else if (strcmp(name, "printer-name") == 0) {
            copy_line(pending.queue, sizeof(pending.queue), ippGetString(attribute, 0, NULL));
        } else if (strcmp(name, "job-name") == 0) {
            const char *job_name = ippGetString(attribute, 0, NULL);
            pending.levels_job = job_name && strcmp(job_name, "ReportLevels") == 0;
        } else if ((strcmp(name, "notify-printer-uri") == 0 || strcmp(name, "job-printer-uri") == 0) &&
                   !pending.uri_queue[0]) {
            queue_from_uri(ippGetString(attribute, 0, NULL), pending.uri_queue, sizeof(pending.uri_queue));
        }
    }
    finish_event(&pending, subscription_id, first, events, max, &count, last);
    return count;
}

int p1600_notify_wait(http_t *http, int subscription_id, int *sequence, int wait_seconds, P1600NotifyEvent *events,
                   int max) {
    if (!http || subscription_id <= 0 || !sequence || max < 0 || (max > 0 && !events)) return P1600_NOTIFY_FAILED;
    if (*sequence < 1) *sequence = 1;
    if (wait_seconds < 0) wait_seconds = 0;
    const double deadline = monotonic_seconds() + wait_seconds;

    for (;;) {
        const double started = monotonic_seconds();
        char resource[16];
        ipp_t *request = new_request(IPP_OP_GET_NOTIFICATIONS, NULL, resource, sizeof(resource));
        if (!request) return P1600_NOTIFY_FAILED;
        ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "notify-subscription-ids", subscription_id);
        ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "notify-sequence-numbers", *sequence);
        ippAddBoolean(request, IPP_TAG_OPERATION, "notify-wait", 1);
        /* A waiting server answers within the wait; allow for it. */
        ipp_t *response = send_request(http, request, resource, REQUEST_TIMEOUT_SECONDS + wait_seconds);
        int status = response_status(response);
        if (status != P1600_NOTIFY_OK) {
            ippDelete(response);
            return status;
        }
        int last = 0;
        int count = read_events(response, subscription_id, *sequence, events, max, &last);
        bool complete = ippGetStatusCode(response) == IPP_STATUS_OK_EVENTS_COMPLETE;
        ipp_attribute_t *interval = ippFindAttribute(response, "notify-get-interval", IPP_TAG_INTEGER);
        int hint = interval ? ippGetInteger(interval, 0) : -1;
        ippDelete(response);

        if (count > 0) {
            *sequence = last + 1;
            return count;
        }
        if (complete) return P1600_NOTIFY_NOT_FOUND;  /* the subscription has ended */
        double now = monotonic_seconds();
        double remaining = deadline - now;
        if (remaining <= 0.0) return 0;
        if (hint >= 0) {
            /* cupsd ignores notify-wait and says when to ask again. */
            double pause = hint > 1 ? (double)hint : 1.0;
            sleep_seconds(remaining < pause ? remaining : pause);
        } else if (now - started < 0.25) {
            /* Neither waited nor named an interval: do not ask in a loop. */
            sleep_seconds(remaining < 1.0 ? remaining : 1.0);
        }
        if (monotonic_seconds() >= deadline) return 0;
    }
}

int p1600_notify_request_levels(const char *queue) {
    if (!valid_queue(queue)) return P1600_NOTIFY_FAILED;
    /* Create-Job as one request: cupsCreateJob() would first look the queue
     * up with two more requests and, for a missing queue, browse the network
     * with DNS-SD for a second. */
    char resource[256];
    ipp_t *request = new_request(IPP_OP_CREATE_JOB, queue, resource, sizeof(resource));
    if (!request) return P1600_NOTIFY_FAILED;
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "job-name", NULL, "ReportLevels");
    ipp_t *response = cupsDoRequest(CUPS_HTTP_DEFAULT, request, resource);
    int status = response_status(response);
    ipp_attribute_t *id = status == P1600_NOTIFY_OK ? ippFindAttribute(response, "job-id", IPP_TAG_INTEGER) : NULL;
    int job_id = id ? ippGetInteger(id, 0) : 0;
    ippDelete(response);
    if (status != P1600_NOTIFY_OK) return status;
    if (job_id <= 0) return P1600_NOTIFY_FAILED;

    static const char command[] = P1600_NOTIFY_REPORT_LEVELS;
    if (cupsStartDocument(CUPS_HTTP_DEFAULT, queue, job_id, "ReportLevels", CUPS_FORMAT_COMMAND, 1) !=
        HTTP_STATUS_CONTINUE) {
        status = failure_status();
    } else if (cupsWriteRequestData(CUPS_HTTP_DEFAULT, command, sizeof(command) - 1U) != HTTP_STATUS_CONTINUE) {
        status = failure_status();
    } else if (cupsFinishDocument(CUPS_HTTP_DEFAULT, queue) > IPP_STATUS_OK_EVENTS_COMPLETE) {
        status = failure_status();
    }
    if (status != P1600_NOTIFY_OK) {
        cupsCancelJob2(CUPS_HTTP_DEFAULT, queue, job_id, 0);
        return status;
    }
    return job_id;
}

int p1600_notify_cancel_levels(const char *queue, int job_id) {
    if (!valid_queue(queue) || job_id <= 0) return P1600_NOTIFY_FAILED;
    char resource[256];
    ipp_t *request = new_request(IPP_OP_CANCEL_JOB, queue, resource, sizeof(resource));
    if (!request) return P1600_NOTIFY_FAILED;
    ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "job-id", job_id);
    ipp_t *response = cupsDoRequest(CUPS_HTTP_DEFAULT, request, resource);
    /* A job that has finished cannot be canceled: nothing left to do. */
    bool finished = response && ippGetStatusCode(response) == IPP_STATUS_ERROR_NOT_POSSIBLE;
    int status = finished ? P1600_NOTIFY_OK : response_status(response);
    ippDelete(response);
    return status;
}
