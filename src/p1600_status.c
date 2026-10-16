#include "p1600_status.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread/qos.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* cupsIPPReason specs, indexed by P1600TextKey. */
static const char *const text_specs[P1600_TEXT_COUNT] = {
    "PrinterReady", "CoverOpen", "ManualDuplex", "TonerMissing", "ManualFeed",
    "OutOfPaper", "InputJam", "OutputJam", "DoorOpen", "EngineFuser",
    "EngineScanner", "EngineComm", "Printingpage", "Complete",
};

/* The PJL device status codes the filter reports, with the driver's own
 * fallback texts for when the PPD has none. */
static const P1600StatusCode status_codes[] = {
    {10001, 0, NULL, P1600_TEXT_PRINTER_READY, NULL},
    {10003, 0, NULL, P1600_TEXT_PRINTER_READY, NULL},
    {10023, 0, NULL, P1600_TEXT_PRINTER_READY, NULL},
    {40021, 0, "The cartridge door is open", P1600_TEXT_COVER_OPEN, "cover-open"},
    {40120, 0, "Second sides next: the printed sheets will need reloading", P1600_TEXT_MANUAL_DUPLEX, "media-needed"},
    {40121, 0, "Reload the printed sheets for their second side", P1600_TEXT_MANUAL_DUPLEX, "media-needed"},
    {40131, 0, "Reload the printed sheets for their second side (other paper path)", P1600_TEXT_MANUAL_DUPLEX, "media-needed"},
    {40600, 0, "No toner cartridge", P1600_TEXT_TONER_MISSING, "toner-empty"},
    {41100, 41199, "Load paper in the manual feed slot", P1600_TEXT_MANUAL_FEED, "media-needed"},
    {41002, 41004, "No paper", P1600_TEXT_OUT_OF_PAPER, "media-empty"},
    {42000, 0, "Paper stuck while feeding", P1600_TEXT_INPUT_JAM, "media-jam"},
    {42001, 0, "Paper stopped after feeding", P1600_TEXT_OUTPUT_JAM, "media-jam"},
    {42002, 0, "Paper late at the fuser", P1600_TEXT_INPUT_JAM, "media-jam"},
    {42003, 0, "Paper stopped in the fuser", P1600_TEXT_OUTPUT_JAM, "media-jam"},
    {42004, 0, "Paper stuck in the input path", P1600_TEXT_INPUT_JAM, "media-jam"},
    {42005, 0, "A door is open after a paper jam", P1600_TEXT_DOOR_OPEN, "cover-open"},
    {50003, 0, "Fuser error", P1600_TEXT_ENGINE_FUSER, "connecting-to-device"},
    {50005, 0, "Laser scanner error", P1600_TEXT_ENGINE_SCANNER, "connecting-to-device"},
    {50007, 0, "Engine communication error", P1600_TEXT_ENGINE_COMM, "connecting-to-device"},
};

/* Every reason the table can set, cleared before a new state is reported. */
static const char *const status_reasons[] = {
    "cover-open", "media-needed", "toner-empty", "media-empty", "media-jam", "connecting-to-device",
};

void p1600_status_language(char *language, size_t size) {
    if (!language || size == 0) return;
    language[0] = '\0';

    static const char *const variables[] = {"LC_ALL", "LC_MESSAGES", "LANG"};
    const char *value = NULL;
    for (size_t i = 0; i < sizeof(variables) / sizeof(variables[0]) && !value; i++) {
        const char *candidate = getenv(variables[i]);
        if (candidate && *candidate) value = candidate;
    }
    if (!value) return;

    /* "ll" or "ll_CC"; "C", "POSIX" and English use the base keyword. */
    char parsed[6] = "";
    size_t length = 0;
    for (const char *p = value; *p && *p != '.' && *p != '@'; p++) {
        unsigned char c = (unsigned char)*p;
        if (length == sizeof(parsed) - 1U) return;
        if (length == 2U && (c == '_' || c == '-')) {
            parsed[length++] = '_';
        } else if (isalpha(c) && length != 2U) {
            parsed[length] = (char)(length < 2U ? tolower(c) : toupper(c));
            length++;
        } else {
            return;
        }
    }
    parsed[length] = '\0';
    if ((length != 2U && length != 5U) || strncmp(parsed, "en", 2) == 0) return;
    /* macOS names Norwegian "nb" (Bokmål) or "nn"; the PPD uses "no". */
    if (strncmp(parsed, "nb", 2) == 0 || strncmp(parsed, "nn", 2) == 0) memcpy(parsed, "no", 2);
    if (strlen(parsed) < size) strcpy(language, parsed);
}

