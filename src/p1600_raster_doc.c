#include "p1600_raster_doc.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <cups/raster.h>

#define READ_BUFFER_SIZE (64U * 1024U)
#define URF_FILE_HEADER 12U
#define URF_PAGE_HEADER 32U

static int fail(char *error, size_t error_size, const char *format, ...) {
    if (error && error_size > 0) {
        va_list arguments;
        va_start(arguments, format);
        vsnprintf(error, error_size, format, arguments);
        va_end(arguments);
    }
    return -1;
}

/* --- buffered input ------------------------------------------------------- */

typedef struct {
    P1600RasterRead read;
    void *context;
    size_t position, length;
    bool end, failed;
    unsigned char buffer[READ_BUFFER_SIZE];
} Reader;

static Reader *reader_new(P1600RasterRead read, void *context) {
    Reader *reader = malloc(sizeof(*reader));
    if (!reader) return NULL;
    reader->read = read;
    reader->context = context;
    reader->position = reader->length = 0;
    reader->end = reader->failed = false;
    return reader;
}

/* True when buffered bytes are available. */
static bool reader_fill(Reader *reader) {
    if (reader->position < reader->length) return true;
    if (reader->end || reader->failed) return false;
    ssize_t count = reader->read(reader->context, reader->buffer, sizeof(reader->buffer));
    if (count < 0) {
        reader->failed = true;
        return false;
    }
    if (count == 0) {
        reader->end = true;
        return false;
    }
    reader->position = 0;
    reader->length = (size_t)count;
    return true;
}

/* True when `count` bytes are buffered (count <= the buffer size). */
static bool reader_peek(Reader *reader, size_t count) {
    while (reader->length - reader->position < count) {
        if (reader->end || reader->failed) return false;
        if (reader->position > 0) {
            memmove(reader->buffer, reader->buffer + reader->position, reader->length - reader->position);
            reader->length -= reader->position;
            reader->position = 0;
        }
        ssize_t got = reader->read(reader->context, reader->buffer + reader->length,
                                   sizeof(reader->buffer) - reader->length);
        if (got < 0) {
            reader->failed = true;
        } else if (got == 0) {
            reader->end = true;
        } else {
            reader->length += (size_t)got;
        }
    }
    return true;
}

static inline int reader_byte(Reader *reader) {
    if (reader->position >= reader->length && !reader_fill(reader)) return -1;
    return reader->buffer[reader->position++];
}

/* Copies up to `length` bytes; fewer only at the end or on an error. */
static size_t reader_bytes(Reader *reader, unsigned char *out, size_t length) {
    size_t done = 0;
    while (done < length && reader_fill(reader)) {
        size_t available = reader->length - reader->position;
        size_t take = length - done < available ? length - done : available;
        memcpy(out + done, reader->buffer + reader->position, take);
        reader->position += take;
        done += take;
    }
    return done;
}

static uint32_t be32(const unsigned char *bytes) {
    return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 | (uint32_t)bytes[2] << 8 | (uint32_t)bytes[3];
}

/* The limits every decoded page must meet; `bytes` receives its size. */
static int check_geometry(uint32_t width, uint32_t height, uint32_t dpi_x, uint32_t dpi_y, unsigned components,
                          size_t *bytes, char *error, size_t error_size) {
    if (dpi_x < P1600_RASTER_MIN_DPI || dpi_x > P1600_RASTER_MAX_DPI || dpi_y < P1600_RASTER_MIN_DPI ||
        dpi_y > P1600_RASTER_MAX_DPI) {
        return fail(error, error_size, "Unsupported resolution %ux%u dpi", dpi_x, dpi_y);
    }
    if (width == 0 || height == 0) return fail(error, error_size, "Empty page (%ux%u pixels)", width, height);
    if ((uint64_t)width > (uint64_t)P1600_RASTER_MAX_INCHES * dpi_x ||
        (uint64_t)height > (uint64_t)P1600_RASTER_MAX_INCHES * dpi_y) {
        return fail(error, error_size, "Page of %ux%u pixels at %ux%u dpi is larger than %u inches", width, height,
                    dpi_x, dpi_y, P1600_RASTER_MAX_INCHES);
    }
    uint64_t total = (uint64_t)width * components * height;
    if (total > P1600_RASTER_MAX_PAGE_BYTES) {
        return fail(error, error_size, "Page of %ux%u pixels needs too much memory", width, height);
    }
    *bytes = (size_t)total;
    return 0;
}

/* --- Apple raster (URF) ------------------------------------------------------ */

