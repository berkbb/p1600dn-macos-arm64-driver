#ifndef P1600_DIAGNOSTIC_H
#define P1600_DIAGNOSTIC_H

/*
 * The driver's diagnostic page, laid out like the printer's own information
 * pages: one sheet, front and back, that shows at a glance whether the
 * driver does what it promises.
 *   - Bottom edge: a black bar is the last thing printed; nothing may appear
 *     below it (the stripes this printer once printed there).
 *   - Gray levels 0-100 %, fine lines of 1-8 printer dots, text sizes.
 *   - Corner marks on the printable area (12 pt from each edge).
 *   - Arrows on both sides for the 2-sided orientation.
 *   - The printer, queue, connection, paper, resolution and driver version.
 * The page is a PDF printed through the CUPS queue, so it takes the path of
 * every job: cgpdftoraster, the driver filter and its page guard.
 *
 * Sürücünün teşhis sayfası (yazıcının bilgi sayfaları düzeninde).
 */

#include <stdbool.h>
#include <stddef.h>

#define P1600_DRIVER_VERSION "6.0.0"

typedef enum {
    P1600_SIDES_ONE = 0,
    P1600_SIDES_LONG_EDGE,
    P1600_SIDES_SHORT_EDGE
} P1600Sides;

typedef struct {
    char language[8];       /* "tr" or "en" (every other language prints English) */
    char queue[128];
    char make_model[256];
    char connection[160];
    char paper[32];         /* PPD PageSize name ("A4") */
    char paper_label[96];   /* "A4 (210 × 297 mm)" */
    double width;           /* page size in points */
    double height;
    char resolution[64];
    P1600Sides sides;
    char date[64];
    char system[64];
} P1600DiagnosticInfo;

/* "tr" for Turkish localizations ("tr", "tr_TR", "tr-TR"), else "en". */
const char *p1600_diagnostic_language(const char *localization);

/* A4, long-edge, this Mac's macOS version and the current time, no queue. */
void p1600_diagnostic_defaults(P1600DiagnosticInfo *info, const char *localization);

/* Sets the paper from a PPD PageSize name ("A4", "Letter", ...), a PWG
 * media name ("iso_a4_210x297mm") or a size in points (within one point).
 * Returns 0, or -1 when the printer has no such paper. */
int p1600_diagnostic_set_paper(P1600DiagnosticInfo *info, const char *name);
int p1600_diagnostic_set_paper_size(P1600DiagnosticInfo *info, double width, double height);

/* "one-sided", "two-sided-long-edge", "two-sided-short-edge". */
const char *p1600_sides_keyword(P1600Sides sides);
/* Accepts the keywords and "one", "long", "short". Returns 0, or -1. */
int p1600_sides_parse(const char *text, P1600Sides *sides);

/* The resolution the page names and is printed at, "600 dpi" or "FastRes
 * 1200": the queue's Resolution option (the user's lpoptions or the queue's
 * own) when it has one, else the queue PPD's default as cupsd reports it
 * (printer-resolution-default; 0 x 0 when unknown). */
const char *p1600_diagnostic_resolution(const char *option, int default_x, int default_y);

/* The PPD Resolution choice the page is printed with ("600dpi" or
 * "1200x600dpi"), the one info->resolution names. */
const char *p1600_diagnostic_resolution_option(const P1600DiagnosticInfo *info);

/* Queue, printer, connection, default paper and resolution of a CUPS queue.
 * Returns 0, or -1 with a reason. */
int p1600_diagnostic_info_for_queue(const char *queue, P1600DiagnosticInfo *info, char *error, size_t error_size);

/* Writes the two pages as a PDF. Returns 0, or -1 with a reason. */
int p1600_diagnostic_write_pdf(const char *path, const P1600DiagnosticInfo *info, char *error, size_t error_size);

/* Writes the PDF to a private temporary file and prints it on info->queue
 * with info->sides, its paper and its resolution. Returns 0 with the CUPS
 * job id, or -1 with a reason. */
int p1600_diagnostic_print(const P1600DiagnosticInfo *info, int *job_id, char *error, size_t error_size);

#endif /* P1600_DIAGNOSTIC_H */
