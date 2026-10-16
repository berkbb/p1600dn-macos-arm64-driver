#ifndef P1600_STATUS_H
#define P1600_STATUS_H

/*
 * Printer status reporting: the job asks for PJL USTATUS, a reader thread
 * parses the replies the printer sends back over the CUPS backchannel, and
 * device codes become printer-state-reasons plus the localized texts of the
 * PPD's cupsIPPReason entries. The page progress line
 * (`INFO: Printing Page n, p% Complete...`) is produced here as well.
 * See reference/zjstream/ZJS_STREAM_SPEC.md, section 6.
 */

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* The CUPS backchannel descriptor of filters. */
#define P1600_BACKCHANNEL_FD 3

/* PPD cupsIPPReason specs the filter reads (device status and progress). */
typedef enum {
    P1600_TEXT_PRINTER_READY = 0,
    P1600_TEXT_COVER_OPEN,
    P1600_TEXT_MANUAL_DUPLEX,
    P1600_TEXT_TONER_MISSING,
    P1600_TEXT_MANUAL_FEED,
    P1600_TEXT_OUT_OF_PAPER,
    P1600_TEXT_INPUT_JAM,
    P1600_TEXT_OUTPUT_JAM,
    P1600_TEXT_DOOR_OPEN,
    P1600_TEXT_ENGINE_FUSER,
    P1600_TEXT_ENGINE_SCANNER,
    P1600_TEXT_ENGINE_COMM,
    P1600_TEXT_PRINTING_PAGE,
    P1600_TEXT_COMPLETE,
    P1600_TEXT_COUNT
} P1600TextKey;

typedef struct {
    char text[128];   /* translation string after the slash ("Printing Page") */
    char value[512];  /* quoted value ("Manual Duplex: Take the printed sheets ...") */
    int rank;         /* 0 missing, 1 base keyword, 2 "ll.", 3 "ll_CC." */
} P1600PpdText;

typedef struct {
    P1600PpdText entries[P1600_TEXT_COUNT];
} P1600StatusTexts;

/* Job language for the PPD lookup from LC_ALL, LC_MESSAGES or LANG
 * ("tr_TR.UTF-8" -> "tr_TR"); an empty string selects the base keyword. */
void p1600_status_language(char *language, size_t size);

/* Reads the cupsIPPReason texts of `ppd_path` for `language`, preferring
 * "ll_CC.cupsIPPReason", then "ll.cupsIPPReason", then "cupsIPPReason" per
 * entry. Returns 0, or -1 when the file cannot be read (texts stay empty). */
int p1600_status_texts_load(P1600StatusTexts *texts, const char *ppd_path, const char *language);

/* The status table entry for a PJL device status code, or NULL. */
typedef struct {
    int low;
    int high;              /* 0 for a single code */
    const char *description; /* the driver's fallback text; NULL for "printer ready" codes */
    P1600TextKey key;
    const char *reason;    /* IPP reason without the "-error" suffix; NULL when ready */
} P1600StatusCode;

const P1600StatusCode *p1600_status_code_lookup(int code);

/* Formats the CUPS stderr lines for a device status code into `out`
 * (empty for codes that are not reported). Returns the length written. */
size_t p1600_status_format_code(const P1600StatusTexts *texts, int code, char *out, size_t size);

/* Progress is printed after line `line_index` (0-based) of a page with
 * `total` lines when (total - line_index - 1) % 256 == 0. */
bool p1600_progress_due(uint32_t line_index, uint32_t total, unsigned int *percent);

/* `INFO: <Printingpage> <page>, <percent>% <Complete>...` or an empty
 * string when the PPD has no progress texts. Returns the length written. */
size_t p1600_progress_format(const P1600StatusTexts *texts, int page, unsigned int percent,
                          char *out, size_t size);

/* PJL USTATUS stream framing: a form feed ends a message, a NUL discards
 * what precedes it, NUL 0xAC starts a 16-byte binary block. */
#define P1600_USTATUS_BUFFER 2048U

typedef enum {
    P1600_USTATUS_NONE = 0,
    P1600_USTATUS_DEVICE,     /* @PJL USTATUS DEVICE / @PJL INFO STATUS with CODE */
    P1600_USTATUS_PAGE,
    P1600_USTATUS_JOB_START,
    P1600_USTATUS_JOB_END,
    P1600_USTATUS_JOB_CANCELED,
    P1600_USTATUS_OTHER
} P1600UstatusKind;

typedef struct {
    unsigned char buffer[P1600_USTATUS_BUFFER];
    size_t length;
    unsigned int skip;     /* bytes left of a NUL 0xAC block */
    bool after_nul;
    bool discarding;       /* rest of an oversized message */
} P1600UstatusParser;

/* Classifies one complete message (without its form feed). */
P1600UstatusKind p1600_ustatus_classify(const char *message, int *code);

/* Appends received bytes; `on_message` runs for every complete message.
 * Bytes that do not fit are dropped together with the unterminated
 * message they belong to. */
typedef void (*P1600UstatusHandler)(void *context, P1600UstatusKind kind, int code);
void p1600_ustatus_feed(P1600UstatusParser *parser, const unsigned char *data, size_t length,
                     P1600UstatusHandler on_message, void *context);

/* Backchannel reader thread. It blocks in poll() on the backchannel and a
 * wake-up pipe, and the waiting filter blocks on a condition variable, so a
 * job that waits minutes for the printer causes no timer wake-ups. */
typedef struct {
    int fd;
    int wake[2];                  /* written once to stop the thread */
    FILE *log;
    const P1600StatusTexts *texts;
    P1600UstatusParser parser;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    bool running;
    /* Guarded by lock. */
    bool finished;
    bool heard;                   /* at least one complete USTATUS message */
    bool job_done;                /* USTATUS JOB END or CANCELED */
    uint64_t last_message_ns;
} P1600StatusReader;

/* True when the job runs on a backend status is read from (usb, socket,
 * mdns, dnssd) and `fd` is a readable pipe, socket or device. */
bool p1600_status_backchannel_available(const char *device_uri, int fd);

/* Starts the reader thread. Returns 0, or -1 (status is then not read). */
int p1600_status_reader_start(P1600StatusReader *reader, int fd, const P1600StatusTexts *texts, FILE *log);

/* Waits for the printer to end the job (USTATUS JOB END/CANCELED), the
 * backchannel to close, or a timeout, then stops the thread. Before the
 * printer has answered at all, `quiet_seconds` counted from this call
 * limits the wait; afterwards `idle_seconds` since the later of this call
 * and the last message does. Returns true when the printer ended the job. */
bool p1600_status_reader_finish(P1600StatusReader *reader, double quiet_seconds, double idle_seconds);

#endif /* P1600_STATUS_H */