static int urf_page(Reader *reader, const unsigned char *header, int number, P1600RasterPage *page, char *error,
                    size_t error_size) {
    unsigned bits = header[0], space = header[1];
    memset(page, 0, sizeof(*page));
    page->width = be32(header + 12);
    page->height = be32(header + 16);
    page->dpi_x = page->dpi_y = be32(header + 20);
    /* 0 sGray, 4 device gray (cgpdftoraster for gray PPDs), 1 sRGB, 5 device RGB. */
    if ((space == 0 || space == 4) && bits == 8) {
        page->components = 1;
        page->device = space == 4;
    } else if ((space == 1 || space == 5) && bits == 24) {
        page->components = 3;
        page->device = space == 5;
    } else {
        return fail(error, error_size, "URF page %d: unsupported color space %u with %u bits per pixel", number,
                    space, bits);
    }
    size_t bytes = 0;
    char reason[200];
    if (check_geometry(page->width, page->height, page->dpi_x, page->dpi_y, page->components, &bytes, reason,
                       sizeof(reason)) != 0) {
        return fail(error, error_size, "URF page %d: %s", number, reason);
    }
    page->pixels = malloc(bytes);
    if (!page->pixels) return fail(error, error_size, "URF page %d: out of memory", number);

    const size_t components = page->components;
    const size_t width = page->width, height = page->height;
    const size_t row_bytes = width * components;
    for (size_t y = 0; y < height;) {
        int repeat = reader_byte(reader);
        if (repeat < 0) goto truncated;
        unsigned char *row = page->pixels + y * row_bytes;
        size_t x = 0;
        while (x < width) {
            int code = reader_byte(reader);
            if (code < 0) goto truncated;
            if (code == 0x80) {
                memset(row + x * components, 0xff, (width - x) * components);
                x = width;
            } else if (code < 0x80) {
                size_t count = (size_t)code + 1U;
                unsigned char pixel[3];
                if (count > width - x) goto overflow;
                if (reader_bytes(reader, pixel, components) != components) goto truncated;
                unsigned char *out = row + x * components;
                if (components == 1) {
                    memset(out, pixel[0], count);
                } else {
                    for (size_t i = 0; i < count; i++, out += 3) memcpy(out, pixel, 3);
                }
                x += count;
            } else {
                size_t count = 257U - (size_t)code;
                if (count > width - x) goto overflow;
                if (reader_bytes(reader, row + x * components, count * components) != count * components) {
                    goto truncated;
                }
                x += count;
            }
        }
        size_t lines = (size_t)repeat + 1U;
        if (lines > height - y) {
            free(page->pixels);
            page->pixels = NULL;
            return fail(error, error_size, "URF page %d: line %zu repeats past the end of the page", number, y + 1U);
        }
        for (size_t copy = 1; copy < lines; copy++) memcpy(row + copy * row_bytes, row, row_bytes);
        y += lines;
    }
    return 0;

truncated:
    free(page->pixels);
    page->pixels = NULL;
    return fail(error, error_size, reader->failed ? "URF page %d: read error" : "URF page %d: truncated data",
                number);
overflow:
    free(page->pixels);
    page->pixels = NULL;
    return fail(error, error_size, "URF page %d: pixels run past the end of a line", number);
}

int p1600_urf_decode(P1600RasterRead read, void *context, P1600RasterSink sink, void *sink_context, char *error,
                  size_t error_size) {
    if (!read || !sink) return fail(error, error_size, "Invalid arguments");
    Reader *reader = reader_new(read, context);
    if (!reader) return fail(error, error_size, "Out of memory");
    int pages = -1;
    unsigned char header[URF_PAGE_HEADER];
    if (reader_bytes(reader, header, URF_FILE_HEADER) != URF_FILE_HEADER || memcmp(header, "UNIRAST", 8) != 0) {
        fail(error, error_size, "Not an Apple raster (URF) document");
        goto done;
    }
    /* The page count in the file header is not reliable; pages run to the end. */
    for (int count = 0;;) {
        size_t got = reader_bytes(reader, header, URF_PAGE_HEADER);
        if (got == 0 && reader->end) {
            if (count == 0) {
                fail(error, error_size, "URF document without pages");
                goto done;
            }
            pages = count;
            break;
        }
        if (got != URF_PAGE_HEADER) {
            fail(error, error_size, reader->failed ? "URF read error" : "URF page %d: truncated page header",
                 count + 1);
            goto done;
        }
        if (count >= P1600_RASTER_MAX_PAGES) {
            fail(error, error_size, "URF document has more than %d pages", P1600_RASTER_MAX_PAGES);
            goto done;
        }
        P1600RasterPage page;
        if (urf_page(reader, header, count + 1, &page, error, error_size) != 0) goto done;
        int status = sink(sink_context, &page, error, error_size);
        free(page.pixels);
        if (status != 0) goto done;
        count++;
    }
done:
    free(reader);
    return pages;
}

