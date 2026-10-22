/*
 * rastertop1600dn - CUPS raster filter for the HP LaserJet Professional
 * P1600dn series (P1606dn, P1607dn, P1608dn, P1609dn).
 *
 * Produces the PJL/ZjStream job layout the printer expects: 600 dpi gray
 * halftoned with the project's own screens, the printer's page items and
 * paper/media codes, one JBIG image per page, automatic duplex as
 * 2,1,4,3,... with a blank partner for an odd last page, and manual duplex
 * (even pages, pause, odd pages) otherwise. It reads the printer's PJL
 * USTATUS replies from the CUPS backchannel during the job and reports them
 * as printer-state-reasons, and prints page progress lines. Every page
 * passes the page guard (zjs_guard.h) before it is sent: a page that is not
 * exactly what the printer expects is never sent.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <unistd.h>
#include <cups/cups.h>
#include <cups/raster.h>

#include "p1600_encoder_pool.h"
#include "p1600_raster.h"
#include "p1600_status.h"
#include "zjs.h"
#include "zjs_encoder.h"
#include "zjs_guard.h"

static int parse_int_range(const char *value, int minimum, int maximum, int *result) {
    if (!value || !result || *value == '\0') return -1;
    for (const unsigned char *p = (const unsigned char *)value; *p != '\0'; p++) {
        if (*p < (unsigned char)'0' || *p > (unsigned char)'9') return -1;
    }

    errno = 0;
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed < minimum || parsed > maximum) {
        return -1;
    }
    *result = (int)parsed;
    return 0;
}

static bool is_true(const char *value) {
    return value && (strcasecmp(value, "True") == 0 || strcasecmp(value, "Yes") == 0 ||
                     strcasecmp(value, "On") == 0 || strcmp(value, "1") == 0);
}

static bool is_false(const char *value) {
    return value && (strcasecmp(value, "False") == 0 || strcasecmp(value, "No") == 0 ||
                     strcasecmp(value, "Off") == 0 || strcmp(value, "0") == 0);
}

static const char *first_option(const char *const *names, int num_options, cups_option_t *options) {
    for (; *names; names++) {
        const char *value = cupsGetOption(*names, num_options, options);
        if (value) return value;
    }
    return NULL;
}

/* PPD 3.x queues send MediaType and InputSlot only as option names. */
static uint32_t legacy_media_code(const char *name) {
    static const struct { const char *name; uint32_t code; } media[] = {
        {"Standard", DMMEDIA_PLAIN}, {"Plain", DMMEDIA_PLAIN}, {"Paper", DMMEDIA_PLAIN},
        {"Transparency", DMMEDIA_TRANSPARENCY}, {"Glossy", 3}, {"Thick", 0x105},
        {"Cardstock", 0x105}, {"Postcard", 0x106}, {"Labels", DMMEDIA_LABELS},
        {"Envelope", DMMEDIA_ENVELOPE}, {"Letterhead", DMMEDIA_LETTERHEAD},
    };
    for (size_t i = 0; name && i < sizeof(media) / sizeof(media[0]); i++) {
        if (strcasecmp(name, media[i].name) == 0) return media[i].code;
    }
    return DMMEDIA_PLAIN;
}

static uint32_t legacy_source_code(const char *name) {
    if (!name) return DMBIN_AUTO;
    if (strcasecmp(name, "Manual") == 0 || strcasecmp(name, "Tray1") == 0) return DMBIN_MANUAL;
    if (strcasecmp(name, "Tray2") == 0 || strcasecmp(name, "Lower") == 0) return 2;
    if (strcasecmp(name, "Cassette") == 0) return 14;
    return DMBIN_AUTO;
}

/* The queue's own settings (installable options, and options set with
 * lpadmin -o or in the printer's settings) live in its PPD as
 * *Default<keyword>; cupsd does not repeat them in the job options, so the
 * filter reads them from the PPD itself. */
#define PPD_VALUE_SIZE 32

