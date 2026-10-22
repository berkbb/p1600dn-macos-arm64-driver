#ifndef P1600_RASTER_DOC_H
#define P1600_RASTER_DOC_H

/*
 * AirPrint and IPP Everywhere raster documents for a P1600dn CUPS queue:
 * Apple raster (image/urf) and PWG raster (image/pwg-raster) are decoded one
 * page at a time and written as a PDF with one image per page at the raster
 * resolution. macOS cupsd cannot convert URF for PPD queues, but its PDF chain
 * (cgpdftoraster -> rastertop1600dn) prints this PDF like any other.
 *
 * Apple raster (URF): "UNIRAST\0", a big-endian page count (advisory: Apple's
 * cgpdftoraster writes the PPD's cupsInteger0 there), then per page a 32-byte
 * header (bits per pixel, color space, duplex, quality, two words, width,
 * height, dpi, two words) and the lines. Each line starts with a repeat byte
 * (the line is used n + 1 times); then 0x00-0x7f repeats the next pixel n + 1
 * times, 0x80 fills the rest of the line with white and 0x81-0xff are
 * 257 - n literal pixels.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define P1600_RASTER_MIN_DPI 150U
#define P1600_RASTER_MAX_DPI 1200U
#define P1600_RASTER_MAX_INCHES 18U
#define P1600_RASTER_MAX_PAGES 10000
/* One decoded page in memory: 600 dpi sRGB covers 18 x 18 in, 1200 dpi
 * sRGB covers US Legal. */
#define P1600_RASTER_MAX_PAGE_BYTES ((size_t)768 * 1024 * 1024)
/* A whole document. A few hundred bytes of URF describe a 744 MB page, so
 * the pages a document decodes to are limited, not only its size: 32 GiB is
 * about 950 gray or 320 sRGB US Letter pages at 600 dpi (more than the 256 MB
 * an AirPrint document may have) and under a minute of conversion on an M1.
 * Line repeats can also make a PDF far larger than the document. */
#define P1600_RASTER_MAX_DOCUMENT_BYTES ((uint64_t)32 * 1024 * 1024 * 1024)
#define P1600_RASTER_MAX_PDF_BYTES ((uint64_t)1024 * 1024 * 1024)

typedef enum { P1600_RASTER_FORMAT_URF, P1600_RASTER_FORMAT_PWG } P1600RasterFormat;

typedef struct {
    uint32_t width, height;     /* pixels */
    uint32_t dpi_x, dpi_y;
    unsigned components;        /* 1: gray (0 black, 255 white), 3: RGB */
    bool device;                /* device gray/RGB rather than sGray/sRGB */
    unsigned char *pixels;      /* height rows of width * components bytes */
} P1600RasterPage;

/* Reads like read(2): bytes, 0 at the end, -1 on error. */
typedef ssize_t (*P1600RasterRead)(void *context, void *buffer, size_t length);

/* Receives each decoded page. It may take the pixels (setting page->pixels
 * to NULL); the decoder frees whatever is left. Non-zero stops decoding. */
typedef int (*P1600RasterSink)(void *context, P1600RasterPage *page, char *error, size_t error_size);

/* Decode a whole document. Return the page count, or -1 with a reason. */
int p1600_urf_decode(P1600RasterRead read, void *context, P1600RasterSink sink, void *sink_context, char *error,
                  size_t error_size);
int p1600_pwg_decode(P1600RasterRead read, void *context, P1600RasterSink sink, void *sink_context, char *error,
                  size_t error_size);

/* A PDF written to `fd` with CoreGraphics, one image per page. The page size
 * in points is pixels x 72 / dpi; gray pages are DeviceGray, sRGB pages sRGB. */
typedef struct P1600PdfWriter P1600PdfWriter;

P1600PdfWriter *p1600_pdf_writer_open(int fd, char *error, size_t error_size);
/* Takes page->pixels in every case (sets it to NULL). */
int p1600_pdf_writer_add_page(P1600PdfWriter *writer, P1600RasterPage *page, char *error, size_t error_size);
/* Finishes the file and frees the writer; -1 when nothing or not all was written. */
int p1600_pdf_writer_close(P1600PdfWriter *writer, char *error, size_t error_size);

typedef struct {
    int pages;
    double width_points, height_points;    /* first page */
    uint32_t dpi_x, dpi_y;                 /* first page */
} P1600RasterSummary;

/* Limits of one conversion, checked before each page is written. */
typedef struct {
    uint64_t max_decoded_bytes;     /* pixels of all pages; 0: P1600_RASTER_MAX_DOCUMENT_BYTES */
    uint64_t max_pdf_bytes;         /* 0: P1600_RASTER_MAX_PDF_BYTES */
    bool (*stop)(void *context);    /* true stops (job canceled, server stopping); may be NULL */
    void *context;
    /* > 0 (print-scaling=fit): each page image is scaled down, centred, into
     * the page less this margin in points, the printer's printable area. */
    double fit_margin_points;
} P1600RasterLimits;

/* True when the first page of the PDF at `path` is wider than tall (its
 * CropBox, turned by its /Rotate). */
bool p1600_pdf_landscape(const char *path);

/* Converts the raster document read from `in_fd` into a PDF written to
 * `pdf_fd`; `limits` may be NULL for the defaults. Returns the page count,
 * or -1 with a reason. */
int p1600_raster_to_pdf(P1600RasterFormat format, int in_fd, int pdf_fd, const P1600RasterLimits *limits,
                     P1600RasterSummary *summary, char *error, size_t error_size);

#endif /* P1600_RASTER_DOC_H */