/* Copies at most size - 1 bytes without splitting a UTF-8 sequence and
 * turns control characters into spaces (status lines are one line each). */
static void copy_text(char *destination, size_t size, const char *source, size_t length) {
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

static int keyword_rank(const char *keyword, size_t length, const char *language) {
    static const char base[] = "cupsIPPReason";
    const size_t base_length = sizeof(base) - 1U;
    if (length == base_length && memcmp(keyword, base, base_length) == 0) return 1;
    if (!language[0] || length <= base_length + 1U ||
        memcmp(keyword + length - base_length, base, base_length) != 0 ||
        keyword[length - base_length - 1U] != '.') return 0;

    size_t prefix = length - base_length - 1U;
    if (prefix == strlen(language) && strncmp(keyword, language, prefix) == 0) {
        return prefix == 5U ? 3 : 2;
    }
    if (prefix == 2U && strncmp(keyword, language, 2) == 0) return 2;
    return 0;
}

int p1600_status_texts_load(P1600StatusTexts *texts, const char *ppd_path, const char *language) {
    if (!texts) return -1;
    memset(texts, 0, sizeof(*texts));
    if (!language) language = "";
    FILE *ppd = ppd_path ? fopen(ppd_path, "r") : NULL;
    if (!ppd) return -1;

    char *line = NULL;
    size_t capacity = 0;
    bool at_end = false;
    while (!at_end && getline(&line, &capacity, ppd) > 0) {
        if (line[0] != '*') continue;
        const char *keyword = line + 1;
        size_t keyword_length = strcspn(keyword, " \t:");
        int rank = keyword_rank(keyword, keyword_length, language);
        if (rank == 0) continue;

        /* *keyword spec/text: "value" */
        const char *spec = keyword + keyword_length;
        spec += strspn(spec, " \t");
        size_t spec_length = strcspn(spec, "/:\r\n");
        int key = -1;
        for (int k = 0; k < P1600_TEXT_COUNT; k++) {
            if (strlen(text_specs[k]) == spec_length && strncmp(spec, text_specs[k], spec_length) == 0) {
                key = k;
                break;
            }
        }
        if (key < 0 || rank <= texts->entries[key].rank) continue;

        const char *text = spec + spec_length;
        size_t text_length = 0;
        if (*text == '/') {
            text++;
            text_length = strcspn(text, ":\r\n");
        }
        const char *colon = strchr(text + text_length, ':');
        const char *quote = colon ? strchr(colon, '"') : NULL;
        if (!quote) continue;

        /* Quoted values may continue on the following lines; copy_text()
         * cuts the result to the entry size on a UTF-8 boundary. */
        char value[2U * sizeof(texts->entries[0].value)];
        size_t value_length = 0;
        const char *part = quote + 1;
        bool closed = false;
        char *continuation = NULL;
        size_t continuation_capacity = 0;
        for (;;) {
            const char *end = strchr(part, '"');
            size_t part_length = end ? (size_t)(end - part) : strlen(part);
            size_t room = sizeof(value) - 1U - value_length;
            memcpy(value + value_length, part, part_length < room ? part_length : room);
            value_length += part_length < room ? part_length : room;
            if (end) {
                closed = true;
                break;
            }
            if (getline(&continuation, &continuation_capacity, ppd) <= 0) {
                at_end = true;
                break;
            }
            part = continuation;
        }
        free(continuation);
        if (!closed) continue;

        P1600PpdText *entry = &texts->entries[key];
        copy_text(entry->text, sizeof(entry->text), text, text_length);
        copy_text(entry->value, sizeof(entry->value), value, value_length);
        entry->rank = rank;
    }
    free(line);
    fclose(ppd);
    return 0;
}

const P1600StatusCode *p1600_status_code_lookup(int code) {
    for (size_t i = 0; i < sizeof(status_codes) / sizeof(status_codes[0]); i++) {
        const P1600StatusCode *entry = &status_codes[i];
        if ((entry->high == 0 && entry->low == code) ||
            (entry->high != 0 && entry->low <= code && code <= entry->high)) return entry;
    }
    return NULL;
}

/* Appends to `out` and keeps track of truncation. */
typedef struct {
    char *out;
    size_t size;
    size_t length;
} TextBuffer;

static void append(TextBuffer *buffer, const char *text) {
    size_t length = strlen(text);
    if (buffer->length + length >= buffer->size) {
        buffer->length = buffer->size;
        return;
    }
    memcpy(buffer->out + buffer->length, text, length + 1U);
    buffer->length += length;
}

size_t p1600_status_format_code(const P1600StatusTexts *texts, int code, char *out, size_t size) {
    if (!out || size == 0) return 0;
    out[0] = '\0';
    const P1600StatusCode *entry = p1600_status_code_lookup(code);
    if (!entry) return 0;

    const P1600PpdText *text = texts ? &texts->entries[entry->key] : NULL;
    const char *message = text && text->rank != 0 ? text->value
                                                  : (entry->description ? entry->description : "");

    /* Not "STATE: none": only the reasons this table sets are cleared
     * here so reasons of the backend survive. */
    TextBuffer buffer = {out, size, 0};
    bool first = true;
    append(&buffer, "STATE: -");
    for (size_t i = 0; i < sizeof(status_reasons) / sizeof(status_reasons[0]); i++) {
        if (entry->reason && strcmp(entry->reason, status_reasons[i]) == 0) continue;
        if (!first) append(&buffer, ",");
        append(&buffer, status_reasons[i]);
        append(&buffer, "-error");
        first = false;
    }
    append(&buffer, "\nINFO: ");
    append(&buffer, message);
    append(&buffer, "\n");
    if (entry->reason) {
        append(&buffer, "STATE: +");
        append(&buffer, entry->reason);
        append(&buffer, "-error\n");
    }
    if (buffer.length >= size) {
        out[0] = '\0';
        return 0;
    }
    return buffer.length;
}

bool p1600_progress_due(uint32_t line_index, uint32_t total, unsigned int *percent) {
    if (total == 0 || line_index >= total || (total - line_index - 1U) % 256U != 0) return false;
    if (percent) *percent = (unsigned int)(((uint64_t)line_index + 1U) * 100U / total);
    return true;
}

size_t p1600_progress_format(const P1600StatusTexts *texts, int page, unsigned int percent,
                          char *out, size_t size) {
    if (!out || size == 0) return 0;
    out[0] = '\0';
    if (!texts) return 0;
    const P1600PpdText *printing = &texts->entries[P1600_TEXT_PRINTING_PAGE];
    const P1600PpdText *complete = &texts->entries[P1600_TEXT_COMPLETE];
    if (printing->rank == 0 || complete->rank == 0 || !printing->text[0] || !complete->text[0]) return 0;
    int length = snprintf(out, size, "INFO: %s %d, %u%% %s...\n", printing->text, page, percent, complete->text);
    if (length < 0 || (size_t)length >= size) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)length;
}

