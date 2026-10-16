#ifndef P1600_RASTER_H
#define P1600_RASTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <cups/raster.h>

/* cupsInteger0 value the P1600dn PPD hands to the filter. */
#define P1600_RASTER_MODEL_ID 1606U

typedef enum {
    /* PPD 4+ format: 600x600 dpi, 8-bit CUPS_CSPACE_W, screened by the filter;
     * cupsCompression bit 0 selects FastRes 1200 (two bits per pixel). */
    P1600_RASTER_GRAY = 0,
    /* PPD 3.x format kept for existing queues: 1200x600 dpi 1-bit DeviceK,
     * two horizontal 1200-dpi dots per FastRes video pixel. */
    P1600_RASTER_LEGACY_1BIT = 1
} P1600RasterKind;

typedef struct {
    P1600RasterKind kind;
    uint32_t width;          /* VIDEO_X: 600-dpi pixels per line */
    uint32_t height;         /* VIDEO_Y: content height rounded up to 12 lines */
    uint32_t content_height; /* cupsHeight */
    uint32_t bpp;            /* 1 or 2 video bits per pixel */
    size_t line_bytes;       /* zjs_line_bytes(width, bpp) */
} P1600PageGeometry;

/* Validates a raster page header and derives its ZjStream geometry.
 * Returns 0, or -1 after writing a reason into `error`. */
int p1600_page_geometry(const cups_page_header2_t *header, P1600PageGeometry *geometry,
                     char *error, size_t error_size);

/* Reads the page's cupsHeight lines from `raster` and renders them into
 * `bitmap` (geometry->height * geometry->line_bytes bytes, zeroed by the
 * caller): halftone screening for P1600_RASTER_GRAY, bit copy for the legacy format.
 * `rotate` turns the page content by 180 degrees. Returns 0 on success. */
int p1600_render_page(cups_raster_t *raster, const cups_page_header2_t *header,
                   const P1600PageGeometry *geometry, bool rotate, unsigned char *bitmap);

/* p1600_render_page() that reports every VIDEO_Y line as it is done: `line`
 * runs from 0 to geometry->height - 1 (white padding lines included) and
 * `total` is geometry->height. */
typedef void (*P1600LineProgress)(void *context, uint32_t line, uint32_t total);
int p1600_render_page_progress(cups_raster_t *raster, const cups_page_header2_t *header,
                            const P1600PageGeometry *geometry, bool rotate, unsigned char *bitmap,
                            P1600LineProgress progress, void *context);

/* CUPS raster input read through cupsRasterOpenIO(p1600_raster_input_read, &input). */
typedef struct {
    int fd;
    unsigned char sync[4];   /* first four stream bytes */
    size_t delivered;
    bool short_read;         /* end of input inside a read request */
    bool clean_eof;          /* end of input at the start of a read request */
    int io_error;
} P1600RasterInput;

ssize_t p1600_raster_input_read(void *context, unsigned char *buffer, size_t length);

/* After cupsRasterReadHeader2() returns 0: 0 for a clean end of the job, or
 * -1 with the reason (I/O error, truncated stream, rejected page header).
 * Compressed (RaS2) streams are read ahead by libcups, so for them only I/O
 * errors are reported here; truncated pixel data still fails while reading. */
int p1600_raster_input_end(const P1600RasterInput *input, char *error, size_t error_size);

/* Reopens closed standard descriptors on /dev/null so spool files can never
 * take their place. Returns -1 when stdout was closed (the job has nowhere to go). */
int p1600_prepare_standard_descriptors(void);

/* DMPAPER code for a PageSize in points: the PPD's sizes and their landscape
 * forms (exact, then within one point), else DMPAPER_USER (256, custom).
 * 0 (DMPAPER_NONE) is no paper code and is never returned. */
uint32_t p1600_paper_code(uint32_t width_points, uint32_t height_points);

#endif /* P1600_RASTER_H */
