#include "p1600_airprint.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dns_sd.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <cups/http.h>
#include <cups/ipp.h>

#include "p1600_raster_doc.h"

#define IO_BUFFER_SIZE (64U * 1024U)
#define ICON_LIMIT ((size_t)8 * 1024 * 1024)
#define DRAIN_LIMIT (P1600_AIR_DOCUMENT_LIMIT + P1600_AIR_IPP_LIMIT)
#define MAX_RANGES 100
#define MAX_POLLED_JOBS 64
#define LINGER_MS 2000
#define SMALL_BODY ((size_t)1024 * 1024)

static atomic_int verbose_level;

void p1600_air_log(const char *format, ...) {
    char line[2048];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    fprintf(stderr, "airprint: %s\n", line);
}

#define VERBOSE(...)                                          \
    do {                                                      \
        if (atomic_load(&verbose_level) > 0) p1600_air_log(__VA_ARGS__); \
    } while (0)

static int fail(char *error, size_t error_size, const char *format, ...) {
    if (error && error_size > 0) {
        va_list arguments;
        va_start(arguments, format);
        vsnprintf(error, error_size, format, arguments);
        va_end(arguments);
    }
    return -1;
}

/* Copies at most `max_bytes` of UTF-8 text without cutting a character. */
static void copy_text(char *out, size_t size, const char *in, size_t max_bytes) {
    if (!out || size == 0) return;
    if (!in) in = "";
    size_t limit = size - 1U < max_bytes ? size - 1U : max_bytes;
    size_t length = strlen(in);
    if (length > limit) {
        length = limit;
        while (length > 0 && ((unsigned char)in[length] & 0xc0U) == 0x80U) length--;
    }
    memcpy(out, in, length);
    out[length] = '\0';
    /* Control characters never reach IPP text, logs or option files. */
    for (char *p = out; *p; p++) {
        if ((unsigned char)*p < 0x20U || *p == 0x7f) *p = ' ';
    }
}

static bool is_keyword(const char *text) {
    if (!text || !*text || strlen(text) > 255) return false;
    for (const char *p = text; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.')) {
            return false;
        }
    }
    return true;
}

static bool in_list(const char *value, const char *const *list, size_t count) {
    if (!value) return false;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(value, list[i]) == 0) return true;
    }
    return false;
}

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

/* --- media ------------------------------------------------------------------------ */

const P1600AirMedia p1600_air_media[] = {
    {"Letter", "na_letter_8.5x11in", 21590, 27940},
    {"Legal", "na_legal_8.5x14in", 21590, 35560},
    {"A4", "iso_a4_210x297mm", 21000, 29700},
    {"Executive", "na_executive_7.25x10.5in", 18415, 26670},
    {"FanFoldGermanLegal", "na_foolscap_8.5x13in", 21590, 33020},
    {"A5", "iso_a5_148x210mm", 14800, 21000},
    {"A6", "iso_a6_105x148mm", 10500, 14800},
    {"B5", "jis_b5_182x257mm", 18200, 25700},
    {"195x270mm", "om_16k_195x270mm", 19500, 27000},
    {"184x260mm", "om_16k_184x260mm", 18400, 26000},
    {"197x273mm", "roc_16k_7.75x10.75in", 19685, 27305},
    {"Postcard", "jpn_hagaki_100x148mm", 10000, 14800},
    /* The PPD's double postcard is landscape (567 x 420 pt). */
    {"DoublePostcard", "jpn_oufuku_148x200mm", 20000, 14800},
    {"Env10", "na_number-10_4.125x9.5in", 10477, 24130},
    {"EnvMonarch", "na_monarch_3.875x7.5in", 9842, 19050},
    {"EnvISOB5", "iso_b5_176x250mm", 17600, 25000},
    {"EnvC5", "iso_c5_162x229mm", 16200, 22900},
    {"EnvDL", "iso_dl_110x220mm", 11000, 22000},
};
const size_t p1600_air_media_count = COUNT(p1600_air_media);

const P1600AirMedia *p1600_air_media_by_ppd(const char *page_size) {
    if (!page_size) return NULL;
    for (size_t i = 0; i < p1600_air_media_count; i++) {
        if (strcasecmp(page_size, p1600_air_media[i].ppd) == 0) return &p1600_air_media[i];
    }
    return NULL;
}

const P1600AirMedia *p1600_air_media_by_size(int width, int length) {
    const P1600AirMedia *best = NULL;
    int best_distance = 0;
    for (size_t i = 0; i < p1600_air_media_count; i++) {
        const P1600AirMedia *media = &p1600_air_media[i];
        for (int turn = 0; turn < 2; turn++) {
            int w = turn ? media->length : media->width, l = turn ? media->width : media->length;
            int distance = abs(width - w) + abs(length - l);
            if (abs(width - w) <= 100 && abs(length - l) <= 100 && (!best || distance < best_distance)) {
                best = media;
                best_distance = distance;
            }
        }
    }
    return best;
}

const P1600AirMedia *p1600_air_media_by_name(const char *name) {
    if (!name || !*name) return NULL;
    for (size_t i = 0; i < p1600_air_media_count; i++) {
        if (strcmp(name, p1600_air_media[i].pwg) == 0) return &p1600_air_media[i];
    }
    const P1600AirMedia *media = p1600_air_media_by_ppd(name);
    if (media) return media;
    pwg_media_t *pwg = pwgMediaForLegacy(name);
    if (!pwg) pwg = pwgMediaForPWG(name);
    if (pwg) {
        for (size_t i = 0; i < p1600_air_media_count; i++) {
            if (pwg->pwg && strcmp(pwg->pwg, p1600_air_media[i].pwg) == 0) return &p1600_air_media[i];
        }
        return p1600_air_media_by_size(pwg->width, pwg->length);
    }
    return NULL;
}

const P1600AirMediaType p1600_air_media_types[] = {
    {"stationery", "Paper"},
    {"stationery-lightweight", "Light60-74g"},
    {"com.hp.midweight", "Mid-Weight96-110g"},
    {"stationery-heavyweight", "Heavy111-130g"},
    {"com.hp.extra-heavy", "ExtraHeavy131-175g"},
    {"transparency", "TransparencyPaper"},
    {"labels", "LabelsPaper"},
    {"stationery-letterhead", "LetterheadPaper"},
    {"envelope", "EnvelopePaper"},
    {"stationery-preprinted", "PreprintedPaper"},
    {"stationery-prepunched", "PrepunchedPaper"},
    {"stationery-colored", "ColorPaper"},
    {"com.hp.bond", "BondPaper"},
    {"stationery-recycled", "RecycledPaper"},
    {"com.hp.rough", "RoughPaper"},
    {"com.hp.vellum", "VellumPaper"},
};
const size_t p1600_air_media_type_count = COUNT(p1600_air_media_types);

const char *p1600_air_media_type_ppd(const char *keyword) {
    if (!keyword) return NULL;
    for (size_t i = 0; i < p1600_air_media_type_count; i++) {
        if (strcmp(keyword, p1600_air_media_types[i].ipp) == 0) return p1600_air_media_types[i].ppd;
    }
    return NULL;
}

const char *p1600_air_media_source_ppd(const char *keyword) {
    if (!keyword) return NULL;
    if (strcmp(keyword, "auto") == 0) return "Auto";
    if (strcmp(keyword, "manual") == 0) return "Manual";
    return NULL;
}

/* --- formats and options -------------------------------------------------------- */

static const char *const document_formats[] = {"application/pdf", "image/jpeg", "image/png", "image/urf",
                                               "image/pwg-raster", "application/octet-stream"};

const char *p1600_air_detect_format(const unsigned char *data, size_t length) {
    if (!data) return NULL;
    if (length >= 8 && memcmp(data, "UNIRAST", 8) == 0) return "image/urf";
    if (length >= 4 && memcmp(data, "RaS2", 4) == 0) return "image/pwg-raster";
    if (length >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff) return "image/jpeg";
    if (length >= 8 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) return "image/png";
    /* A PDF header may follow up to 1 KiB of other bytes. */
    size_t limit = length < 1024U ? length : 1024U;
    for (size_t i = 0; i + 5U <= limit; i++) {
        if (memcmp(data + i, "%PDF-", 5) == 0) return "application/pdf";
    }
    return NULL;
}

static const char *const sides_supported[] = {"one-sided", "two-sided-long-edge", "two-sided-short-edge"};
static const char *const scaling_supported[] = {"auto", "auto-fit", "fill", "fit", "none"};
static const char *const color_modes[] = {"auto", "monochrome"};
static const char *const which_jobs_supported[] = {"completed", "not-completed", "all",    "aborted",
                                                    "canceled",  "pending",       "pending-held", "processing"};

int p1600_air_job_options(ipp_t *job, int num_options, cups_option_t **options) {
    if (!job || !options) return num_options;
    ipp_attribute_t *attr;
    char value[1024];
    if ((attr = ippFindAttribute(job, "copies", IPP_TAG_INTEGER)) != NULL) {
        snprintf(value, sizeof(value), "%d", ippGetInteger(attr, 0));
        num_options = cupsAddOption("copies", value, num_options, options);
    }
    if ((attr = ippFindAttribute(job, "sides", IPP_TAG_KEYWORD)) != NULL) {
        num_options = cupsAddOption("sides", ippGetString(attr, 0, NULL), num_options, options);
    }
    const P1600AirMedia *media = NULL;
    const char *source = NULL, *type = NULL;
    if ((attr = ippFindAttribute(job, "media-col", IPP_TAG_BEGIN_COLLECTION)) != NULL) {
        ipp_t *col = ippGetCollection(attr, 0);
        ipp_attribute_t *member;
        if ((member = ippFindAttribute(col, "media-size-name", IPP_TAG_ZERO)) != NULL) {
            media = p1600_air_media_by_name(ippGetString(member, 0, NULL));
        }
        if (!media && (member = ippFindAttribute(col, "media-size", IPP_TAG_BEGIN_COLLECTION)) != NULL) {
            ipp_t *size = ippGetCollection(member, 0);
            ipp_attribute_t *x = ippFindAttribute(size, "x-dimension", IPP_TAG_INTEGER);
            ipp_attribute_t *y = ippFindAttribute(size, "y-dimension", IPP_TAG_INTEGER);
            if (x && y) media = p1600_air_media_by_size(ippGetInteger(x, 0), ippGetInteger(y, 0));
        }
        if ((member = ippFindAttribute(col, "media-source", IPP_TAG_ZERO)) != NULL) {
            source = p1600_air_media_source_ppd(ippGetString(member, 0, NULL));
        }
        if ((member = ippFindAttribute(col, "media-type", IPP_TAG_ZERO)) != NULL) {
            type = p1600_air_media_type_ppd(ippGetString(member, 0, NULL));
        }
    }
    if (!media && (attr = ippFindAttribute(job, "media", IPP_TAG_ZERO)) != NULL) {
        media = p1600_air_media_by_name(ippGetString(attr, 0, NULL));
    }
    if (media) num_options = cupsAddOption("PageSize", media->ppd, num_options, options);
    if (source) num_options = cupsAddOption("InputSlot", source, num_options, options);
    if (type) num_options = cupsAddOption("MediaType", type, num_options, options);
    if ((attr = ippFindAttribute(job, "print-quality", IPP_TAG_ENUM)) != NULL) {
        int quality = ippGetInteger(attr, 0);
        if (quality == IPP_QUALITY_NORMAL) num_options = cupsAddOption("Resolution", "600dpi", num_options, options);
        if (quality == IPP_QUALITY_HIGH) num_options = cupsAddOption("Resolution", "1200x600dpi", num_options, options);
    }
    if ((attr = ippFindAttribute(job, "page-ranges", IPP_TAG_RANGE)) != NULL) {
        size_t used = 0;
        value[0] = '\0';
        for (int i = 0; i < ippGetCount(attr) && used < sizeof(value); i++) {
            int upper = 0, lower = ippGetRange(attr, i, &upper);
            int written = snprintf(value + used, sizeof(value) - used, "%s%d-%d", i ? "," : "", lower, upper);
            if (written < 0) break;
            used += (size_t)written;
        }
        if (used < sizeof(value)) num_options = cupsAddOption("page-ranges", value, num_options, options);
    }
    if ((attr = ippFindAttribute(job, "orientation-requested", IPP_TAG_ENUM)) != NULL) {
        snprintf(value, sizeof(value), "%d", ippGetInteger(attr, 0));
        num_options = cupsAddOption("orientation-requested", value, num_options, options);
    }
    if ((attr = ippFindAttribute(job, "print-scaling", IPP_TAG_KEYWORD)) != NULL) {
        const char *scaling = ippGetString(attr, 0, NULL);
        num_options = cupsAddOption("print-scaling", scaling, num_options, options);
        if (strcmp(scaling, "fit") == 0) num_options = cupsAddOption("fit-to-page", "true", num_options, options);
    }
    return num_options;
}

/* --- Bonjour names and records -------------------------------------------------- */

void p1600_air_uuid(const char *host, const char *queue, char *uuid, size_t size) {
    char seed[768];
    int length = snprintf(seed, sizeof(seed), "p1600dn-airprint\n%s\n%s", host ? host : "", queue ? queue : "");
    if (length < 0) length = 0;
    if ((size_t)length >= sizeof(seed)) length = (int)sizeof(seed) - 1;
    unsigned char hash[32];
    memset(hash, 0, sizeof(hash));
    if (cupsHashData("sha-256", seed, (size_t)length, hash, sizeof(hash)) < 16) {
        /* FNV-1a as a last resort: still the same for the same names. */
        uint64_t value = 1469598103934665603ULL;
        for (int i = 0; i < length; i++) value = (value ^ (unsigned char)seed[i]) * 1099511628211ULL;
        for (int i = 0; i < 16; i++) hash[i] = (unsigned char)(value >> ((i % 8) * 8)) ^ (unsigned char)i;
    }
    hash[6] = (unsigned char)((hash[6] & 0x0fU) | 0x50U);  /* name based (version 5) */
    hash[8] = (unsigned char)((hash[8] & 0x3fU) | 0x80U);  /* RFC 4122 variant */
    snprintf(uuid, size,
             "urn:uuid:%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", hash[0], hash[1],
             hash[2], hash[3], hash[4], hash[5], hash[6], hash[7], hash[8], hash[9], hash[10], hash[11], hash[12],
             hash[13], hash[14], hash[15]);
}

void p1600_air_service_name(const char *info, const char *computer, char *name, size_t size) {
    char full[600];
    snprintf(full, sizeof(full), "%s @ %s", info && *info ? info : "Printer",
             computer && *computer ? computer : "Mac");
    /* DNS-SD instance names are at most 63 bytes. */
    copy_text(name, size, full, 63);
    size_t length = strlen(name);
    while (length > 0 && name[length - 1] == ' ') name[--length] = '\0';
}

int p1600_air_txt_record(const P1600AirTxt *txt, unsigned char *buffer, size_t size, uint16_t *length) {
    if (!txt || !buffer || size == 0 || !length) return -1;
    char product[258], make_model[256], location[256];
    copy_text(make_model, sizeof(make_model), txt->make_model, 253);
    copy_text(location, sizeof(location), txt->location, 255);
    snprintf(product, sizeof(product), "(%s)", make_model);
    const struct {
        const char *key, *value;
    } items[] = {
        {"txtvers", "1"}, {"qtotal", "1"}, {"rp", txt->resource}, {"ty", make_model}, {"product", product},
        {"note", location}, {"adminurl", txt->admin_url}, {"pdl", P1600_AIR_PDL}, {"URF", P1600_AIR_URF_SUPPORTED},
        {"Color", "F"}, {"Duplex", "T"}, {"Copies", "T"}, {"Scan", "F"}, {"Fax", "F"}, {"kind", "document"},
        {"PaperMax", "legal-A4"}, {"priority", "50"}, {"UUID", txt->uuid}, {"air", "none"},
    };
    /* dns_sd grows its own buffer as needed; the record is copied out. */
    TXTRecordRef record;
    TXTRecordCreate(&record, 0, NULL);
    int status = 0;
    for (size_t i = 0; i < COUNT(items) && status == 0; i++) {
        const char *value = items[i].value ? items[i].value : "";
        size_t value_length = strlen(value);
        if (value_length > 255U) {
            status = -1;
            break;
        }
        if (TXTRecordSetValue(&record, items[i].key, (uint8_t)value_length, value) != kDNSServiceErr_NoError) {
            status = -1;
        }
    }
    uint16_t total = TXTRecordGetLength(&record);
    if (status == 0 && total <= size) {
        memcpy(buffer, TXTRecordGetBytesPtr(&record), total);
        *length = total;
    } else {
        status = -1;
        *length = 0;
    }
    TXTRecordDeallocate(&record);
    return status;
}

/* --- the local CUPS ---------------------------------------------------------------- */

static http_t *cups_connect(const P1600AirCups *cups) {
    const char *host = cups && cups->server && *cups->server ? cups->server : cupsServer();
    int port = cups && cups->port > 0 ? cups->port : ippPort();
    return httpConnect2(host, port, NULL, AF_UNSPEC, HTTP_ENCRYPTION_IF_REQUESTED, 1, 30000, NULL);
}

int p1600_air_cups_submit(const P1600AirCups *cups, const char *queue, const char *title, const char *format, int fd,
                       int num_options, cups_option_t *options, char *error, size_t error_size) {
    if (!queue || !format || fd < 0) return fail(error, error_size, "Invalid job");
    if (lseek(fd, 0, SEEK_SET) < 0) return fail(error, error_size, "Unable to read the document: %s", strerror(errno));
    http_t *http = cups_connect(cups);
    if (!http) return fail(error, error_size, "Unable to connect to CUPS: %s", cupsLastErrorString());
    int job_id = cupsCreateJob(http, queue, title ? title : "Untitled", num_options, options);
    if (job_id <= 0) {
        fail(error, error_size, "CUPS did not create the job: %s", cupsLastErrorString());
        httpClose(http);
        return -1;
    }
    http_status_t status = cupsStartDocument(http, queue, job_id, title ? title : "Untitled", format, 1);
    unsigned char *buffer = malloc(IO_BUFFER_SIZE);
    bool read_failed = buffer == NULL;
    while (buffer && status == HTTP_STATUS_CONTINUE) {
        ssize_t count = read(fd, buffer, IO_BUFFER_SIZE);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) read_failed = true;
        if (count <= 0) break;
        status = cupsWriteRequestData(http, (const char *)buffer, (size_t)count);
    }
    free(buffer);
    ipp_status_t finished = cupsFinishDocument(http, queue);
    if (read_failed || status != HTTP_STATUS_CONTINUE || finished > IPP_STATUS_OK_EVENTS_COMPLETE) {
        fail(error, error_size, "CUPS did not accept the document: %s",
             read_failed ? "read error" : cupsLastErrorString());
        cupsCancelJob2(http, queue, job_id, 0);
        httpClose(http);
        return -1;
    }
    httpClose(http);
    return job_id;
}

int p1600_air_cups_job_state(const P1600AirCups *cups, int job_id, ipp_jstate_t *state, char *message,
                          size_t message_size) {
    if (job_id <= 0 || !state) return -1;
    http_t *http = cups_connect(cups);
    if (!http) return -1;
    char uri[HTTP_MAX_URI];
    httpAssembleURIf(HTTP_URI_CODING_ALL, uri, sizeof(uri), "ipp", NULL, "localhost",
                     cups && cups->port > 0 ? cups->port : ippPort(), "/jobs/%d", job_id);
    ipp_t *request = ippNewRequest(IPP_OP_GET_JOB_ATTRIBUTES);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "job-uri", NULL, uri);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsUser());
    static const char *const wanted[] = {"job-state", "job-state-message"};
    ippAddStrings(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "requested-attributes", (int)COUNT(wanted), NULL,
                  wanted);
    ipp_t *response = cupsDoRequest(http, request, "/jobs/");
    httpClose(http);
    if (!response) return -1;
    int result = -1;
    ipp_status_t status = ippGetStatusCode(response);
    ipp_attribute_t *attr;
    if (status == IPP_STATUS_ERROR_NOT_FOUND || status == IPP_STATUS_ERROR_GONE) {
        result = 1;
    } else if (status <= IPP_STATUS_OK_EVENTS_COMPLETE &&
               (attr = ippFindAttribute(response, "job-state", IPP_TAG_ENUM)) != NULL) {
        *state = (ipp_jstate_t)ippGetInteger(attr, 0);
        if (message && message_size > 0) {
            attr = ippFindAttribute(response, "job-state-message", IPP_TAG_TEXT);
            copy_text(message, message_size, attr ? ippGetString(attr, 0, NULL) : "", 255);
        }
        result = 0;
    }
    ippDelete(response);
    return result;
}

int p1600_air_cups_cancel(const P1600AirCups *cups, const char *queue, int job_id) {
    http_t *http = cups_connect(cups);
    if (!http) return -1;
    ipp_status_t status = cupsCancelJob2(http, queue, job_id, 0);
    httpClose(http);
    return status <= IPP_STATUS_OK_EVENTS_COMPLETE ? 0 : -1;
}

ipp_t *p1600_air_cups_printer(const P1600AirCups *cups, const char *queue) {
    if (!queue || !*queue) return NULL;
    http_t *http = cups_connect(cups);
    if (!http) return NULL;
    char uri[HTTP_MAX_URI];
    httpAssembleURIf(HTTP_URI_CODING_ALL, uri, sizeof(uri), "ipp", NULL, "localhost",
                     cups && cups->port > 0 ? cups->port : ippPort(), "/printers/%s", queue);
    ipp_t *request = ippNewRequest(IPP_OP_GET_PRINTER_ATTRIBUTES);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, uri);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsUser());
    static const char *const wanted[] = {
        "marker-colors",  "marker-high-levels", "marker-levels",       "marker-low-levels",
        "marker-names",   "marker-types",       "media-default",       "printer-is-accepting-jobs",
        "printer-location", "printer-state",    "printer-state-message", "printer-state-reasons",
    };
    ippAddStrings(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "requested-attributes", (int)COUNT(wanted), NULL,
                  wanted);
    ipp_t *response = cupsDoRequest(http, request, "/");
    httpClose(http);
    if (response && ippGetStatusCode(response) > IPP_STATUS_OK_EVENTS_COMPLETE) {
        ippDelete(response);
        response = NULL;
    }
    return response;
}