/* "@PJL USTATUS PAGE" style prefix with any whitespace between the words,
 * followed by whitespace or the end of the message. Returns the rest. */
static const char *match_words(const char *message, const char *const *words) {
    const char *p = message;
    for (; *words; words++) {
        p += strspn(p, " \t\r\n");
        size_t length = strlen(*words);
        if (strncmp(p, *words, length) != 0) return NULL;
        p += length;
        if (*p && !isspace((unsigned char)*p) && words[1] == NULL) return NULL;
    }
    return p;
}

static bool read_code(const char *rest, int *code) {
    rest += strspn(rest, " \t\r\n");
    if (strncmp(rest, "CODE=", 5) != 0) return false;
    char *end = NULL;
    errno = 0;
    long value = strtol(rest + 5, &end, 10);
    if (end == rest + 5 || errno != 0 || value < 0 || value > 99999) return false;
    if (code) *code = (int)value;
    return true;
}

P1600UstatusKind p1600_ustatus_classify(const char *message, int *code) {
    static const char *const device[] = {"@PJL", "USTATUS", "DEVICE", NULL};
    static const char *const info[] = {"@PJL", "INFO", "STATUS", NULL};
    static const char *const page[] = {"@PJL", "USTATUS", "PAGE", NULL};
    static const char *const job[] = {"@PJL", "USTATUS", "JOB", NULL};
    if (!message) return P1600_USTATUS_NONE;

    const char *rest;
    if ((rest = match_words(message, device)) || (rest = match_words(message, info))) {
        return read_code(rest, code) ? P1600_USTATUS_DEVICE : P1600_USTATUS_OTHER;
    }
    if (match_words(message, page)) return P1600_USTATUS_PAGE;
    if ((rest = match_words(message, job))) {
        rest += strspn(rest, " \t\r\n");
        size_t length = strcspn(rest, " \t\r\n");
        if (length == 5 && strncmp(rest, "START", 5) == 0) return P1600_USTATUS_JOB_START;
        if (length == 3 && strncmp(rest, "END", 3) == 0) return P1600_USTATUS_JOB_END;
        if (length == 8 && strncmp(rest, "CANCELED", 8) == 0) return P1600_USTATUS_JOB_CANCELED;
    }
    return P1600_USTATUS_OTHER;
}

