#ifndef ZJS_ENCODER_H
#define ZJS_ENCODER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/*
 * HP LaserJet Professional P1600dn ZjStream writer.
 *
 * The byte layout is the one the printer expects:
 * PJL job header with CRLF lines and ENTER LANGUAGE=ZJS, "JZJZ", START_DOC
 * with DMCOLLATE only, per page START_PAGE (fixed item order), one JBIG image
 * (L0 128, MX 0, order 3, options 0x5c, trailing stripe zeros kept) in
 * 64 KiB BID chunks, END_JBIG, END_PAGE, then END_DOC and the PJL trailer.
 */

typedef struct {
    const char *job_name;  /* CUPS job title, sent as @PJL JOB NAME */
    int density;           /* 1..5, PJL print density (3 when out of range) */
    bool jam_recovery;     /* PJL JAMRECOVERY = AUTO instead of OFF */
    bool collate;          /* START_DOC DMCOLLATE */
} ZjsJobOptions;

typedef struct {
    uint32_t paper;        /* DMPAPER code (DMPAPER_USER, 256, for custom sizes) */
    uint32_t copies;       /* DMCOPIES, printer-side uncollated copies */
    uint32_t source;       /* DMDEFAULTSOURCE */
    uint32_t media;        /* DMMEDIATYPE (PPD cupsMediaType) */
    uint32_t width;        /* VIDEO_X: 600-dpi pixels per line */
    uint32_t height;       /* VIDEO_Y: lines, a multiple of ZJS_VIDEO_LINE_MULTIPLE */
    uint32_t content_height; /* cupsHeight; the lines below it are white (0 = height) */
    uint32_t bpp;          /* 1 = 600 dpi, 2 = FastRes 1200 */
    bool auto_duplex;      /* hardware duplex page (DMDUPLEX 2) */
    bool econo_mode;       /* ECONOMODE item value */
} ZjsPageOptions;

/* Every page height is rounded up to whole 12-line groups of white lines. */
#define ZJS_VIDEO_LINE_MULTIPLE 12U

static inline uint32_t zjs_video_height(uint32_t height) {
    return (height + ZJS_VIDEO_LINE_MULTIPLE - 1U) /
           ZJS_VIDEO_LINE_MULTIPLE * ZJS_VIDEO_LINE_MULTIPLE;
}

/* Bytes per bitmap line: width * bpp bits rounded up to 128 bits. */
static inline size_t zjs_line_bytes(uint32_t width, uint32_t bpp) {
    return ((size_t)width * bpp + 127U) / 128U * 16U;
}

/* PJL job header, ZjStream signature and START_DOC. Returns 0 on success. */
int zjs_start_document(FILE *out, const ZjsJobOptions *job);

/* One page; `bitmap` holds page->height lines of zjs_line_bytes() bytes,
 * MSB-first, 1 = toner. Returns 0 on success. */
int zjs_write_page(FILE *out, const ZjsPageOptions *page, const unsigned char *bitmap);

/* Manual-duplex pause between the two halves of the job. */
int zjs_write_pause(FILE *out);

/* END_DOC and the PJL job trailer. Returns 0 on success. It goes out in
 * two parts, with the printer's USTATUS JOB END awaited in between:
 * zjs_end_job (END_DOC, UEL, @PJL EOJ, UEL) and zjs_end_status
 * (@PJL USTATUSOFF, UEL); zjs_end_document writes both. */
int zjs_end_job(FILE *out);
int zjs_end_status(FILE *out);
int zjs_end_document(FILE *out);

/* Rotate the first `height` lines of a bit-addressed bitmap by 180 degrees
 * without moving the right-side line padding. Returns 0 on success. */
int zjs_rotate_bitmap_180(unsigned char *buf, int width, int height, int stride);

#endif /* ZJS_ENCODER_H */