/* --- server state ----------------------------------------------------------------- */

typedef struct P1600AirJob {
    struct P1600AirJob *next;
    int id;
    int printer;                /* index into the server's printers */
    char name[256];
    char user[256];
    char document_name[256];
    char format[64];            /* the document as it is (detected) */
    char format_supplied[64];
    ipp_t *attrs;               /* accepted job template attributes */
    ipp_jstate_t state;
    const char *reason;
    char message[256];
    time_t created, processing, completed;
    char document[PATH_MAX];    /* the received document until it is processed */
    bool has_document;
    bool queued;                /* waiting for the worker */
    int pins;                   /* threads using it outside the lock */
    size_t octets;
    int impressions;
    int cups_id;
} P1600AirJob;

typedef struct {
    P1600AirServer *server;
    int index;
    char name[128];
    char info[256];
    char make_model[256];
    char resource[300];         /* /ipp/print/<name> */
    char uuid[64];
    char service_name[64];
    char dns_sd_name[64];
    char device_id[512];
    char location[256];
    ipp_t *mirror;              /* attributes of the CUPS queue */
    bool mirror_failed;
    time_t refreshed, state_time;
    char state_key[1024];
    bool refreshing;
    DNSServiceRef ipp_ref, printer_ref;
} Printer;

typedef struct {
    P1600AirServer *server;
    http_t *http;
    int fd;
    pthread_t thread;
    bool closing, done;         /* under the server lock */
    time_t header_deadline;     /* under the server lock: while a request's header is read, else 0 */
    bool cut;                   /* under the server lock: closed for a slow header */
    char peer[256];
    char uri_host[256];
    int uri_port;
    http_state_t operation;
    bool keep_alive, must_close;
    bool unread_body;           /* request data left unread when closing */
    size_t ipp_bytes;
    bool ipp_too_large;
} Client;

struct P1600AirServer {
    P1600AirConfig config;
    char spool_dir[PATH_MAX];
    char cups_server[256];
    char temp_dir[PATH_MAX];
    char computer_name[256];
    char host_name[256];
    unsigned char *icon;
    size_t icon_size;
    int listeners[2];
    int listener_count;
    int port;
    time_t start_time;
    pthread_mutex_t lock;
    pthread_cond_t work;
    Printer printers[P1600_AIR_MAX_QUEUES];
    int printer_count;
    P1600AirJob *jobs, *last_job;
    int job_count;
    int next_job_id;
    Client *clients[P1600_AIR_MAX_CLIENTS];
    pthread_t worker;
    bool worker_started;
    atomic_bool stopping;
    size_t receiving;           /* under the lock: document bytes of uploads under way */
    DNSServiceRef dnssd;
    time_t dnssd_retry;
};

/* Seconds since the server started, always above zero. */
static int up_time(const P1600AirServer *server, time_t when) {
    return when ? (int)(when - server->start_time) + 1 : 0;
}

static bool job_active(const P1600AirJob *job) {
    return job->state < IPP_JSTATE_CANCELED;
}

static P1600AirJob *find_job(P1600AirServer *server, int id) {
    for (P1600AirJob *job = server->jobs; job; job = job->next) {
        if (job->id == id) return job;
    }
    return NULL;
}

static void discard_document(P1600AirJob *job) {
    if (job->document[0]) {
        unlink(job->document);
        job->document[0] = '\0';
    }
}

/* Under the lock: a final or new state with its reason and message. */
static void set_job_state(P1600AirJob *job, ipp_jstate_t state, const char *reason, const char *message) {
    job->state = state;
    job->reason = reason;
    copy_text(job->message, sizeof(job->message), message ? message : "", 255);
    time_t now = time(NULL);
    if (state == IPP_JSTATE_PROCESSING && !job->processing) job->processing = now;
    if (state >= IPP_JSTATE_CANCELED && !job->completed) job->completed = now;
}

static void free_job(P1600AirJob *job) {
    discard_document(job);
    ippDelete(job->attrs);
    free(job);
}

/* Under the lock: true when no further job (or document) fits: too many
 * active jobs, or too many document bytes waiting to be processed. */
static size_t queue_limit(const P1600AirServer *server) {
    return server->config.queue_limit ? server->config.queue_limit : P1600_AIR_QUEUE_LIMIT;
}

/* Under the lock: the bytes of the documents waiting for the worker and of
 * the uploads under way. */
static size_t queued_bytes(const P1600AirServer *server) {
    size_t pending = server->receiving;
    for (const P1600AirJob *job = server->jobs; job; job = job->next) {
        if (job->document[0]) pending += job->octets;
    }
    return pending;
}

static bool server_busy(P1600AirServer *server, bool new_job) {
    int active = 0;
    for (P1600AirJob *job = server->jobs; job; job = job->next) {
        if (job_active(job)) active++;
    }
    return (new_job && active >= P1600_AIR_MAX_ACTIVE_JOBS) || queued_bytes(server) >= queue_limit(server);
}

/* Counts `bytes` more of a document being received (in `*reserved`, given
 * back when the upload ends): uploads under way count against the queue
 * limit too, or many at once would pass it. False when it is passed; the
 * refused upload's bytes then stop counting at once, under the same lock,
 * so the other uploads are judged without them (not two refused at once). */
static bool reserve_bytes(P1600AirServer *server, size_t bytes, size_t *reserved) {
    pthread_mutex_lock(&server->lock);
    server->receiving += bytes;
    *reserved += bytes;
    bool fits = queued_bytes(server) <= queue_limit(server);
    if (!fits) {
        server->receiving -= *reserved;
        *reserved = 0;
    }
    pthread_mutex_unlock(&server->lock);
    return fits;
}

/* Under the lock: forget the oldest finished jobs beyond the history. */
static void prune_jobs(P1600AirServer *server) {
    while (server->job_count > P1600_AIR_JOB_HISTORY) {
        P1600AirJob *previous = NULL, *job = server->jobs;
        while (job && (job_active(job) || job->pins > 0)) {
            previous = job;
            job = job->next;
        }
        if (!job) return;
        if (previous) {
            previous->next = job->next;
        } else {
            server->jobs = job->next;
        }
        if (server->last_job == job) server->last_job = previous;
        server->job_count--;
        free_job(job);
    }
}

/* --- requested attributes -------------------------------------------------------- */

enum { CLASS_DESCRIPTION, CLASS_TEMPLATE, CLASS_EXPLICIT };

typedef struct {
    bool all, description, template_group;
    cups_array_t *names;
} Requested;

static int compare_names(void *first, void *second, void *data) {
    (void)data;
    return strcmp(first, second);
}

static void *copy_name(void *element, void *data) {
    (void)data;
    return strdup(element);
}

static void free_name(void *element, void *data) {
    (void)data;
    free(element);
}

/* No requested-attributes: every attribute (`defaults` NULL) or `defaults`. */
static void requested_init(Requested *requested, ipp_t *request, const char *const *defaults) {
    memset(requested, 0, sizeof(*requested));
    requested->names = cupsArrayNew3(compare_names, NULL, NULL, 0, copy_name, free_name);
    ipp_attribute_t *attr = ippFindAttribute(request, "requested-attributes", IPP_TAG_KEYWORD);
    if (!attr) {
        if (!defaults) {
            requested->all = true;
        } else {
            for (; *defaults; defaults++) cupsArrayAdd(requested->names, (void *)*defaults);
        }
        return;
    }
    for (int i = 0; i < ippGetCount(attr); i++) {
        const char *value = ippGetString(attr, i, NULL);
        if (!value) continue;
        if (strcmp(value, "all") == 0) {
            requested->all = true;
        } else if (strcmp(value, "printer-description") == 0 || strcmp(value, "job-description") == 0) {
            requested->description = true;
        } else if (strcmp(value, "job-template") == 0) {
            requested->template_group = true;
        } else if (!cupsArrayFind(requested->names, (void *)value)) {
            cupsArrayAdd(requested->names, (void *)value);
        }
    }
}

static void requested_free(Requested *requested) {
    cupsArrayDelete(requested->names);
    requested->names = NULL;
}

static bool want(const Requested *requested, const char *name, int class) {
    if (class != CLASS_EXPLICIT) {
        if (requested->all) return true;
        if (class == CLASS_DESCRIPTION && requested->description) return true;
        if (class == CLASS_TEMPLATE && requested->template_group) return true;
    }
    return requested->names && cupsArrayFind(requested->names, (void *)name) != NULL;
}

/* --- URIs ------------------------------------------------------------------------- */

static void make_uri(const Client *client, const char *scheme, const char *resource, char *uri, size_t size) {
    httpAssembleURI(HTTP_URI_CODING_ALL, uri, (int)size, scheme, NULL, client->uri_host, client->uri_port,
                    resource);
}

/* The printer (and job id) a request resource names, or NULL. */
static Printer *find_printer(P1600AirServer *server, const char *resource, int *job_id) {
    if (job_id) *job_id = 0;
    if (!resource) return NULL;
    char path[1024];
    copy_text(path, sizeof(path), resource, sizeof(path) - 1U);
    char *query = strchr(path, '?');
    if (query) *query = '\0';
    size_t length = strlen(path);
    while (length > 1 && path[length - 1] == '/') path[--length] = '\0';
    if (strncmp(path, "/ipp/print", 10) != 0) return NULL;
    const char *rest = path + 10;
    if (*rest == '\0') return server->printer_count == 1 ? &server->printers[0] : NULL;
    if (*rest != '/') return NULL;
    rest++;
    for (int i = 0; i < server->printer_count; i++) {
        Printer *printer = &server->printers[i];
        size_t name_length = strlen(printer->name);
        if (strncasecmp(rest, printer->name, name_length) != 0) continue;
        if (rest[name_length] == '\0') return printer;
        if (rest[name_length] != '/' || !job_id) continue;
        const char *digits = rest + name_length + 1;
        char *end = NULL;
        long id = strtol(digits, &end, 10);
        if (end != digits && *end == '\0' && id > 0 && id <= INT_MAX && digits[0] != '-' && digits[0] != '+') {
            *job_id = (int)id;
            return printer;
        }
    }
    if (server->printer_count == 1 && job_id) {
        char *end = NULL;
        long id = strtol(rest, &end, 10);
        if (end != rest && *end == '\0' && id > 0 && id <= INT_MAX && rest[0] != '-' && rest[0] != '+') {
            *job_id = (int)id;
            return &server->printers[0];
        }
    }
    return NULL;
}

/* A Host header value: host name or address and port; false when unusable. */
static bool parse_host(const char *value, char *host, size_t host_size, int *port) {
    if (!value || !*value || strlen(value) >= host_size) return false;
    const char *port_text = NULL;
    if (*value == '[') {
        const char *end = strchr(value, ']');
        if (!end || end == value + 1) return false;
        size_t length = (size_t)(end - value - 1);
        for (const char *p = value + 1; p < end; p++) {
            if (!(isxdigit((unsigned char)*p) || *p == ':' || *p == '.')) return false;
        }
        memcpy(host, value + 1, length);
        host[length] = '\0';
        if (end[1] == ':') {
            port_text = end + 2;
        } else if (end[1] != '\0') {
            return false;
        }
    } else {
        const char *colon = strchr(value, ':');
        size_t length = colon ? (size_t)(colon - value) : strlen(value);
        if (length == 0) return false;
        for (size_t i = 0; i < length; i++) {
            char c = value[i];
            if (!(isalnum((unsigned char)c) || c == '-' || c == '.' || c == '_')) return false;
        }
        memcpy(host, value, length);
        host[length] = '\0';
        if (colon) port_text = colon + 1;
    }
    if (port_text) {
        char *end = NULL;
        long number = strtol(port_text, &end, 10);
        if (end == port_text || *end || number < 1 || number > 65535) return false;
        *port = (int)number;
    }
    return true;
}

/* --- printer state ---------------------------------------------------------------- */

typedef struct {
    ipp_pstate_t state;
    const char *reasons[32];
    int reason_count;
    char message[256];
    bool accepting;
    char location[256];
    const P1600AirMedia *media_default;
} PrinterState;

/* Under the lock. */
static void printer_state(P1600AirServer *server, Printer *printer, PrinterState *out) {
    memset(out, 0, sizeof(*out));
    out->state = IPP_PSTATE_IDLE;
    out->accepting = true;
    copy_text(out->location, sizeof(out->location), printer->location, 127);
    out->media_default = p1600_air_media_by_ppd("A4");
    if (!server->config.mirror_cups) {
        out->reasons[out->reason_count++] = "none";
        return;
    }
    if (!printer->mirror) {
        out->state = IPP_PSTATE_STOPPED;
        out->reasons[out->reason_count++] = "other-error";
        copy_text(out->message, sizeof(out->message), "The CUPS queue is not available.", 255);
        return;
    }
    ipp_attribute_t *attr;
    if ((attr = ippFindAttribute(printer->mirror, "printer-state", IPP_TAG_ENUM)) != NULL) {
        int state = ippGetInteger(attr, 0);
        if (state >= IPP_PSTATE_IDLE && state <= IPP_PSTATE_STOPPED) out->state = (ipp_pstate_t)state;
    }
    if ((attr = ippFindAttribute(printer->mirror, "printer-state-reasons", IPP_TAG_KEYWORD)) != NULL) {
        for (int i = 0; i < ippGetCount(attr) && out->reason_count < (int)COUNT(out->reasons); i++) {
            const char *reason = ippGetString(attr, i, NULL);
            if (is_keyword(reason)) out->reasons[out->reason_count++] = reason;
        }
    }
    if (out->reason_count == 0) out->reasons[out->reason_count++] = "none";
    if ((attr = ippFindAttribute(printer->mirror, "printer-state-message", IPP_TAG_TEXT)) != NULL) {
        copy_text(out->message, sizeof(out->message), ippGetString(attr, 0, NULL), 255);
    }
    if ((attr = ippFindAttribute(printer->mirror, "printer-is-accepting-jobs", IPP_TAG_BOOLEAN)) != NULL) {
        out->accepting = ippGetBoolean(attr, 0) != 0;
    }
    if ((attr = ippFindAttribute(printer->mirror, "printer-location", IPP_TAG_TEXT)) != NULL) {
        copy_text(out->location, sizeof(out->location), ippGetString(attr, 0, NULL), 127);
    }
    if ((attr = ippFindAttribute(printer->mirror, "media-default", IPP_TAG_ZERO)) != NULL) {
        const P1600AirMedia *media = p1600_air_media_by_name(ippGetString(attr, 0, NULL));
        if (media) out->media_default = media;
    }
}

/* Asks cupsd for the queue's state when the cache is older than 2 s. */
static void refresh_printer(P1600AirServer *server, Printer *printer, bool force) {
    if (!server->config.mirror_cups) return;
    time_t now = time(NULL);
    pthread_mutex_lock(&server->lock);
    bool fresh = printer->refreshed && now - printer->refreshed < P1600_AIR_STATE_CACHE_SECONDS;
    if (printer->refreshing || (!force && fresh)) {
        pthread_mutex_unlock(&server->lock);
        return;
    }
    printer->refreshing = true;
    pthread_mutex_unlock(&server->lock);

    ipp_t *attrs = p1600_air_cups_printer(&server->config.cups, printer->name);

    pthread_mutex_lock(&server->lock);
    if (!attrs && !printer->mirror_failed) p1600_air_log("CUPS queue \"%s\" did not answer", printer->name);
    printer->mirror_failed = attrs == NULL;
    ippDelete(printer->mirror);
    printer->mirror = attrs;
    PrinterState state;
    printer_state(server, printer, &state);
    char key[sizeof(printer->state_key)];
    int used = snprintf(key, sizeof(key), "%d", (int)state.state);
    for (int i = 0; i < state.reason_count && used > 0 && (size_t)used < sizeof(key); i++) {
        used += snprintf(key + used, sizeof(key) - (size_t)used, ",%s", state.reasons[i]);
    }
    if (strcmp(key, printer->state_key) != 0) {
        snprintf(printer->state_key, sizeof(printer->state_key), "%s", key);
        if (printer->refreshed) printer->state_time = time(NULL);
    }
    copy_text(printer->location, sizeof(printer->location), state.location, 127);
    printer->refreshed = time(NULL);
    printer->refreshing = false;
    pthread_mutex_unlock(&server->lock);
}

/* --- printer attributes -------------------------------------------------------------- */

static ipp_t *media_size_col(const P1600AirMedia *media) {
    ipp_t *size = ippNew();
    ippAddInteger(size, IPP_TAG_ZERO, IPP_TAG_INTEGER, "x-dimension", media->width);
    ippAddInteger(size, IPP_TAG_ZERO, IPP_TAG_INTEGER, "y-dimension", media->length);
    return size;
}

static ipp_t *media_col(const P1600AirMedia *media, const char *source, const char *type) {
    ipp_t *col = ippNew();
    ipp_t *size = media_size_col(media);
    ippAddInteger(col, IPP_TAG_ZERO, IPP_TAG_INTEGER, "media-bottom-margin", P1600_AIR_MARGIN);
    ippAddInteger(col, IPP_TAG_ZERO, IPP_TAG_INTEGER, "media-left-margin", P1600_AIR_MARGIN);
    ippAddInteger(col, IPP_TAG_ZERO, IPP_TAG_INTEGER, "media-right-margin", P1600_AIR_MARGIN);
    ippAddCollection(col, IPP_TAG_ZERO, "media-size", size);
    ippDelete(size);
    if (source) ippAddString(col, IPP_TAG_ZERO, IPP_TAG_KEYWORD, "media-source", NULL, source);
    ippAddInteger(col, IPP_TAG_ZERO, IPP_TAG_INTEGER, "media-top-margin", P1600_AIR_MARGIN);
    if (type) ippAddString(col, IPP_TAG_ZERO, IPP_TAG_KEYWORD, "media-type", NULL, type);
    return col;
}

static void add_collections(ipp_t *out, const char *name, ipp_t **cols, int count) {
    ipp_attribute_t *attr = NULL;
    for (int i = 0; i < count; i++) {
        if (!attr) {
            attr = ippAddCollection(out, IPP_TAG_PRINTER, name, cols[i]);
        } else {
            ippSetCollection(out, &attr, i, cols[i]);
        }
        ippDelete(cols[i]);
    }
}

static void add_keywords(ipp_t *out, const char *name, const char *const *values, size_t count) {
    ippAddStrings(out, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, name, (int)count, NULL, values);
}

/* printer-supply for the queue's marker-* attributes, or the toner at an
 * unknown level (-2) when the queue has none. */
static void add_supplies(ipp_t *out, Printer *printer, const Requested *requested) {
    ipp_attribute_t *names = printer->mirror ? ippFindAttribute(printer->mirror, "marker-names", IPP_TAG_ZERO) : NULL;
    ipp_attribute_t *levels = printer->mirror ? ippFindAttribute(printer->mirror, "marker-levels", IPP_TAG_INTEGER)
                                              : NULL;
    ipp_attribute_t *types = printer->mirror ? ippFindAttribute(printer->mirror, "marker-types", IPP_TAG_KEYWORD)
                                             : NULL;
    ipp_attribute_t *colors = printer->mirror ? ippFindAttribute(printer->mirror, "marker-colors", IPP_TAG_ZERO)
                                              : NULL;
    int count = names && levels ? ippGetCount(names) : 0;
    if (levels && ippGetCount(levels) < count) count = ippGetCount(levels);
    if (count > 16) count = 16;
    ipp_attribute_t *supply = NULL, *description = NULL;
    for (int i = 0; i < (count > 0 ? count : 1); i++) {
        char type[64] = "toner", colorant[32] = "black", name[256] = "Black Toner", value[256];
        int level = -2;
        if (count > 0) {
            level = ippGetInteger(levels, i);
            copy_text(name, sizeof(name), ippGetString(names, i, NULL), 255);
            const char *marker_type = types && i < ippGetCount(types) ? ippGetString(types, i, NULL) : NULL;
            if (marker_type && is_keyword(marker_type)) copy_text(type, sizeof(type), marker_type, 63);
            const char *color = colors && i < ippGetCount(colors) ? ippGetString(colors, i, NULL) : NULL;
            if (color && strcasecmp(color, "#000000") != 0) snprintf(colorant, sizeof(colorant), "unknown");
            /* The Printer MIB has no "toner-cartridge" supply type. */
            if (strcmp(type, "toner-cartridge") == 0) snprintf(type, sizeof(type), "toner");
        }
        int length = snprintf(value, sizeof(value),
                              "index=%d;class=supplyThatIsConsumed;type=%s;unit=percent;maxcapacity=100;level=%d;"
                              "colorantname=%s;",
                              i + 1, type, level, colorant);
        if (length < 0 || (size_t)length >= sizeof(value)) continue;
        if (want(requested, "printer-supply", CLASS_DESCRIPTION)) {
            if (!supply) {
                supply = ippAddOctetString(out, IPP_TAG_PRINTER, "printer-supply", value, length);
            } else {
                ippSetOctetString(out, &supply, ippGetCount(supply), value, length);
            }
        }
        if (want(requested, "printer-supply-description", CLASS_DESCRIPTION)) {
            if (!description) {
                description =
                    ippAddString(out, IPP_TAG_PRINTER, IPP_TAG_TEXT, "printer-supply-description", NULL, name);
            } else {
                ippSetString(out, &description, ippGetCount(description), name);
            }
        }
    }
    if (printer->mirror && count > 0) {
        static const char *const markers[] = {"marker-colors", "marker-high-levels", "marker-levels",
                                              "marker-low-levels", "marker-names", "marker-types"};
        for (size_t i = 0; i < COUNT(markers); i++) {
            ipp_attribute_t *attr;
            if (want(requested, markers[i], CLASS_DESCRIPTION) &&
                (attr = ippFindAttribute(printer->mirror, markers[i], IPP_TAG_ZERO)) != NULL) {
                ipp_attribute_t *copy = ippCopyAttribute(out, attr, 0);
                if (copy) ippSetGroupTag(out, &copy, IPP_TAG_PRINTER);
            }
        }
    }
}