void p1600_ustatus_feed(P1600UstatusParser *parser, const unsigned char *data, size_t length,
                     P1600UstatusHandler on_message, void *context) {
    if (!parser || (!data && length != 0)) return;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = data[i];
        if (parser->skip > 0) {
            parser->skip--;
            continue;
        }
        if (parser->after_nul) {
            parser->after_nul = false;
            if (c == 0xACU) {
                parser->skip = 14;  /* NUL, 0xAC and 14 more bytes */
                continue;
            }
        }
        if (c == 0x00U) {
            parser->length = 0;
            parser->discarding = false;
            parser->after_nul = true;
            continue;
        }
        if (c == 0x0CU) {
            if (!parser->discarding) {
                parser->buffer[parser->length] = '\0';
                int code = 0;
                P1600UstatusKind kind = p1600_ustatus_classify((const char *)parser->buffer, &code);
                if (on_message) on_message(context, kind, code);
            }
            parser->length = 0;
            parser->discarding = false;
            continue;
        }
        if (parser->discarding) continue;
        if (parser->length == sizeof(parser->buffer) - 1U) {
            parser->length = 0;
            parser->discarding = true;
            continue;
        }
        parser->buffer[parser->length++] = c;
    }
}

static uint64_t monotonic_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

bool p1600_status_backchannel_available(const char *device_uri, int fd) {
    static const char *const schemes[] = {"usb", "socket", "mdns", "dnssd"};
    if (!device_uri) return false;
    size_t scheme_length = strcspn(device_uri, ":");
    if (device_uri[scheme_length] != ':') return false;
    bool known = false;
    for (size_t i = 0; i < sizeof(schemes) / sizeof(schemes[0]); i++) {
        if (strlen(schemes[i]) == scheme_length && strncasecmp(device_uri, schemes[i], scheme_length) == 0) {
            known = true;
        }
    }
    if (!known) return false;

    struct stat info;
    if (fstat(fd, &info) != 0 || !(S_ISFIFO(info.st_mode) || S_ISSOCK(info.st_mode) || S_ISCHR(info.st_mode))) {
        return false;
    }
    int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && ((flags & O_ACCMODE) == O_RDONLY || (flags & O_ACCMODE) == O_RDWR);
}

static void handle_message(void *context, P1600UstatusKind kind, int code) {
    P1600StatusReader *reader = context;
    char lines[1024];
    switch (kind) {
    case P1600_USTATUS_DEVICE:
        fprintf(reader->log, "DEBUG: Printer status code %d\n", code);
        if (p1600_status_format_code(reader->texts, code, lines, sizeof(lines)) > 0) fputs(lines, reader->log);
        break;
    case P1600_USTATUS_PAGE:
        fputs("DEBUG: Printer reported a printed page\n", reader->log);
        break;
    case P1600_USTATUS_JOB_START:
        fputs("DEBUG: Printer started the job\n", reader->log);
        break;
    case P1600_USTATUS_JOB_END:
    case P1600_USTATUS_JOB_CANCELED:
        fprintf(reader->log, "DEBUG: Printer %s the job\n", kind == P1600_USTATUS_JOB_END ? "finished" : "canceled");
        break;
    case P1600_USTATUS_NONE:
    case P1600_USTATUS_OTHER:
        break;
    }
    fflush(reader->log);

    pthread_mutex_lock(&reader->lock);
    reader->heard = true;
    reader->last_message_ns = monotonic_ns();
    if (kind == P1600_USTATUS_JOB_END || kind == P1600_USTATUS_JOB_CANCELED) reader->job_done = true;
    pthread_cond_signal(&reader->changed);
    pthread_mutex_unlock(&reader->lock);
}