static bool ppd_default(const char *keyword, char value[PPD_VALUE_SIZE]) {
    const char *path = getenv("PPD");
    FILE *ppd = path ? fopen(path, "r") : NULL;
    if (!ppd) return false;

    char prefix[64];
    int prefix_length = snprintf(prefix, sizeof(prefix), "*Default%s:", keyword);
    bool found = false;
    char line[256];
    while (!found && prefix_length > 0 && (size_t)prefix_length < sizeof(prefix) && fgets(line, sizeof(line), ppd)) {
        if (strncmp(line, prefix, (size_t)prefix_length) == 0) {
            found = sscanf(line + prefix_length, " %31s", value) == 1;
            if (!found) break;
        }
    }
    fclose(ppd);
    return found;
}

/* The job option under any of its names, else the queue PPD's default for
 * the first name (the PPD's keyword). */
static const char *option_or_default(const char *const *names, int num_options, cups_option_t *options,
                                     char value[PPD_VALUE_SIZE]) {
    const char *option = first_option(names, num_options, options);
    if (option) return option;
    return ppd_default(names[0], value) ? value : NULL;
}

/* cupsd sends SIGTERM when the job is canceled, and stops the filters before
 * this one too, so the raster input ends. The job then stops at a page
 * boundary and is closed like a failed one (END_DOC, EOJ, USTATUSOFF): the
 * printer never gets part of a page and is ready again at once. SA_RESTART
 * keeps the page being written whole. */
static atomic_bool job_canceled;

static void cancel_job(int signal_number) {
    (void)signal_number;
    atomic_store(&job_canceled, true);
}

/* Manual duplex jobs are spooled and replayed in sheet order. */
typedef struct {
    off_t offset;
    off_t length;
} SpooledPage;

typedef struct {
    FILE *file;
    SpooledPage *pages;
    size_t count;
    size_t capacity;
} Spool;

static int spool_reserve(Spool *spool) {
    if (spool->count < spool->capacity) return 0;
    size_t capacity = spool->capacity ? spool->capacity * 2U : 16U;
    SpooledPage *pages = realloc(spool->pages, capacity * sizeof(*pages));
    if (!pages) return -1;
    spool->pages = pages;
    spool->capacity = capacity;
    return 0;
}

static int spool_record(Spool *spool, off_t start) {
    off_t end = ftello(spool->file);
    if (start < 0 || end < start) return -1;
    spool->pages[spool->count++] = (SpooledPage){.offset = start, .length = end - start};
    return 0;
}

static int spool_bytes(Spool *spool, const unsigned char *data, size_t length) {
    if (spool_reserve(spool) != 0) return -1;
    off_t start = ftello(spool->file);
    if (fwrite(data, 1, length, spool->file) != length) return -1;
    return spool_record(spool, start);
}


/* A page is read back whole before any of it is written, so the output
 * never holds part of a page unless the output itself failed. */
static int replay_page(const Spool *spool, size_t index, FILE *out) {
    off_t length = spool->pages[index].length;
    if (length <= 0 || (uintmax_t)length > SIZE_MAX) return -1;
    unsigned char *page = malloc((size_t)length);
    int status = page && fseeko(spool->file, spool->pages[index].offset, SEEK_SET) == 0 &&
                         fread(page, 1, (size_t)length, spool->file) == (size_t)length &&
                         fwrite(page, 1, (size_t)length, out) == (size_t)length
                     ? 0 : -1;
    free(page);
    return status;
}

/* Manual duplex order, with the blank partner of an odd last page spooled as
 * page n + 1: n, ..., 4, 2, PAUSE, 1, 3, ..., n - 1. */
static int replay_manual_duplex(const Spool *spool, FILE *out) {
    for (size_t back = spool->count; back >= 2U; back -= 2U) {
        if (replay_page(spool, back - 1U, out) != 0) return -1;
    }
    if (zjs_write_pause(out) != 0) return -1;
    for (size_t front = 0; front < spool->count; front += 2U) {
        if (replay_page(spool, front, out) != 0) return -1;
    }
    return 0;
}

static int write_bytes(FILE *out, const unsigned char *data, size_t length) {
    return length == 0 || fwrite(data, 1, length, out) == length ? 0 : -1;
}

/*
 * Encoded pages, in raster order, go to their place in sheet order:
 * simplex pages straight out; automatic duplex as 2, 1, 4, 3, ... the moment
 * a sheet's back side is encoded (the whole job is not spooled first);
 * manual duplex into the spool.
 */