static int queued_jobs(P1600AirServer *server, const Printer *printer) {
    int count = 0;
    for (P1600AirJob *job = server->jobs; job; job = job->next) {
        if (job->printer == printer->index && job_active(job)) count++;
    }
    return count;
}

/* Under the lock. */
static void add_printer_attributes(P1600AirServer *server, Printer *printer, const Client *client, ipp_t *out,
                                   const Requested *r) {
    const ipp_tag_t G = IPP_TAG_PRINTER;
    const int D = CLASS_DESCRIPTION, T = CLASS_TEMPLATE;
    char printer_uri[HTTP_MAX_URI], root_uri[HTTP_MAX_URI], icon_uri[HTTP_MAX_URI];
    make_uri(client, "ipp", printer->resource, printer_uri, sizeof(printer_uri));
    make_uri(client, "http", "/", root_uri, sizeof(root_uri));
    make_uri(client, "http", "/icon.png", icon_uri, sizeof(icon_uri));
    PrinterState state;
    printer_state(server, printer, &state);
    const P1600AirMedia *media_default = state.media_default;
    time_t now = time(NULL);

    /* Job template attributes. */
    if (want(r, "copies-default", T)) ippAddInteger(out, G, IPP_TAG_INTEGER, "copies-default", 1);
    if (want(r, "copies-supported", T)) ippAddRange(out, G, "copies-supported", 1, 999);
    if (want(r, "finishings-default", T)) {
        ippAddInteger(out, G, IPP_TAG_ENUM, "finishings-default", IPP_FINISHINGS_NONE);
    }
    if (want(r, "finishings-supported", T)) {
        ippAddInteger(out, G, IPP_TAG_ENUM, "finishings-supported", IPP_FINISHINGS_NONE);
    }
    if (want(r, "job-hold-until-default", T)) {
        ippAddString(out, G, IPP_TAG_KEYWORD, "job-hold-until-default", NULL, "no-hold");
    }
    if (want(r, "job-hold-until-supported", T)) {
        ippAddString(out, G, IPP_TAG_KEYWORD, "job-hold-until-supported", NULL, "no-hold");
    }
    if (want(r, "job-sheets-default", T)) ippAddString(out, G, IPP_TAG_KEYWORD, "job-sheets-default", NULL, "none");
    if (want(r, "job-sheets-supported", T)) ippAddString(out, G, IPP_TAG_KEYWORD, "job-sheets-supported", NULL, "none");
    if (want(r, "media-default", T)) ippAddString(out, G, IPP_TAG_KEYWORD, "media-default", NULL, media_default->pwg);
    if (want(r, "media-ready", T)) ippAddString(out, G, IPP_TAG_KEYWORD, "media-ready", NULL, media_default->pwg);
    if (want(r, "media-supported", T)) {
        const char *names[COUNT(p1600_air_media)];
        for (size_t i = 0; i < p1600_air_media_count; i++) names[i] = p1600_air_media[i].pwg;
        add_keywords(out, "media-supported", names, p1600_air_media_count);
    }
    if (want(r, "media-col-default", T)) {
        ipp_t *col = media_col(media_default, "auto", "stationery");
        ippAddCollection(out, G, "media-col-default", col);
        ippDelete(col);
    }
    if (want(r, "media-col-ready", T)) {
        ipp_t *cols[1] = {media_col(media_default, "auto", "stationery")};
        add_collections(out, "media-col-ready", cols, 1);
    }
    if (want(r, "media-col-supported", T)) {
        static const char *const members[] = {"media-bottom-margin", "media-left-margin", "media-right-margin",
                                              "media-size",          "media-size-name",   "media-source",
                                              "media-top-margin",    "media-type"};
        add_keywords(out, "media-col-supported", members, COUNT(members));
    }
    if (want(r, "media-size-supported", T)) {
        ipp_t *cols[COUNT(p1600_air_media)];
        for (size_t i = 0; i < p1600_air_media_count; i++) cols[i] = media_size_col(&p1600_air_media[i]);
        add_collections(out, "media-size-supported", cols, (int)p1600_air_media_count);
    }
    if (want(r, "media-source-supported", T)) {
        static const char *const sources[] = {"auto", "manual"};
        add_keywords(out, "media-source-supported", sources, COUNT(sources));
    }
    if (want(r, "media-type-supported", T)) {
        const char *types[COUNT(p1600_air_media_types)];
        for (size_t i = 0; i < p1600_air_media_type_count; i++) types[i] = p1600_air_media_types[i].ipp;
        add_keywords(out, "media-type-supported", types, p1600_air_media_type_count);
    }
    static const char *const margins[] = {"media-bottom-margin-supported", "media-left-margin-supported",
                                          "media-right-margin-supported", "media-top-margin-supported"};
    for (size_t i = 0; i < COUNT(margins); i++) {
        if (want(r, margins[i], T)) ippAddInteger(out, G, IPP_TAG_INTEGER, margins[i], P1600_AIR_MARGIN);
    }
    if (want(r, "media-col-database", CLASS_EXPLICIT)) {
        ipp_t *cols[COUNT(p1600_air_media)];
        for (size_t i = 0; i < p1600_air_media_count; i++) cols[i] = media_col(&p1600_air_media[i], NULL, NULL);
        add_collections(out, "media-col-database", cols, (int)p1600_air_media_count);
    }
    if (want(r, "number-up-default", T)) ippAddInteger(out, G, IPP_TAG_INTEGER, "number-up-default", 1);
    if (want(r, "number-up-supported", T)) ippAddInteger(out, G, IPP_TAG_INTEGER, "number-up-supported", 1);
    if (want(r, "orientation-requested-default", T)) {
        ippAddInteger(out, G, IPP_TAG_ENUM, "orientation-requested-default", IPP_ORIENT_PORTRAIT);
    }
    if (want(r, "orientation-requested-supported", T)) {
        static const int orientations[] = {IPP_ORIENT_PORTRAIT, IPP_ORIENT_LANDSCAPE, IPP_ORIENT_REVERSE_LANDSCAPE,
                                           IPP_ORIENT_REVERSE_PORTRAIT};
        ippAddIntegers(out, G, IPP_TAG_ENUM, "orientation-requested-supported", (int)COUNT(orientations), orientations);
    }
    if (want(r, "output-bin-default", T)) {
        ippAddString(out, G, IPP_TAG_KEYWORD, "output-bin-default", NULL, "face-down");
    }
    if (want(r, "output-bin-supported", T)) {
        ippAddString(out, G, IPP_TAG_KEYWORD, "output-bin-supported", NULL, "face-down");
    }
    if (want(r, "overrides-supported", T)) {
        static const char *const overrides[] = {"document-number", "pages"};
        add_keywords(out, "overrides-supported", overrides, COUNT(overrides));
    }
    if (want(r, "page-ranges-supported", T)) ippAddBoolean(out, G, "page-ranges-supported", 1);
    if (want(r, "print-color-mode-default", T)) {
        ippAddString(out, G, IPP_TAG_KEYWORD, "print-color-mode-default", NULL, "monochrome");
    }
    if (want(r, "print-color-mode-supported", T)) {
        add_keywords(out, "print-color-mode-supported", color_modes, COUNT(color_modes));
    }
    if (want(r, "print-content-optimize-default", T)) {
        ippAddString(out, G, IPP_TAG_KEYWORD, "print-content-optimize-default", NULL, "auto");
    }
    if (want(r, "print-content-optimize-supported", T)) {
        ippAddString(out, G, IPP_TAG_KEYWORD, "print-content-optimize-supported", NULL, "auto");
    }
    if (want(r, "print-quality-default", T)) {
        ippAddInteger(out, G, IPP_TAG_ENUM, "print-quality-default", IPP_QUALITY_HIGH);
    }
    if (want(r, "print-quality-supported", T)) {
        static const int qualities[] = {IPP_QUALITY_NORMAL, IPP_QUALITY_HIGH};
        ippAddIntegers(out, G, IPP_TAG_ENUM, "print-quality-supported", (int)COUNT(qualities), qualities);
    }
    if (want(r, "print-rendering-intent-default", T)) {
        ippAddString(out, G, IPP_TAG_KEYWORD, "print-rendering-intent-default", NULL, "auto");
    }
    if (want(r, "print-rendering-intent-supported", T)) {
        ippAddString(out, G, IPP_TAG_KEYWORD, "print-rendering-intent-supported", NULL, "auto");
    }
    if (want(r, "print-scaling-default", T)) {
        ippAddString(out, G, IPP_TAG_KEYWORD, "print-scaling-default", NULL, "auto");
    }
    if (want(r, "print-scaling-supported", T)) {
        add_keywords(out, "print-scaling-supported", scaling_supported, COUNT(scaling_supported));
    }
    if (want(r, "printer-resolution-default", T)) {
        ippAddResolution(out, G, "printer-resolution-default", IPP_RES_PER_INCH, 600, 600);
    }
    if (want(r, "printer-resolution-supported", T)) {
        ippAddResolution(out, G, "printer-resolution-supported", IPP_RES_PER_INCH, 600, 600);
    }
    if (want(r, "sides-default", T)) ippAddString(out, G, IPP_TAG_KEYWORD, "sides-default", NULL, "one-sided");
    if (want(r, "sides-supported", T)) add_keywords(out, "sides-supported", sides_supported, COUNT(sides_supported));

    /* Printer description attributes. */
    if (want(r, "charset-configured", D)) ippAddString(out, G, IPP_TAG_CHARSET, "charset-configured", NULL, "utf-8");
    if (want(r, "charset-supported", D)) {
        static const char *const charsets[] = {"us-ascii", "utf-8"};
        ippAddStrings(out, G, IPP_TAG_CHARSET, "charset-supported", (int)COUNT(charsets), NULL, charsets);
    }
    if (want(r, "color-supported", D)) ippAddBoolean(out, G, "color-supported", 0);
    if (want(r, "compression-supported", D)) {
        static const char *const compressions[] = {"deflate", "gzip", "none"};
        add_keywords(out, "compression-supported", compressions, COUNT(compressions));
    }
    if (want(r, "document-format-default", D)) {
        ippAddString(out, G, IPP_TAG_MIMETYPE, "document-format-default", NULL, "application/octet-stream");
    }
    if (want(r, "document-format-supported", D)) {
        ippAddStrings(out, G, IPP_TAG_MIMETYPE, "document-format-supported", (int)COUNT(document_formats), NULL,
                      document_formats);
    }
    if (want(r, "generated-natural-language-supported", D)) {
        ippAddString(out, G, IPP_TAG_LANGUAGE, "generated-natural-language-supported", NULL, "en");
    }
    if (want(r, "identify-actions-default", D)) {
        ippAddString(out, G, IPP_TAG_KEYWORD, "identify-actions-default", NULL, "flash");
    }
    if (want(r, "identify-actions-supported", D)) {
        ippAddString(out, G, IPP_TAG_KEYWORD, "identify-actions-supported", NULL, "flash");
    }
    if (want(r, "ipp-features-supported", D)) {
        ippAddString(out, G, IPP_TAG_KEYWORD, "ipp-features-supported", NULL, "ipp-everywhere");
    }
    if (want(r, "ipp-versions-supported", D)) {
        static const char *const versions[] = {"1.1", "2.0"};
        add_keywords(out, "ipp-versions-supported", versions, COUNT(versions));
    }
    if (want(r, "job-creation-attributes-supported", D)) {
        static const char *const creation[] = {
            "copies", "finishings", "ipp-attribute-fidelity", "job-hold-until", "job-name", "job-sheets",
            "media", "media-col", "number-up", "orientation-requested", "output-bin", "overrides", "page-ranges",
            "print-color-mode", "print-content-optimize", "print-quality", "print-rendering-intent",
            "print-scaling", "printer-resolution", "sides"};
        add_keywords(out, "job-creation-attributes-supported", creation, COUNT(creation));
    }
    if (want(r, "job-ids-supported", D)) ippAddBoolean(out, G, "job-ids-supported", 1);
    if (want(r, "jpeg-k-octets-supported", D)) {
        ippAddRange(out, G, "jpeg-k-octets-supported", 0, (int)(P1600_AIR_DOCUMENT_LIMIT / 1024U));
    }
    if (want(r, "pdf-k-octets-supported", D)) {
        ippAddRange(out, G, "pdf-k-octets-supported", 0, (int)(P1600_AIR_DOCUMENT_LIMIT / 1024U));
    }
    if (want(r, "pdf-versions-supported", D)) {
        static const char *const versions[] = {"adobe-1.3", "adobe-1.4", "adobe-1.5", "adobe-1.6", "adobe-1.7",
                                               "iso-32000-1_2008"};
        add_keywords(out, "pdf-versions-supported", versions, COUNT(versions));
    }
    if (want(r, "multiple-document-jobs-supported", D)) ippAddBoolean(out, G, "multiple-document-jobs-supported", 0);
    if (want(r, "multiple-operation-time-out", D)) {
        ippAddInteger(out, G, IPP_TAG_INTEGER, "multiple-operation-time-out", P1600_AIR_MULTIPLE_OPERATION_TIMEOUT);
    }
    if (want(r, "multiple-operation-time-out-action", D)) {
        ippAddString(out, G, IPP_TAG_KEYWORD, "multiple-operation-time-out-action", NULL, "abort-job");
    }
    if (want(r, "natural-language-configured", D)) {
        ippAddString(out, G, IPP_TAG_LANGUAGE, "natural-language-configured", NULL, "en");
    }
    if (want(r, "operations-supported", D)) {
        static const int operations[] = {
            IPP_OP_PRINT_JOB, IPP_OP_VALIDATE_JOB, IPP_OP_CREATE_JOB, IPP_OP_SEND_DOCUMENT, IPP_OP_CANCEL_JOB,
            IPP_OP_GET_JOB_ATTRIBUTES, IPP_OP_GET_JOBS, IPP_OP_GET_PRINTER_ATTRIBUTES, IPP_OP_CANCEL_MY_JOBS,
            IPP_OP_CLOSE_JOB, IPP_OP_IDENTIFY_PRINTER};
        ippAddIntegers(out, G, IPP_TAG_ENUM, "operations-supported", (int)COUNT(operations), operations);
    }
    if (want(r, "pages-per-minute", D)) ippAddInteger(out, G, IPP_TAG_INTEGER, "pages-per-minute", 25);
    if (want(r, "pdl-override-supported", D)) {
        ippAddString(out, G, IPP_TAG_KEYWORD, "pdl-override-supported", NULL, "attempted");
    }
    if (want(r, "preferred-attributes-supported", D)) ippAddBoolean(out, G, "preferred-attributes-supported", 0);
    if (want(r, "printer-config-change-date-time", D)) {
        ippAddDate(out, G, "printer-config-change-date-time", ippTimeToDate(server->start_time));
    }
    if (want(r, "printer-config-change-time", D)) {
        ippAddInteger(out, G, IPP_TAG_INTEGER, "printer-config-change-time", up_time(server, server->start_time));
    }
    if (want(r, "printer-current-time", D)) ippAddDate(out, G, "printer-current-time", ippTimeToDate(now));
    if (want(r, "printer-device-id", D)) {
        ippAddString(out, G, IPP_TAG_TEXT, "printer-device-id", NULL, printer->device_id);
    }
    if (want(r, "printer-dns-sd-name", D)) {
        ippAddString(out, G, IPP_TAG_NAME, "printer-dns-sd-name", NULL,
                     printer->dns_sd_name[0] ? printer->dns_sd_name : printer->service_name);
    }
    if (want(r, "printer-geo-location", D)) ippAddOutOfBand(out, G, IPP_TAG_UNKNOWN, "printer-geo-location");
    if (want(r, "printer-get-attributes-supported", D)) {
        ippAddString(out, G, IPP_TAG_KEYWORD, "printer-get-attributes-supported", NULL, "document-format");
    }
    if (want(r, "printer-icons", D)) ippAddString(out, G, IPP_TAG_URI, "printer-icons", NULL, icon_uri);
    if (want(r, "printer-info", D)) ippAddString(out, G, IPP_TAG_TEXT, "printer-info", NULL, printer->info);
    if (want(r, "printer-is-accepting-jobs", D)) ippAddBoolean(out, G, "printer-is-accepting-jobs", state.accepting);
    if (want(r, "printer-kind", D)) ippAddString(out, G, IPP_TAG_KEYWORD, "printer-kind", NULL, "document");
    if (want(r, "printer-location", D)) ippAddString(out, G, IPP_TAG_TEXT, "printer-location", NULL, state.location);
    if (want(r, "printer-make-and-model", D)) {
        ippAddString(out, G, IPP_TAG_TEXT, "printer-make-and-model", NULL, printer->make_model);
    }
    if (want(r, "printer-more-info", D)) ippAddString(out, G, IPP_TAG_URI, "printer-more-info", NULL, root_uri);
    if (want(r, "printer-name", D)) ippAddString(out, G, IPP_TAG_NAME, "printer-name", NULL, printer->name);
    if (want(r, "printer-organization", D)) ippAddString(out, G, IPP_TAG_TEXT, "printer-organization", NULL, "");
    if (want(r, "printer-organizational-unit", D)) {
        ippAddString(out, G, IPP_TAG_TEXT, "printer-organizational-unit", NULL, "");
    }
    if (want(r, "printer-state", D)) ippAddInteger(out, G, IPP_TAG_ENUM, "printer-state", (int)state.state);
    time_t state_time = printer->state_time ? printer->state_time : server->start_time;
    if (want(r, "printer-state-change-date-time", D)) {
        ippAddDate(out, G, "printer-state-change-date-time", ippTimeToDate(state_time));
    }
    if (want(r, "printer-state-change-time", D)) {
        ippAddInteger(out, G, IPP_TAG_INTEGER, "printer-state-change-time", up_time(server, state_time));
    }
    if (want(r, "printer-state-message", D)) {
        ippAddString(out, G, IPP_TAG_TEXT, "printer-state-message", NULL, state.message);
    }
    if (want(r, "printer-state-reasons", D)) {
        add_keywords(out, "printer-state-reasons", state.reasons, (size_t)state.reason_count);
    }
    add_supplies(out, printer, r);
    if (want(r, "printer-supply-info-uri", D)) {
        ippAddString(out, G, IPP_TAG_URI, "printer-supply-info-uri", NULL, root_uri);
    }
    if (want(r, "printer-up-time", D)) ippAddInteger(out, G, IPP_TAG_INTEGER, "printer-up-time", up_time(server, now));
    if (want(r, "printer-uri-supported", D)) {
        ippAddString(out, G, IPP_TAG_URI, "printer-uri-supported", NULL, printer_uri);
    }
    if (want(r, "printer-uuid", D)) ippAddString(out, G, IPP_TAG_URI, "printer-uuid", NULL, printer->uuid);
    if (want(r, "pwg-raster-document-resolution-supported", D)) {
        static const int resolutions[] = {300, 600};
        ippAddResolutions(out, G, "pwg-raster-document-resolution-supported", (int)COUNT(resolutions),
                          IPP_RES_PER_INCH, resolutions, resolutions);
    }
    if (want(r, "pwg-raster-document-sheet-back", D)) {
        ippAddString(out, G, IPP_TAG_KEYWORD, "pwg-raster-document-sheet-back", NULL, "normal");
    }
    if (want(r, "pwg-raster-document-type-supported", D)) {
        static const char *const types[] = {"sgray_8", "srgb_8"};
        add_keywords(out, "pwg-raster-document-type-supported", types, COUNT(types));
    }
    if (want(r, "queued-job-count", D)) {
        ippAddInteger(out, G, IPP_TAG_INTEGER, "queued-job-count", queued_jobs(server, printer));
    }
    if (want(r, "uri-authentication-supported", D)) {
        ippAddString(out, G, IPP_TAG_KEYWORD, "uri-authentication-supported", NULL, "requesting-user-name");
    }
    if (want(r, "uri-security-supported", D)) {
        ippAddString(out, G, IPP_TAG_KEYWORD, "uri-security-supported", NULL, "none");
    }
    if (want(r, "urf-supported", D)) {
        char list[] = P1600_AIR_URF_SUPPORTED;
        const char *values[16];
        int count = 0;
        for (char *item = strtok(list, ","); item && count < 16; item = strtok(NULL, ",")) values[count++] = item;
        add_keywords(out, "urf-supported", values, (size_t)count);
    }
    if (want(r, "which-jobs-supported", D)) {
        add_keywords(out, "which-jobs-supported", which_jobs_supported, COUNT(which_jobs_supported));
    }
}

/* --- job attributes ---------------------------------------------------------------- */

static void add_time(ipp_t *out, P1600AirServer *server, const Requested *r, const char *time_name,
                     const char *date_name, time_t when) {
    if (want(r, time_name, CLASS_DESCRIPTION)) {
        if (when) {
            ippAddInteger(out, IPP_TAG_JOB, IPP_TAG_INTEGER, time_name, up_time(server, when));
        } else {
            ippAddOutOfBand(out, IPP_TAG_JOB, IPP_TAG_NOVALUE, time_name);
        }
    }
    if (want(r, date_name, CLASS_DESCRIPTION)) {
        if (when) {
            ippAddDate(out, IPP_TAG_JOB, date_name, ippTimeToDate(when));
        } else {
            ippAddOutOfBand(out, IPP_TAG_JOB, IPP_TAG_NOVALUE, date_name);
        }
    }
}