static bool job_done(P1600StatusReader *reader) {
    pthread_mutex_lock(&reader->lock);
    bool done = reader->job_done;
    pthread_mutex_unlock(&reader->lock);
    return done;
}

static void *reader_main(void *argument) {
    P1600StatusReader *reader = argument;
    unsigned char data[1024];
    while (!job_done(reader)) {
        struct pollfd channels[2] = {{.fd = reader->fd, .events = POLLIN}, {.fd = reader->wake[0], .events = POLLIN}};
        if (poll(channels, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (channels[1].revents != 0) break;  /* the filter stops waiting */
        if (channels[0].revents == 0) continue;
        ssize_t received = read(reader->fd, data, sizeof(data));
        if (received < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            break;
        }
        if (received == 0) break;  /* the backend closed the backchannel */
        p1600_ustatus_feed(&reader->parser, data, (size_t)received, handle_message, reader);
    }
    pthread_mutex_lock(&reader->lock);
    reader->finished = true;
    pthread_cond_signal(&reader->changed);
    pthread_mutex_unlock(&reader->lock);
    return NULL;
}

int p1600_status_reader_start(P1600StatusReader *reader, int fd, const P1600StatusTexts *texts, FILE *log) {
    if (!reader || fd < 0 || !log) return -1;
    memset(reader, 0, sizeof(*reader));
    reader->fd = fd;
    reader->log = log;
    reader->texts = texts;
    if (pipe(reader->wake) != 0) return -1;
    fcntl(reader->wake[0], F_SETFD, FD_CLOEXEC);
    fcntl(reader->wake[1], F_SETFD, FD_CLOEXEC);

    /* Mostly asleep and never urgent: utility QoS lets macOS keep it on the
     * efficiency cores. */
    pthread_attr_t attributes;
    bool created = pthread_mutex_init(&reader->lock, NULL) == 0;
    if (created && pthread_cond_init(&reader->changed, NULL) != 0) {
        pthread_mutex_destroy(&reader->lock);
        created = false;
    }
    if (created && pthread_attr_init(&attributes) == 0) {
        pthread_attr_set_qos_class_np(&attributes, QOS_CLASS_UTILITY, 0);
        created = pthread_create(&reader->thread, &attributes, reader_main, reader) == 0;
        pthread_attr_destroy(&attributes);
        if (!created) {
            pthread_cond_destroy(&reader->changed);
            pthread_mutex_destroy(&reader->lock);
        }
    } else if (created) {
        pthread_cond_destroy(&reader->changed);
        pthread_mutex_destroy(&reader->lock);
        created = false;
    }
    if (!created) {
        close(reader->wake[0]);
        close(reader->wake[1]);
        return -1;
    }
    reader->running = true;
    return 0;
}

bool p1600_status_reader_finish(P1600StatusReader *reader, double quiet_seconds, double idle_seconds) {
    if (!reader || !reader->running) return false;
    const uint64_t start = monotonic_ns();
    pthread_mutex_lock(&reader->lock);
    while (!reader->finished) {
        uint64_t since = reader->heard && reader->last_message_ns > start ? reader->last_message_ns : start;
        double limit = reader->heard ? idle_seconds : quiet_seconds;
        uint64_t deadline = since + (uint64_t)(limit > 0.0 ? limit * 1e9 : 0.0);
        uint64_t now = monotonic_ns();
        if (now >= deadline) break;
        uint64_t wait = deadline - now;
        struct timespec relative = {(time_t)(wait / 1000000000ULL), (long)(wait % 1000000000ULL)};
        pthread_cond_timedwait_relative_np(&reader->changed, &reader->lock, &relative);
    }
    bool done = reader->job_done;
    pthread_mutex_unlock(&reader->lock);

    static const char stop = 1;
    while (write(reader->wake[1], &stop, 1) < 0 && errno == EINTR) {
    }
    pthread_join(reader->thread, NULL);
    close(reader->wake[0]);
    close(reader->wake[1]);
    pthread_cond_destroy(&reader->changed);
    pthread_mutex_destroy(&reader->lock);
    reader->running = false;
    return done;
}