typedef struct {
    FILE *out;
    bool duplex;
    bool automatic;
    Spool spool;
    unsigned char *front;   /* automatic duplex: odd page waiting for its back */
    size_t front_length;
    int count;
} PageSink;

/* Takes ownership of `data`. */
static int sink_page(PageSink *sink, unsigned char *data, size_t length) {
    sink->count++;
    int status;
    if (!sink->duplex) {
        status = write_bytes(sink->out, data, length) == 0 && fflush(sink->out) == 0 ? 0 : -1;
    } else if (!sink->automatic) {
        status = spool_bytes(&sink->spool, data, length);
    } else if (sink->count % 2 != 0) {
        sink->front = data;
        sink->front_length = length;
        return 0;
    } else {
        status = write_bytes(sink->out, data, length) == 0 &&
                         write_bytes(sink->out, sink->front, sink->front_length) == 0 && fflush(sink->out) == 0
                     ? 0 : -1;
        free(sink->front);
        sink->front = NULL;
    }
    free(data);
    return status;
}

/* The blank partner of an odd last page, with that page's settings,
 * through the page guard like every other page. */
static unsigned char *blank_partner(const ZjsPageOptions *last_page, size_t *length) {
    ZjsPageOptions page = *last_page;
    page.content_height = page.height;
    unsigned char *blank = calloc(page.height, zjs_line_bytes(page.width, page.bpp));
    unsigned char *data = NULL;
    char reason[192];
    if (!blank) return NULL;
    if (zjs_encode_page_checked(&page, blank, &data, length, reason, sizeof(reason)) != 0) {
        fprintf(stderr, "ERROR: The blank back side was not sent: %s\n", reason);
    }
    free(blank);
    return data;
}

/* Duplex job end: the blank partner of an odd last page (sent as that
 * sheet's back side), then the manual-duplex replay. */
static int sink_finish(PageSink *sink, const ZjsPageOptions *last_page) {
    if (!sink->duplex || sink->count == 0) return 0;
    /* A one-page manual duplex job needs no second pass. */
    if (!sink->automatic && sink->count == 1) return replay_page(&sink->spool, 0, sink->out);

    int status = 0;
    if (sink->count % 2 != 0) {
        size_t length = 0;
        unsigned char *blank = blank_partner(last_page, &length);
        if (!blank) return -1;
        if (sink->automatic) {
            status = write_bytes(sink->out, blank, length) == 0 &&
                             write_bytes(sink->out, sink->front, sink->front_length) == 0
                         ? 0 : -1;
            free(sink->front);
            sink->front = NULL;
        } else {
            status = spool_bytes(&sink->spool, blank, length);
        }
        free(blank);
    }
    if (status == 0 && !sink->automatic) status = replay_manual_duplex(&sink->spool, sink->out);
    return status;
}

/* Writes encoded pages as soon as they are ready, while the main thread
 * waits for the next raster page. After an error it keeps taking pages (so
 * the renderer never waits on a full pool) but drops them. */
typedef struct {
    P1600EncoderPool *pool;
    PageSink *sink;
    pthread_t thread;
    bool running;
    atomic_bool failed;
} PageWriter;

/* Takes the next page back from the pool; false once none are left. */
static bool write_next_page(PageWriter *writer) {
    unsigned char *data = NULL;
    size_t length = 0;
    int result = p1600_encoder_pool_next(writer->pool, &data, &length);
    if (result == 0) return false;
    if (atomic_load(&writer->failed) || atomic_load(&job_canceled)) {
        free(data);
    } else if (result < 0) {
        fprintf(stderr, "ERROR: Page %d was not sent to the printer: %s\n", writer->sink->count + 1,
                p1600_encoder_pool_error(writer->pool));
        atomic_store(&writer->failed, true);
        free(data);
    } else if (sink_page(writer->sink, data, length) != 0) {
        fprintf(stderr, "ERROR: Unable to write page %d\n", writer->sink->count);
        atomic_store(&writer->failed, true);
    }
    return true;
}

static void *writer_main(void *argument) {
    while (write_next_page(argument)) {
    }
    return NULL;
}