static const char *job_reason(const P1600AirJob *job) {
    if (job->reason) return job->reason;
    switch (job->state) {
    case IPP_JSTATE_PENDING: return "job-queued";
    case IPP_JSTATE_HELD: return "job-incoming";
    case IPP_JSTATE_PROCESSING: return "job-printing";
    case IPP_JSTATE_STOPPED: return "job-stopped";
    case IPP_JSTATE_CANCELED: return "job-canceled-by-user";
    case IPP_JSTATE_ABORTED: return "aborted-by-system";
    default: return "job-completed-successfully";
    }
}

/* Under the lock. */
static void add_job_attributes(P1600AirServer *server, const Client *client, P1600AirJob *job, ipp_t *out,
                               const Requested *r) {
    const ipp_tag_t G = IPP_TAG_JOB;
    const int D = CLASS_DESCRIPTION;
    Printer *printer = &server->printers[job->printer];
    char printer_uri[HTTP_MAX_URI], job_uri[HTTP_MAX_URI], resource[400];
    make_uri(client, "ipp", printer->resource, printer_uri, sizeof(printer_uri));
    snprintf(resource, sizeof(resource), "%s/%d", printer->resource, job->id);
    make_uri(client, "ipp", resource, job_uri, sizeof(job_uri));

    if (want(r, "job-id", D)) ippAddInteger(out, G, IPP_TAG_INTEGER, "job-id", job->id);
    if (want(r, "job-uri", D)) ippAddString(out, G, IPP_TAG_URI, "job-uri", NULL, job_uri);
    if (want(r, "job-printer-uri", D)) ippAddString(out, G, IPP_TAG_URI, "job-printer-uri", NULL, printer_uri);
    if (want(r, "job-name", D)) ippAddString(out, G, IPP_TAG_NAME, "job-name", NULL, job->name);
    if (want(r, "job-originating-user-name", D)) {
        ippAddString(out, G, IPP_TAG_NAME, "job-originating-user-name", NULL, job->user);
    }
    if (want(r, "job-state", D)) ippAddInteger(out, G, IPP_TAG_ENUM, "job-state", (int)job->state);
    if (want(r, "job-state-reasons", D)) {
        ippAddString(out, G, IPP_TAG_KEYWORD, "job-state-reasons", NULL, job_reason(job));
    }
    if (want(r, "job-state-message", D)) ippAddString(out, G, IPP_TAG_TEXT, "job-state-message", NULL, job->message);
    if (want(r, "job-printer-up-time", D)) {
        ippAddInteger(out, G, IPP_TAG_INTEGER, "job-printer-up-time", up_time(server, time(NULL)));
    }
    add_time(out, server, r, "time-at-creation", "date-time-at-creation", job->created);
    add_time(out, server, r, "time-at-processing", "date-time-at-processing", job->processing);
    add_time(out, server, r, "time-at-completed", "date-time-at-completed", job->completed);
    if (want(r, "job-k-octets", D)) {
        ippAddInteger(out, G, IPP_TAG_INTEGER, "job-k-octets", (int)((job->octets + 1023U) / 1024U));
    }
    if (want(r, "number-of-documents", D)) {
        ippAddInteger(out, G, IPP_TAG_INTEGER, "number-of-documents", job->has_document ? 1 : 0);
    }
    if (job->format_supplied[0] && want(r, "document-format-supplied", D)) {
        ippAddString(out, G, IPP_TAG_MIMETYPE, "document-format-supplied", NULL, job->format_supplied);
    }
    if (job->document_name[0] && want(r, "document-name-supplied", D)) {
        ippAddString(out, G, IPP_TAG_NAME, "document-name-supplied", NULL, job->document_name);
    }
    if (job->impressions > 0 && want(r, "job-impressions", D)) {
        ippAddInteger(out, G, IPP_TAG_INTEGER, "job-impressions", job->impressions);
    }
    if (job->attrs) {
        for (ipp_attribute_t *attr = ippFirstAttribute(job->attrs); attr; attr = ippNextAttribute(job->attrs)) {
            const char *name = ippGetName(attr);
            if (name && want(r, name, CLASS_TEMPLATE)) ippCopyAttribute(out, attr, 0);
        }
    }
}

/* --- IPP requests ------------------------------------------------------------------ */

typedef struct {
    Client *client;
    P1600AirServer *server;
    ipp_t *request;
    ipp_t *response;
    ipp_t *unsupported;
    Printer *printer;
    int job_id;
    char user[256];
    bool user_supplied;
    bool responded;
} Request;

/* Sets the status and message, then the unsupported attributes group;
 * the operation's own attributes follow. */
static void respond(Request *rq, ipp_status_t status, const char *format, ...) {
    if (rq->responded) return;
    rq->responded = true;
    ippSetStatusCode(rq->response, status);
    if (format) {
        char message[512];
        va_list arguments;
        va_start(arguments, format);
        vsnprintf(message, sizeof(message), format, arguments);
        va_end(arguments);
        ippAddString(rq->response, IPP_TAG_OPERATION, IPP_TAG_TEXT, "status-message", NULL, message);
    }
    for (ipp_attribute_t *attr = ippFirstAttribute(rq->unsupported); attr; attr = ippNextAttribute(rq->unsupported)) {
        ipp_attribute_t *copy = ippCopyAttribute(rq->response, attr, 0);
        if (copy) ippSetGroupTag(rq->response, &copy, IPP_TAG_UNSUPPORTED_GROUP);
    }
}

static void unsupported(Request *rq, ipp_attribute_t *attr) {
    if (attr) ippCopyAttribute(rq->unsupported, attr, 0);
}

static bool is_keyword_value(ipp_attribute_t *attr) {
    return ippGetValueTag(attr) == IPP_TAG_KEYWORD && ippGetCount(attr) == 1;
}

/* The operation attribute `name`, which must have the given syntax (any
 * name/keyword syntax for IPP_TAG_NAME). NULL when absent; `bad` when wrong. */
static ipp_attribute_t *operation_attribute(Request *rq, const char *name, ipp_tag_t tag, bool *bad) {
    ipp_attribute_t *attr = ippFindAttribute(rq->request, name, IPP_TAG_ZERO);
    if (!attr) return NULL;
    ipp_tag_t value_tag = ippGetValueTag(attr);
    bool ok = ippGetGroupTag(attr) == IPP_TAG_OPERATION && ippGetCount(attr) == 1;
    if (tag == IPP_TAG_NAME) {
        ok = ok && (value_tag == IPP_TAG_NAME || value_tag == IPP_TAG_NAMELANG);
    } else if (tag == IPP_TAG_TEXT) {
        ok = ok && (value_tag == IPP_TAG_TEXT || value_tag == IPP_TAG_TEXTLANG);
    } else {
        ok = ok && value_tag == tag;
    }
    if (!ok) *bad = true;
    return attr;
}

/* --- job template validation --------------------------------------------------- */

static bool valid_page_ranges(ipp_attribute_t *attr) {
    int count = ippGetCount(attr);
    if (ippGetValueTag(attr) != IPP_TAG_RANGE || count < 1 || count > MAX_RANGES) return false;
    int previous = 0;
    for (int i = 0; i < count; i++) {
        int upper = 0, lower = ippGetRange(attr, i, &upper);
        if (lower < 1 || upper < lower || lower <= previous) return false;
        previous = upper;
    }
    return true;
}

/* A media-col with the members this printer supports; `complete` is false
 * when members had to be left out. NULL when nothing usable remains. */
static ipp_t *sanitize_media_col(ipp_t *col, bool *complete) {
    *complete = true;
    if (!col) return NULL;
    ipp_t *out = ippNew();
    const P1600AirMedia *media = NULL;
    bool any = false;
    for (ipp_attribute_t *member = ippFirstAttribute(col); member; member = ippNextAttribute(col)) {
        const char *name = ippGetName(member);
        ipp_tag_t tag = ippGetValueTag(member);
        bool ok = false;
        if (!name) continue;
        if (strcmp(name, "media-size") == 0 && tag == IPP_TAG_BEGIN_COLLECTION && ippGetCount(member) == 1) {
            ipp_t *size = ippGetCollection(member, 0);
            ipp_attribute_t *x = NULL, *y = NULL;
            bool extra = false;
            for (ipp_attribute_t *dimension = ippFirstAttribute(size); dimension; dimension = ippNextAttribute(size)) {
                const char *dimension_name = ippGetName(dimension);
                bool integer = ippGetValueTag(dimension) == IPP_TAG_INTEGER && ippGetCount(dimension) == 1;
                if (dimension_name && integer && strcmp(dimension_name, "x-dimension") == 0) {
                    x = dimension;
                } else if (dimension_name && integer && strcmp(dimension_name, "y-dimension") == 0) {
                    y = dimension;
                } else {
                    extra = true;
                }
            }
            const P1600AirMedia *found = x && y && !extra ? p1600_air_media_by_size(ippGetInteger(x, 0), ippGetInteger(y, 0))
                                                       : NULL;
            if (found && (!media || media == found)) {
                media = found;
                ipp_t *canonical = media_size_col(found);
                ippAddCollection(out, IPP_TAG_ZERO, "media-size", canonical);
                ippDelete(canonical);
                ok = true;
            }
        } else if (strcmp(name, "media-size-name") == 0 && (tag == IPP_TAG_KEYWORD || tag == IPP_TAG_NAME) &&
                   ippGetCount(member) == 1) {
            const P1600AirMedia *found = p1600_air_media_by_name(ippGetString(member, 0, NULL));
            if (found && (!media || media == found)) {
                media = found;
                ippAddString(out, IPP_TAG_ZERO, IPP_TAG_KEYWORD, "media-size-name", NULL, found->pwg);
                ok = true;
            }
        } else if (strcmp(name, "media-source") == 0 && (tag == IPP_TAG_KEYWORD || tag == IPP_TAG_NAME) &&
                   ippGetCount(member) == 1 && p1600_air_media_source_ppd(ippGetString(member, 0, NULL))) {
            ippCopyAttribute(out, member, 0);
            ok = true;
        } else if (strcmp(name, "media-type") == 0 && (tag == IPP_TAG_KEYWORD || tag == IPP_TAG_NAME) &&
                   ippGetCount(member) == 1 && p1600_air_media_type_ppd(ippGetString(member, 0, NULL))) {
            ippCopyAttribute(out, member, 0);
            ok = true;
        } else if ((strcmp(name, "media-bottom-margin") == 0 || strcmp(name, "media-left-margin") == 0 ||
                    strcmp(name, "media-right-margin") == 0 || strcmp(name, "media-top-margin") == 0) &&
                   tag == IPP_TAG_INTEGER && ippGetCount(member) == 1 && ippGetInteger(member, 0) == P1600_AIR_MARGIN) {
            ippCopyAttribute(out, member, 0);
            ok = true;
        } else if (strcmp(name, "media-key") == 0) {
            ok = true; /* a name for the entry the client picked; nothing to apply */
        }
        if (ok) {
            any = true;
        } else {
            *complete = false;
        }
    }
    if (!any) {
        ippDelete(out);
        return NULL;
    }
    return out;
}

/* True when the job template attribute is supported as given; media-col
 * may come back sanitized in `replacement`. */
static bool valid_template(ipp_attribute_t *attr, ipp_t **replacement) {
    *replacement = NULL;
    const char *name = ippGetName(attr);
    ipp_tag_t tag = ippGetValueTag(attr);
    int count = ippGetCount(attr);
    if (!name) return false;
    if (strcmp(name, "copies") == 0) {
        return tag == IPP_TAG_INTEGER && count == 1 && ippGetInteger(attr, 0) >= 1 && ippGetInteger(attr, 0) <= 999;
    }
    if (strcmp(name, "sides") == 0) {
        return is_keyword_value(attr) && in_list(ippGetString(attr, 0, NULL), sides_supported, COUNT(sides_supported));
    }
    if (strcmp(name, "media") == 0) {
        return (tag == IPP_TAG_KEYWORD || tag == IPP_TAG_NAME || tag == IPP_TAG_NAMELANG) && count == 1 &&
               p1600_air_media_by_name(ippGetString(attr, 0, NULL)) != NULL;
    }
    if (strcmp(name, "media-col") == 0) {
        if (tag != IPP_TAG_BEGIN_COLLECTION || count != 1) return false;
        bool complete = false;
        *replacement = sanitize_media_col(ippGetCollection(attr, 0), &complete);
        return complete;
    }
    if (strcmp(name, "orientation-requested") == 0) {
        return tag == IPP_TAG_ENUM && count == 1 && ippGetInteger(attr, 0) >= IPP_ORIENT_PORTRAIT &&
               ippGetInteger(attr, 0) <= IPP_ORIENT_REVERSE_PORTRAIT;
    }
    if (strcmp(name, "output-bin") == 0) {
        return (tag == IPP_TAG_KEYWORD || tag == IPP_TAG_NAME) && count == 1 &&
               strcmp(ippGetString(attr, 0, NULL), "face-down") == 0;
    }
    if (strcmp(name, "page-ranges") == 0) return valid_page_ranges(attr);
    if (strcmp(name, "print-color-mode") == 0) {
        return is_keyword_value(attr) && in_list(ippGetString(attr, 0, NULL), color_modes, COUNT(color_modes));
    }
    if (strcmp(name, "print-content-optimize") == 0 || strcmp(name, "print-rendering-intent") == 0) {
        return is_keyword_value(attr) && strcmp(ippGetString(attr, 0, NULL), "auto") == 0;
    }
    if (strcmp(name, "print-quality") == 0) {
        return tag == IPP_TAG_ENUM && count == 1 &&
               (ippGetInteger(attr, 0) == IPP_QUALITY_NORMAL || ippGetInteger(attr, 0) == IPP_QUALITY_HIGH);
    }
    if (strcmp(name, "print-scaling") == 0) {
        return is_keyword_value(attr) &&
               in_list(ippGetString(attr, 0, NULL), scaling_supported, COUNT(scaling_supported));
    }
    if (strcmp(name, "printer-resolution") == 0) {
        if (tag != IPP_TAG_RESOLUTION || count != 1) return false;
        int y = 0;
        ipp_res_t units = IPP_RES_PER_INCH;
        int x = ippGetResolution(attr, 0, &y, &units);
        return units == IPP_RES_PER_INCH && x == 600 && y == 600;
    }
    if (strcmp(name, "finishings") == 0) {
        return tag == IPP_TAG_ENUM && count == 1 && ippGetInteger(attr, 0) == IPP_FINISHINGS_NONE;
    }
    if (strcmp(name, "job-sheets") == 0) {
        return (tag == IPP_TAG_KEYWORD || tag == IPP_TAG_NAME) && count == 1 &&
               strcmp(ippGetString(attr, 0, NULL), "none") == 0;
    }
    if (strcmp(name, "job-hold-until") == 0) {
        return (tag == IPP_TAG_KEYWORD || tag == IPP_TAG_NAME) && count == 1 &&
               strcmp(ippGetString(attr, 0, NULL), "no-hold") == 0;
    }
    if (strcmp(name, "number-up") == 0) return tag == IPP_TAG_INTEGER && count == 1 && ippGetInteger(attr, 0) == 1;
    /* Advertised in overrides-supported; accepted and printed without overrides. */
    if (strcmp(name, "overrides") == 0) return tag == IPP_TAG_BEGIN_COLLECTION;
    return false;
}

typedef struct {
    char job_name[256];
    char document_name[256];
    char format[64];
    char compression[16];
    bool fidelity;
    bool ignored;
    ipp_t *attrs;   /* accepted job template attributes */
} JobRequest;

static bool supported_format(const char *format) {
    return in_list(format, document_formats, COUNT(document_formats));
}

/* document-format and compression of a document operation. */
static bool validate_document(Request *rq, JobRequest *jr) {
    bool bad = false;
    ipp_attribute_t *format = operation_attribute(rq, "document-format", IPP_TAG_MIMETYPE, &bad);
    ipp_attribute_t *compression = operation_attribute(rq, "compression", IPP_TAG_KEYWORD, &bad);
    ipp_attribute_t *name = operation_attribute(rq, "document-name", IPP_TAG_NAME, &bad);
    if (bad) {
        respond(rq, IPP_STATUS_ERROR_BAD_REQUEST, "Bad document-format, compression or document-name attribute.");
        return false;
    }
    snprintf(jr->format, sizeof(jr->format), "%s", format ? ippGetString(format, 0, NULL) : "application/octet-stream");
    if (!supported_format(jr->format)) {
        unsupported(rq, format);
        respond(rq, IPP_STATUS_ERROR_DOCUMENT_FORMAT_NOT_SUPPORTED, "Unsupported document format \"%s\".", jr->format);
        return false;
    }
    snprintf(jr->compression, sizeof(jr->compression), "%s",
             compression ? ippGetString(compression, 0, NULL) : "none");
    if (strcmp(jr->compression, "none") != 0 && strcmp(jr->compression, "gzip") != 0 &&
        strcmp(jr->compression, "deflate") != 0) {
        unsupported(rq, compression);
        respond(rq, IPP_STATUS_ERROR_COMPRESSION_NOT_SUPPORTED, "Unsupported compression \"%s\".",
                ippGetString(compression, 0, NULL));
        return false;
    }
    if (name) copy_text(jr->document_name, sizeof(jr->document_name), ippGetString(name, 0, NULL), 255);
    return true;
}

/* The operation and job template attributes of Print-Job, Validate-Job and
 * Create-Job. False after an error response. */
static bool validate_job(Request *rq, JobRequest *jr, bool document) {
    memset(jr, 0, sizeof(*jr));
    bool bad = false;
    ipp_attribute_t *job_name = operation_attribute(rq, "job-name", IPP_TAG_NAME, &bad);
    ipp_attribute_t *fidelity = operation_attribute(rq, "ipp-attribute-fidelity", IPP_TAG_BOOLEAN, &bad);
    ipp_attribute_t *k_octets = operation_attribute(rq, "job-k-octets", IPP_TAG_INTEGER, &bad);
    ipp_attribute_t *impressions = operation_attribute(rq, "job-impressions", IPP_TAG_INTEGER, &bad);
    ipp_attribute_t *sheets = operation_attribute(rq, "job-media-sheets", IPP_TAG_INTEGER, &bad);
    if (bad || (impressions && ippGetInteger(impressions, 0) < 0) || (sheets && ippGetInteger(sheets, 0) < 0) ||
        (k_octets && ippGetInteger(k_octets, 0) < 0)) {
        respond(rq, IPP_STATUS_ERROR_BAD_REQUEST, "Bad job operation attribute.");
        return false;
    }
    if (k_octets && (size_t)ippGetInteger(k_octets, 0) > P1600_AIR_DOCUMENT_LIMIT / 1024U) {
        unsupported(rq, k_octets);
        respond(rq, IPP_STATUS_ERROR_REQUEST_ENTITY, "The document is larger than %zu MiB.",
                P1600_AIR_DOCUMENT_LIMIT / (1024U * 1024U));
        return false;
    }
    jr->fidelity = fidelity && ippGetBoolean(fidelity, 0);
    /* Create-Job may name the format the documents will have. */
    if (document || ippFindAttribute(rq->request, "document-format", IPP_TAG_ZERO) ||
        ippFindAttribute(rq->request, "compression", IPP_TAG_ZERO)) {
        if (!validate_document(rq, jr)) return false;
    } else {
        snprintf(jr->format, sizeof(jr->format), "application/octet-stream");
        snprintf(jr->compression, sizeof(jr->compression), "none");
    }
    copy_text(jr->job_name, sizeof(jr->job_name),
              job_name ? ippGetString(job_name, 0, NULL) : jr->document_name[0] ? jr->document_name : "Untitled", 255);
    if (!jr->job_name[0]) snprintf(jr->job_name, sizeof(jr->job_name), "Untitled");

    jr->attrs = ippNew();
    for (ipp_attribute_t *attr = ippFirstAttribute(rq->request); attr; attr = ippNextAttribute(rq->request)) {
        if (ippGetGroupTag(attr) != IPP_TAG_JOB || !ippGetName(attr)) continue;
        ipp_t *replacement = NULL;
        bool ok = valid_template(attr, &replacement);
        if (replacement) {
            ippAddCollection(jr->attrs, IPP_TAG_JOB, ippGetName(attr), replacement);
            ippDelete(replacement);
        } else if (ok) {
            ippCopyAttribute(jr->attrs, attr, 0);
        }
        if (!ok) {
            unsupported(rq, attr);
            jr->ignored = true;
        }
    }
    if (jr->ignored && jr->fidelity) {
        ippDelete(jr->attrs);
        jr->attrs = NULL;
        respond(rq, IPP_STATUS_ERROR_ATTRIBUTES_OR_VALUES, "Unsupported job attributes or values.");
        return false;
    }
    return true;
}

/* --- receiving documents ------------------------------------------------------------- */