/* --- PWG raster ---------------------------------------------------------------- */

static ssize_t pwg_io(void *context, unsigned char *buffer, size_t length) {
    Reader *reader = context;
    if (!reader_fill(reader)) {
        if (!reader->failed) return 0;
        errno = EIO;  /* libcups retries on EINTR/EAGAIN */
        return -1;
    }
    size_t available = reader->length - reader->position;
    size_t take = length < available ? length : available;
    memcpy(buffer, reader->buffer + reader->position, take);
    reader->position += take;
    return (ssize_t)take;
}

typedef enum { PWG_GRAY8, PWG_RGB8, PWG_BLACK1 } PwgKind;

static int pwg_header(const cups_page_header2_t *header, int number, PwgKind *kind, P1600RasterPage *page,
                      size_t *bytes, char *error, size_t error_size) {
    memset(page, 0, sizeof(*page));
    page->width = header->cupsWidth;
    page->height = header->cupsHeight;
    page->dpi_x = header->HWResolution[0];
    page->dpi_y = header->HWResolution[1];
    unsigned space = (unsigned)header->cupsColorSpace;
    if (header->cupsColorOrder != CUPS_ORDER_CHUNKED) {
        return fail(error, error_size, "PWG page %d: color order %u is not chunked", number,
                    (unsigned)header->cupsColorOrder);
    }
    if (header->cupsBitsPerColor == 8 && header->cupsBitsPerPixel == 8 &&
        (space == CUPS_CSPACE_SW || space == CUPS_CSPACE_W)) {
        *kind = PWG_GRAY8;
        page->components = 1;
        page->device = space == CUPS_CSPACE_W;
    } else if (header->cupsBitsPerColor == 8 && header->cupsBitsPerPixel == 24 &&
               (space == CUPS_CSPACE_SRGB || space == CUPS_CSPACE_RGB)) {
        *kind = PWG_RGB8;
        page->components = 3;
        page->device = space == CUPS_CSPACE_RGB;
    } else if (header->cupsBitsPerColor == 1 && header->cupsBitsPerPixel == 1 && space == CUPS_CSPACE_K) {
        *kind = PWG_BLACK1;
        page->components = 1;
        page->device = true;
    } else {
        return fail(error, error_size, "PWG page %d: unsupported color space %u with %u bits per pixel", number,
                    space, (unsigned)header->cupsBitsPerPixel);
    }
    char reason[200];
    if (check_geometry(page->width, page->height, page->dpi_x, page->dpi_y, page->components, bytes, reason,
                       sizeof(reason)) != 0) {
        return fail(error, error_size, "PWG page %d: %s", number, reason);
    }
    uint64_t line = ((uint64_t)header->cupsWidth * header->cupsBitsPerPixel + 7U) / 8U;
    if (header->cupsBytesPerLine != line) {
        return fail(error, error_size, "PWG page %d: %u bytes per line for %u pixels", number,
                    (unsigned)header->cupsBytesPerLine, (unsigned)header->cupsWidth);
    }
    return 0;
}