/* How long the job waits after its last byte for the printer to report the
 * end of the job: a printer that never answered gets a short grace period,
 * one that answers keeps the job open (manual duplex waits for the user). */
#define STATUS_QUIET_SECONDS 15.0
#define STATUS_IDLE_SECONDS 600.0

typedef struct {
    const P1600StatusTexts *texts;
    int page;
} ProgressContext;

static void report_progress(void *context, uint32_t line, uint32_t total) {
    const ProgressContext *progress = context;
    unsigned int percent = 0;
    char message[320];
    if (p1600_progress_due(line, total, &percent) &&
        p1600_progress_format(progress->texts, progress->page, percent, message, sizeof(message)) > 0) {
        fputs(message, stderr);
    }
}

static bool automatic_duplex_supported(uint32_t paper, uint32_t media) {
    bool paper_ok = paper == DMPAPER_LETTER || paper == DMPAPER_LEGAL ||
                    paper == DMPAPER_A4 || paper == DMPAPER_FOLIO_8_5X13;
    bool media_ok = media == DMMEDIA_PLAIN || media == DMMEDIA_LIGHT || media == DMMEDIA_MID_WEIGHT;
    return paper_ok && media_ok;
}

int main(int argc, char *argv[]) {
    if (argc < 6 || argc > 7) {
        fprintf(stderr, "ERROR: Usage: %s job-id user title copies options [file]\n", argv[0]);
        return 1;
    }
    /* Decided before any file is opened: only CUPS' backchannel may be fd 3. */
    bool backchannel = p1600_status_backchannel_available(getenv("DEVICE_URI"), P1600_BACKCHANNEL_FD);
    if (p1600_prepare_standard_descriptors() != 0) {
        fprintf(stderr, "ERROR: Standard output is not available for the print job\n");
        return 1;
    }
    struct sigaction cancel = {.sa_handler = cancel_job, .sa_flags = SA_RESTART};
    sigemptyset(&cancel.sa_mask);
    sigaction(SIGTERM, &cancel, NULL);

    int legacy_copies = 1;
    if (parse_int_range(argv[4], 1, INT_MAX, &legacy_copies) != 0) {
        fprintf(stderr, "ERROR: Invalid copy count: %s\n", argv[4]);
        return 1;
    }

    cups_option_t *options = NULL;
    int num_options = cupsParseOptions(argv[5], 0, &options);
    if (num_options < 0) {
        fprintf(stderr, "ERROR: Unable to parse CUPS job options\n");
        return 1;
    }

    static const char *const econo_names[] = {"EconoMode", "TonerSave", NULL};
    static const char *const density_names[] = {"pjlDensity", "PrintDensity", "Density", NULL};
    static const char *const jam_names[] = {"pjlJamRecovery", "JamRecovery", NULL};
    static const char *const rotate_names[] = {"TersKose", "ReverseLandscape", "ReverseOrientation", NULL};
    static const char *const media_names[] = {"MediaType", NULL};
    static const char *const source_names[] = {"InputSlot", "MediaSource", NULL};

    ZjsJobOptions job = {.job_name = argv[3], .density = 3};
    char ppd_value[PPD_VALUE_SIZE];
    int density = 0;
    if (parse_int_range(option_or_default(density_names, num_options, options, ppd_value), 1, 5, &density) == 0) {
        job.density = density;
    }
    job.jam_recovery = is_true(option_or_default(jam_names, num_options, options, ppd_value));
    bool econo_mode = is_true(option_or_default(econo_names, num_options, options, ppd_value));
    bool reverse_corner = is_true(option_or_default(rotate_names, num_options, options, ppd_value));
    static const char *const dup_unit_names[] = {"DupUnit", NULL};
    const char *dup_unit = option_or_default(dup_unit_names, num_options, options, ppd_value);
    bool dup_unit_installed = !dup_unit || !is_false(dup_unit);
    const char *media_name = first_option(media_names, num_options, options);
    const char *source_name = first_option(source_names, num_options, options);

    char language[16];
    P1600StatusTexts texts;
    p1600_status_language(language, sizeof(language));
    p1600_status_texts_load(&texts, getenv("PPD"), language);
    P1600StatusReader reader = {.running = false};

    int fd = 0;
    if (argc == 7 && (fd = open(argv[6], O_RDONLY)) < 0) {
        fprintf(stderr, "ERROR: Unable to open raster file: %s\n", argv[6]);
        cupsFreeOptions(num_options, options);
        return 1;
    }

    P1600RasterInput input = {.fd = fd};
    cups_raster_t *raster = cupsRasterOpenIO(p1600_raster_input_read, &input, CUPS_RASTER_READ);
    if (!raster) {
        fprintf(stderr, "ERROR: Unable to open CUPS raster stream\n");
        if (argc == 7) close(fd);
        cupsFreeOptions(num_options, options);
        return 1;
    }

    cups_page_header2_t header;
    PageSink sink = {.out = stdout};
    P1600EncoderPool *pool = NULL;
    PageWriter writer = {.sink = &sink};
    atomic_init(&writer.failed, false);
    ZjsPageOptions last_page = {0};
    bool duplex = false;
    bool long_edge = false;
    bool automatic = false;
    bool document_started = false;
    int page_number = 0;
    int status = 0;

    while (status == 0 && !atomic_load(&job_canceled) && cupsRasterReadHeader2(raster, &header)) {
        int next_page = page_number + 1;
        P1600PageGeometry geometry;
        char reason[160];
        if (p1600_page_geometry(&header, &geometry, reason, sizeof(reason)) != 0) {
            fprintf(stderr, "ERROR: %s on page %d\n", reason, next_page);
            status = 1;
            break;
        }

        ZjsPageOptions page = {
            .paper = p1600_paper_code(header.PageSize[0], header.PageSize[1]),
            .copies = geometry.kind == P1600_RASTER_GRAY ? (header.NumCopies > 0 ? header.NumCopies : 1U)
                                                      : (uint32_t)legacy_copies,
            .source = header.MediaPosition != 0 ? header.MediaPosition : legacy_source_code(source_name),
            .media = header.cupsMediaType != 0 ? header.cupsMediaType
                                               : legacy_media_code(header.MediaType[0] ? header.MediaType : media_name),
            .width = geometry.width,
            .height = geometry.height,
            .content_height = geometry.content_height,
            .bpp = geometry.bpp,
            .econo_mode = econo_mode,
        };

        if (page_number == 0) {
            duplex = header.Duplex != 0;
            long_edge = duplex && header.Tumble == 0;
            automatic = duplex && dup_unit_installed && automatic_duplex_supported(page.paper, page.media);
            job.collate = header.Collate != 0;
            if (backchannel && p1600_status_reader_start(&reader, P1600_BACKCHANNEL_FD, &texts, stderr) != 0) {
                fputs("DEBUG: Unable to start the printer status reader\n", stderr);
            }
            sink.duplex = duplex;
            sink.automatic = automatic;
            if (duplex && !automatic && !(sink.spool.file = tmpfile())) {
                fprintf(stderr, "ERROR: Unable to create duplex spool file: %s\n", strerror(errno));
                status = 1;
                break;
            }
            /* One encoder per performance core; the main thread renders
             * the next page meanwhile. Without threads, encode inline. */
            unsigned workers = p1600_encoder_default_workers();
            if (!(pool = p1600_encoder_pool_create(workers, workers + 1U)) &&
                !(pool = p1600_encoder_pool_create(0, 1))) {
                fprintf(stderr, "ERROR: Unable to start the page encoder\n");
                status = 1;
                break;
            }
            writer.pool = pool;
            writer.running = pthread_create(&writer.thread, NULL, writer_main, &writer) == 0;
            if (!writer.running) {
                /* No writer thread: encode and write each page in turn. */
                p1600_encoder_pool_destroy(pool);
                writer.pool = pool = p1600_encoder_pool_create(0, 1);
                if (!pool) {
                    fprintf(stderr, "ERROR: Unable to start the page encoder\n");
                    status = 1;
                    break;
                }
            }
            if (zjs_start_document(stdout, &job) != 0) {
                fprintf(stderr, "ERROR: Unable to write document header\n");
                status = 1;
                break;
            }
            document_started = true;
        } else if ((header.Duplex != 0) != duplex || (duplex && (header.Tumble == 0) != long_edge)) {
            fprintf(stderr, "ERROR: Duplex mode changed inside the raster job on page %d\n", next_page);
            status = 1;
            break;
        }
        page.auto_duplex = automatic;

        /* PPD 4+ queues get long-edge back sides turned by CUPS (cupsBackSide
         * Rotated); PPD 3.x queues are turned here. */
        bool rotate = reverse_corner;
        if (geometry.kind == P1600_RASTER_LEGACY_1BIT && long_edge && next_page % 2 == 0) rotate = !rotate;

        unsigned char *bitmap = calloc(geometry.height, geometry.line_bytes);
        if (!bitmap) {
            fprintf(stderr, "ERROR: Unable to allocate memory for page %d\n", next_page);
            status = 1;
            break;
        }
        ProgressContext progress = {.texts = &texts, .page = next_page};
        if (p1600_render_page_progress(raster, &header, &geometry, rotate, bitmap, report_progress, &progress) != 0) {
            /* A canceled job's input ends where cupsd stopped the filter before. */
            if (!atomic_load(&job_canceled)) fprintf(stderr, "ERROR: Truncated raster data on page %d\n", next_page);
            free(bitmap);
            status = 1;
            break;
        }
        if (atomic_load(&job_canceled)) {
            free(bitmap);
            break;
        }

        page_number = next_page;
        fprintf(stderr, "PAGE: %d %u\n", page_number, page.copies);
        /* The pool frees the bitmap; it waits while the printer is behind. */
        if (atomic_load(&writer.failed) || p1600_encoder_pool_submit(pool, &page, bitmap) != 0) {
            if (!atomic_load(&writer.failed)) fprintf(stderr, "ERROR: Unable to encode page %d\n", page_number);
            status = 1;
            break;
        }
        if (!writer.running) write_next_page(&writer);
        last_page = page;
    }
    bool canceled = atomic_load(&job_canceled);
    if (canceled) {
        fputs("DEBUG: Job canceled; closing the print job after the last whole page\n", stderr);
        status = 1;
    }
    if (pool) {
        /* After an error or a cancel the remaining pages are dropped, not printed. */
        if (status != 0) atomic_store(&writer.failed, true);
        p1600_encoder_pool_close(pool);
        if (writer.running) pthread_join(writer.thread, NULL);
        else while (write_next_page(&writer)) {
        }
        if (atomic_load(&writer.failed)) status = 1;
    }

    char input_error[192];
    if (status == 0 && p1600_raster_input_end(&input, input_error, sizeof(input_error)) != 0) {
        fprintf(stderr, "ERROR: %s after page %d\n", input_error, page_number);
        status = 1;
    }

    bool job_ended = false;
    if (status == 0 && page_number > 0) {
        if (sink_finish(&sink, &last_page) != 0) status = 1;
        /* The job waits for USTATUS JOB END between EOJ and USTATUSOFF, so
         * the printer's messages (manual duplex, jams, ...) reach CUPS until
         * the last sheet is out. */
        if (status == 0 && zjs_end_job(stdout) != 0) status = 1;
        job_ended = status == 0;
        if (status == 0) p1600_status_reader_finish(&reader, STATUS_QUIET_SECONDS, STATUS_IDLE_SECONDS);
        if (status == 0 && zjs_end_status(stdout) != 0) status = 1;
        if (status != 0) fprintf(stderr, "ERROR: Unable to write the print job\n");
    }
    if (status != 0 && document_started && !job_ended && !ferror(stdout)) {
        /* Just stopping would leave the printer waiting for the rest of the
         * job until its I/O timeout. Pages and automatic-duplex sheets are
         * written whole, and a partial write marks stdout with an error, so
         * without one the stream is at a page boundary: close the job, the
         * printer finishes the sheets it has and is ready again at once. */
        if (zjs_end_job(stdout) == 0) zjs_end_status(stdout);
    }
    p1600_status_reader_finish(&reader, 0.0, 0.0);

    p1600_encoder_pool_destroy(pool);
    free(sink.front);
    if (sink.spool.file) fclose(sink.spool.file);
    free(sink.spool.pages);
    cupsRasterClose(raster);
    if (argc == 7) close(fd);
    cupsFreeOptions(num_options, options);
    return status;
}