static int write_all(int fd, const void *data, size_t length) {
    const unsigned char *bytes = data;
    while (length > 0) {
        ssize_t written = write(fd, bytes, length);
        if (written < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        bytes += written;
        length -= (size_t)written;
    }
    return 0;
}

typedef enum { CODING_NONE, CODING_GZIP, CODING_DEFLATE } Coding;

/* Reads the document that follows the IPP message into a private temporary
 * file, inflating gzip or deflate data, within the document limit. On
 * failure the error has been answered and false is returned. */
static bool receive_document(Request *rq, const char *compression, char *path, size_t path_size, size_t *octets) {
    Client *client = rq->client;
    P1600AirServer *server = rq->server;
    snprintf(path, path_size, "%s/doc-XXXXXX", server->temp_dir);
    int fd = mkstemp(path);
    if (fd < 0) {
        path[0] = '\0';
        respond(rq, IPP_STATUS_ERROR_INTERNAL, "Unable to store the document.");
        return false;
    }
    /* libcups already undid an HTTP Content-Encoding. */
    const char *http_coding = httpGetField(client->http, HTTP_FIELD_CONTENT_ENCODING);
    Coding coding = CODING_NONE;
    if (!http_coding || !*http_coding || strcasecmp(http_coding, "identity") == 0) {
        if (strcmp(compression, "gzip") == 0) coding = CODING_GZIP;
        if (strcmp(compression, "deflate") == 0) coding = CODING_DEFLATE;
    }
    VERBOSE("%s: receiving a document (compression %s, HTTP content coding %s)", client->peer, compression,
            http_coding && *http_coding ? http_coding : "none");
    unsigned char *input = malloc(IO_BUFFER_SIZE), *output = malloc(IO_BUFFER_SIZE);
    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    /* member_end: a gzip member ended with the data so far; another may follow. */
    bool inflating = false, stream_end = false, member_end = false;
    /* carry: deflate bytes kept until its framing can be told (two bytes). */
    size_t raw = 0, total = 0, carry = 0, reserved = 0;
    ipp_status_t status = IPP_STATUS_OK;
    const char *message = NULL;
    if (!input || !output) {
        status = IPP_STATUS_ERROR_INTERNAL;
        message = "Out of memory.";
    }
    while (status == IPP_STATUS_OK) {
        ssize_t count = httpRead2(client->http, (char *)input + carry, IO_BUFFER_SIZE - carry);
        if (count < 0) {
            status = IPP_STATUS_ERROR_BAD_REQUEST;
            message = "The document could not be read.";
            client->must_close = true;
            break;
        }
        if (count == 0) {
            /* libcups also ends the body with 0 when the client closed the
             * connection or stalled past the timeout: the body must have
             * arrived whole (its Content-Length, or chunked to the end). */
            if (httpGetState(client->http) == HTTP_STATE_POST_RECV || httpError(client->http) != 0 ||
                httpGetRemaining(client->http) > 0) {
                status = IPP_STATUS_ERROR_BAD_REQUEST;
                message = "The document is incomplete.";
                client->must_close = true;
            }
            break;
        }
        raw += (size_t)count;
        if (raw > P1600_AIR_DOCUMENT_LIMIT) {
            status = IPP_STATUS_ERROR_REQUEST_ENTITY;
            message = "The document is too large.";
            client->must_close = true;
            client->unread_body = true;
            break;
        }
        if (coding == CODING_NONE) {
            total += (size_t)count;
            if (write_all(fd, input, (size_t)count) != 0) {
                status = IPP_STATUS_ERROR_INTERNAL;
                message = "Unable to store the document.";
            } else if (!reserve_bytes(server, (size_t)count, &reserved)) {
                status = IPP_STATUS_ERROR_BUSY;
                message = "The printer is busy; try again later.";
                client->must_close = true;
                client->unread_body = true;
            }
            continue;
        }
        if (stream_end) continue;  /* bytes after the compressed data */
        size_t available = carry + (size_t)count;
        if (!inflating) {
            /* deflate: zlib framing (RFC 1950) or raw deflate (RFC 1951, as libcups writes). */
            int bits = 15 + 16;
            if (coding == CODING_DEFLATE) {
                /* Its first two bytes tell; they may come in two reads. */
                if (available < 2) {
                    carry = available;
                    continue;
                }
                bool zlib_header = (input[0] & 0x0fU) == 8U && (input[0] >> 4) <= 7U &&
                                   (((unsigned)input[0] << 8) | input[1]) % 31U == 0U;
                bits = zlib_header ? 15 : -15;
            }
            if (inflateInit2(&stream, bits) != Z_OK) {
                status = IPP_STATUS_ERROR_INTERNAL;
                message = "Unable to decompress the document.";
                break;
            }
            inflating = true;
            carry = 0;
        }
        stream.next_in = input;
        stream.avail_in = (uInt)available;
        while (status == IPP_STATUS_OK && !stream_end && (stream.avail_in > 0 || stream.avail_out == 0)) {
            if (member_end && stream.avail_in > 0) {
                /* The next gzip member, or bytes after the last one (ignored). */
                if (stream.next_in[0] != 0x1fU) {
                    stream_end = true;
                    break;
                }
                member_end = false;
            }
            stream.next_out = output;
            stream.avail_out = IO_BUFFER_SIZE;
            int result = inflate(&stream, Z_NO_FLUSH);
            size_t produced = IO_BUFFER_SIZE - stream.avail_out;
            if (result != Z_OK && result != Z_STREAM_END && result != Z_BUF_ERROR) {
                status = IPP_STATUS_ERROR_COMPRESSION_ERROR;
                message = "The compressed document is damaged.";
                break;
            }
            total += produced;
            if (total > P1600_AIR_DOCUMENT_LIMIT) {
                status = IPP_STATUS_ERROR_REQUEST_ENTITY;
                message = "The document is too large.";
                client->must_close = true;
                client->unread_body = true;
                break;
            }
            if (produced > 0 && write_all(fd, output, produced) != 0) {
                status = IPP_STATUS_ERROR_INTERNAL;
                message = "Unable to store the document.";
                break;
            }
            if (produced > 0 && !reserve_bytes(server, produced, &reserved)) {
                status = IPP_STATUS_ERROR_BUSY;
                message = "The printer is busy; try again later.";
                client->must_close = true;
                client->unread_body = true;
                break;
            }
            if (result == Z_STREAM_END) {
                if (coding == CODING_GZIP) {
                    /* gzip may hold several members, also in later reads
                     * (an HTTP chunk may end where a member does). */
                    inflateReset(&stream);
                    member_end = true;
                } else {
                    stream_end = true;
                }
            } else if (result == Z_BUF_ERROR && produced == 0) {
                break;
            }
        }
    }
    if (status == IPP_STATUS_OK && coding != CODING_NONE && !stream_end && !member_end) {
        status = IPP_STATUS_ERROR_COMPRESSION_ERROR;
        message = "The compressed document is incomplete.";
    }
    if (status == IPP_STATUS_OK && total == 0) {
        status = IPP_STATUS_ERROR_BAD_REQUEST;
        message = "No document data.";
    }
    /* From here the document counts as a waiting job's (or is gone). */
    pthread_mutex_lock(&server->lock);
    server->receiving -= reserved;
    pthread_mutex_unlock(&server->lock);
    if (inflating) inflateEnd(&stream);
    free(input);
    free(output);
    close(fd);
    if (status != IPP_STATUS_OK) {
        unlink(path);
        path[0] = '\0';
        respond(rq, status, "%s", message);
        return false;
    }
    *octets = total;
    return true;
}

/* The format of a received document: the detected type, which must agree
 * with the one supplied. NULL after an error response. */
static const char *document_format(Request *rq, const char *path, const char *supplied) {
    unsigned char head[1024];
    ssize_t count = -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        count = pread(fd, head, sizeof(head), 0);
        close(fd);
    }
    const char *detected = count > 0 ? p1600_air_detect_format(head, (size_t)count) : NULL;
    if (strcmp(supplied, "application/octet-stream") == 0) {
        if (!detected) {
            respond(rq, IPP_STATUS_ERROR_DOCUMENT_FORMAT_NOT_SUPPORTED, "Unable to recognize the document format.");
        }
        return detected;
    }
    if (!detected || strcmp(detected, supplied) != 0) {
        respond(rq, IPP_STATUS_ERROR_DOCUMENT_FORMAT_ERROR, "The document is not %s.", supplied);
        return NULL;
    }
    return detected;
}

/* --- jobs ------------------------------------------------------------------------- */

/* Under the lock. */
static P1600AirJob *create_job(Request *rq, JobRequest *jr, ipp_jstate_t state) {
    P1600AirServer *server = rq->server;
    P1600AirJob *job = calloc(1, sizeof(*job));
    if (!job) return NULL;
    job->id = server->next_job_id++;
    job->printer = rq->printer->index;
    snprintf(job->name, sizeof(job->name), "%s", jr->job_name);
    snprintf(job->user, sizeof(job->user), "%s", rq->user);
    snprintf(job->document_name, sizeof(job->document_name), "%s", jr->document_name);
    job->attrs = jr->attrs;
    jr->attrs = NULL;
    job->state = state;
    job->created = time(NULL);
    if (server->last_job) {
        server->last_job->next = job;
    } else {
        server->jobs = job;
    }
    server->last_job = job;
    server->job_count++;
    prune_jobs(server);
    return job;
}

/* Under the lock: the document is complete and waits for the worker. */
static void queue_job(P1600AirServer *server, P1600AirJob *job, const char *path, const char *format,
                      const char *supplied, size_t octets) {
    snprintf(job->document, sizeof(job->document), "%s", path);
    snprintf(job->format, sizeof(job->format), "%s", format);
    snprintf(job->format_supplied, sizeof(job->format_supplied), "%s", supplied);
    job->has_document = true;
    job->octets = octets;
    job->queued = true;
    set_job_state(job, IPP_JSTATE_PENDING, "job-queued", "");
    pthread_cond_broadcast(&server->work);
}

static void respond_job(Request *rq, P1600AirJob *job, ipp_status_t status) {
    static const char *const created[] = {"job-id", "job-state", "job-state-message", "job-state-reasons",
                                          "job-uri", NULL};
    Requested requested;
    requested_init(&requested, NULL, created);
    respond(rq, status, NULL);
    add_job_attributes(rq->server, rq->client, job, rq->response, &requested);
    requested_free(&requested);
}

static bool accepting_jobs(Request *rq) {
    refresh_printer(rq->server, rq->printer, false);
    pthread_mutex_lock(&rq->server->lock);
    PrinterState state;
    printer_state(rq->server, rq->printer, &state);
    pthread_mutex_unlock(&rq->server->lock);
    if (!state.accepting) {
        respond(rq, IPP_STATUS_ERROR_NOT_ACCEPTING_JOBS, "The printer is not accepting jobs.");
        return false;
    }
    return true;
}

static void op_print_job(Request *rq) {
    JobRequest jr;
    if (!validate_job(rq, &jr, true)) return;
    if (!accepting_jobs(rq)) {
        ippDelete(jr.attrs);
        return;
    }
    pthread_mutex_lock(&rq->server->lock);
    bool busy = server_busy(rq->server, true);
    pthread_mutex_unlock(&rq->server->lock);
    if (busy) {
        ippDelete(jr.attrs);
        respond(rq, IPP_STATUS_ERROR_BUSY, "The printer is busy; try again later.");
        return;
    }
    char path[PATH_MAX];
    size_t octets = 0;
    if (!receive_document(rq, jr.compression, path, sizeof(path), &octets)) {
        ippDelete(jr.attrs);
        return;
    }
    const char *format = document_format(rq, path, jr.format);
    if (!format) {
        unlink(path);
        ippDelete(jr.attrs);
        return;
    }
    pthread_mutex_lock(&rq->server->lock);
    P1600AirJob *job = create_job(rq, &jr, IPP_JSTATE_PENDING);
    if (job) {
        queue_job(rq->server, job, path, format, jr.format, octets);
        p1600_air_log("job %d \"%s\" from %s for %s: %s, %zu bytes", job->id, job->name, job->user, rq->printer->name,
                   format, octets);
        respond_job(rq, job, jr.ignored ? IPP_STATUS_OK_IGNORED_OR_SUBSTITUTED : IPP_STATUS_OK);
    }
    pthread_mutex_unlock(&rq->server->lock);
    if (!job) {
        unlink(path);
        ippDelete(jr.attrs);
        respond(rq, IPP_STATUS_ERROR_INTERNAL, "Out of memory.");
    }
}

static void op_validate_job(Request *rq) {
    JobRequest jr;
    if (!validate_job(rq, &jr, true)) return;
    ippDelete(jr.attrs);
    if (!accepting_jobs(rq)) return;
    respond(rq, jr.ignored ? IPP_STATUS_OK_IGNORED_OR_SUBSTITUTED : IPP_STATUS_OK, NULL);
}

static void op_create_job(Request *rq) {
    JobRequest jr;
    if (!validate_job(rq, &jr, false)) return;
    if (!accepting_jobs(rq)) {
        ippDelete(jr.attrs);
        return;
    }
    pthread_mutex_lock(&rq->server->lock);
    if (server_busy(rq->server, true)) {
        pthread_mutex_unlock(&rq->server->lock);
        ippDelete(jr.attrs);
        respond(rq, IPP_STATUS_ERROR_BUSY, "The printer is busy; try again later.");
        return;
    }
    P1600AirJob *job = create_job(rq, &jr, IPP_JSTATE_HELD);
    if (job) {
        job->reason = "job-incoming";
        VERBOSE("job %d \"%s\" created by %s for %s", job->id, job->name, job->user, rq->printer->name);
        respond_job(rq, job, jr.ignored ? IPP_STATUS_OK_IGNORED_OR_SUBSTITUTED : IPP_STATUS_OK);
    }
    pthread_mutex_unlock(&rq->server->lock);
    if (!job) {
        ippDelete(jr.attrs);
        respond(rq, IPP_STATUS_ERROR_INTERNAL, "Out of memory.");
    }
}

/* Under the lock: the job of a job operation, checked against the printer
 * and the requesting user; NULL after an error response. */
static P1600AirJob *job_for_request(Request *rq) {
    P1600AirJob *job = find_job(rq->server, rq->job_id);
    if (!job || job->printer != rq->printer->index) {
        respond(rq, IPP_STATUS_ERROR_NOT_FOUND, "Job #%d does not exist.", rq->job_id);
        return NULL;
    }
    return job;
}

static bool owner_matches(const Request *rq, const P1600AirJob *job) {
    return !rq->user_supplied || strcmp(rq->user, job->user) == 0;
}

static void op_send_document(Request *rq) {
    P1600AirServer *server = rq->server;
    pthread_mutex_lock(&server->lock);
    P1600AirJob *job = job_for_request(rq);
    if (job && !owner_matches(rq, job)) {
        respond(rq, IPP_STATUS_ERROR_NOT_AUTHORIZED, "Job #%d belongs to another user.", job->id);
        job = NULL;
    }
    if (job && (job->state != IPP_JSTATE_HELD || job->has_document || job->pins > 0)) {
        if (job->has_document && job_active(job)) {
            respond(rq, IPP_STATUS_ERROR_MULTIPLE_JOBS_NOT_SUPPORTED, "Job #%d already has its document.", job->id);
        } else {
            respond(rq, IPP_STATUS_ERROR_NOT_POSSIBLE, "Job #%d is not waiting for a document.", job->id);
        }
        job = NULL;
    }
    if (job && server_busy(server, false)) {
        respond(rq, IPP_STATUS_ERROR_BUSY, "The printer is busy; try again later.");
        job = NULL;
    }
    if (job) job->pins++;
    pthread_mutex_unlock(&server->lock);
    if (!job) return;

    JobRequest jr;
    memset(&jr, 0, sizeof(jr));
    ipp_attribute_t *last = ippFindAttribute(rq->request, "last-document", IPP_TAG_ZERO);
    char path[PATH_MAX] = "";
    size_t octets = 0;
    const char *format = NULL;
    bool transfer = false;  /* the document itself was read */
    if (!last || ippGetGroupTag(last) != IPP_TAG_OPERATION) {
        respond(rq, IPP_STATUS_ERROR_BAD_REQUEST, "Missing required last-document attribute.");
    } else if (ippGetValueTag(last) != IPP_TAG_BOOLEAN || ippGetCount(last) != 1 || !ippGetBoolean(last, 0)) {
        /* One document per job (multiple-document-jobs-supported is false). */
        unsupported(rq, last);
        respond(rq, IPP_STATUS_ERROR_ATTRIBUTES_OR_VALUES, "Only last-document=true is supported.");
    } else if (validate_document(rq, &jr)) {
        transfer = true;
        if (receive_document(rq, jr.compression, path, sizeof(path), &octets)) {
            format = document_format(rq, path, jr.format);
        }
    }

    pthread_mutex_lock(&server->lock);
    job->pins--;
    if (!format) {
        if (path[0]) unlink(path);
        /* A refused request leaves the job waiting; a failed transfer ends it. */
        if (transfer && job_active(job)) {
            set_job_state(job, IPP_JSTATE_ABORTED, "aborted-by-system", "The document could not be received.");
        }
    } else if (job->state != IPP_JSTATE_HELD) {
        unlink(path);
        respond(rq, IPP_STATUS_ERROR_JOB_CANCELED, "Job #%d was canceled.", job->id);
    } else {
        if (!job->document_name[0]) snprintf(job->document_name, sizeof(job->document_name), "%s", jr.document_name);
        queue_job(server, job, path, format, jr.format, octets);
        p1600_air_log("job %d \"%s\" from %s for %s: %s, %zu bytes", job->id, job->name, job->user, rq->printer->name,
                   format, octets);
        respond_job(rq, job, IPP_STATUS_OK);
    }
    pthread_mutex_unlock(&server->lock);
}

/* Under the lock: cancels an active job; returns its CUPS job id (0 when
 * none) for cancellation outside the lock. */
static int cancel_job_locked(P1600AirJob *job, const char *reason) {
    set_job_state(job, IPP_JSTATE_CANCELED, reason, "Canceled.");
    job->queued = false;
    if (job->pins == 0) discard_document(job);
    return job->cups_id;
}

static void cancel_in_cups(P1600AirServer *server, const char *queue, int cups_id) {
    if (cups_id <= 0 || server->spool_dir[0]) return;
    if (p1600_air_cups_cancel(&server->config.cups, queue, cups_id) != 0) {
        p1600_air_log("CUPS job %d on %s could not be canceled: %s", cups_id, queue, cupsLastErrorString());
    }
}

static void op_cancel_job(Request *rq) {
    P1600AirServer *server = rq->server;
    int cups_id = 0;
    pthread_mutex_lock(&server->lock);
    P1600AirJob *job = job_for_request(rq);
    if (job && !owner_matches(rq, job)) {
        respond(rq, IPP_STATUS_ERROR_NOT_AUTHORIZED, "Job #%d belongs to another user.", job->id);
    } else if (job && !job_active(job)) {
        respond(rq, IPP_STATUS_ERROR_NOT_POSSIBLE, "Job #%d is already %s.", job->id,
                job->state == IPP_JSTATE_CANCELED ? "canceled"
                : job->state == IPP_JSTATE_ABORTED ? "aborted"
                                                    : "completed");
    } else if (job) {
        cups_id = cancel_job_locked(job, "job-canceled-by-user");
        p1600_air_log("job %d canceled by %s", job->id, rq->user);
        respond(rq, IPP_STATUS_OK, NULL);
    }
    pthread_mutex_unlock(&server->lock);
    cancel_in_cups(server, rq->printer->name, cups_id);
}

static void op_cancel_my_jobs(Request *rq) {
    P1600AirServer *server = rq->server;
    ipp_attribute_t *ids = ippFindAttribute(rq->request, "job-ids", IPP_TAG_ZERO);
    if (ids && (ippGetValueTag(ids) != IPP_TAG_INTEGER || ippGetGroupTag(ids) != IPP_TAG_OPERATION)) {
        respond(rq, IPP_STATUS_ERROR_BAD_REQUEST, "Bad job-ids attribute.");
        return;
    }
    int cancel_ids[P1600_AIR_JOB_HISTORY + 1];
    int count = 0;
    pthread_mutex_lock(&server->lock);
    for (P1600AirJob *job = server->jobs; job; job = job->next) {
        if (job->printer != rq->printer->index || !job_active(job) || strcmp(job->user, rq->user) != 0) continue;
        if (ids && !ippContainsInteger(ids, job->id)) continue;
        int cups_id = cancel_job_locked(job, "job-canceled-by-user");
        if (cups_id > 0 && count < (int)COUNT(cancel_ids)) cancel_ids[count++] = cups_id;
    }
    respond(rq, IPP_STATUS_OK, NULL);
    pthread_mutex_unlock(&server->lock);
    for (int i = 0; i < count; i++) cancel_in_cups(server, rq->printer->name, cancel_ids[i]);
}

static void op_close_job(Request *rq) {
    P1600AirServer *server = rq->server;
    pthread_mutex_lock(&server->lock);
    P1600AirJob *job = job_for_request(rq);
    if (job && !owner_matches(rq, job)) {
        respond(rq, IPP_STATUS_ERROR_NOT_AUTHORIZED, "Job #%d belongs to another user.", job->id);
    } else if (job && !job_active(job)) {
        respond(rq, IPP_STATUS_ERROR_NOT_POSSIBLE, "Job #%d is already finished.", job->id);
    } else if (job) {
        if (job->state == IPP_JSTATE_HELD && !job->has_document && job->pins == 0) {
            set_job_state(job, IPP_JSTATE_ABORTED, "aborted-by-system", "The job was closed without a document.");
        }
        respond(rq, IPP_STATUS_OK, NULL);
    }
    pthread_mutex_unlock(&server->lock);
}

static void op_get_job_attributes(Request *rq) {
    Requested requested;
    requested_init(&requested, rq->request, NULL);
    pthread_mutex_lock(&rq->server->lock);
    P1600AirJob *job = job_for_request(rq);
    if (job) {
        respond(rq, IPP_STATUS_OK, NULL);
        add_job_attributes(rq->server, rq->client, job, rq->response, &requested);
    }
    pthread_mutex_unlock(&rq->server->lock);
    requested_free(&requested);
}

static bool job_matches(const P1600AirJob *job, const char *which) {
    if (strcmp(which, "all") == 0) return true;
    if (strcmp(which, "completed") == 0) return !job_active(job);
    if (strcmp(which, "not-completed") == 0) return job_active(job);
    if (strcmp(which, "aborted") == 0) return job->state == IPP_JSTATE_ABORTED;
    if (strcmp(which, "canceled") == 0) return job->state == IPP_JSTATE_CANCELED;
    if (strcmp(which, "pending") == 0) return job->state == IPP_JSTATE_PENDING;
    if (strcmp(which, "pending-held") == 0) return job->state == IPP_JSTATE_HELD;
    if (strcmp(which, "processing") == 0) return job->state == IPP_JSTATE_PROCESSING;
    return false;
}