int p1600_pwg_decode(P1600RasterRead read, void *context, P1600RasterSink sink, void *sink_context, char *error,
                  size_t error_size) {
    if (!read || !sink) return fail(error, error_size, "Invalid arguments");
    Reader *reader = reader_new(read, context);
    if (!reader) return fail(error, error_size, "Out of memory");
    int pages = -1;
    cups_raster_t *raster = NULL;
    unsigned char *line = NULL;
    /* PWG raster always starts with the "RaS2" synchronization word. */
    if (!reader_peek(reader, 4) || memcmp(reader->buffer + reader->position, "RaS2", 4) != 0) {
        fail(error, error_size, "Not a PWG raster document");
        goto done;
    }
    raster = cupsRasterOpenIO(pwg_io, reader, CUPS_RASTER_READ);
    if (!raster) {
        fail(error, error_size, "Not a PWG raster document");
        goto done;
    }
    int count = 0;
    cups_page_header2_t header;
    while (cupsRasterReadHeader2(raster, &header)) {
        if (count >= P1600_RASTER_MAX_PAGES) {
            fail(error, error_size, "PWG document has more than %d pages", P1600_RASTER_MAX_PAGES);
            goto done;
        }
        P1600RasterPage page;
        PwgKind kind = PWG_GRAY8;
        size_t bytes = 0;
        if (pwg_header(&header, count + 1, &kind, &page, &bytes, error, error_size) != 0) goto done;
        if (header.cupsBytesPerLine == 0 || bytes == 0) {
            fail(error, error_size, "PWG page %d: empty page", count + 1);
            goto done;
        }
        free(line);
        line = malloc(header.cupsBytesPerLine);
        page.pixels = malloc(bytes);
        if (!line || !page.pixels) {
            free(page.pixels);
            fail(error, error_size, "PWG page %d: out of memory", count + 1);
            goto done;
        }
        const size_t row_bytes = (size_t)page.width * page.components;
        for (uint32_t y = 0; y < page.height; y++) {
            if (cupsRasterReadPixels(raster, line, header.cupsBytesPerLine) != header.cupsBytesPerLine) {
                free(page.pixels);
                fail(error, error_size, reader->failed ? "PWG page %d: read error" : "PWG page %d: truncated data",
                     count + 1);
                goto done;
            }
            unsigned char *row = page.pixels + (size_t)y * row_bytes;
            if (kind == PWG_BLACK1) {
                /* black_1: a set bit is black ink. */
                for (uint32_t x = 0; x < page.width; x++) {
                    row[x] = (line[x >> 3] & (0x80U >> (x & 7U))) ? 0x00 : 0xff;
                }
            } else {
                memcpy(row, line, row_bytes);
            }
        }
        int status = sink(sink_context, &page, error, error_size);
        free(page.pixels);
        if (status != 0) goto done;
        count++;
    }
    if (reader->failed) {
        fail(error, error_size, "PWG raster read error");
    } else if (!reader->end) {
        /* libcups stopped before the end of the data: a damaged page header. */
        fail(error, error_size, "PWG page %d: invalid page header", count + 1);
    } else if (count == 0) {
        fail(error, error_size, "PWG raster document without pages");
    } else {
        pages = count;
    }
done:
    if (raster) cupsRasterClose(raster);
    free(line);
    free(reader);
    return pages;
}

/* --- PDF output --------------------------------------------------------------- */

struct P1600PdfWriter {
    CGContextRef context;
    int fd;
    int pages;
    bool write_failed;
};

static size_t pdf_put(void *info, const void *buffer, size_t count) {
    P1600PdfWriter *writer = info;
    const unsigned char *bytes = buffer;
    size_t left = count;
    while (left > 0 && !writer->write_failed) {
        ssize_t written = write(writer->fd, bytes, left);
        if (written < 0) {
            if (errno == EINTR) continue;
            writer->write_failed = true;
            break;
        }
        bytes += written;
        left -= (size_t)written;
    }
    return writer->write_failed ? 0 : count;
}

static void pdf_release(void *info) {
    (void)info;
}

P1600PdfWriter *p1600_pdf_writer_open(int fd, char *error, size_t error_size) {
    if (fd < 0) {
        fail(error, error_size, "Invalid PDF output");
        return NULL;
    }
    P1600PdfWriter *writer = calloc(1, sizeof(*writer));
    if (!writer) {
        fail(error, error_size, "Out of memory");
        return NULL;
    }
    writer->fd = fd;
    CGDataConsumerCallbacks callbacks = {pdf_put, pdf_release};
    CGDataConsumerRef consumer = CGDataConsumerCreate(writer, &callbacks);
    CFMutableDictionaryRef info =
        CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (info) CFDictionarySetValue(info, kCGPDFContextCreator, CFSTR("p1600dn-airprint"));
    if (consumer) writer->context = CGPDFContextCreate(consumer, NULL, info);
    if (info) CFRelease(info);
    if (consumer) CGDataConsumerRelease(consumer);
    if (!writer->context) {
        free(writer);
        fail(error, error_size, "Unable to create the PDF context");
        return NULL;
    }
    return writer;
}

static void release_pixels(void *info, const void *data, size_t size) {
    (void)info;
    (void)size;
    free((void *)data);
}