static void op_get_jobs(Request *rq) {
    bool bad = false;
    ipp_attribute_t *which = operation_attribute(rq, "which-jobs", IPP_TAG_KEYWORD, &bad);
    ipp_attribute_t *my_jobs = operation_attribute(rq, "my-jobs", IPP_TAG_BOOLEAN, &bad);
    ipp_attribute_t *limit = operation_attribute(rq, "limit", IPP_TAG_INTEGER, &bad);
    ipp_attribute_t *ids = ippFindAttribute(rq->request, "job-ids", IPP_TAG_ZERO);
    if (ids && (ippGetValueTag(ids) != IPP_TAG_INTEGER || ippGetGroupTag(ids) != IPP_TAG_OPERATION)) bad = true;
    if (bad || (limit && ippGetInteger(limit, 0) < 1)) {
        respond(rq, IPP_STATUS_ERROR_BAD_REQUEST, "Bad Get-Jobs attribute.");
        return;
    }
    const char *which_jobs = which ? ippGetString(which, 0, NULL) : ids ? "all" : "not-completed";
    if (!in_list(which_jobs, which_jobs_supported, COUNT(which_jobs_supported))) {
        unsupported(rq, which);
        respond(rq, IPP_STATUS_ERROR_ATTRIBUTES_OR_VALUES, "Unsupported which-jobs \"%s\".", which_jobs);
        return;
    }
    bool mine = my_jobs && ippGetBoolean(my_jobs, 0);
    int remaining = limit ? ippGetInteger(limit, 0) : INT_MAX;
    static const char *const defaults[] = {"job-id", "job-uri", NULL};
    Requested requested;
    requested_init(&requested, rq->request, defaults);
    respond(rq, IPP_STATUS_OK, NULL);
    pthread_mutex_lock(&rq->server->lock);
    /* Finished jobs newest first, others in order of arrival. */
    bool newest_first = strcmp(which_jobs, "completed") == 0 || strcmp(which_jobs, "aborted") == 0 ||
                        strcmp(which_jobs, "canceled") == 0;
    P1600AirJob *list[P1600_AIR_JOB_HISTORY * 2];
    int count = 0;
    for (P1600AirJob *job = rq->server->jobs; job && count < (int)COUNT(list); job = job->next) list[count++] = job;
    bool first = true;
    for (int n = 0; n < count && remaining > 0; n++) {
        P1600AirJob *job = list[newest_first ? count - 1 - n : n];
        if (job->printer != rq->printer->index || !job_matches(job, which_jobs)) continue;
        if (mine && strcmp(job->user, rq->user) != 0) continue;
        if (ids && !ippContainsInteger(ids, job->id)) continue;
        if (!first) ippAddSeparator(rq->response);
        first = false;
        add_job_attributes(rq->server, rq->client, job, rq->response, &requested);
        remaining--;
    }
    pthread_mutex_unlock(&rq->server->lock);
    requested_free(&requested);
}

static void op_get_printer_attributes(Request *rq) {
    bool bad = false;
    ipp_attribute_t *format = operation_attribute(rq, "document-format", IPP_TAG_MIMETYPE, &bad);
    if (bad) {
        respond(rq, IPP_STATUS_ERROR_BAD_REQUEST, "Bad document-format attribute.");
        return;
    }
    if (format && !supported_format(ippGetString(format, 0, NULL))) {
        unsupported(rq, format);
        respond(rq, IPP_STATUS_ERROR_DOCUMENT_FORMAT_NOT_SUPPORTED, "Unsupported document format \"%s\".",
                ippGetString(format, 0, NULL));
        return;
    }
    refresh_printer(rq->server, rq->printer, false);
    Requested requested;
    requested_init(&requested, rq->request, NULL);
    respond(rq, IPP_STATUS_OK, NULL);
    pthread_mutex_lock(&rq->server->lock);
    add_printer_attributes(rq->server, rq->printer, rq->client, rq->response, &requested);
    pthread_mutex_unlock(&rq->server->lock);
    requested_free(&requested);
}

static void op_identify_printer(Request *rq) {
    ipp_attribute_t *message = ippFindAttribute(rq->request, "message", IPP_TAG_ZERO);
    char text[256] = "";
    if (message && (ippGetValueTag(message) == IPP_TAG_TEXT || ippGetValueTag(message) == IPP_TAG_TEXTLANG)) {
        copy_text(text, sizeof(text), ippGetString(message, 0, NULL), 255);
    }
    p1600_air_log("Identify-Printer for %s from %s%s%s", rq->printer->name, rq->client->peer, text[0] ? ": " : "", text);
    respond(rq, IPP_STATUS_OK, NULL);
}

static bool job_operation(ipp_op_t op) {
    return op == IPP_OP_SEND_DOCUMENT || op == IPP_OP_CANCEL_JOB || op == IPP_OP_GET_JOB_ATTRIBUTES ||
           op == IPP_OP_CLOSE_JOB;
}

/* The target printer (and job) of the request; false after an error response. */
static bool resolve_target(Request *rq, ipp_op_t op, Printer *http_printer, int http_job_id) {
    ipp_attribute_t *printer_uri = ippFindAttribute(rq->request, "printer-uri", IPP_TAG_ZERO);
    ipp_attribute_t *job_uri = ippFindAttribute(rq->request, "job-uri", IPP_TAG_ZERO);
    ipp_attribute_t *target = printer_uri ? printer_uri : job_operation(op) ? job_uri : NULL;
    if (!target) {
        respond(rq, IPP_STATUS_ERROR_BAD_REQUEST, "Missing printer-uri.");
        return false;
    }
    if (ippGetGroupTag(target) != IPP_TAG_OPERATION || ippGetValueTag(target) != IPP_TAG_URI ||
        ippGetCount(target) != 1) {
        respond(rq, IPP_STATUS_ERROR_BAD_REQUEST, "Bad %s attribute.", ippGetName(target));
        return false;
    }
    char scheme[32], userpass[256], host[256], resource[1024];
    int port = 0;
    if (httpSeparateURI(HTTP_URI_CODING_MOST, ippGetString(target, 0, NULL), scheme, sizeof(scheme), userpass,
                        sizeof(userpass), host, sizeof(host), &port, resource, sizeof(resource)) < HTTP_URI_STATUS_OK ||
        (strcmp(scheme, "ipp") != 0 && strcmp(scheme, "ipps") != 0 && strcmp(scheme, "http") != 0 &&
         strcmp(scheme, "https") != 0)) {
        respond(rq, IPP_STATUS_ERROR_BAD_REQUEST, "Bad %s value.", ippGetName(target));
        return false;
    }
    int job_id = 0;
    Printer *printer = find_printer(rq->server, resource, target == job_uri ? &job_id : NULL);
    if (!printer || (target == job_uri && job_id <= 0)) {
        respond(rq, IPP_STATUS_ERROR_NOT_FOUND, "%s not found.", target == job_uri ? "Job" : "Printer");
        return false;
    }
    (void)http_printer;
    rq->printer = printer;
    if (job_operation(op) && target == printer_uri) {
        ipp_attribute_t *id = ippFindAttribute(rq->request, "job-id", IPP_TAG_ZERO);
        if (!id) {
            respond(rq, IPP_STATUS_ERROR_BAD_REQUEST, "Missing job-id.");
            return false;
        }
        if (ippGetGroupTag(id) != IPP_TAG_OPERATION || ippGetValueTag(id) != IPP_TAG_INTEGER ||
            ippGetCount(id) != 1 || ippGetInteger(id, 0) < 1) {
            respond(rq, IPP_STATUS_ERROR_BAD_REQUEST, "Bad job-id attribute.");
            return false;
        }
        job_id = ippGetInteger(id, 0);
    } else if (job_operation(op) && job_id <= 0) {
        job_id = http_job_id;
    }
    rq->job_id = job_id;
    return true;
}

static ipp_t *process_ipp(Client *client, Printer *http_printer, int http_job_id, ipp_t *request) {
    Request rq;
    memset(&rq, 0, sizeof(rq));
    rq.client = client;
    rq.server = client->server;
    rq.request = request;
    rq.response = ippNewResponse(request);
    rq.unsupported = ippNew();
    snprintf(rq.user, sizeof(rq.user), "anonymous");
    ipp_op_t op = ippGetOperation(request);
    int minor = 0, major = ippGetVersion(request, &minor);
    ipp_attribute_t *charset = ippFirstAttribute(request);
    ipp_attribute_t *language = ippNextAttribute(request);

    if (major < 1 || major > 2) {
        respond(&rq, IPP_STATUS_ERROR_VERSION_NOT_SUPPORTED, "Bad request version %d.%d.", major, minor);
    } else if (ippGetRequestId(request) <= 0) {
        respond(&rq, IPP_STATUS_ERROR_BAD_REQUEST, "Bad request-id %d.", ippGetRequestId(request));
    } else if (!charset || !ippGetName(charset) || strcmp(ippGetName(charset), "attributes-charset") != 0 ||
               ippGetValueTag(charset) != IPP_TAG_CHARSET || ippGetGroupTag(charset) != IPP_TAG_OPERATION ||
               ippGetCount(charset) != 1) {
        respond(&rq, IPP_STATUS_ERROR_BAD_REQUEST, "Missing or misplaced attributes-charset.");
    } else if (!language || !ippGetName(language) || strcmp(ippGetName(language), "attributes-natural-language") != 0 ||
               ippGetValueTag(language) != IPP_TAG_LANGUAGE || ippGetGroupTag(language) != IPP_TAG_OPERATION ||
               ippGetCount(language) != 1) {
        respond(&rq, IPP_STATUS_ERROR_BAD_REQUEST, "Missing or misplaced attributes-natural-language.");
    } else if (strcasecmp(ippGetString(charset, 0, NULL), "utf-8") != 0 &&
               strcasecmp(ippGetString(charset, 0, NULL), "us-ascii") != 0) {
        unsupported(&rq, charset);
        respond(&rq, IPP_STATUS_ERROR_CHARSET, "Unsupported character set \"%s\".", ippGetString(charset, 0, NULL));
    } else if (!ippValidateAttributes(request)) {
        respond(&rq, IPP_STATUS_ERROR_BAD_REQUEST, "%s", cupsLastErrorString());
    } else if (op != IPP_OP_PRINT_JOB && op != IPP_OP_VALIDATE_JOB && op != IPP_OP_CREATE_JOB &&
               op != IPP_OP_SEND_DOCUMENT && op != IPP_OP_CANCEL_JOB && op != IPP_OP_GET_JOB_ATTRIBUTES &&
               op != IPP_OP_GET_JOBS && op != IPP_OP_GET_PRINTER_ATTRIBUTES && op != IPP_OP_CANCEL_MY_JOBS &&
               op != IPP_OP_CLOSE_JOB && op != IPP_OP_IDENTIFY_PRINTER) {
        respond(&rq, IPP_STATUS_ERROR_OPERATION_NOT_SUPPORTED, "Operation %s is not supported.", ippOpString(op));
    } else if (resolve_target(&rq, op, http_printer, http_job_id)) {
        bool bad = false;
        ipp_attribute_t *user = operation_attribute(&rq, "requesting-user-name", IPP_TAG_NAME, &bad);
        if (bad) {
            respond(&rq, IPP_STATUS_ERROR_BAD_REQUEST, "Bad requesting-user-name attribute.");
        } else {
            if (user && ippGetString(user, 0, NULL)[0]) {
                copy_text(rq.user, sizeof(rq.user), ippGetString(user, 0, NULL), 255);
                rq.user_supplied = true;
            }
            switch (op) {
            case IPP_OP_PRINT_JOB: op_print_job(&rq); break;
            case IPP_OP_VALIDATE_JOB: op_validate_job(&rq); break;
            case IPP_OP_CREATE_JOB: op_create_job(&rq); break;
            case IPP_OP_SEND_DOCUMENT: op_send_document(&rq); break;
            case IPP_OP_CANCEL_JOB: op_cancel_job(&rq); break;
            case IPP_OP_GET_JOB_ATTRIBUTES: op_get_job_attributes(&rq); break;
            case IPP_OP_GET_JOBS: op_get_jobs(&rq); break;
            case IPP_OP_GET_PRINTER_ATTRIBUTES: op_get_printer_attributes(&rq); break;
            case IPP_OP_CANCEL_MY_JOBS: op_cancel_my_jobs(&rq); break;
            case IPP_OP_CLOSE_JOB: op_close_job(&rq); break;
            case IPP_OP_IDENTIFY_PRINTER: op_identify_printer(&rq); break;
            default: break;
            }
        }
    }
    if (!rq.responded) respond(&rq, IPP_STATUS_ERROR_INTERNAL, "No response.");
    VERBOSE("%s %s %s: %s", client->peer, ippOpString(op), rq.printer ? rq.printer->name : "-",
            ippErrorString(ippGetStatusCode(rq.response)));
    ippDelete(rq.unsupported);
    return rq.response;
}

/* --- the job worker ---------------------------------------------------------------- */

static int open_temp(P1600AirServer *server, const char *prefix, char *path, size_t size) {
    snprintf(path, size, "%s/%s-XXXXXX", server->temp_dir, prefix);
    int fd = mkstemp(path);
    if (fd < 0) path[0] = '\0';
    return fd;
}

static int copy_file(const char *from, int to_fd) {
    int from_fd = open(from, O_RDONLY | O_CLOEXEC);
    if (from_fd < 0) return -1;
    unsigned char *buffer = malloc(IO_BUFFER_SIZE);
    int status = buffer ? 0 : -1;
    while (status == 0) {
        ssize_t count = read(from_fd, buffer, IO_BUFFER_SIZE);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) status = -1;
        if (count <= 0) break;
        if (write_all(to_fd, buffer, (size_t)count) != 0) status = -1;
    }
    free(buffer);
    close(from_fd);
    return status;
}

/* Test mode: job-<id>.<ext> (the document CUPS would get) and
 * job-<id>.options (document-format, title and the CUPS options). */