int p1600_pdf_writer_add_page(P1600PdfWriter *writer, P1600RasterPage *page, char *error, size_t error_size) {
    unsigned char *pixels = page ? page->pixels : NULL;
    if (page) page->pixels = NULL;
    if (!writer || !page || !pixels || (page->components != 1 && page->components != 3) || page->dpi_x == 0 ||
        page->dpi_y == 0) {
        free(pixels);
        return fail(error, error_size, "Invalid page");
    }
    size_t row_bytes = (size_t)page->width * page->components;
    size_t bytes = row_bytes * page->height;
    /* CoreGraphics owns the pixels from here and frees them after the page. */
    CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, pixels, bytes, release_pixels);
    if (!provider) {
        free(pixels);
        return fail(error, error_size, "Out of memory");
    }
    CGColorSpaceRef space = page->components == 1 ? CGColorSpaceCreateDeviceGray()
                            : page->device        ? CGColorSpaceCreateDeviceRGB()
                                                  : CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGImageRef image = space ? CGImageCreate(page->width, page->height, 8, 8 * page->components, row_bytes, space,
                                             (CGBitmapInfo)kCGImageAlphaNone, provider, NULL, false,
                                             kCGRenderingIntentDefault)
                             : NULL;
    CGDataProviderRelease(provider);
    if (space) CGColorSpaceRelease(space);
    if (!image) return fail(error, error_size, "Unable to create the page image");

    CGRect box = CGRectMake(0, 0, (CGFloat)page->width * 72.0 / page->dpi_x,
                            (CGFloat)page->height * 72.0 / page->dpi_y);
    CFDataRef box_data = CFDataCreate(NULL, (const UInt8 *)&box, sizeof(box));
    const void *keys[] = {kCGPDFContextMediaBox};
    const void *values[] = {box_data};
    CFDictionaryRef page_info = box_data ? CFDictionaryCreate(NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
                                                              &kCFTypeDictionaryValueCallBacks)
                                         : NULL;
    if (!page_info) {
        if (box_data) CFRelease(box_data);
        CGImageRelease(image);
        return fail(error, error_size, "Out of memory");
    }
    CGPDFContextBeginPage(writer->context, page_info);
    CGContextDrawImage(writer->context, box, image);
    CGPDFContextEndPage(writer->context);
    CGImageRelease(image);
    CFRelease(page_info);
    CFRelease(box_data);
    writer->pages++;
    if (writer->write_failed) return fail(error, error_size, "Unable to write the PDF: %s", strerror(errno));
    return 0;
}

int p1600_pdf_writer_close(P1600PdfWriter *writer, char *error, size_t error_size) {
    if (!writer) return fail(error, error_size, "Invalid PDF writer");
    CGPDFContextClose(writer->context);
    CGContextRelease(writer->context);
    int status = 0;
    if (writer->write_failed) {
        status = fail(error, error_size, "Unable to write the PDF");
    } else if (writer->pages == 0) {
        status = fail(error, error_size, "PDF without pages");
    }
    free(writer);
    return status;
}

/* --- raster document to PDF ---------------------------------------------------- */

static ssize_t fd_read(void *context, void *buffer, size_t length) {
    int fd = *(const int *)context;
    for (;;) {
        ssize_t count = read(fd, buffer, length);
        if (count < 0 && errno == EINTR) continue;
        return count;
    }
}

typedef struct {
    P1600PdfWriter *writer;
    P1600RasterSummary *summary;
} PdfSink;

static int pdf_sink(void *context, P1600RasterPage *page, char *error, size_t error_size) {
    PdfSink *sink = context;
    if (sink->summary->pages == 0) {
        sink->summary->width_points = (double)page->width * 72.0 / page->dpi_x;
        sink->summary->height_points = (double)page->height * 72.0 / page->dpi_y;
        sink->summary->dpi_x = page->dpi_x;
        sink->summary->dpi_y = page->dpi_y;
    }
    sink->summary->pages++;
    return p1600_pdf_writer_add_page(sink->writer, page, error, error_size);
}

int p1600_raster_to_pdf(P1600RasterFormat format, int in_fd, int pdf_fd, P1600RasterSummary *summary, char *error,
                     size_t error_size) {
    P1600RasterSummary local;
    if (!summary) summary = &local;
    memset(summary, 0, sizeof(*summary));
    P1600PdfWriter *writer = p1600_pdf_writer_open(pdf_fd, error, error_size);
    if (!writer) return -1;
    PdfSink sink = {writer, summary};
    int pages = format == P1600_RASTER_FORMAT_URF ? p1600_urf_decode(fd_read, &in_fd, pdf_sink, &sink, error, error_size)
                                               : p1600_pwg_decode(fd_read, &in_fd, pdf_sink, &sink, error, error_size);
    char close_error[200];
    int closed = p1600_pdf_writer_close(writer, close_error, sizeof(close_error));
    if (pages < 0) return -1;
    if (closed != 0) return fail(error, error_size, "%s", close_error);
    return pages;
}