static int spool_job(P1600AirServer *server, int id, const char *title, const char *path, const char *format,
                     int num_options, cups_option_t *options, char *error, size_t error_size) {
    const char *extension = strcmp(format, "image/jpeg") == 0 ? "jpg"
                            : strcmp(format, "image/png") == 0  ? "png"
                                                                : "pdf";
    char final_path[PATH_MAX], part[PATH_MAX];
    snprintf(final_path, sizeof(final_path), "%s/job-%d.options", server->spool_dir, id);
    snprintf(part, sizeof(part), "%s/.job-%d.options.part", server->spool_dir, id);
    int fd = open(part, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return fail(error, error_size, "Unable to write %s: %s", part, strerror(errno));
    char line[1200], value[1024];
    int status = 0;
    snprintf(line, sizeof(line), "document-format=%s\n", format);
    status |= write_all(fd, line, strlen(line));
    copy_text(value, sizeof(value), title, 255);
    snprintf(line, sizeof(line), "title=%s\n", value);
    status |= write_all(fd, line, strlen(line));
    for (int i = 0; i < num_options; i++) {
        copy_text(value, sizeof(value), options[i].value, sizeof(value) - 1U);
        snprintf(line, sizeof(line), "%s=%s\n", options[i].name, value);
        status |= write_all(fd, line, strlen(line));
    }
    if (close(fd) != 0 || status != 0 || rename(part, final_path) != 0) {
        unlink(part);
        return fail(error, error_size, "Unable to write %s", final_path);
    }
    snprintf(final_path, sizeof(final_path), "%s/job-%d.%s", server->spool_dir, id, extension);
    snprintf(part, sizeof(part), "%s/.job-%d.%s.part", server->spool_dir, id, extension);
    fd = open(part, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return fail(error, error_size, "Unable to write %s: %s", part, strerror(errno));
    status = copy_file(path, fd);
    if (close(fd) != 0 || status != 0 || rename(part, final_path) != 0) {
        unlink(part);
        return fail(error, error_size, "Unable to write %s", final_path);
    }
    return 0;
}

static void finish_job(P1600AirServer *server, P1600AirJob *job, ipp_jstate_t state, const char *reason,
                       const char *message) {
    pthread_mutex_lock(&server->lock);
    if (job_active(job)) set_job_state(job, state, reason, message);
    pthread_mutex_unlock(&server->lock);
}

static bool job_canceled(P1600AirServer *server, P1600AirJob *job) {
    pthread_mutex_lock(&server->lock);
    bool canceled = !job_active(job);
    pthread_mutex_unlock(&server->lock);
    return canceled;
}

typedef struct {
    P1600AirServer *server;
    P1600AirJob *job;
} Conversion;

/* Asked between pages: Cancel-Job and the shutdown stop a conversion. */
static bool conversion_stopped(void *context) {
    Conversion *conversion = context;
    return atomic_load(&conversion->server->stopping) || job_canceled(conversion->server, conversion->job);
}

/* Converts (URF, PWG raster) and submits (CUPS or the spool directory) a
 * job; the job is pinned by the worker. */
static void process_job(P1600AirServer *server, P1600AirJob *job) {
    char document[PATH_MAX], format[64], title[256], queue[128];
    ipp_t *attrs = ippNew();
    pthread_mutex_lock(&server->lock);
    if (job->state != IPP_JSTATE_PENDING || !job->document[0]) {
        discard_document(job);
        pthread_mutex_unlock(&server->lock);
        ippDelete(attrs);
        return;
    }
    bool raster = strcmp(job->format, "image/urf") == 0 || strcmp(job->format, "image/pwg-raster") == 0;
    set_job_state(job, IPP_JSTATE_PROCESSING, raster ? "job-transforming" : "job-printing", "");
    snprintf(document, sizeof(document), "%s", job->document);
    snprintf(format, sizeof(format), "%s", job->format);
    snprintf(title, sizeof(title), "%s", job->name);
    snprintf(queue, sizeof(queue), "%s", server->printers[job->printer].name);
    if (job->attrs) ippCopyAttributes(attrs, job->attrs, 0, NULL, NULL);
    int id = job->id;
    pthread_mutex_unlock(&server->lock);

    char error[512] = "", pdf[PATH_MAX] = "";
    const char *submit_path = document, *submit_format = format;
    int num_options = 0;
    cups_option_t *options = NULL;
    const P1600AirMedia *raster_media = NULL;
    if (raster) {
        int pdf_fd = open_temp(server, "pdf", pdf, sizeof(pdf));
        int in_fd = open(document, O_RDONLY | O_CLOEXEC);
        P1600RasterSummary summary;
        int pages = -1;
        if (pdf_fd < 0 || in_fd < 0) {
            snprintf(error, sizeof(error), "Unable to convert the document.");
        } else {
            Conversion conversion = {server, job};
            P1600RasterLimits limits = {.max_decoded_bytes = server->config.max_raster_bytes,
                                     .stop = conversion_stopped,
                                     .context = &conversion};
            /* print-scaling=fit: into the printable area (cgpdftoraster's
             * fit-to-page leaves a page the size of the paper as it is). */
            ipp_attribute_t *scaling = ippFindAttribute(attrs, "print-scaling", IPP_TAG_KEYWORD);
            const char *scaling_value = scaling ? ippGetString(scaling, 0, NULL) : NULL;
            if (scaling_value && strcmp(scaling_value, "fit") == 0) {
                limits.fit_margin_points = P1600_AIR_MARGIN * 72.0 / 2540.0;
            }
            pages = p1600_raster_to_pdf(strcmp(format, "image/urf") == 0 ? P1600_RASTER_FORMAT_URF : P1600_RASTER_FORMAT_PWG,
                                     in_fd, pdf_fd, &limits, &summary, error, sizeof(error));
        }
        if (in_fd >= 0) close(in_fd);
        if (pdf_fd >= 0) close(pdf_fd);
        if (pages < 0 && job_canceled(server, job)) {
            VERBOSE("job %d: canceled while it was converted", id);
            goto cleanup;
        }
        if (pages < 0) {
            p1600_air_log("job %d: %s", id, error);
            finish_job(server, job, IPP_JSTATE_ABORTED, "document-format-error", error);
            goto cleanup;
        }
        VERBOSE("job %d: %d raster page%s converted to PDF", id, pages, pages == 1 ? "" : "s");
        pthread_mutex_lock(&server->lock);
        job->impressions = pages;
        pthread_mutex_unlock(&server->lock);
        raster_media = p1600_air_media_by_size((int)(summary.width_points * 2540.0 / 72.0 + 0.5),
                                            (int)(summary.height_points * 2540.0 / 72.0 + 0.5));
        submit_path = pdf;
        submit_format = "application/pdf";
    }
    num_options = p1600_air_job_options(attrs, 0, &options);
    /* A raster page is already the size of the paper the client chose. */
    if (raster_media && !cupsGetOption("PageSize", num_options, options)) {
        num_options = cupsAddOption("PageSize", raster_media->ppd, num_options, &options);
    }
    /* cgpdftoraster turns a page that is taller than wide by 90 degrees,
     * unscaled, for orientation-requested 4 and 5: its top and bottom fall
     * off the sheet. Raster pages come laid out by the client already. */
    const char *orientation = cupsGetOption("orientation-requested", num_options, options);
    if (orientation && (raster || (strcmp(submit_format, "application/pdf") == 0 &&
                                   (strcmp(orientation, "4") == 0 || strcmp(orientation, "5") == 0) &&
                                   !p1600_pdf_landscape(submit_path)))) {
        num_options = cupsRemoveOption("orientation-requested", num_options, &options);
    }
    if (job_canceled(server, job)) goto cleanup;

    if (server->spool_dir[0]) {
        if (spool_job(server, id, title, submit_path, submit_format, num_options, options, error, sizeof(error)) != 0) {
            p1600_air_log("job %d: %s", id, error);
            finish_job(server, job, IPP_JSTATE_ABORTED, "aborted-by-system", error);
        } else {
            VERBOSE("job %d spooled", id);
            finish_job(server, job, IPP_JSTATE_COMPLETED, "job-completed-successfully", "");
        }
    } else {
        int fd = open(submit_path, O_RDONLY | O_CLOEXEC);
        int cups_id = fd >= 0 ? p1600_air_cups_submit(&server->config.cups, queue, title, submit_format, fd, num_options,
                                                   options, error, sizeof(error))
                              : fail(error, sizeof(error), "Unable to read the document.");
        if (fd >= 0) close(fd);
        bool cancel = false;
        pthread_mutex_lock(&server->lock);
        if (cups_id < 0) {
            p1600_air_log("job %d: %s", id, error);
            if (job_active(job)) set_job_state(job, IPP_JSTATE_ABORTED, "aborted-by-system", error);
        } else {
            job->cups_id = cups_id;
            cancel = !job_active(job);
            if (!cancel) job->reason = "job-printing";
            p1600_air_log("job %d is CUPS job %s-%d", id, queue, cups_id);
        }
        pthread_mutex_unlock(&server->lock);
        if (cancel) cancel_in_cups(server, queue, cups_id);
    }

cleanup:
    cupsFreeOptions(num_options, options);
    ippDelete(attrs);
    if (pdf[0]) unlink(pdf);
    pthread_mutex_lock(&server->lock);
    discard_document(job);
    pthread_mutex_unlock(&server->lock);
}

static const char *cups_reason(ipp_jstate_t state) {
    switch (state) {
    case IPP_JSTATE_PENDING: return "job-queued";
    case IPP_JSTATE_HELD: return "job-hold-until-specified";
    case IPP_JSTATE_PROCESSING: return "job-printing";
    case IPP_JSTATE_STOPPED: return "printer-stopped";
    case IPP_JSTATE_CANCELED: return "job-canceled-at-device";
    case IPP_JSTATE_ABORTED: return "aborted-by-system";
    default: return "job-completed-successfully";
    }
}

/* Mirrors the state of submitted jobs from cupsd. */
static void poll_cups_jobs(P1600AirServer *server) {
    int ids[MAX_POLLED_JOBS], cups_ids[MAX_POLLED_JOBS], count = 0;
    pthread_mutex_lock(&server->lock);
    for (P1600AirJob *job = server->jobs; job && count < MAX_POLLED_JOBS; job = job->next) {
        if (job->cups_id > 0 && job_active(job)) {
            ids[count] = job->id;
            cups_ids[count++] = job->cups_id;
        }
    }
    pthread_mutex_unlock(&server->lock);
    for (int i = 0; i < count && !atomic_load(&server->stopping); i++) {
        ipp_jstate_t state = IPP_JSTATE_PROCESSING;
        char message[256] = "";
        int result = p1600_air_cups_job_state(&server->config.cups, cups_ids[i], &state, message, sizeof(message));
        if (result < 0) continue;
        if (result == 1) state = IPP_JSTATE_COMPLETED;  /* gone from the CUPS history */
        pthread_mutex_lock(&server->lock);
        P1600AirJob *job = find_job(server, ids[i]);
        if (job && job_active(job) && state >= IPP_JSTATE_PENDING && state <= IPP_JSTATE_COMPLETED) {
            if (state != job->state) {
                VERBOSE("job %d: CUPS job %d is %s", job->id, job->cups_id, ippEnumString("job-state", (int)state));
            }
            set_job_state(job, state, cups_reason(state), message);
            if (state >= IPP_JSTATE_CANCELED) p1600_air_log("job %d %s", job->id, ippEnumString("job-state", (int)state));
        }
        pthread_mutex_unlock(&server->lock);
    }
}

/* Under the lock: Create-Job jobs that never got their document. */
static void expire_held_jobs(P1600AirServer *server) {
    time_t now = time(NULL);
    for (P1600AirJob *job = server->jobs; job; job = job->next) {
        if (job->state == IPP_JSTATE_HELD && !job->has_document && job->pins == 0 &&
            now - job->created > P1600_AIR_MULTIPLE_OPERATION_TIMEOUT) {
            set_job_state(job, IPP_JSTATE_ABORTED, "aborted-by-system", "No document arrived in time.");
            p1600_air_log("job %d aborted: no document within %d seconds", job->id, P1600_AIR_MULTIPLE_OPERATION_TIMEOUT);
        }
    }
}

static void *worker_main(void *argument) {
    P1600AirServer *server = argument;
    time_t last_poll = 0;
    pthread_mutex_lock(&server->lock);
    while (!atomic_load(&server->stopping)) {
        P1600AirJob *job = NULL;
        for (P1600AirJob *candidate = server->jobs; candidate; candidate = candidate->next) {
            if (candidate->queued) {
                job = candidate;
                break;
            }
        }
        if (job) {
            job->queued = false;
            job->pins++;
            pthread_mutex_unlock(&server->lock);
            process_job(server, job);
            pthread_mutex_lock(&server->lock);
            job->pins--;
            prune_jobs(server);
            continue;
        }
        time_t now = time(NULL);
        if (now - last_poll >= P1600_AIR_STATE_CACHE_SECONDS) {
            last_poll = now;
            expire_held_jobs(server);
            prune_jobs(server);
            if (!server->spool_dir[0]) {
                pthread_mutex_unlock(&server->lock);
                poll_cups_jobs(server);
                pthread_mutex_lock(&server->lock);
            }
            continue;
        }
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += 1;
        pthread_cond_timedwait(&server->work, &server->lock, &deadline);
    }
    pthread_mutex_unlock(&server->lock);
    return NULL;
}

/* --- HTTP --------------------------------------------------------------------------- */

static bool send_response(Client *client, http_status_t code, const char *type, const void *body, size_t length) {
    http_t *http = client->http;
    if (code == HTTP_STATUS_CONTINUE) return (int)httpWriteResponse(http, HTTP_STATUS_CONTINUE) >= 0;
    char text[160];
    if (!type && !body && code != HTTP_STATUS_OK) {
        snprintf(text, sizeof(text), "%d - %s\n", (int)code, httpStatus(code));
        type = "text/plain; charset=utf-8";
        body = text;
        length = strlen(text);
    }
    bool head = client->operation == HTTP_STATE_HEAD;
    httpClearFields(http);
    if (code == HTTP_STATUS_METHOD_NOT_ALLOWED || client->operation == HTTP_STATE_OPTIONS) {
        httpSetField(http, HTTP_FIELD_ALLOW, "GET, HEAD, OPTIONS, POST");
    }
    if (type) httpSetField(http, HTTP_FIELD_CONTENT_TYPE, type);
    if (client->must_close || !client->keep_alive) {
        httpSetKeepAlive(http, HTTP_KEEPALIVE_OFF);
        httpSetField(http, HTTP_FIELD_CONNECTION, "close");
    } else {
        httpSetField(http, HTTP_FIELD_KEEP_ALIVE, "timeout=30");
    }
    httpSetLength(http, head ? 0 : length);
    if ((int)httpWriteResponse(http, code) < 0) return false;
    if (!head && length > 0 && httpWrite2(http, body, length) < 0) return false;
    return httpFlushWrite(http) >= 0;
}

/* Reads and drops the rest of a request body; false when it is too long or
 * broken (the connection then closes). */
static bool drain_body(Client *client) {
    if (client->must_close) return false;
    char *buffer = malloc(IO_BUFFER_SIZE);
    size_t total = 0;
    bool ok = buffer != NULL;
    while (ok && httpGetState(client->http) == HTTP_STATE_POST_RECV) {
        ssize_t count = httpRead2(client->http, buffer, IO_BUFFER_SIZE);
        if (count == 0) break;
        if (count < 0) ok = false;
        total += count > 0 ? (size_t)count : 0U;
        if (total > DRAIN_LIMIT) ok = false;
    }
    free(buffer);
    if (!ok) client->must_close = true;
    return ok;
}

static ssize_t read_ipp(void *context, ipp_uchar_t *buffer, size_t length) {
    Client *client = context;
    size_t done = 0;
    while (done < length) {
        if (client->ipp_bytes >= P1600_AIR_IPP_LIMIT) {
            client->ipp_too_large = true;
            break;
        }
        size_t want_bytes = length - done;
        if (want_bytes > P1600_AIR_IPP_LIMIT - client->ipp_bytes) want_bytes = P1600_AIR_IPP_LIMIT - client->ipp_bytes;
        ssize_t count = httpRead2(client->http, (char *)buffer + done, want_bytes);
        if (count <= 0) break;
        done += (size_t)count;
        client->ipp_bytes += (size_t)count;
    }
    return (ssize_t)done;
}

typedef struct {
    char *data;
    size_t length, capacity;
    bool failed;
} Buffer;

static void buffer_append(Buffer *buffer, const char *text, size_t length) {
    if (buffer->failed) return;
    if (buffer->length + length + 1U > buffer->capacity) {
        size_t capacity = buffer->capacity ? buffer->capacity : 4096U;
        while (capacity < buffer->length + length + 1U) capacity *= 2U;
        char *grown = realloc(buffer->data, capacity);
        if (!grown) {
            buffer->failed = true;
            return;
        }
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->length, text, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
}

static void buffer_printf(Buffer *buffer, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void buffer_printf(Buffer *buffer, const char *format, ...) {
    char text[2048];
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    if (length > 0) buffer_append(buffer, text, (size_t)length < sizeof(text) ? (size_t)length : sizeof(text) - 1U);
}

static void buffer_html(Buffer *buffer, const char *text) {
    for (const char *p = text ? text : ""; *p; p++) {
        switch (*p) {
        case '&': buffer_append(buffer, "&amp;", 5); break;
        case '<': buffer_append(buffer, "&lt;", 4); break;
        case '>': buffer_append(buffer, "&gt;", 4); break;
        case '"': buffer_append(buffer, "&quot;", 6); break;
        case '\'': buffer_append(buffer, "&#39;", 5); break;
        default: buffer_append(buffer, p, 1); break;
        }
    }
}

static const char *state_name(int state) {
    switch (state) {
    case IPP_PSTATE_IDLE: return "idle";
    case IPP_PSTATE_PROCESSING: return "processing";
    default: return "stopped";
    }
}

static bool send_status_page(Client *client) {
    P1600AirServer *server = client->server;
    for (int i = 0; i < server->printer_count; i++) refresh_printer(server, &server->printers[i], false);
    Buffer page = {0};
    buffer_printf(&page, "<!DOCTYPE html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">"
                         "<meta name=\"viewport\" content=\"width=device-width\"><title>P1600dn AirPrint</title>"
                         "<style>body{font-family:-apple-system,Helvetica,sans-serif;margin:2em;color:#222}"
                         "table{border-collapse:collapse}td,th{border:1px solid #ccc;padding:.3em .6em;text-align:left}"
                         "</style></head><body><h1>HP LaserJet Professional P1600dn series &mdash; AirPrint</h1>"
                         "<table><tr><th>Printer</th><th>Model</th><th>State</th><th>IPP URI</th></tr>\n");
    pthread_mutex_lock(&server->lock);
    for (int i = 0; i < server->printer_count; i++) {
        Printer *printer = &server->printers[i];
        PrinterState state;
        printer_state(server, printer, &state);
        char uri[HTTP_MAX_URI];
        make_uri(client, "ipp", printer->resource, uri, sizeof(uri));
        buffer_append(&page, "<tr><td>", 8);
        buffer_html(&page, printer->info);
        buffer_append(&page, "</td><td>", 9);
        buffer_html(&page, printer->make_model);
        buffer_append(&page, "</td><td>", 9);
        buffer_html(&page, state_name(state.state));
        if (state.message[0]) {
            buffer_append(&page, ": ", 2);
            buffer_html(&page, state.message);
        }
        buffer_append(&page, "</td><td>", 9);
        buffer_html(&page, uri);
        buffer_append(&page, "</td></tr>\n", 11);
    }
    buffer_printf(&page, "</table><h2>Recent jobs</h2><table><tr><th>Job</th><th>Printer</th><th>Name</th>"
                         "<th>User</th><th>State</th></tr>\n");
    int shown = 0;
    P1600AirJob *list[P1600_AIR_JOB_HISTORY * 2];
    int count = 0;
    for (P1600AirJob *job = server->jobs; job && count < (int)COUNT(list); job = job->next) list[count++] = job;
    for (int i = count - 1; i >= 0 && shown < 20; i--, shown++) {
        P1600AirJob *job = list[i];
        buffer_printf(&page, "<tr><td>%d</td><td>", job->id);
        buffer_html(&page, server->printers[job->printer].name);
        buffer_append(&page, "</td><td>", 9);
        buffer_html(&page, job->name);
        buffer_append(&page, "</td><td>", 9);
        buffer_html(&page, job->user);
        buffer_append(&page, "</td><td>", 9);
        buffer_html(&page, ippEnumString("job-state", (int)job->state));
        if (job->message[0]) {
            buffer_append(&page, ": ", 2);
            buffer_html(&page, job->message);
        }
        buffer_append(&page, "</td></tr>\n", 11);
    }
    pthread_mutex_unlock(&server->lock);
    buffer_printf(&page, "</table></body></html>\n");
    bool ok = !page.failed ? send_response(client, HTTP_STATUS_OK, "text/html; charset=utf-8", page.data, page.length)
                           : send_response(client, HTTP_STATUS_SERVER_ERROR, NULL, NULL, 0);
    free(page.data);
    return ok;
}

/* A request body the server does not want: a small one that is already on
 * its way is read and dropped, otherwise the connection closes after the
 * answer (lingering, so the answer is not lost to a reset). */
static void refuse_body(Client *client) {
    http_t *http = client->http;
    off_t length = httpGetLength2(http);
    if (httpGetState(http) == HTTP_STATE_POST_RECV && !httpGetExpect(http) && !httpIsChunked(http) && length >= 0 &&
        length <= (off_t)SMALL_BODY && drain_body(client)) {
        return;
    }
    client->must_close = true;
    client->unread_body = true;
}

/* One HTTP request; false when the connection should close. */
static bool process_http(Client *client) {
    P1600AirServer *server = client->server;
    http_t *http = client->http;
    char uri[1024] = "";
    http_state_t state;
    int waits = 0;
    int header_seconds = server->config.header_seconds > 0 ? server->config.header_seconds : P1600_AIR_HEADER_SECONDS;
    pthread_mutex_lock(&server->lock);
    client->header_deadline = time(NULL) + header_seconds;
    pthread_mutex_unlock(&server->lock);
    while ((state = httpReadRequest(http, uri, sizeof(uri))) == HTTP_STATE_WAITING) {
        if (++waits > P1600_AIR_IDLE_SECONDS * 100 || atomic_load(&server->stopping)) return false;
        usleep(10000);
    }
    client->operation = state;
    client->must_close = false;
    client->unread_body = false;
    client->keep_alive = false;
    if (state == HTTP_STATE_ERROR) return false;
    if (state == HTTP_STATE_UNKNOWN_METHOD || state == HTTP_STATE_UNKNOWN_VERSION) {
        client->must_close = true;
        send_response(client, state == HTTP_STATE_UNKNOWN_METHOD ? HTTP_STATUS_METHOD_NOT_ALLOWED
                                                                  : HTTP_STATUS_NOT_SUPPORTED,
                      NULL, NULL, 0);
        return false;
    }
    http_status_t status;
    while ((status = httpUpdate(http)) == HTTP_STATUS_CONTINUE) {
    }
    pthread_mutex_lock(&server->lock);
    client->header_deadline = 0;
    pthread_mutex_unlock(&server->lock);
    if (status != HTTP_STATUS_OK) {
        client->must_close = true;
        send_response(client, HTTP_STATUS_BAD_REQUEST, NULL, NULL, 0);
        return false;
    }
    http_version_t version = httpGetVersion(http);
    const char *connection = httpGetField(http, HTTP_FIELD_CONNECTION);
    client->keep_alive = version >= HTTP_VERSION_1_1 ? strcasecmp(connection ? connection : "", "close") != 0
                                                     : strcasecmp(connection ? connection : "", "keep-alive") == 0;
    bool body = httpGetLength2(http) > 0 || httpIsChunked(http);
    const char *host_field = httpGetField(http, HTTP_FIELD_HOST);
    if (version >= HTTP_VERSION_1_1 && (!host_field || !*host_field)) {
        client->must_close = true;
        client->unread_body = body;
        send_response(client, HTTP_STATUS_BAD_REQUEST, NULL, NULL, 0);
        return false;
    }
    client->uri_port = server->port;
    if (!parse_host(host_field, client->uri_host, sizeof(client->uri_host), &client->uri_port)) {
        snprintf(client->uri_host, sizeof(client->uri_host), "%s", server->host_name);
        client->uri_port = server->port;
    }
    char scheme[32] = "", userpass[256] = "", host[256] = "", resource[1024] = "";
    int port = 0;
    if (httpSeparateURI(HTTP_URI_CODING_MOST, uri, scheme, sizeof(scheme), userpass, sizeof(userpass), host,
                        sizeof(host), &port, resource, sizeof(resource)) < HTTP_URI_STATUS_OK &&
        !(state == HTTP_STATE_OPTIONS && strcmp(uri, "*") == 0)) {
        client->must_close = true;
        client->unread_body = body;
        send_response(client, HTTP_STATUS_BAD_REQUEST, NULL, NULL, 0);
        return false;
    }
    char *query = strchr(resource, '?');
    if (query) *query = '\0';
    const char *coding = httpGetField(http, HTTP_FIELD_CONTENT_ENCODING);
    if (coding && *coding && strcasecmp(coding, "identity") != 0 && strcasecmp(coding, "gzip") != 0 &&
        strcasecmp(coding, "x-gzip") != 0 && strcasecmp(coding, "deflate") != 0) {
        client->must_close = true;
        client->unread_body = body;
        send_response(client, HTTP_STATUS_UNSUPPORTED_MEDIATYPE, NULL, NULL, 0);
        return false;
    }

    int job_id = 0;
    Printer *printer = find_printer(server, resource, &job_id);
    if (state != HTTP_STATE_POST) {
        /* Only POST carries a body here. */
        if (body) {
            client->must_close = true;
            client->unread_body = true;
        }
        bool ok;
        if (state == HTTP_STATE_OPTIONS) {
            ok = send_response(client, HTTP_STATUS_OK, NULL, "", 0);
        } else if (state == HTTP_STATE_GET || state == HTTP_STATE_HEAD) {
            VERBOSE("%s %s %s", client->peer, state == HTTP_STATE_GET ? "GET" : "HEAD", resource);
            if (strcmp(resource, "/") == 0) {
                ok = send_status_page(client);
            } else if (strcmp(resource, "/icon.png") == 0 && server->icon) {
                ok = send_response(client, HTTP_STATUS_OK, "image/png", server->icon, server->icon_size);
            } else {
                ok = send_response(client, printer ? HTTP_STATUS_METHOD_NOT_ALLOWED : HTTP_STATUS_NOT_FOUND, NULL,
                                   NULL, 0);
            }
        } else {
            ok = send_response(client, HTTP_STATUS_METHOD_NOT_ALLOWED, NULL, NULL, 0);
        }
        return ok && !client->must_close && client->keep_alive;
    }

    /* POST: IPP requests to a printer resource only. */
    const char *content_type = httpGetField(http, HTTP_FIELD_CONTENT_TYPE);
    http_status_t refusal = HTTP_STATUS_OK;
    if (!printer) {
        refusal = HTTP_STATUS_NOT_FOUND;
    } else if (!content_type || strncasecmp(content_type, "application/ipp", 15) != 0 ||
               (content_type[15] != '\0' && content_type[15] != ';' && content_type[15] != ' ')) {
        refusal = HTTP_STATUS_BAD_REQUEST;
    } else if (httpGetLength2(http) > (off_t)DRAIN_LIMIT) {
        refusal = HTTP_STATUS_REQUEST_TOO_LARGE;
    } else if (httpGetExpect(http) && httpGetExpect(http) != HTTP_STATUS_CONTINUE) {
        refusal = HTTP_STATUS_EXPECTATION_FAILED;
    }
    if (refusal != HTTP_STATUS_OK) {
        VERBOSE("%s POST %s: %s", client->peer, resource, httpStatus(refusal));
        if (body) refuse_body(client);
        bool ok = send_response(client, refusal, NULL, NULL, 0);
        return ok && !client->must_close && client->keep_alive;
    }
    if (httpGetExpect(http) == HTTP_STATUS_CONTINUE && !send_response(client, HTTP_STATUS_CONTINUE, NULL, NULL, 0)) {
        return false;
    }
    client->ipp_bytes = 0;
    client->ipp_too_large = false;
    ipp_t *request = ippNew();
    ipp_state_t ipp_state = IPP_STATE_IDLE;
    for (int round = 0; round < 1000 && ipp_state != IPP_STATE_DATA && ipp_state != IPP_STATE_ERROR; round++) {
        ipp_state = ippReadIO(client, read_ipp, 1, NULL, request);
    }
    if (ipp_state != IPP_STATE_DATA) {
        ippDelete(request);
        VERBOSE("%s POST %s: bad IPP message", client->peer, resource);
        client->must_close = true;
        client->unread_body = httpGetState(http) == HTTP_STATE_POST_RECV;
        send_response(client, client->ipp_too_large ? HTTP_STATUS_REQUEST_TOO_LARGE : HTTP_STATUS_BAD_REQUEST, NULL,
                      NULL, 0);
        return false;
    }
    ipp_t *response = process_ipp(client, printer, job_id, request);
    ippDelete(request);
    /* Whatever the operation did not read is dropped. */
    if (httpGetState(http) == HTTP_STATE_POST_RECV && !drain_body(client)) client->unread_body = true;
    httpClearFields(http);
    httpSetField(http, HTTP_FIELD_CONTENT_TYPE, "application/ipp");
    if (client->must_close || !client->keep_alive) {
        httpSetKeepAlive(http, HTTP_KEEPALIVE_OFF);
        httpSetField(http, HTTP_FIELD_CONNECTION, "close");
    } else {
        httpSetField(http, HTTP_FIELD_KEEP_ALIVE, "timeout=30");
    }
    httpSetLength(http, ippLength(response));
    bool ok = (int)httpWriteResponse(http, HTTP_STATUS_OK) >= 0;
    if (ok) {
        ippSetState(response, IPP_STATE_IDLE);
        ok = ippWrite(http, response) == IPP_STATE_DATA && httpFlushWrite(http) >= 0;
    }
    ippDelete(response);
    return ok && !client->must_close && client->keep_alive;
}

/* After an early answer: stop sending, then read what the client still
 * sends for a moment so the answer is not lost to a reset. */
static void linger(Client *client) {
    shutdown(client->fd, SHUT_WR);
    char buffer[4096];
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000L + (now.tv_nsec - start.tv_nsec) / 1000000L;
        if (elapsed >= LINGER_MS) break;
        struct pollfd channel = {.fd = client->fd, .events = POLLIN};
        int ready = poll(&channel, 1, (int)(LINGER_MS - elapsed));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) break;
        ssize_t count = recv(client->fd, buffer, sizeof(buffer), 0);
        if (count <= 0) break;
    }
}

static void *client_main(void *argument) {
    Client *client = argument;
    P1600AirServer *server = client->server;
    httpBlocking(client->http, 1);
    httpSetTimeout(client->http, P1600_AIR_IDLE_SECONDS, NULL, NULL);
    while (!atomic_load(&server->stopping)) {
        if (!httpWait(client->http, P1600_AIR_IDLE_SECONDS * 1000)) break;
        if (!process_http(client)) break;
    }
    if (client->unread_body && !atomic_load(&server->stopping)) linger(client);
    pthread_mutex_lock(&server->lock);
    client->closing = true;
    pthread_mutex_unlock(&server->lock);
    httpClose(client->http);
    client->http = NULL;
    pthread_mutex_lock(&server->lock);
    client->done = true;
    pthread_mutex_unlock(&server->lock);
    return NULL;
}

/* Under the lock: closes connections whose request header is late. Their
 * thread then sees the socket end and finishes. */
static void cut_slow_clients(P1600AirServer *server) {
    time_t now = time(NULL);
    for (int i = 0; i < P1600_AIR_MAX_CLIENTS; i++) {
        Client *client = server->clients[i];
        if (!client || client->closing || client->cut || !client->header_deadline || now < client->header_deadline) {
            continue;
        }
        client->cut = true;
        p1600_air_log("closing a connection from %s: its request header took too long", client->peer);
        shutdown(client->fd, SHUT_RDWR);
    }
}

/* Under the lock: joins finished connection threads. */
static void reap_clients(P1600AirServer *server, bool all) {
    for (int i = 0; i < P1600_AIR_MAX_CLIENTS; i++) {
        Client *client = server->clients[i];
        if (!client || (!client->done && !all)) continue;
        server->clients[i] = NULL;
        pthread_mutex_unlock(&server->lock);
        pthread_join(client->thread, NULL);
        free(client);
        pthread_mutex_lock(&server->lock);
    }
}

static void accept_client(P1600AirServer *server, int listener) {
    http_t *http = httpAcceptConnection(listener, 1);
    if (!http) return;
    int fd = httpGetFd(http);
    /* Accepted sockets inherit O_NONBLOCK from the listener on macOS. */
    int flags = fcntl(fd, F_GETFL);
    if (flags >= 0) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
    struct timeval timeout = {P1600_AIR_IDLE_SECONDS, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    Client *client = calloc(1, sizeof(*client));
    if (!client) {
        httpClose(http);
        return;
    }
    client->server = server;
    client->http = http;
    client->fd = fd;
    httpGetHostname(http, client->peer, (int)sizeof(client->peer));
    int per_host = server->config.max_clients_per_host > 0 ? server->config.max_clients_per_host
                                                            : P1600_AIR_MAX_CLIENTS_PER_HOST;
    pthread_mutex_lock(&server->lock);
    reap_clients(server, false);
    int slot = -1, from_host = 0;
    for (int i = 0; i < P1600_AIR_MAX_CLIENTS; i++) {
        if (!server->clients[i]) {
            if (slot < 0) slot = i;
        } else if (strcmp(server->clients[i]->peer, client->peer) == 0) {
            from_host++;
        }
    }
    bool host_full = from_host >= per_host;
    bool started = slot >= 0 && !host_full && pthread_create(&client->thread, NULL, client_main, client) == 0;
    if (started) server->clients[slot] = client;
    pthread_mutex_unlock(&server->lock);
    if (!started) {
        p1600_air_log("refusing a connection from %s: %s", client->peer,
                   host_full  ? "too many connections from this address"
                   : slot < 0 ? "too many connections"
                              : "no thread");
        httpClose(http);
        free(client);
        return;
    }
    VERBOSE("connection from %s", client->peer);
}

/* --- Bonjour ----------------------------------------------------------------------- */

static void DNSSD_API registered(DNSServiceRef reference, DNSServiceFlags flags, DNSServiceErrorType error,
                                 const char *name, const char *type, const char *domain, void *context) {
    (void)reference;
    (void)flags;
    (void)domain;
    Printer *printer = context;
    if (error != kDNSServiceErr_NoError) {
        p1600_air_log("Bonjour registration of %s for %s failed (%d)", type, printer->name, (int)error);
        return;
    }
    if (strncmp(type, "_ipp.", 5) == 0) {
        pthread_mutex_lock(&printer->server->lock);
        copy_text(printer->dns_sd_name, sizeof(printer->dns_sd_name), name, 63);
        pthread_mutex_unlock(&printer->server->lock);
        p1600_air_log("Bonjour: \"%s\" (%s) for %s", name, type, printer->name);
    }
}

static void dnssd_stop(P1600AirServer *server) {
    for (int i = 0; i < server->printer_count; i++) {
        Printer *printer = &server->printers[i];
        if (printer->ipp_ref) DNSServiceRefDeallocate(printer->ipp_ref);
        if (printer->printer_ref) DNSServiceRefDeallocate(printer->printer_ref);
        printer->ipp_ref = printer->printer_ref = NULL;
    }
    if (server->dnssd) DNSServiceRefDeallocate(server->dnssd);
    server->dnssd = NULL;
}

static int dnssd_start(P1600AirServer *server) {
    if (DNSServiceCreateConnection(&server->dnssd) != kDNSServiceErr_NoError) {
        server->dnssd = NULL;
        p1600_air_log("Bonjour is not available");
        return -1;
    }
    uint32_t interface = server->config.local_only ? kDNSServiceInterfaceIndexLocalOnly : kDNSServiceInterfaceIndexAny;
    char admin_url[HTTP_MAX_URI];
    httpAssembleURI(HTTP_URI_CODING_ALL, admin_url, sizeof(admin_url), "http", NULL, server->host_name, server->port,
                    "/");
    for (int i = 0; i < server->printer_count; i++) {
        Printer *printer = &server->printers[i];
        char location[256];
        pthread_mutex_lock(&server->lock);
        snprintf(location, sizeof(location), "%s", printer->location);
        pthread_mutex_unlock(&server->lock);
        unsigned char txt[2048];
        uint16_t length = 0;
        P1600AirTxt record = {printer->resource + 1, printer->make_model, location, admin_url,
                           printer->uuid + 9 /* without urn:uuid: */};
        if (p1600_air_txt_record(&record, txt, sizeof(txt), &length) != 0) {
            p1600_air_log("Bonjour TXT record for %s is too large", printer->name);
            continue;
        }
        printer->ipp_ref = server->dnssd;
        DNSServiceErrorType error =
            DNSServiceRegister(&printer->ipp_ref, kDNSServiceFlagsShareConnection, interface, printer->service_name,
                               "_ipp._tcp,_universal,_print", NULL, NULL, htons((uint16_t)server->port), length, txt,
                               registered, printer);
        if (error != kDNSServiceErr_NoError) {
            printer->ipp_ref = NULL;
            p1600_air_log("Bonjour registration for %s failed (%d)", printer->name, (int)error);
            continue;
        }
        /* Apple: claim the name for LPD as well, on port 0 (not served). */
        printer->printer_ref = server->dnssd;
        error = DNSServiceRegister(&printer->printer_ref, kDNSServiceFlagsShareConnection, interface,
                                   printer->service_name, "_printer._tcp", NULL, NULL, 0, 0, NULL, registered, printer);
        if (error != kDNSServiceErr_NoError) printer->printer_ref = NULL;
    }
    return 0;
}

/* --- lifecycle ---------------------------------------------------------------------- */

static int open_listener(int family, bool loopback, int *port, char *error, size_t error_size) {
    int fd = socket(family, SOCK_STREAM, 0);
    if (fd < 0) return fail(error, error_size, "socket: %s", strerror(errno));
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    int bound;
    if (family == AF_INET6) {
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on));
        struct sockaddr_in6 address;
        memset(&address, 0, sizeof(address));
        address.sin6_len = sizeof(address);
        address.sin6_family = AF_INET6;
        address.sin6_addr = loopback ? in6addr_loopback : in6addr_any;
        address.sin6_port = htons((uint16_t)*port);
        bound = bind(fd, (struct sockaddr *)&address, sizeof(address));
    } else {
        struct sockaddr_in address;
        memset(&address, 0, sizeof(address));
        address.sin_len = sizeof(address);
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(loopback ? INADDR_LOOPBACK : INADDR_ANY);
        address.sin_port = htons((uint16_t)*port);
        bound = bind(fd, (struct sockaddr *)&address, sizeof(address));
    }
    if (bound != 0 || listen(fd, 64) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return fail(error, error_size, "%s port %d: %s", family == AF_INET6 ? "IPv6" : "IPv4", *port, strerror(saved));
    }
    if (*port == 0) {
        struct sockaddr_storage address;
        socklen_t length = sizeof(address);
        if (getsockname(fd, (struct sockaddr *)&address, &length) == 0) {
            *port = ntohs(family == AF_INET6 ? ((struct sockaddr_in6 *)&address)->sin6_port
                                             : ((struct sockaddr_in *)&address)->sin_port);
        }
    }
    int flags = fcntl(fd, F_GETFL);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

static int open_listeners(P1600AirServer *server, char *error, size_t error_size) {
    for (int attempt = 0; attempt < 8; attempt++) {
        int port = server->config.port;
        int v4 = open_listener(AF_INET, server->config.loopback, &port, error, error_size);
        if (v4 < 0) return -1;
        char v6_error[256];
        int v6 = open_listener(AF_INET6, server->config.loopback, &port, v6_error, sizeof(v6_error));
        if (v6 < 0 && errno == EADDRINUSE && server->config.port == 0) {
            close(v4);  /* the free IPv4 port is taken on IPv6: try another */
            continue;
        }
        server->listeners[0] = v4;
        server->listener_count = 1;
        if (v6 >= 0) {
            server->listeners[server->listener_count++] = v6;
        } else {
            p1600_air_log("no IPv6 listener (%s)", v6_error);
        }
        server->port = port;
        return 0;
    }
    return fail(error, error_size, "No free port for IPv4 and IPv6");
}

static void cf_string(CFStringRef string, char *out, size_t size) {
    out[0] = '\0';
    if (string) {
        if (!CFStringGetCString(string, out, (CFIndex)size, kCFStringEncodingUTF8)) out[0] = '\0';
        CFRelease(string);
    }
}

static void host_names(P1600AirServer *server) {
    cf_string(SCDynamicStoreCopyComputerName(NULL, NULL), server->computer_name, sizeof(server->computer_name));
    char local[256];
    cf_string(SCDynamicStoreCopyLocalHostName(NULL), local, sizeof(local));
    char hostname[256] = "";
    if (gethostname(hostname, sizeof(hostname) - 1U) != 0) hostname[0] = '\0';
    hostname[sizeof(hostname) - 1U] = '\0';
    char *dot = strstr(hostname, ".local");
    if (!local[0]) {
        snprintf(local, sizeof(local), "%s", hostname[0] ? hostname : "localhost");
        dot = strchr(local, '.');
        if (dot) *dot = '\0';
    }
    snprintf(server->host_name, sizeof(server->host_name), "%s.local", local);
    if (!server->computer_name[0]) {
        if (dot) *dot = '\0';
        snprintf(server->computer_name, sizeof(server->computer_name), "%s", hostname[0] ? hostname : local);
    }
}

static void load_icon(P1600AirServer *server, const char *path) {
    if (!path || !*path) return;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat info;
    if (fd < 0 || fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
        (size_t)info.st_size > ICON_LIMIT) {
        if (fd >= 0) close(fd);
        p1600_air_log("no printer icon at %s", path);
        return;
    }
    unsigned char *icon = malloc((size_t)info.st_size);
    size_t done = 0;
    while (icon && done < (size_t)info.st_size) {
        ssize_t count = read(fd, icon + done, (size_t)info.st_size - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        done += (size_t)count;
    }
    close(fd);
    if (!icon || done != (size_t)info.st_size || done < 8 || memcmp(icon, "\x89PNG", 4) != 0) {
        free(icon);
        p1600_air_log("the printer icon %s is not a PNG file", path);
        return;
    }
    server->icon = icon;
    server->icon_size = done;
}

static void device_id(const char *make_model, char *out, size_t size) {
    char model[256];
    const char *start = make_model;
    if (strncasecmp(start, "HP ", 3) == 0) start += 3;
    if (strncasecmp(start, "Hewlett-Packard ", 16) == 0) start += 16;
    copy_text(model, sizeof(model), start, 200);
    for (char *p = model; *p; p++) {
        if (*p == ';' || *p == ':') *p = ' ';
    }
    snprintf(out, size, "MFG:HP;MDL:%s;CMD:PDF,URF,JPEG,PNG,PWGRaster;CLS:PRINTER;", model);
}

static void remove_temp_dir(P1600AirServer *server) {
    if (!server->temp_dir[0]) return;
    /* Only this server's own temporary documents live here. */
    for (P1600AirJob *job = server->jobs; job; job = job->next) discard_document(job);
    if (rmdir(server->temp_dir) != 0 && errno == ENOTEMPTY) {
        p1600_air_log("temporary directory %s is not empty", server->temp_dir);
    }
    server->temp_dir[0] = '\0';
}

P1600AirServer *p1600_air_server_create(const P1600AirConfig *config, char *error, size_t error_size) {
    if (!config || config->queue_count < 1 || config->queue_count > P1600_AIR_MAX_QUEUES || config->port < 0 ||
        config->port > 65535) {
        fail(error, error_size, "Invalid server configuration");
        return NULL;
    }
    P1600AirServer *server = calloc(1, sizeof(*server));
    if (!server) {
        fail(error, error_size, "Out of memory");
        return NULL;
    }
    server->config = *config;
    server->listeners[0] = server->listeners[1] = -1;
    atomic_init(&server->stopping, false);
    atomic_store(&verbose_level, config->verbose);
    server->start_time = time(NULL);
    server->next_job_id = 1;
    pthread_mutex_init(&server->lock, NULL);
    pthread_cond_init(&server->work, NULL);
    if (config->spool_dir && *config->spool_dir) {
        struct stat info;
        if (!realpath(config->spool_dir, server->spool_dir) || stat(server->spool_dir, &info) != 0 ||
            !S_ISDIR(info.st_mode)) {
            fail(error, error_size, "Spool directory %s is not available", config->spool_dir);
            goto failed;
        }
    }
    snprintf(server->cups_server, sizeof(server->cups_server), "%s",
             config->cups.server ? config->cups.server : "");
    server->config.cups.server = server->cups_server;
    server->config.spool_dir = server->spool_dir;
    server->config.icon_path = NULL;
    host_names(server);
    load_icon(server, config->icon_path);

    /* A private directory for received and converted documents. */
    const char *tmp = getenv("TMPDIR");
    char base[PATH_MAX];
    if (!tmp || !*tmp || tmp[0] != '/') {
        size_t length = confstr(_CS_DARWIN_USER_TEMP_DIR, base, sizeof(base));
        if (length == 0 || length > sizeof(base)) snprintf(base, sizeof(base), "/tmp");
    } else {
        snprintf(base, sizeof(base), "%s", tmp);
    }
    size_t base_length = strlen(base);
    while (base_length > 1 && base[base_length - 1] == '/') base[--base_length] = '\0';
    snprintf(server->temp_dir, sizeof(server->temp_dir), "%s/p1600dn-airprint.XXXXXX", base);
    if (!mkdtemp(server->temp_dir) || chmod(server->temp_dir, 0700) != 0) {
        fail(error, error_size, "Unable to create a temporary directory in %s: %s", base, strerror(errno));
        server->temp_dir[0] = '\0';
        goto failed;
    }

    for (int i = 0; i < config->queue_count; i++) {
        Printer *printer = &server->printers[i];
        const P1600AirQueueSpec *spec = &config->queues[i];
        if (!spec->name[0] || strchr(spec->name, '/') || strlen(spec->name) > 127) {
            fail(error, error_size, "Invalid queue name \"%s\"", spec->name);
            goto failed;
        }
        printer->server = server;
        printer->index = i;
        copy_text(printer->name, sizeof(printer->name), spec->name, 127);
        copy_text(printer->info, sizeof(printer->info), spec->info[0] ? spec->info : spec->name, 127);
        copy_text(printer->make_model, sizeof(printer->make_model),
                  spec->make_model[0] ? spec->make_model : "HP LaserJet Professional P1600dn Series", 127);
        snprintf(printer->resource, sizeof(printer->resource), "/ipp/print/%s", printer->name);
        p1600_air_uuid(server->host_name, printer->name, printer->uuid, sizeof(printer->uuid));
        p1600_air_service_name(printer->info, server->computer_name, printer->service_name, sizeof(printer->service_name));
        device_id(printer->make_model, printer->device_id, sizeof(printer->device_id));
        server->printer_count++;
        for (int j = 0; j < i; j++) {
            if (strcasecmp(server->printers[j].name, printer->name) == 0) {
                fail(error, error_size, "Queue \"%s\" is listed twice", printer->name);
                goto failed;
            }
        }
    }
    for (int i = 0; i < server->printer_count; i++) refresh_printer(server, &server->printers[i], true);
    if (open_listeners(server, error, error_size) != 0) goto failed;
    if (pthread_create(&server->worker, NULL, worker_main, server) != 0) {
        fail(error, error_size, "Unable to start the job thread");
        goto failed;
    }
    server->worker_started = true;
    return server;

failed:
    p1600_air_server_free(server);
    return NULL;
}

int p1600_air_server_port(const P1600AirServer *server) {
    return server ? server->port : 0;
}

int p1600_air_server_run(P1600AirServer *server, int stop_fd) {
    if (!server) return -1;
    p1600_air_log("listening on port %d", server->port);
    for (int i = 0; i < server->printer_count; i++) {
        p1600_air_log("serving %s (%s) at %s, %s", server->printers[i].name, server->printers[i].make_model,
                   server->printers[i].resource, server->spool_dir[0] ? "spool directory" : "CUPS");
    }
    int retry_seconds = server->config.dnssd_retry_seconds > 0 ? server->config.dnssd_retry_seconds
                                                               : P1600_AIR_DNSSD_RETRY_SECONDS;
    /* Bonjour not there yet (mDNSResponder starting or restarting): try
     * again, as after a lost connection, instead of serving unseen. */
    if (server->config.dnssd && dnssd_start(server) != 0) server->dnssd_retry = time(NULL) + retry_seconds;
    while (!atomic_load(&server->stopping)) {
        struct pollfd channels[4];
        int count = 0;
        for (int i = 0; i < server->listener_count; i++) {
            channels[count++] = (struct pollfd){.fd = server->listeners[i], .events = POLLIN};
        }
        int dnssd_index = -1, stop_index = -1;
        if (server->dnssd) {
            dnssd_index = count;
            channels[count++] = (struct pollfd){.fd = DNSServiceRefSockFD(server->dnssd), .events = POLLIN};
        }
        if (stop_fd >= 0) {
            stop_index = count;
            channels[count++] = (struct pollfd){.fd = stop_fd, .events = POLLIN};
        }
        int ready = poll(channels, (nfds_t)count, 1000);
        if (ready < 0 && errno != EINTR) {
            p1600_air_log("poll: %s", strerror(errno));
            break;
        }
        if (stop_index >= 0 && channels[stop_index].revents) break;
        for (int i = 0; ready > 0 && i < server->listener_count; i++) {
            if (channels[i].revents & POLLIN) accept_client(server, server->listeners[i]);
        }
        if (dnssd_index >= 0 && channels[dnssd_index].revents) {
            DNSServiceErrorType error = DNSServiceProcessResult(server->dnssd);
            if (error != kDNSServiceErr_NoError) {
                p1600_air_log("Bonjour connection lost (%d); registering again in %d seconds", (int)error, retry_seconds);
                dnssd_stop(server);
                server->dnssd_retry = time(NULL) + retry_seconds;
            }
        }
        if (server->config.dnssd && !server->dnssd && server->dnssd_retry && time(NULL) >= server->dnssd_retry) {
            server->dnssd_retry = 0;
            if (dnssd_start(server) != 0) server->dnssd_retry = time(NULL) + 3 * retry_seconds;  /* 30 s */
        }
        pthread_mutex_lock(&server->lock);
        cut_slow_clients(server);
        reap_clients(server, false);
        pthread_mutex_unlock(&server->lock);
    }
    p1600_air_log("shutting down");
    dnssd_stop(server);
    return 0;
}

void p1600_air_server_free(P1600AirServer *server) {
    if (!server) return;
    atomic_store(&server->stopping, true);
    dnssd_stop(server);
    for (int i = 0; i < server->listener_count; i++) close(server->listeners[i]);
    server->listener_count = 0;
    pthread_mutex_lock(&server->lock);
    for (int i = 0; i < P1600_AIR_MAX_CLIENTS; i++) {
        Client *client = server->clients[i];
        if (client && !client->closing) shutdown(client->fd, SHUT_RDWR);
    }
    reap_clients(server, true);
    pthread_cond_broadcast(&server->work);
    pthread_mutex_unlock(&server->lock);
    if (server->worker_started) pthread_join(server->worker, NULL);
    remove_temp_dir(server);
    while (server->jobs) {
        P1600AirJob *job = server->jobs;
        server->jobs = job->next;
        free_job(job);
    }
    for (int i = 0; i < server->printer_count; i++) ippDelete(server->printers[i].mirror);
    free(server->icon);
    pthread_cond_destroy(&server->work);
    pthread_mutex_destroy(&server->lock);
    free(server);
}
