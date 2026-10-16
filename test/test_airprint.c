/*
 * Tests for the AirPrint server (src/p1600_airprint.c) and the raster document
 * converter (src/p1600_raster_doc.c): Apple raster (URF) and PWG raster
 * decoding against hand-built and libcups-written data, the CoreGraphics PDF
 * writer, the 18 media sizes against the PPD, IPP job attributes to CUPS
 * options, the Bonjour TXT record and UUID, the server in test mode (HTTP
 * and IPP edge cases, compression, URF/PWG jobs) and in real mode against a
 * stand-in cupsd on the loopback address. No real CUPS queue is touched and
 * nothing is advertised.
 *
 * Usage: test_airprint [ppd]
 *        test_airprint --write-pwg OUT sgray_8|srgb_8 DPI PAGES   (sample for test_airprint.sh)
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
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
#include <CoreGraphics/CoreGraphics.h>
#include <cups/cups.h>
#include <cups/raster.h>
#include <dns_sd.h>

#include "p1600_airprint.h"
#include "p1600_raster_doc.h"

#define CHECK(condition)                                                                   \
    do {                                                                                   \
        if (!(condition)) {                                                                \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            exit(1);                                                                       \
        }                                                                                  \
    } while (0)

static char temp_root[1024];

/* --- memory buffers ------------------------------------------------------------- */

typedef struct {
    unsigned char *data;
    size_t length, capacity;
} Bytes;

static void bytes_add(Bytes *bytes, const void *data, size_t length) {
    if (bytes->length + length > bytes->capacity) {
        size_t capacity = bytes->capacity ? bytes->capacity : 1024;
        while (capacity < bytes->length + length) capacity *= 2;
        bytes->data = realloc(bytes->data, capacity);
        CHECK(bytes->data != NULL);
        bytes->capacity = capacity;
    }
    if (length) memcpy(bytes->data + bytes->length, data, length);
    bytes->length += length;
}

static void bytes_byte(Bytes *bytes, unsigned value) {
    unsigned char byte = (unsigned char)value;
    bytes_add(bytes, &byte, 1);
}

static void bytes_be32(Bytes *bytes, uint32_t value) {
    unsigned char word[4] = {(unsigned char)(value >> 24), (unsigned char)(value >> 16), (unsigned char)(value >> 8),
                             (unsigned char)value};
    bytes_add(bytes, word, 4);
}

typedef struct {
    const unsigned char *data;
    size_t length, position, chunk;
} MemoryReader;

static ssize_t memory_read(void *context, void *buffer, size_t length) {
    MemoryReader *reader = context;
    size_t left = reader->length - reader->position;
    if (length > left) length = left;
    if (reader->chunk && length > reader->chunk) length = reader->chunk;
    memcpy(buffer, reader->data + reader->position, length);
    reader->position += length;
    return (ssize_t)length;
}

/* Collected pages of a decode. */
typedef struct {
    int count;
    P1600RasterPage pages[8];
} Pages;

static int collect(void *context, P1600RasterPage *page, char *error, size_t error_size) {
    (void)error;
    (void)error_size;
    Pages *pages = context;
    if (pages->count >= 8) return -1;
    pages->pages[pages->count] = *page;
    page->pixels = NULL;
    pages->count++;
    return 0;
}

static void pages_free(Pages *pages) {
    for (int i = 0; i < pages->count; i++) free(pages->pages[i].pixels);
    pages->count = 0;
}

static int decode(P1600RasterFormat format, const Bytes *bytes, size_t chunk, Pages *pages, char *error,
                  size_t error_size) {
    MemoryReader reader = {bytes->data, bytes->length, 0, chunk};
    memset(pages, 0, sizeof(*pages));
    return format == P1600_RASTER_FORMAT_URF ? p1600_urf_decode(memory_read, &reader, collect, pages, error, error_size)
                                          : p1600_pwg_decode(memory_read, &reader, collect, pages, error, error_size);
}

/* --- URF ---------------------------------------------------------------------------- */

static void urf_file_header(Bytes *bytes, uint32_t pages) {
    bytes_add(bytes, "UNIRAST", 8);
    bytes_be32(bytes, pages);
}

static void urf_page_header(Bytes *bytes, unsigned bpp, unsigned space, uint32_t width, uint32_t height, uint32_t dpi) {
    unsigned char header[32];
    memset(header, 0, sizeof(header));
    header[0] = (unsigned char)bpp;
    header[1] = (unsigned char)space;
    header[2] = 1;  /* simplex */
    header[3] = 4;
    for (int i = 0; i < 4; i++) {
        header[12 + i] = (unsigned char)(width >> (24 - 8 * i));
        header[16 + i] = (unsigned char)(height >> (24 - 8 * i));
        header[20 + i] = (unsigned char)(dpi >> (24 - 8 * i));
    }
    bytes_add(bytes, header, sizeof(header));
}

/* A plain URF encoder (repeated lines, pixel runs, literals, white fill). */
static void urf_encode(Bytes *bytes, const unsigned char *pixels, uint32_t width, uint32_t height,
                       unsigned components) {
    size_t row = (size_t)width * components;
    for (uint32_t y = 0; y < height;) {
        const unsigned char *line = pixels + (size_t)y * row;
        uint32_t repeat = 1;
        while (y + repeat < height && repeat < 256 && memcmp(line, line + repeat * row, row) == 0) repeat++;
        bytes_byte(bytes, repeat - 1);
        for (uint32_t x = 0; x < width;) {
            bool white_rest = true;
            for (size_t i = (size_t)x * components; i < row && white_rest; i++) white_rest = line[i] == 0xff;
            if (white_rest && x + 1 < width) {
                bytes_byte(bytes, 0x80);
                break;
            }
            uint32_t run = 1;
            while (x + run < width && run < 128 &&
                   memcmp(line + (size_t)x * components, line + (size_t)(x + run) * components, components) == 0) {
                run++;
            }
            if (run > 1 || x + 1 == width) {
                bytes_byte(bytes, run - 1);
                bytes_add(bytes, line + (size_t)x * components, components);
                x += run;
                continue;
            }
            uint32_t literal = 1;
            while (x + literal < width && literal < 128 &&
                   (x + literal + 1 >= width ||
                    memcmp(line + (size_t)(x + literal) * components, line + (size_t)(x + literal + 1) * components,
                           components) != 0)) {
                literal++;
            }
            if (literal == 1) {
                bytes_byte(bytes, 0);
                bytes_add(bytes, line + (size_t)x * components, components);
            } else {
                bytes_byte(bytes, 257 - literal);
                bytes_add(bytes, line + (size_t)x * components, (size_t)literal * components);
            }
            x += literal;
        }
        y += repeat;
    }
}

static unsigned char *pattern(uint32_t width, uint32_t height, unsigned components, unsigned seed) {
    unsigned char *pixels = malloc((size_t)width * height * components);
    CHECK(pixels != NULL);
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            for (unsigned c = 0; c < components; c++) {
                unsigned char value = 0xff;  /* white margins and blank lines */
                if (y % 7 != 3 && x > 2 && x + 3 < width) {
                    value = (unsigned char)((x * 13 + y * 7 + c * 50 + seed) & 0xff);
                    if ((x / 9 + y / 5) % 3 == 0) value = 0x00;  /* runs */
                }
                pixels[((size_t)y * width + x) * components + c] = value;
            }
        }
    }
    return pixels;
}

static void test_urf(void) {
    char error[256];
    Pages pages;

    /* Hand-built gray page: every code. 6 x 4 pixels at 300 dpi. */
    Bytes bytes = {0};
    urf_file_header(&bytes, 1);
    urf_page_header(&bytes, 8, 4, 6, 4, 300);
    static const unsigned char gray[] = {
        0x01, 0x05, 0x10,                          /* lines 1-2: 0x10 repeated 6 times */
        0x00, 0xfe, 1, 2, 3, 0x80,                 /* line 3: 3 literal pixels, then white */
        0x00, 0x00, 0x00, 0x81, 1, 2, 3, 4, 5,     /* line 4: 0x81 = 128 literals, more than the line holds */
    };
    bytes_add(&bytes, gray, 9);
    /* A correct line 4: one 0x00, 4 literal pixels, one 0x77. */
    static const unsigned char line4[] = {0x00, 0x00, 0x00, 0xfd, 1, 2, 3, 4, 0x00, 0x77};
    bytes_add(&bytes, line4, sizeof(line4));
    CHECK(decode(P1600_RASTER_FORMAT_URF, &bytes, 0, &pages, error, sizeof(error)) == 1);
    CHECK(pages.count == 1 && pages.pages[0].width == 6 && pages.pages[0].height == 4);
    CHECK(pages.pages[0].components == 1 && pages.pages[0].device && pages.pages[0].dpi_x == 300);
    static const unsigned char expected[] = {
        0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
        1,    2,    3,    0xff, 0xff, 0xff, 0x00, 1,    2,    3,    4,    0x77,
    };
    CHECK(memcmp(pages.pages[0].pixels, expected, sizeof(expected)) == 0);
    pages_free(&pages);
    /* The same with 1-byte reads. */
    CHECK(decode(P1600_RASTER_FORMAT_URF, &bytes, 1, &pages, error, sizeof(error)) == 1);
    CHECK(memcmp(pages.pages[0].pixels, expected, sizeof(expected)) == 0);
    pages_free(&pages);

    /* A literal run past the end of the line is refused. */
    Bytes bad = {0};
    urf_file_header(&bad, 1);
    urf_page_header(&bad, 8, 0, 6, 4, 300);
    bytes_add(&bad, gray, sizeof(gray));
    CHECK(decode(P1600_RASTER_FORMAT_URF, &bad, 0, &pages, error, sizeof(error)) < 0 && strstr(error, "past the end"));
    free(bad.data);

    /* sRGB: a two-pixel run, a literal pixel and white fill on 4 x 2. */
    Bytes rgb = {0};
    urf_file_header(&rgb, 1);
    urf_page_header(&rgb, 24, 1, 4, 2, 600);
    static const unsigned char rgb_lines[] = {0x00, 0x01, 10, 20, 30, 0xff, 40, 50, 60, 70, 80, 90,
                                              0x00, 0x00, 1,  2,  3,  0x80};
    bytes_add(&rgb, rgb_lines, sizeof(rgb_lines));
    CHECK(decode(P1600_RASTER_FORMAT_URF, &rgb, 3, &pages, error, sizeof(error)) == 1);
    static const unsigned char rgb_expected[] = {10, 20, 30, 10, 20, 30, 40, 50, 60, 70, 80, 90,
                                                 1,  2,  3,  255, 255, 255, 255, 255, 255, 255, 255, 255};
    CHECK(pages.pages[0].components == 3 && !pages.pages[0].device && pages.pages[0].dpi_y == 600);
    CHECK(memcmp(pages.pages[0].pixels, rgb_expected, sizeof(rgb_expected)) == 0);
    pages_free(&pages);
    free(rgb.data);

    /* Multi-page round trip through the test encoder (the page count in the
     * header is advisory: Apple's cgpdftoraster writes the PPD's 1606). */
    Bytes multi = {0};
    urf_file_header(&multi, 1606);
    unsigned char *page1 = pattern(300, 200, 1, 1), *page2 = pattern(301, 77, 3, 2), *page3 = pattern(150, 150, 1, 3);
    urf_page_header(&multi, 8, 0, 300, 200, 300);
    urf_encode(&multi, page1, 300, 200, 1);
    urf_page_header(&multi, 24, 1, 301, 77, 150);
    urf_encode(&multi, page2, 301, 77, 3);
    size_t two_pages = multi.length;
    urf_page_header(&multi, 8, 4, 150, 150, 1200);
    urf_encode(&multi, page3, 150, 150, 1);
    for (size_t chunk = 0; chunk <= 7; chunk += 7) {
        CHECK(decode(P1600_RASTER_FORMAT_URF, &multi, chunk, &pages, error, sizeof(error)) == 3);
        CHECK(memcmp(pages.pages[0].pixels, page1, 300 * 200) == 0);
        CHECK(pages.pages[1].width == 301 && memcmp(pages.pages[1].pixels, page2, 301 * 77 * 3) == 0);
        CHECK(pages.pages[2].dpi_x == 1200 && memcmp(pages.pages[2].pixels, page3, 150 * 150) == 0);
        pages_free(&pages);
    }
    /* Truncated anywhere inside a page header or its lines. */
    for (size_t cut = 1; cut < multi.length; cut += 1 + cut / 9) {
        if (cut == two_pages) continue;  /* a page boundary is a shorter document */
        Bytes part = {multi.data, cut, cut};
        int result = decode(P1600_RASTER_FORMAT_URF, &part, 0, &pages, error, sizeof(error));
        pages_free(&pages);
        CHECK(result < 0);
    }
    Bytes part = {multi.data, two_pages, two_pages};
    CHECK(decode(P1600_RASTER_FORMAT_URF, &part, 0, &pages, error, sizeof(error)) == 2);
    pages_free(&pages);
    free(page1);
    free(page2);
    free(page3);
    free(multi.data);

    /* Headers that are refused. */
    static const struct {
        unsigned bpp, space;
        uint32_t width, height, dpi;
        const char *reason;
    } refused[] = {
        {8, 0, 100, 100, 100, "resolution"},          {8, 0, 100, 100, 2400, "resolution"},
        {8, 0, 18 * 300 + 1, 10, 300, "larger"},       {8, 0, 10, 18 * 600 + 1, 600, "larger"},
        {8, 0, 0, 10, 300, "Empty"},                   {8, 0, 10, 0, 300, "Empty"},
        {16, 0, 10, 10, 300, "color space"},           {24, 0, 10, 10, 300, "color space"},
        {8, 1, 10, 10, 300, "color space"},            {32, 6, 10, 10, 300, "color space"},
        {8, 2, 10, 10, 300, "color space"},            {24, 3, 10, 10, 300, "color space"},
        {8, 0, 0xffffffffU, 0xffffffffU, 600, "larger"},
    };
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        Bytes header = {0};
        urf_file_header(&header, 1);
        urf_page_header(&header, refused[i].bpp, refused[i].space, refused[i].width, refused[i].height, refused[i].dpi);
        bytes_add(&header, "\x00\x80", 2);
        CHECK(decode(P1600_RASTER_FORMAT_URF, &header, 0, &pages, error, sizeof(error)) < 0);
        if (!strstr(error, refused[i].reason)) fprintf(stderr, "case %zu: %s\n", i, error);
        CHECK(strstr(error, refused[i].reason) != NULL);
        free(header.data);
    }
    /* Bad magic, no pages, a line repeated past the page, a cut file header. */
    Bytes magic = {0};
    bytes_add(&magic, "UNIRAS\0\0\0\0\0\1", 12);
    CHECK(decode(P1600_RASTER_FORMAT_URF, &magic, 0, &pages, error, sizeof(error)) < 0 && strstr(error, "Not an Apple"));
    magic.length = 0;
    urf_file_header(&magic, 0);
    CHECK(decode(P1600_RASTER_FORMAT_URF, &magic, 0, &pages, error, sizeof(error)) < 0 && strstr(error, "without pages"));
    urf_page_header(&magic, 8, 0, 4, 2, 300);
    bytes_add(&magic, "\x02\x80", 2);
    CHECK(decode(P1600_RASTER_FORMAT_URF, &magic, 0, &pages, error, sizeof(error)) < 0 && strstr(error, "repeats past"));
    magic.length = 5;
    CHECK(decode(P1600_RASTER_FORMAT_URF, &magic, 0, &pages, error, sizeof(error)) < 0);
    free(magic.data);
    free(bytes.data);
    puts("[PASS] URF: gray and sRGB, every pixel code, line repeats, 0x80 fill, multi-page, 1-byte reads");
    puts("[PASS] URF: truncated data, oversized pages, bad resolutions, color spaces and headers refused");
}

/* libcups' own Apple raster writer, decoded by p1600_urf_decode. */
static ssize_t bytes_write(void *context, unsigned char *buffer, size_t length) {
    bytes_add(context, buffer, length);
    return (ssize_t)length;
}

static void write_raster_page(cups_raster_t *raster, cups_page_header2_t *header, const unsigned char *pixels) {
    CHECK(cupsRasterWriteHeader2(raster, header));
    for (unsigned y = 0; y < header->cupsHeight; y++) {
        CHECK(cupsRasterWritePixels(raster, (unsigned char *)pixels + (size_t)y * header->cupsBytesPerLine,
                                    header->cupsBytesPerLine) == header->cupsBytesPerLine);
    }
}

static void init_header(cups_page_header2_t *header, const char *type, unsigned width, unsigned height, int dpi) {
    CHECK(cupsRasterInitPWGHeader(header, pwgMediaForPWG("iso_a6_105x148mm"), type, dpi, dpi, "one-sided", NULL));
    header->cupsWidth = width;
    header->cupsHeight = height;
    header->cupsBytesPerLine = (width * header->cupsBitsPerPixel + 7) / 8;
    header->cupsInteger[0] = 2;  /* total page count */
}

static void test_libcups_urf(void) {
    Bytes bytes = {0};
    cups_raster_t *raster = cupsRasterOpenIO(bytes_write, &bytes, CUPS_RASTER_WRITE_APPLE);
    CHECK(raster != NULL);
    cups_page_header2_t header;
    unsigned char *gray = pattern(203, 91, 1, 5), *rgb = pattern(64, 33, 3, 6);
    init_header(&header, "sgray_8", 203, 91, 300);
    write_raster_page(raster, &header, gray);
    init_header(&header, "srgb_8", 64, 33, 600);
    write_raster_page(raster, &header, rgb);
    cupsRasterClose(raster);
    char error[256];
    Pages pages;
    CHECK(decode(P1600_RASTER_FORMAT_URF, &bytes, 0, &pages, error, sizeof(error)) == 2);
    CHECK(pages.pages[0].width == 203 && pages.pages[0].height == 91 && pages.pages[0].components == 1);
    CHECK(memcmp(pages.pages[0].pixels, gray, 203 * 91) == 0);
    CHECK(pages.pages[1].components == 3 && pages.pages[1].dpi_x == 600 &&
          memcmp(pages.pages[1].pixels, rgb, 64 * 33 * 3) == 0);
    pages_free(&pages);
    free(gray);
    free(rgb);
    free(bytes.data);
    puts("[PASS] URF written by libcups (CUPS_RASTER_WRITE_APPLE) decodes pixel for pixel");
}

/* --- PWG raster ------------------------------------------------------------------- */

static void test_pwg(void) {
    char error[256];
    Pages pages;
    Bytes bytes = {0};
    cups_raster_t *raster = cupsRasterOpenIO(bytes_write, &bytes, CUPS_RASTER_WRITE_PWG);
    CHECK(raster != NULL);
    cups_page_header2_t header;
    unsigned char *gray = pattern(255, 120, 1, 7), *rgb = pattern(97, 50, 3, 8);
    init_header(&header, "sgray_8", 255, 120, 300);
    write_raster_page(raster, &header, gray);
    init_header(&header, "srgb_8", 97, 50, 600);
    write_raster_page(raster, &header, rgb);
    /* black_1: 1 is black ink. */
    unsigned char black[2 * 3] = {0xf0, 0x0f, 0x80, 0x00, 0xff, 0x01};
    init_header(&header, "black_1", 20, 2, 150);
    write_raster_page(raster, &header, black);
    cupsRasterClose(raster);
    for (size_t chunk = 0; chunk <= 5; chunk += 5) {
        CHECK(decode(P1600_RASTER_FORMAT_PWG, &bytes, chunk, &pages, error, sizeof(error)) == 3);
        CHECK(pages.pages[0].components == 1 && !pages.pages[0].device &&
              memcmp(pages.pages[0].pixels, gray, 255 * 120) == 0);
        CHECK(pages.pages[1].components == 3 && pages.pages[1].dpi_x == 600 &&
              memcmp(pages.pages[1].pixels, rgb, 97 * 50 * 3) == 0);
        const unsigned char *b = pages.pages[2].pixels;
        /* Line 1: f0 0f 80, line 2: 00 ff 01 (the last bit is padding). */
        CHECK(pages.pages[2].width == 20 && b[0] == 0 && b[3] == 0 && b[4] == 0xff && b[11] == 0xff && b[12] == 0 &&
              b[15] == 0 && b[16] == 0 && b[17] == 0xff && b[19] == 0xff);
        CHECK(b[20 + 7] == 0xff && b[20 + 8] == 0 && b[20 + 15] == 0 && b[20 + 16] == 0xff && b[20 + 19] == 0xff);
        pages_free(&pages);
    }
    /* Truncated pixel data. */
    Bytes part = {bytes.data, bytes.length - 3, bytes.length - 3};
    CHECK(decode(P1600_RASTER_FORMAT_PWG, &part, 0, &pages, error, sizeof(error)) < 0);
    pages_free(&pages);
    part.length = 1796 + 4 + 20;
    CHECK(decode(P1600_RASTER_FORMAT_PWG, &part, 0, &pages, error, sizeof(error)) < 0);
    pages_free(&pages);
    /* Not PWG: CUPS raster sync words, garbage, empty. */
    Bytes other = {0};
    bytes_add(&other, "RaS3", 4);
    bytes_add(&other, bytes.data + 4, 1796);
    CHECK(decode(P1600_RASTER_FORMAT_PWG, &other, 0, &pages, error, sizeof(error)) < 0 && strstr(error, "Not a PWG"));
    other.length = 0;
    CHECK(decode(P1600_RASTER_FORMAT_PWG, &other, 0, &pages, error, sizeof(error)) < 0);
    bytes_add(&other, "RaS2", 4);
    CHECK(decode(P1600_RASTER_FORMAT_PWG, &other, 0, &pages, error, sizeof(error)) < 0 && strstr(error, "without pages"));
    free(other.data);
    free(bytes.data);

    /* Types and sizes that are refused. */
    static const struct {
        const char *type;
        unsigned width, height;
        int dpi;
        const char *reason;
    } refused[] = {
        {"cmyk_8", 8, 8, 300, "color space"}, {"srgb_16", 8, 8, 300, "color space"},
        {"sgray_16", 8, 8, 300, "color space"}, {"adobe-rgb_8", 8, 8, 300, "color space"},
        {"sgray_8", 8, 8, 100, "resolution"}, {"sgray_8", 8, 8, 2400, "resolution"},
        {"sgray_8", 18 * 150 + 1, 8, 150, "larger"},
    };
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        Bytes one = {0};
        raster = cupsRasterOpenIO(bytes_write, &one, CUPS_RASTER_WRITE_PWG);
        CHECK(raster != NULL);
        CHECK(cupsRasterInitPWGHeader(&header, pwgMediaForPWG("iso_a6_105x148mm"), refused[i].type, refused[i].dpi,
                                      refused[i].dpi, "one-sided", NULL));
        header.cupsWidth = refused[i].width;
        header.cupsHeight = refused[i].height;
        header.cupsBytesPerLine = (header.cupsWidth * header.cupsBitsPerPixel + 7) / 8;
        size_t size = (size_t)header.cupsBytesPerLine * header.cupsHeight;
        unsigned char *data = calloc(1, size);
        write_raster_page(raster, &header, data);
        cupsRasterClose(raster);
        free(data);
        CHECK(decode(P1600_RASTER_FORMAT_PWG, &one, 0, &pages, error, sizeof(error)) < 0);
        if (!strstr(error, refused[i].reason)) fprintf(stderr, "case %s: %s\n", refused[i].type, error);
        CHECK(strstr(error, refused[i].reason) != NULL);
        free(one.data);
    }
    free(gray);
    free(rgb);
    puts("[PASS] PWG raster: sgray_8, srgb_8 and black_1 from cupsRasterOpenIO(CUPS_RASTER_WRITE_PWG), 5-byte reads");
    puts("[PASS] PWG raster: truncated data, other sync words, cmyk/16-bit/adobe-rgb, resolutions and sizes refused");
}

/* --- PDF writer --------------------------------------------------------------------- */

typedef struct {
    size_t count;
    CGRect boxes[8];
} PdfInfo;

static bool pdf_info(const char *path, PdfInfo *info) {
    memset(info, 0, sizeof(*info));
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    CGPDFDocumentRef document = url ? CGPDFDocumentCreateWithURL(url) : NULL;
    if (url) CFRelease(url);
    if (!document) return false;
    info->count = CGPDFDocumentGetNumberOfPages(document);
    for (size_t i = 0; i < info->count && i < 8; i++) {
        CGPDFPageRef page = CGPDFDocumentGetPage(document, i + 1);
        info->boxes[i] = CGPDFPageGetBoxRect(page, kCGPDFMediaBox);
    }
    CGPDFDocumentRelease(document);
    return true;
}

static bool near(double a, double b) {
    return a - b < 0.01 && b - a < 0.01;
}

static int temp_file(const char *name, char *path, size_t size) {
    snprintf(path, size, "%s/%s", temp_root, name);
    return open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
}

static void test_pdf(void) {
    char error[256], path[1024], in_path[1024];
    /* URF: A4 at 300 dpi (2480 x 3508) and 150 x 75 sRGB at 150 dpi. */
    Bytes urf = {0};
    urf_file_header(&urf, 2);
    unsigned char *a4 = pattern(2480, 3508, 1, 9), *small = pattern(150, 75, 3, 10);
    urf_page_header(&urf, 8, 0, 2480, 3508, 300);
    urf_encode(&urf, a4, 2480, 3508, 1);
    urf_page_header(&urf, 24, 1, 150, 75, 150);
    urf_encode(&urf, small, 150, 75, 3);
    int in_fd = temp_file("in.urf", in_path, sizeof(in_path));
    CHECK(in_fd >= 0 && write(in_fd, urf.data, urf.length) == (ssize_t)urf.length && lseek(in_fd, 0, SEEK_SET) == 0);
    int pdf_fd = temp_file("out.pdf", path, sizeof(path));
    P1600RasterSummary summary;
    CHECK(p1600_raster_to_pdf(P1600_RASTER_FORMAT_URF, in_fd, pdf_fd, &summary, error, sizeof(error)) == 2);
    close(in_fd);
    close(pdf_fd);
    CHECK(summary.pages == 2 && near(summary.width_points, 2480 * 72.0 / 300) && summary.dpi_x == 300);
    PdfInfo info;
    CHECK(pdf_info(path, &info) && info.count == 2);
    CHECK(near(info.boxes[0].size.width, 595.2) && near(info.boxes[0].size.height, 841.92));
    CHECK(near(info.boxes[1].size.width, 72.0) && near(info.boxes[1].size.height, 36.0));
    const P1600AirMedia *media = p1600_air_media_by_size((int)(summary.width_points * 2540 / 72 + 0.5),
                                                   (int)(summary.height_points * 2540 / 72 + 0.5));
    CHECK(media && strcmp(media->ppd, "A4") == 0);
    free(a4);
    free(small);

    /* A damaged second page leaves no PDF behind as a success. */
    urf.length -= 10;
    in_fd = temp_file("in.urf", in_path, sizeof(in_path));
    CHECK(write(in_fd, urf.data, urf.length) == (ssize_t)urf.length && lseek(in_fd, 0, SEEK_SET) == 0);
    pdf_fd = temp_file("out.pdf", path, sizeof(path));
    CHECK(p1600_raster_to_pdf(P1600_RASTER_FORMAT_URF, in_fd, pdf_fd, &summary, error, sizeof(error)) < 0);
    CHECK(strstr(error, "page 2") != NULL);
    close(in_fd);
    close(pdf_fd);
    free(urf.data);

    /* PWG raster with three Letter pages at 600 dpi, written page by page. */
    Bytes pwg = {0};
    cups_raster_t *raster = cupsRasterOpenIO(bytes_write, &pwg, CUPS_RASTER_WRITE_PWG);
    cups_page_header2_t header;
    unsigned char *letter = pattern(5100, 6600, 1, 11);
    for (int page = 0; page < 3; page++) {
        CHECK(cupsRasterInitPWGHeader(&header, pwgMediaForPWG("na_letter_8.5x11in"), "sgray_8", 600, 600, "one-sided",
                                      NULL));
        write_raster_page(raster, &header, letter);
    }
    cupsRasterClose(raster);
    free(letter);
    in_fd = temp_file("in.pwg", in_path, sizeof(in_path));
    CHECK(write(in_fd, pwg.data, pwg.length) == (ssize_t)pwg.length && lseek(in_fd, 0, SEEK_SET) == 0);
    pdf_fd = temp_file("out.pdf", path, sizeof(path));
    CHECK(p1600_raster_to_pdf(P1600_RASTER_FORMAT_PWG, in_fd, pdf_fd, &summary, error, sizeof(error)) == 3);
    close(in_fd);
    close(pdf_fd);
    CHECK(pdf_info(path, &info) && info.count == 3);
    for (int i = 0; i < 3; i++) CHECK(near(info.boxes[i].size.width, 612) && near(info.boxes[i].size.height, 792));
    free(pwg.data);

    /* The writer refuses empty documents and bad pages. */
    pdf_fd = temp_file("empty.pdf", path, sizeof(path));
    P1600PdfWriter *writer = p1600_pdf_writer_open(pdf_fd, error, sizeof(error));
    CHECK(writer != NULL);
    P1600RasterPage bad = {10, 10, 300, 300, 2, false, malloc(200)};
    CHECK(p1600_pdf_writer_add_page(writer, &bad, error, sizeof(error)) < 0 && bad.pixels == NULL);
    CHECK(p1600_pdf_writer_close(writer, error, sizeof(error)) < 0 && strstr(error, "without pages"));
    close(pdf_fd);
    CHECK(p1600_pdf_writer_open(-1, error, sizeof(error)) == NULL);
    puts("[PASS] PDF writer: page count and page sizes (pixels x 72 / dpi) checked with CGPDFDocument");
}

/* --- media, formats, options, Bonjour ------------------------------------------------ */

static void test_media(const char *ppd_path) {
    CHECK(p1600_air_media_count == 18);
    /* Every PPD PageSize is in the table with its PaperDimension. */
    FILE *ppd = fopen(ppd_path, "r");
    CHECK(ppd != NULL);
    char line[1024];
    int sizes = 0;
    while (fgets(line, sizeof(line), ppd)) {
        char name[64];
        double width = 0, length = 0;
        if (sscanf(line, "*PaperDimension %63[^/]/%*[^:]: \"%lf %lf\"", name, &width, &length) != 3) continue;
        const P1600AirMedia *media = p1600_air_media_by_ppd(name);
        if (!media) fprintf(stderr, "PPD size %s is missing\n", name);
        CHECK(media != NULL);
        double media_width = media->width * 72.0 / 2540.0, media_length = media->length * 72.0 / 2540.0;
        if (fabs(media_width - width) > 1.5 || fabs(media_length - length) > 1.5) {
            fprintf(stderr, "%s: %.2f x %.2f pt, PPD %.2f x %.2f\n", name, media_width, media_length, width, length);
        }
        CHECK(fabs(media_width - width) <= 1.5 && fabs(media_length - length) <= 1.5);
        sizes++;
    }
    fclose(ppd);
    CHECK(sizes == 18);
    static const char *const pairs[][2] = {
        {"na_letter_8.5x11in", "Letter"},      {"na_legal_8.5x14in", "Legal"},
        {"iso_a4_210x297mm", "A4"},            {"na_executive_7.25x10.5in", "Executive"},
        {"na_foolscap_8.5x13in", "FanFoldGermanLegal"}, {"iso_a5_148x210mm", "A5"},
        {"iso_a6_105x148mm", "A6"},            {"jis_b5_182x257mm", "B5"},
        {"om_16k_195x270mm", "195x270mm"},     {"om_16k_184x260mm", "184x260mm"},
        {"roc_16k_7.75x10.75in", "197x273mm"}, {"jpn_hagaki_100x148mm", "Postcard"},
        {"jpn_oufuku_148x200mm", "DoublePostcard"}, {"na_number-10_4.125x9.5in", "Env10"},
        {"na_monarch_3.875x7.5in", "EnvMonarch"}, {"iso_b5_176x250mm", "EnvISOB5"},
        {"iso_c5_162x229mm", "EnvC5"},         {"iso_dl_110x220mm", "EnvDL"},
    };
    for (size_t i = 0; i < 18; i++) {
        const P1600AirMedia *media = p1600_air_media_by_name(pairs[i][0]);
        CHECK(media && strcmp(media->ppd, pairs[i][1]) == 0 && strcmp(media->pwg, pairs[i][0]) == 0);
        CHECK(p1600_air_media_by_ppd(pairs[i][1]) == media && p1600_air_media_by_size(media->width, media->length) == media);
        CHECK(p1600_air_media_by_size(media->length, media->width) == media);  /* either orientation */
        CHECK(p1600_air_media_by_size(media->width + 90, media->length - 90) == media);
    }
    CHECK(strcmp(p1600_air_media_by_name("A4")->ppd, "A4") == 0 && strcmp(p1600_air_media_by_name("iso-a4")->ppd, "A4") == 0);
    CHECK(strcmp(p1600_air_media_by_name("na-letter")->ppd, "Letter") == 0);
    CHECK(strcmp(p1600_air_media_by_name("custom_mine_210x297mm")->ppd, "A4") == 0);
    CHECK(p1600_air_media_by_name("iso_a3_297x420mm") == NULL && p1600_air_media_by_name("na_index-4x6_4x6in") == NULL);
    CHECK(p1600_air_media_by_name("") == NULL && p1600_air_media_by_name(NULL) == NULL && p1600_air_media_by_ppd("A3") == NULL);
    CHECK(p1600_air_media_by_size(21000 + 150, 29700) == NULL);
    CHECK(p1600_air_media_by_name("jpn_oufuku_148x200mm")->width == 20000);  /* landscape like the PPD */
    CHECK(strcmp(p1600_air_media_type_ppd("stationery"), "Paper") == 0);
    CHECK(strcmp(p1600_air_media_type_ppd("labels"), "LabelsPaper") == 0);
    CHECK(strcmp(p1600_air_media_type_ppd("com.hp.midweight"), "Mid-Weight96-110g") == 0);
    CHECK(p1600_air_media_type_ppd("photographic") == NULL && p1600_air_media_type_count == 16);
    for (size_t i = 0; i < p1600_air_media_type_count; i++) {
        CHECK(p1600_air_media_type_ppd(p1600_air_media_types[i].ipp) == p1600_air_media_types[i].ppd);
    }
    CHECK(strcmp(p1600_air_media_source_ppd("auto"), "Auto") == 0);
    CHECK(strcmp(p1600_air_media_source_ppd("manual"), "Manual") == 0);
    CHECK(p1600_air_media_source_ppd("tray-1") == NULL);
    puts("[PASS] Media: all 18 PPD sizes <-> PWG keywords (PaperDimension within 1.5 pt), legacy names,");
    puts("       media types and sources");
}

static void test_formats(void) {
    CHECK(strcmp(p1600_air_detect_format((const unsigned char *)"%PDF-1.7\n", 9), "application/pdf") == 0);
    unsigned char prefixed[300];
    memset(prefixed, ' ', sizeof(prefixed));
    memcpy(prefixed + 290, "%PDF-1.4", 8);
    CHECK(strcmp(p1600_air_detect_format(prefixed, sizeof(prefixed)), "application/pdf") == 0);
    CHECK(strcmp(p1600_air_detect_format((const unsigned char *)"\xff\xd8\xff\xe0", 4), "image/jpeg") == 0);
    CHECK(strcmp(p1600_air_detect_format((const unsigned char *)"\x89PNG\r\n\x1a\n\0", 9), "image/png") == 0);
    CHECK(strcmp(p1600_air_detect_format((const unsigned char *)"UNIRAST\0\0\0\0\1", 12), "image/urf") == 0);
    CHECK(strcmp(p1600_air_detect_format((const unsigned char *)"RaS2", 4), "image/pwg-raster") == 0);
    CHECK(p1600_air_detect_format((const unsigned char *)"RaS3", 4) == NULL);
    CHECK(p1600_air_detect_format((const unsigned char *)"%!PS-Adobe", 10) == NULL);
    CHECK(p1600_air_detect_format((const unsigned char *)"\x1f\x8b\x08", 3) == NULL);
    CHECK(p1600_air_detect_format((const unsigned char *)"UNIRAST", 7) == NULL && p1600_air_detect_format(NULL, 0) == NULL);
    puts("[PASS] application/octet-stream: PDF, JPEG, PNG, URF and PWG raster recognized by their magic bytes");
}

static const char *option(int count, cups_option_t *options, const char *name) {
    return cupsGetOption(name, count, options);
}

static void test_options(void) {
    ipp_t *job = ippNew();
    ippAddInteger(job, IPP_TAG_JOB, IPP_TAG_INTEGER, "copies", 3);
    ippAddString(job, IPP_TAG_JOB, IPP_TAG_KEYWORD, "sides", NULL, "two-sided-short-edge");
    ipp_t *col = ippNew(), *size = ippNew();
    ippAddInteger(size, IPP_TAG_ZERO, IPP_TAG_INTEGER, "x-dimension", 14800);
    ippAddInteger(size, IPP_TAG_ZERO, IPP_TAG_INTEGER, "y-dimension", 21000);
    ippAddCollection(col, IPP_TAG_ZERO, "media-size", size);
    ippAddString(col, IPP_TAG_ZERO, IPP_TAG_KEYWORD, "media-source", NULL, "manual");
    ippAddString(col, IPP_TAG_ZERO, IPP_TAG_KEYWORD, "media-type", NULL, "labels");
    ippAddCollection(job, IPP_TAG_JOB, "media-col", col);
    ippDelete(size);
    ippDelete(col);
    ippAddString(job, IPP_TAG_JOB, IPP_TAG_KEYWORD, "media", NULL, "na_letter_8.5x11in");  /* media-col wins */
    ippAddInteger(job, IPP_TAG_JOB, IPP_TAG_ENUM, "print-quality", IPP_QUALITY_NORMAL);
    static const int lower[] = {1, 5}, upper[] = {2, 5};
    ippAddRanges(job, IPP_TAG_JOB, "page-ranges", 2, lower, upper);
    ippAddInteger(job, IPP_TAG_JOB, IPP_TAG_ENUM, "orientation-requested", IPP_ORIENT_LANDSCAPE);
    ippAddString(job, IPP_TAG_JOB, IPP_TAG_KEYWORD, "print-scaling", NULL, "fit");
    cups_option_t *options = NULL;
    int count = p1600_air_job_options(job, 0, &options);
    CHECK(strcmp(option(count, options, "copies"), "3") == 0);
    CHECK(strcmp(option(count, options, "sides"), "two-sided-short-edge") == 0);
    CHECK(strcmp(option(count, options, "PageSize"), "A5") == 0);
    CHECK(strcmp(option(count, options, "InputSlot"), "Manual") == 0);
    CHECK(strcmp(option(count, options, "MediaType"), "LabelsPaper") == 0);
    CHECK(strcmp(option(count, options, "Resolution"), "600dpi") == 0);
    CHECK(strcmp(option(count, options, "page-ranges"), "1-2,5-5") == 0);
    CHECK(strcmp(option(count, options, "orientation-requested"), "4") == 0);
    CHECK(strcmp(option(count, options, "print-scaling"), "fit") == 0);
    CHECK(strcmp(option(count, options, "fit-to-page"), "true") == 0);
    CHECK(count == 10);
    cupsFreeOptions(count, options);
    ippDelete(job);

    job = ippNew();
    ippAddString(job, IPP_TAG_JOB, IPP_TAG_KEYWORD, "media", NULL, "na_letter_8.5x11in");
    ippAddInteger(job, IPP_TAG_JOB, IPP_TAG_ENUM, "print-quality", IPP_QUALITY_HIGH);
    ippAddString(job, IPP_TAG_JOB, IPP_TAG_KEYWORD, "print-scaling", NULL, "none");
    col = ippNew();
    ippAddString(col, IPP_TAG_ZERO, IPP_TAG_KEYWORD, "media-source", NULL, "auto");
    ippAddString(col, IPP_TAG_ZERO, IPP_TAG_KEYWORD, "media-type", NULL, "stationery-heavyweight");
    ippAddCollection(job, IPP_TAG_JOB, "media-col", col);
    ippDelete(col);
    options = NULL;
    count = p1600_air_job_options(job, 0, &options);
    CHECK(strcmp(option(count, options, "PageSize"), "Letter") == 0);
    CHECK(strcmp(option(count, options, "Resolution"), "1200x600dpi") == 0);
    CHECK(strcmp(option(count, options, "InputSlot"), "Auto") == 0);
    CHECK(strcmp(option(count, options, "MediaType"), "Heavy111-130g") == 0);
    CHECK(strcmp(option(count, options, "print-scaling"), "none") == 0 && !option(count, options, "fit-to-page"));
    CHECK(!option(count, options, "copies") && !option(count, options, "sides"));
    cupsFreeOptions(count, options);
    ippDelete(job);
    /* media-size-name in media-col, every scaling keyword. */
    static const char *const scalings[] = {"auto", "auto-fit", "fill", "none"};
    for (size_t i = 0; i < 4; i++) {
        job = ippNew();
        col = ippNew();
        ippAddString(col, IPP_TAG_ZERO, IPP_TAG_KEYWORD, "media-size-name", NULL, "iso_dl_110x220mm");
        ippAddCollection(job, IPP_TAG_JOB, "media-col", col);
        ippDelete(col);
        ippAddString(job, IPP_TAG_JOB, IPP_TAG_KEYWORD, "print-scaling", NULL, scalings[i]);
        options = NULL;
        count = p1600_air_job_options(job, 0, &options);
        CHECK(strcmp(option(count, options, "PageSize"), "EnvDL") == 0);
        CHECK(strcmp(option(count, options, "print-scaling"), scalings[i]) == 0);
        CHECK(!option(count, options, "fit-to-page"));
        cupsFreeOptions(count, options);
        ippDelete(job);
    }
    CHECK(p1600_air_job_options(NULL, 0, &options) == 0);
    puts("[PASS] IPP -> CUPS options: copies, sides, media/media-col -> PageSize, InputSlot, MediaType, Resolution,");
    puts("       page-ranges, orientation-requested, print-scaling (fit -> fit-to-page)");
}

static void test_bonjour(void) {
    char uuid[64], again[64], other[64];
    p1600_air_uuid("Mac.local", "Office", uuid, sizeof(uuid));
    p1600_air_uuid("Mac.local", "Office", again, sizeof(again));
    p1600_air_uuid("Mac.local", "Office2", other, sizeof(other));
    CHECK(strcmp(uuid, again) == 0 && strcmp(uuid, other) != 0);
    CHECK(strlen(uuid) == 45 && strncmp(uuid, "urn:uuid:", 9) == 0 && uuid[17] == '-' && uuid[22] == '-');
    CHECK(uuid[23] == '5' && strchr("89ab", uuid[28]) != NULL);
    p1600_air_uuid("Other.local", "Office", other, sizeof(other));
    CHECK(strcmp(uuid, other) != 0);

    char name[64];
    p1600_air_service_name("HP LaserJet P1606dn", "Berk's Mac", name, sizeof(name));
    CHECK(strcmp(name, "HP LaserJet P1606dn @ Berk's Mac") == 0);
    static const char long_info[] = "Yazıcı ğüşiöç ĞÜŞİÖÇ çok uzun bir ad, gerçekten çok uzun";
    p1600_air_service_name(long_info, "Bilgisayarım", name, sizeof(name));
    CHECK(strlen(name) <= 63 && (((unsigned char)name[strlen(name) - 1]) & 0xc0) != 0xc0);

    unsigned char buffer[1024];
    uint16_t length = 0;
    P1600AirTxt txt = {"ipp/print/Office", "HP LaserJet Professional P1606dn", "Room 5", "http://Mac.local:8632/",
                    uuid + 9};
    CHECK(p1600_air_txt_record(&txt, buffer, sizeof(buffer), &length) == 0 && length > 0);
    static const char *const expected[][2] = {
        {"txtvers", "1"}, {"qtotal", "1"}, {"rp", "ipp/print/Office"}, {"ty", "HP LaserJet Professional P1606dn"},
        {"product", "(HP LaserJet Professional P1606dn)"}, {"note", "Room 5"}, {"adminurl", "http://Mac.local:8632/"},
        {"pdl", "application/pdf,image/jpeg,image/png,image/urf,image/pwg-raster,application/octet-stream"},
        {"URF", "V1.4,W8,SRGB24,RS300-600,CP1,DM1,IS1,MT1-2-3-4-5-8,OB10,PQ4-5"}, {"Color", "F"}, {"Duplex", "T"},
        {"Copies", "T"}, {"Scan", "F"}, {"Fax", "F"}, {"kind", "document"}, {"PaperMax", "legal-A4"},
        {"priority", "50"}, {"air", "none"},
    };
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        uint8_t value_length = 0;
        const void *value = TXTRecordGetValuePtr(length, buffer, expected[i][0], &value_length);
        if (!value || value_length != strlen(expected[i][1]) || memcmp(value, expected[i][1], value_length) != 0) {
            fprintf(stderr, "TXT %s is wrong\n", expected[i][0]);
            CHECK(0);
        }
    }
    uint8_t value_length = 0;
    const void *value = TXTRecordGetValuePtr(length, buffer, "UUID", &value_length);
    CHECK(value && value_length == 36 && memcmp(value, uuid + 9, 36) == 0);
    CHECK(!TXTRecordContainsKey(length, buffer, "TLS"));
    CHECK(TXTRecordGetCount(length, buffer) == 19);
    CHECK(p1600_air_txt_record(&txt, buffer, 100, &length) != 0);  /* too small */
    puts("[PASS] Bonjour: TXT record (rp ty product note adminurl pdl URF Color Duplex Copies Scan Fax kind PaperMax");
    puts("       priority UUID air, no TLS), stable name-based printer-uuid, 63-byte service names");
}

/* --- IPP client helpers ------------------------------------------------------------- */

static http_t *connect_to(int port) {
    http_t *http = httpConnect2("127.0.0.1", port, NULL, AF_INET, HTTP_ENCRYPTION_NEVER, 1, 10000, NULL);
    CHECK(http != NULL);
    return http;
}

static ipp_t *new_request(ipp_op_t op, int port, const char *resource) {
    ipp_t *request = ippNewRequest(op);
    char uri[256];
    snprintf(uri, sizeof(uri), "ipp://127.0.0.1:%d%s", port, resource);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, uri);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, "tester");
    return request;
}

/* Sends a request and an optional document; frees the request. */
static ipp_t *exchange(int port, const char *resource, ipp_t *request, const void *data, size_t length) {
    http_t *http = connect_to(port);
    http_status_t status = cupsSendRequest(http, request, resource, ippLength(request) + length);
    /* The server may answer before the document is complete. */
    if (status == HTTP_STATUS_CONTINUE && length > 0) (void)cupsWriteRequestData(http, data, length);
    ipp_t *response = cupsGetResponse(http, resource);
    ippDelete(request);
    httpClose(http);
    CHECK(response != NULL);
    return response;
}

static int job_state(int port, int job_id) {
    ipp_t *request = new_request(IPP_OP_GET_JOB_ATTRIBUTES, port, "/ipp/print/TestQ");
    ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "job-id", job_id);
    ipp_t *response = exchange(port, "/ipp/print/TestQ", request, NULL, 0);
    ipp_attribute_t *attr = ippFindAttribute(response, "job-state", IPP_TAG_ENUM);
    int state = attr ? ippGetInteger(attr, 0) : -1;
    ippDelete(response);
    return state;
}

static int wait_job(int port, int job_id, int at_least) {
    int state = -1;
    for (int i = 0; i < 200; i++) {
        state = job_state(port, job_id);
        if (state >= at_least) break;
        usleep(50000);
    }
    return state;
}

static int print_document(int port, const char *format, const char *compression, const void *data, size_t length,
                          ipp_status_t *status, void (*add)(ipp_t *)) {
    ipp_t *request = new_request(IPP_OP_PRINT_JOB, port, "/ipp/print/TestQ");
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "job-name", NULL, "unit test");
    if (format) ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_MIMETYPE, "document-format", NULL, format);
    if (compression) ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "compression", NULL, compression);
    if (add) add(request);
    ipp_t *response = exchange(port, "/ipp/print/TestQ", request, data, length);
    *status = ippGetStatusCode(response);
    ipp_attribute_t *attr = ippFindAttribute(response, "job-id", IPP_TAG_INTEGER);
    int id = attr ? ippGetInteger(attr, 0) : 0;
    ippDelete(response);
    return id;
}

static bool read_file(const char *path, Bytes *out) {
    out->length = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    unsigned char buffer[65536];
    ssize_t count;
    while ((count = read(fd, buffer, sizeof(buffer))) > 0) bytes_add(out, buffer, (size_t)count);
    close(fd);
    return count == 0;
}

static bool spooled(const char *spool, int id, const char *extension, const char *line) {
    char path[1200];
    Bytes bytes = {0};
    snprintf(path, sizeof(path), "%s/job-%d.%s", spool, id, extension);
    bool ok = read_file(path, &bytes) && bytes.length > 0;
    if (ok && line) {
        bytes_add(&bytes, "", 1);
        char needle[512];
        snprintf(needle, sizeof(needle), "%s\n", line);
        ok = strstr((const char *)bytes.data, needle) != NULL;
        if (!ok) fprintf(stderr, "job-%d.%s lacks \"%s\":\n%s", id, extension, line, (const char *)bytes.data);
    }
    free(bytes.data);
    return ok;
}

static void compress_bytes(const Bytes *in, Bytes *out, int window_bits) {
    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    CHECK(deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, window_bits, 8, Z_DEFAULT_STRATEGY) == Z_OK);
    out->length = 0;
    unsigned char buffer[65536];
    stream.next_in = in->data;
    stream.avail_in = (uInt)in->length;
    int result;
    do {
        stream.next_out = buffer;
        stream.avail_out = sizeof(buffer);
        result = deflate(&stream, Z_FINISH);
        bytes_add(out, buffer, sizeof(buffer) - stream.avail_out);
    } while (result == Z_OK);
    CHECK(result == Z_STREAM_END);
    deflateEnd(&stream);
}

/* A raw HTTP exchange; returns the status code, the response in `out`. */
static int raw_http(int port, const void *request, size_t length, Bytes *out) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address = {.sin_len = sizeof(address), .sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    CHECK(fd >= 0 && connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
    const unsigned char *bytes = request;
    size_t sent = 0;
    while (sent < length) {
        ssize_t count = send(fd, bytes + sent, length - sent, 0);
        if (count <= 0) break;
        sent += (size_t)count;
    }
    out->length = 0;
    for (;;) {
        struct pollfd channel = {.fd = fd, .events = POLLIN};
        if (poll(&channel, 1, 10000) <= 0) break;
        unsigned char buffer[4096];
        ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
        if (count <= 0) break;
        bytes_add(out, buffer, (size_t)count);
    }
    close(fd);
    bytes_add(out, "", 1);
    int code = 0;
    if (out->length > 12 && sscanf((const char *)out->data, "HTTP/1.%*d %d", &code) != 1) code = 0;
    return code;
}

static int connect_raw(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address = {.sin_len = sizeof(address), .sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    CHECK(fd >= 0 && connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    return fd;
}

static int raw_get(int port, const char *method, const char *path, Bytes *out) {
    char request[512];
    snprintf(request, sizeof(request), "%s %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nConnection: close\r\n\r\n", method,
             path, port);
    return raw_http(port, request, strlen(request), out);
}

typedef struct {
    P1600AirServer *server;
    int stop[2];
    pthread_t thread;
} RunningServer;

static void *server_main(void *argument) {
    RunningServer *running = argument;
    p1600_air_server_run(running->server, running->stop[0]);
    return NULL;
}

static void server_start(RunningServer *running, P1600AirConfig *config) {
    char error[256];
    running->server = p1600_air_server_create(config, error, sizeof(error));
    if (!running->server) fprintf(stderr, "server: %s\n", error);
    CHECK(running->server != NULL && pipe(running->stop) == 0);
    CHECK(pthread_create(&running->thread, NULL, server_main, running) == 0);
}

static void server_stop(RunningServer *running) {
    CHECK(write(running->stop[1], "x", 1) == 1);
    pthread_join(running->thread, NULL);
    p1600_air_server_free(running->server);
    close(running->stop[0]);
    close(running->stop[1]);
}

static void add_media_col_with_borderless(ipp_t *request) {
    ipp_t *col = ippNew(), *size = ippNew();
    ippAddInteger(size, IPP_TAG_ZERO, IPP_TAG_INTEGER, "x-dimension", 21590);
    ippAddInteger(size, IPP_TAG_ZERO, IPP_TAG_INTEGER, "y-dimension", 35560);
    ippAddCollection(col, IPP_TAG_ZERO, "media-size", size);
    ippAddInteger(col, IPP_TAG_ZERO, IPP_TAG_INTEGER, "media-top-margin", 0);
    ippAddString(col, IPP_TAG_ZERO, IPP_TAG_KEYWORD, "media-source", NULL, "manual");
    ippAddCollection(request, IPP_TAG_JOB, "media-col", col);
    ippDelete(size);
    ippDelete(col);
}

static void add_fidelity_draft(ipp_t *request) {
    ippAddBoolean(request, IPP_TAG_OPERATION, "ipp-attribute-fidelity", 1);
    ippAddInteger(request, IPP_TAG_JOB, IPP_TAG_ENUM, "print-quality", IPP_QUALITY_DRAFT);
}

static void add_quality_copies(ipp_t *request) {
    ippAddInteger(request, IPP_TAG_JOB, IPP_TAG_ENUM, "print-quality", IPP_QUALITY_NORMAL);
    ippAddInteger(request, IPP_TAG_JOB, IPP_TAG_INTEGER, "copies", 2);
    ippAddString(request, IPP_TAG_JOB, IPP_TAG_KEYWORD, "sides", NULL, "two-sided-long-edge");
}

static const char tiny_pdf[] =
    "%PDF-1.4\n1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n"
    "2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n"
    "3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] >> endobj\n"
    "trailer << /Root 1 0 R >>\n%%EOF\n";

static void test_server_spool(void) {
    char spool[1100];
    snprintf(spool, sizeof(spool), "%s/spool", temp_root);
    CHECK(mkdir(spool, 0700) == 0);
    P1600AirConfig config;
    memset(&config, 0, sizeof(config));
    config.loopback = true;
    config.spool_dir = spool;
    config.icon_path = "assets/P1600dnPrinter.png";
    config.queue_count = 1;
    snprintf(config.queues[0].name, sizeof(config.queues[0].name), "TestQ");
    snprintf(config.queues[0].make_model, sizeof(config.queues[0].make_model), "HP LaserJet Professional P1606dn");
    RunningServer running;
    server_start(&running, &config);
    int port = p1600_air_server_port(running.server);
    CHECK(port > 0);
    Bytes out = {0};

    /* HTTP: status page, icon, 404, 405, bad requests. */
    CHECK(raw_get(port, "GET", "/", &out) == 200 && strstr((char *)out.data, "TestQ") &&
          strstr((char *)out.data, "text/html"));
    CHECK(raw_get(port, "GET", "/icon.png", &out) == 200 && strstr((char *)out.data, "image/png") &&
          memmem(out.data, out.length, "\x89PNG", 4));
    CHECK(raw_get(port, "HEAD", "/icon.png", &out) == 200 && !memmem(out.data, out.length, "\x89PNG", 4));
    CHECK(raw_get(port, "GET", "/nothing", &out) == 404);
    CHECK(raw_get(port, "GET", "/ipp/print/TestQ", &out) == 405);
    CHECK(raw_get(port, "DELETE", "/ipp/print/TestQ", &out) == 405);
    CHECK(raw_get(port, "OPTIONS", "*", &out) == 200 && strstr((char *)out.data, "Allow: GET, HEAD, OPTIONS, POST"));
    char request[600];
    snprintf(request, sizeof(request), "POST /ipp/print/TestQ HTTP/1.1\r\nHost: x\r\nContent-Type: text/plain\r\n"
                                       "Content-Length: 5\r\nConnection: close\r\n\r\nhello");
    CHECK(raw_http(port, request, strlen(request), &out) == 400);
    snprintf(request, sizeof(request), "POST /ipp/print/Other HTTP/1.1\r\nHost: x\r\nContent-Type: application/ipp\r\n"
                                       "Content-Length: 5\r\nConnection: close\r\n\r\nhello");
    CHECK(raw_http(port, request, strlen(request), &out) == 404);
    snprintf(request, sizeof(request), "POST /ipp/print/TestQ HTTP/1.1\r\nHost: x\r\nContent-Type: application/ipp\r\n"
                                       "Content-Length: 999999999999\r\nConnection: close\r\n\r\n");
    CHECK(raw_http(port, request, strlen(request), &out) == 413);
    snprintf(request, sizeof(request), "POST /ipp/print/TestQ HTTP/1.1\r\nHost: x\r\nContent-Type: application/ipp\r\n"
                                       "Content-Length: 5\r\nConnection: close\r\n\r\nhello");
    CHECK(raw_http(port, request, strlen(request), &out) == 400);  /* not an IPP message */
    snprintf(request, sizeof(request), "POST /ipp/print/TestQ HTTP/1.1\r\nContent-Type: application/ipp\r\n"
                                       "Content-Length: 0\r\n\r\n");
    CHECK(raw_http(port, request, strlen(request), &out) == 400);  /* HTTP/1.1 without Host */
    snprintf(request, sizeof(request), "BREW /ipp/print/TestQ HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(raw_http(port, request, strlen(request), &out) == 405);
    snprintf(request, sizeof(request), "POST /ipp/print/TestQ HTTP/1.1\r\nHost: x\r\nContent-Type: application/ipp\r\n"
                                       "Content-Encoding: br\r\nContent-Length: 1\r\n\r\nx");
    CHECK(raw_http(port, request, strlen(request), &out) == 415);
    puts("[PASS] HTTP: status page, /icon.png (GET/HEAD), 404, 405, 400 (content type, IPP, Host), 413, 415, OPTIONS");

    /* IPP: Expect: 100-continue (libcups sends it), PDF with options. */
    ipp_status_t status;
    int id = print_document(port, "application/pdf", NULL, tiny_pdf, sizeof(tiny_pdf) - 1, &status, add_quality_copies);
    CHECK(status == IPP_STATUS_OK && id > 0 && wait_job(port, id, IPP_JSTATE_COMPLETED) == IPP_JSTATE_COMPLETED);
    CHECK(spooled(spool, id, "pdf", NULL) && spooled(spool, id, "options", "document-format=application/pdf") &&
          spooled(spool, id, "options", "Resolution=600dpi") && spooled(spool, id, "options", "copies=2") &&
          spooled(spool, id, "options", "sides=two-sided-long-edge") &&
          spooled(spool, id, "options", "title=unit test"));

    /* octet-stream: detected; compression gzip, deflate (raw and zlib). */
    Bytes pdf = {0}, packed = {0};
    bytes_add(&pdf, tiny_pdf, sizeof(tiny_pdf) - 1);
    static const struct {
        const char *compression;
        int bits;
    } codings[] = {{"gzip", 31}, {"deflate", -15}, {"deflate", 15}};
    for (size_t i = 0; i < 3; i++) {
        compress_bytes(&pdf, &packed, codings[i].bits);
        id = print_document(port, "application/octet-stream", codings[i].compression, packed.data, packed.length,
                            &status, NULL);
        CHECK(status == IPP_STATUS_OK && wait_job(port, id, IPP_JSTATE_COMPLETED) == IPP_JSTATE_COMPLETED);
        char path[1200];
        Bytes copy = {0};
        snprintf(path, sizeof(path), "%s/job-%d.pdf", spool, id);
        CHECK(read_file(path, &copy) && copy.length == pdf.length && memcmp(copy.data, pdf.data, pdf.length) == 0);
        free(copy.data);
    }
    /* Damaged and cut compressed data, unsupported compression. */
    compress_bytes(&pdf, &packed, 31);
    packed.data[packed.length / 2] ^= 0x55;
    packed.data[packed.length / 2 + 1] ^= 0x55;
    print_document(port, "application/pdf", "gzip", packed.data, packed.length, &status, NULL);
    CHECK(status == IPP_STATUS_ERROR_COMPRESSION_ERROR);
    compress_bytes(&pdf, &packed, 31);
    print_document(port, "application/pdf", "gzip", packed.data, packed.length / 2, &status, NULL);
    CHECK(status == IPP_STATUS_ERROR_COMPRESSION_ERROR);
    print_document(port, "application/pdf", "compress", tiny_pdf, sizeof(tiny_pdf) - 1, &status, NULL);
    CHECK(status == IPP_STATUS_ERROR_COMPRESSION_NOT_SUPPORTED);
    /* Wrong and unknown formats, empty documents. */
    print_document(port, "image/png", NULL, tiny_pdf, sizeof(tiny_pdf) - 1, &status, NULL);
    CHECK(status == IPP_STATUS_ERROR_DOCUMENT_FORMAT_ERROR);
    print_document(port, "application/octet-stream", NULL, "%!PS-Adobe-3.0\n", 15, &status, NULL);
    CHECK(status == IPP_STATUS_ERROR_DOCUMENT_FORMAT_NOT_SUPPORTED);
    print_document(port, "application/postscript", NULL, "%!PS-Adobe-3.0\n", 15, &status, NULL);
    CHECK(status == IPP_STATUS_ERROR_DOCUMENT_FORMAT_NOT_SUPPORTED);
    print_document(port, "application/pdf", NULL, NULL, 0, &status, NULL);
    CHECK(status == IPP_STATUS_ERROR_BAD_REQUEST);
    puts("[PASS] IPP: Print-Job with Expect: 100-continue, octet-stream detection, gzip/deflate/zlib documents");
    puts("[PASS] IPP: damaged or cut compressed data, unknown compression and formats, empty documents refused");

    /* URF and PWG raster jobs become PDF with the page's paper size. */
    Bytes urf = {0};
    urf_file_header(&urf, 1);
    unsigned char *a6 = pattern(1240, 1748, 1, 12);
    urf_page_header(&urf, 8, 0, 1240, 1748, 300);
    urf_encode(&urf, a6, 1240, 1748, 1);
    id = print_document(port, "image/urf", NULL, urf.data, urf.length, &status, NULL);
    CHECK(status == IPP_STATUS_OK && wait_job(port, id, IPP_JSTATE_COMPLETED) == IPP_JSTATE_COMPLETED);
    CHECK(spooled(spool, id, "options", "PageSize=A6") &&
          spooled(spool, id, "options", "document-format=application/pdf"));
    char path[1200];
    PdfInfo info;
    snprintf(path, sizeof(path), "%s/job-%d.pdf", spool, id);
    CHECK(pdf_info(path, &info) && info.count == 1 && near(info.boxes[0].size.width, 297.6));
    urf.data[urf.length - 1] = 0x7f;  /* damaged last line: the job aborts */
    bytes_add(&urf, "\x01\x02\x03", 3);
    id = print_document(port, "image/urf", NULL, urf.data, urf.length, &status, NULL);
    CHECK(status == IPP_STATUS_OK && wait_job(port, id, IPP_JSTATE_CANCELED) == IPP_JSTATE_ABORTED);
    free(a6);
    free(urf.data);
    Bytes pwg = {0};
    cups_raster_t *raster = cupsRasterOpenIO(bytes_write, &pwg, CUPS_RASTER_WRITE_PWG);
    cups_page_header2_t header;
    unsigned char *letter = pattern(2550, 3300, 3, 13);
    for (int page = 0; page < 2; page++) {
        CHECK(cupsRasterInitPWGHeader(&header, pwgMediaForPWG("na_letter_8.5x11in"), "srgb_8", 300, 300, "one-sided",
                                      NULL));
        write_raster_page(raster, &header, letter);
    }
    cupsRasterClose(raster);
    free(letter);
    id = print_document(port, "application/octet-stream", NULL, pwg.data, pwg.length, &status, NULL);
    CHECK(status == IPP_STATUS_OK && wait_job(port, id, IPP_JSTATE_COMPLETED) == IPP_JSTATE_COMPLETED);
    CHECK(spooled(spool, id, "options", "PageSize=Letter"));
    snprintf(path, sizeof(path), "%s/job-%d.pdf", spool, id);
    CHECK(pdf_info(path, &info) && info.count == 2 && near(info.boxes[1].size.height, 792));
    free(pwg.data);
    puts("[PASS] IPP: URF and PWG raster (octet-stream) jobs spooled as PDF with PageSize from the page;");
    puts("       a damaged URF job aborts");

    /* Whole-body HTTP Content-Encoding: gzip, decoded by libcups. */
    ipp_t *body = new_request(IPP_OP_PRINT_JOB, port, "/ipp/print/TestQ");
    ippAddString(body, IPP_TAG_OPERATION, IPP_TAG_MIMETYPE, "document-format", NULL, "application/pdf");
    Bytes message = {0};
    ippSetState(body, IPP_STATE_IDLE);
    CHECK(ippWriteIO(&message, (ipp_iocb_t)bytes_write, 1, NULL, body) == IPP_STATE_DATA);
    ippDelete(body);
    bytes_add(&message, tiny_pdf, sizeof(tiny_pdf) - 1);
    compress_bytes(&message, &packed, 31);
    char head[400];
    int head_length = snprintf(head, sizeof(head),
                               "POST /ipp/print/TestQ HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n"
                               "Content-Type: application/ipp\r\nContent-Encoding: gzip\r\n"
                               "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                               port, packed.length);
    Bytes whole = {0};
    bytes_add(&whole, head, (size_t)head_length);
    bytes_add(&whole, packed.data, packed.length);
    CHECK(raw_http(port, whole.data, whole.length, &out) == 200);
    const unsigned char *ipp = memmem(out.data, out.length, "\r\n\r\n", 4);
    CHECK(ipp && ipp[4 + 2] == 0x00 && ipp[4 + 3] == 0x00);  /* successful-ok */
    free(whole.data);
    free(message.data);
    puts("[PASS] IPP: a request body with HTTP Content-Encoding: gzip");

    /* Attributes: substituted media-col, fidelity, bad values. */
    id = print_document(port, "application/pdf", NULL, tiny_pdf, sizeof(tiny_pdf) - 1, &status,
                        add_media_col_with_borderless);
    CHECK(status == IPP_STATUS_OK_IGNORED_OR_SUBSTITUTED);
    CHECK(wait_job(port, id, IPP_JSTATE_COMPLETED) == IPP_JSTATE_COMPLETED);
    CHECK(spooled(spool, id, "options", "PageSize=Legal") && spooled(spool, id, "options", "InputSlot=Manual"));
    print_document(port, "application/pdf", NULL, tiny_pdf, sizeof(tiny_pdf) - 1, &status, add_fidelity_draft);
    CHECK(status == IPP_STATUS_ERROR_ATTRIBUTES_OR_VALUES);
    ipp_t *validate = new_request(IPP_OP_VALIDATE_JOB, port, "/ipp/print/TestQ");
    ippAddInteger(validate, IPP_TAG_JOB, IPP_TAG_INTEGER, "copies", 1000);
    ippAddString(validate, IPP_TAG_JOB, IPP_TAG_KEYWORD, "sides", NULL, "two-sided-long-edge");
    ipp_t *response = exchange(port, "/ipp/print/TestQ", validate, NULL, 0);
    ipp_attribute_t *attr = ippFindAttribute(response, "copies", IPP_TAG_INTEGER);
    CHECK(ippGetStatusCode(response) == IPP_STATUS_OK_IGNORED_OR_SUBSTITUTED && attr &&
          ippGetGroupTag(attr) == IPP_TAG_UNSUPPORTED_GROUP && !ippFindAttribute(response, "sides", IPP_TAG_ZERO));
    ippDelete(response);
    validate = new_request(IPP_OP_VALIDATE_JOB, port, "/ipp/print/TestQ");
    ippAddInteger(validate, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "job-k-octets", 300000);
    response = exchange(port, "/ipp/print/TestQ", validate, NULL, 0);
    CHECK(ippGetStatusCode(response) == IPP_STATUS_ERROR_REQUEST_ENTITY);
    ippDelete(response);
    puts("[PASS] IPP: media-col with unsupported margins substituted, fidelity refuses, unsupported group,");
    puts("       job-k-octets over the limit");

    /* Create-Job, Close-Job, Cancel-My-Jobs, job-uri, Get-Jobs filters. */
    ipp_t *create = new_request(IPP_OP_CREATE_JOB, port, "/ipp/print/TestQ");
    response = exchange(port, "/ipp/print/TestQ", create, NULL, 0);
    attr = ippFindAttribute(response, "job-id", IPP_TAG_INTEGER);
    int held = attr ? ippGetInteger(attr, 0) : 0;
    CHECK(ippGetStatusCode(response) == IPP_STATUS_OK && held > 0);
    attr = ippFindAttribute(response, "job-uri", IPP_TAG_URI);
    char job_uri[256];
    snprintf(job_uri, sizeof(job_uri), "%s", attr ? ippGetString(attr, 0, NULL) : "");
    ippDelete(response);
    CHECK(job_state(port, held) == IPP_JSTATE_HELD);
    ipp_t *by_uri = ippNewRequest(IPP_OP_GET_JOB_ATTRIBUTES);
    ippAddString(by_uri, IPP_TAG_OPERATION, IPP_TAG_URI, "job-uri", NULL, job_uri);
    response = exchange(port, "/ipp/print/TestQ", by_uri, NULL, 0);
    attr = ippFindAttribute(response, "job-id", IPP_TAG_INTEGER);
    CHECK(ippGetStatusCode(response) == IPP_STATUS_OK && attr && ippGetInteger(attr, 0) == held);
    ippDelete(response);
    ipp_t *close_job = new_request(IPP_OP_CLOSE_JOB, port, "/ipp/print/TestQ");
    ippAddInteger(close_job, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "job-id", held);
    response = exchange(port, "/ipp/print/TestQ", close_job, NULL, 0);
    CHECK(ippGetStatusCode(response) == IPP_STATUS_OK && job_state(port, held) == IPP_JSTATE_ABORTED);
    ippDelete(response);
    int second = 0, other = 0;
    create = new_request(IPP_OP_CREATE_JOB, port, "/ipp/print/TestQ");
    response = exchange(port, "/ipp/print/TestQ", create, NULL, 0);
    second = ippGetInteger(ippFindAttribute(response, "job-id", IPP_TAG_INTEGER), 0);
    ippDelete(response);
    create = ippNewRequest(IPP_OP_CREATE_JOB);
    snprintf(path, sizeof(path), "ipp://127.0.0.1:%d/ipp/print/TestQ", port);
    ippAddString(create, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, path);
    ippAddString(create, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, "someone-else");
    response = exchange(port, "/ipp/print/TestQ", create, NULL, 0);
    other = ippGetInteger(ippFindAttribute(response, "job-id", IPP_TAG_INTEGER), 0);
    ippDelete(response);
    ipp_t *cancel = new_request(IPP_OP_CANCEL_JOB, port, "/ipp/print/TestQ");
    ippAddInteger(cancel, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "job-id", other);
    response = exchange(port, "/ipp/print/TestQ", cancel, NULL, 0);
    CHECK(ippGetStatusCode(response) == IPP_STATUS_ERROR_NOT_AUTHORIZED);
    ippDelete(response);
    ipp_t *mine = new_request(IPP_OP_CANCEL_MY_JOBS, port, "/ipp/print/TestQ");
    response = exchange(port, "/ipp/print/TestQ", mine, NULL, 0);
    CHECK(ippGetStatusCode(response) == IPP_STATUS_OK);
    ippDelete(response);
    CHECK(job_state(port, second) == IPP_JSTATE_CANCELED && job_state(port, other) == IPP_JSTATE_HELD);
    ipp_t *jobs = new_request(IPP_OP_GET_JOBS, port, "/ipp/print/TestQ");
    ippAddString(jobs, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "which-jobs", NULL, "pending-held");
    response = exchange(port, "/ipp/print/TestQ", jobs, NULL, 0);
    attr = ippFindAttribute(response, "job-id", IPP_TAG_INTEGER);
    CHECK(attr && ippGetInteger(attr, 0) == other && !ippFindNextAttribute(response, "job-id", IPP_TAG_INTEGER));
    ippDelete(response);
    jobs = new_request(IPP_OP_GET_JOBS, port, "/ipp/print/TestQ");
    ippAddString(jobs, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "which-jobs", NULL, "completed");
    ippAddInteger(jobs, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "limit", 2);
    ippAddString(jobs, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "requested-attributes", NULL, "job-state");
    response = exchange(port, "/ipp/print/TestQ", jobs, NULL, 0);
    int count = 0;
    for (attr = ippFirstAttribute(response); attr; attr = ippNextAttribute(response)) {
        if (ippGetName(attr) && strcmp(ippGetName(attr), "job-state") == 0) count++;
        CHECK(!ippGetName(attr) || strcmp(ippGetName(attr), "job-id") != 0);
    }
    CHECK(count == 2);
    ippDelete(response);
    jobs = new_request(IPP_OP_GET_JOBS, port, "/ipp/print/TestQ");
    ippAddString(jobs, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "which-jobs", NULL, "everything");
    response = exchange(port, "/ipp/print/TestQ", jobs, NULL, 0);
    CHECK(ippGetStatusCode(response) == IPP_STATUS_ERROR_ATTRIBUTES_OR_VALUES);
    ippDelete(response);
    /* Send-Document with last-document false is refused (one document per job). */
    create = new_request(IPP_OP_CREATE_JOB, port, "/ipp/print/TestQ");
    response = exchange(port, "/ipp/print/TestQ", create, NULL, 0);
    int third = ippGetInteger(ippFindAttribute(response, "job-id", IPP_TAG_INTEGER), 0);
    ippDelete(response);
    ipp_t *send = new_request(IPP_OP_SEND_DOCUMENT, port, "/ipp/print/TestQ");
    ippAddInteger(send, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "job-id", other);
    ippAddBoolean(send, IPP_TAG_OPERATION, "last-document", 1);
    response = exchange(port, "/ipp/print/TestQ", send, tiny_pdf, sizeof(tiny_pdf) - 1);
    CHECK(ippGetStatusCode(response) == IPP_STATUS_ERROR_NOT_AUTHORIZED);
    ippDelete(response);
    send = new_request(IPP_OP_SEND_DOCUMENT, port, "/ipp/print/TestQ");
    ippAddInteger(send, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "job-id", third);
    ippAddBoolean(send, IPP_TAG_OPERATION, "last-document", 0);
    response = exchange(port, "/ipp/print/TestQ", send, tiny_pdf, sizeof(tiny_pdf) - 1);
    CHECK(ippGetStatusCode(response) == IPP_STATUS_ERROR_ATTRIBUTES_OR_VALUES);
    CHECK(job_state(port, third) == IPP_JSTATE_HELD);
    ippDelete(response);
    send = new_request(IPP_OP_SEND_DOCUMENT, port, "/ipp/print/TestQ");
    ippAddInteger(send, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "job-id", third);
    ippAddBoolean(send, IPP_TAG_OPERATION, "last-document", 1);
    ippAddString(send, IPP_TAG_OPERATION, IPP_TAG_MIMETYPE, "document-format", NULL, "application/pdf");
    response = exchange(port, "/ipp/print/TestQ", send, tiny_pdf, sizeof(tiny_pdf) - 1);
    CHECK(ippGetStatusCode(response) == IPP_STATUS_OK);
    CHECK(wait_job(port, third, IPP_JSTATE_COMPLETED) == IPP_JSTATE_COMPLETED);
    ippDelete(response);
    send = new_request(IPP_OP_SEND_DOCUMENT, port, "/ipp/print/TestQ");
    ippAddInteger(send, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "job-id", third);
    ippAddBoolean(send, IPP_TAG_OPERATION, "last-document", 1);
    response = exchange(port, "/ipp/print/TestQ", send, tiny_pdf, sizeof(tiny_pdf) - 1);
    CHECK(ippGetStatusCode(response) == IPP_STATUS_ERROR_NOT_POSSIBLE);
    ippDelete(response);
    puts("[PASS] IPP: Create-Job, job-uri, Close-Job, Cancel-Job ownership, Cancel-My-Jobs, Get-Jobs filters,");
    puts("       limit, Send-Document (owner, last-document, a second document refused)");

    /* Other operations and targets. */
    ipp_t *identify = new_request(IPP_OP_IDENTIFY_PRINTER, port, "/ipp/print/TestQ");
    response = exchange(port, "/ipp/print/TestQ", identify, NULL, 0);
    CHECK(ippGetStatusCode(response) == IPP_STATUS_OK);
    ippDelete(response);
    ipp_t *pause = new_request(IPP_OP_PAUSE_PRINTER, port, "/ipp/print/TestQ");
    response = exchange(port, "/ipp/print/TestQ", pause, NULL, 0);
    CHECK(ippGetStatusCode(response) == IPP_STATUS_ERROR_OPERATION_NOT_SUPPORTED);
    ippDelete(response);
    ipp_t *missing = new_request(IPP_OP_GET_PRINTER_ATTRIBUTES, port, "/ipp/print/Nope");
    response = exchange(port, "/ipp/print", missing, NULL, 0);
    CHECK(ippGetStatusCode(response) == IPP_STATUS_ERROR_NOT_FOUND);
    ippDelete(response);
    ipp_t *alias = new_request(IPP_OP_GET_PRINTER_ATTRIBUTES, port, "/ipp/print");
    ippAddString(alias, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "requested-attributes", NULL, "media-col-database");
    response = exchange(port, "/ipp/print", alias, NULL, 0);
    attr = ippFindAttribute(response, "media-col-database", IPP_TAG_BEGIN_COLLECTION);
    CHECK(ippGetStatusCode(response) == IPP_STATUS_OK && attr && ippGetCount(attr) == 18 &&
          !ippFindAttribute(response, "printer-name", IPP_TAG_ZERO));
    ipp_t *entry = ippGetCollection(attr, 12);  /* DoublePostcard */
    ipp_attribute_t *margin = ippFindAttribute(entry, "media-left-margin", IPP_TAG_INTEGER);
    ipp_t *dimensions = ippGetCollection(ippFindAttribute(entry, "media-size", IPP_TAG_BEGIN_COLLECTION), 0);
    CHECK(margin && ippGetInteger(margin, 0) == 423 &&
          ippGetInteger(ippFindAttribute(dimensions, "x-dimension", IPP_TAG_INTEGER), 0) == 20000);
    ippDelete(response);
    ipp_t *all = new_request(IPP_OP_GET_PRINTER_ATTRIBUTES, port, "/ipp/print/TestQ");
    ippAddString(all, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "requested-attributes", NULL, "all");
    response = exchange(port, "/ipp/print/TestQ", all, NULL, 0);
    CHECK(!ippFindAttribute(response, "media-col-database", IPP_TAG_ZERO));
    attr = ippFindAttribute(response, "media-col-ready", IPP_TAG_BEGIN_COLLECTION);
    ipp_t *ready = attr ? ippGetCollection(attr, 0) : NULL;
    ipp_attribute_t *top = ready ? ippFindAttribute(ready, "media-top-margin", IPP_TAG_INTEGER) : NULL;
    CHECK(top && ippGetInteger(top, 0) == 423);
    attr = ippFindAttribute(response, "printer-uuid", IPP_TAG_URI);
    CHECK(attr && strncmp(ippGetString(attr, 0, NULL), "urn:uuid:", 9) == 0);
    CHECK(ippContainsString(ippFindAttribute(response, "urf-supported", IPP_TAG_KEYWORD), "SRGB24"));
    CHECK(ippContainsString(ippFindAttribute(response, "document-format-supported", IPP_TAG_MIMETYPE), "image/urf"));
    ippDelete(response);
    ipp_t *template_group = new_request(IPP_OP_GET_PRINTER_ATTRIBUTES, port, "/ipp/print/TestQ");
    ippAddString(template_group, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "requested-attributes", NULL, "job-template");
    response = exchange(port, "/ipp/print/TestQ", template_group, NULL, 0);
    CHECK(ippFindAttribute(response, "sides-supported", IPP_TAG_KEYWORD) &&
          !ippFindAttribute(response, "printer-name", IPP_TAG_ZERO));
    ippDelete(response);
    puts("[PASS] IPP: Identify-Printer, unsupported operations, unknown printers, /ipp/print alias, requested groups");
    puts("       (media-col-database only when asked, 18 entries with 423 margins)");

    /* At most P1600_AIR_MAX_ACTIVE_JOBS active jobs: then server-error-busy. */
    int created = 0;
    ipp_status_t last = IPP_STATUS_OK;
    for (int i = 0; i <= P1600_AIR_MAX_ACTIVE_JOBS && last == IPP_STATUS_OK; i++) {
        create = new_request(IPP_OP_CREATE_JOB, port, "/ipp/print/TestQ");
        response = exchange(port, "/ipp/print/TestQ", create, NULL, 0);
        last = ippGetStatusCode(response);
        if (last == IPP_STATUS_OK) created++;
        ippDelete(response);
    }
    CHECK(last == IPP_STATUS_ERROR_BUSY && created == P1600_AIR_MAX_ACTIVE_JOBS - 1);  /* "other" is still held */
    print_document(port, "application/pdf", NULL, tiny_pdf, sizeof(tiny_pdf) - 1, &status, NULL);
    CHECK(status == IPP_STATUS_ERROR_BUSY);
    mine = new_request(IPP_OP_CANCEL_MY_JOBS, port, "/ipp/print/TestQ");
    response = exchange(port, "/ipp/print/TestQ", mine, NULL, 0);
    ippDelete(response);
    id = print_document(port, "application/pdf", NULL, tiny_pdf, sizeof(tiny_pdf) - 1, &status, NULL);
    CHECK(status == IPP_STATUS_OK && wait_job(port, id, IPP_JSTATE_COMPLETED) == IPP_JSTATE_COMPLETED);
    puts("[PASS] IPP: server-error-busy beyond 100 active jobs, accepted again once they are canceled");

    /* At most P1600_AIR_MAX_CLIENTS connections: one more is closed at once. */
    usleep(500000);  /* earlier connections finish */
    int sockets[P1600_AIR_MAX_CLIENTS + 1];
    for (int i = 0; i <= P1600_AIR_MAX_CLIENTS; i++) sockets[i] = connect_raw(port);
    int refused = 0;
    for (int i = 0; i <= P1600_AIR_MAX_CLIENTS; i++) {
        struct pollfd channel = {.fd = sockets[i], .events = POLLIN};
        char byte;
        if (poll(&channel, 1, i == 0 ? 3000 : 100) == 1 && recv(sockets[i], &byte, 1, 0) == 0) refused++;
    }
    CHECK(refused == 1);
    for (int i = 0; i <= P1600_AIR_MAX_CLIENTS; i++) close(sockets[i]);
    usleep(1500000);  /* the server reaps the closed connections */
    CHECK(raw_get(port, "GET", "/", &out) == 200);
    puts("[PASS] HTTP: at most 32 connections at a time, slots are reused");
    /* An idle connection does not hold up the shutdown. */
    int idle = connect_raw(port);
    usleep(100000);

    server_stop(&running);
    close(idle);
    free(out.data);
    free(pdf.data);
    free(packed.data);
}

/* --- stand-in cupsd for real mode ------------------------------------------------------ */

typedef struct {
    int listener;
    int port;
    pthread_t thread;
    atomic_bool stop;
    pthread_mutex_t lock;
    int jobs_created;
    char queue[128];
    char title[256];
    char options[4096];
    char format[128];
    size_t document_bytes;
    unsigned char document_head[8];
    int canceled_job;
    int job_state;
    bool job_missing;
    int printer_state;
    bool accepting;
} FakeCups;

static void fake_handle(FakeCups *fake, http_t *http, ipp_t *request, ipp_t *response) {
    if (getenv("FAKE_DEBUG")) fprintf(stderr, "stand-in cupsd: %s\n", ippOpString(ippGetOperation(request)));
    ipp_attribute_t *attr;
    pthread_mutex_lock(&fake->lock);
    switch (ippGetOperation(request)) {
    case IPP_OP_CREATE_JOB: {
        attr = ippFindAttribute(request, "printer-uri", IPP_TAG_URI);
        const char *uri = attr ? ippGetString(attr, 0, NULL) : "";
        const char *queue = strstr(uri, "/printers/");
        snprintf(fake->queue, sizeof(fake->queue), "%s", queue ? queue + 10 : "");
        attr = ippFindAttribute(request, "job-name", IPP_TAG_NAME);
        snprintf(fake->title, sizeof(fake->title), "%s", attr ? ippGetString(attr, 0, NULL) : "");
        fake->options[0] = '\0';
        for (attr = ippFirstAttribute(request); attr; attr = ippNextAttribute(request)) {
            if (ippGetGroupTag(attr) != IPP_TAG_JOB) continue;
            char value[512];
            ippAttributeString(attr, value, sizeof(value));
            size_t used = strlen(fake->options);
            snprintf(fake->options + used, sizeof(fake->options) - used, "%s=%s;", ippGetName(attr), value);
        }
        fake->jobs_created++;
        ippAddInteger(response, IPP_TAG_JOB, IPP_TAG_INTEGER, "job-id", 76 + fake->jobs_created);
        ippAddInteger(response, IPP_TAG_JOB, IPP_TAG_ENUM, "job-state", IPP_JSTATE_PENDING);
        break;
    }
    case IPP_OP_SEND_DOCUMENT: {
        attr = ippFindAttribute(request, "document-format", IPP_TAG_MIMETYPE);
        snprintf(fake->format, sizeof(fake->format), "%s", attr ? ippGetString(attr, 0, NULL) : "");
        pthread_mutex_unlock(&fake->lock);
        unsigned char buffer[65536];
        size_t total = 0;
        unsigned char head[8] = {0};
        ssize_t count;
        while ((count = httpRead2(http, (char *)buffer, sizeof(buffer))) > 0) {
            if (total < 8) memcpy(head + total, buffer, (size_t)count < 8 - total ? (size_t)count : 8 - total);
            total += (size_t)count;
        }
        pthread_mutex_lock(&fake->lock);
        fake->document_bytes = total;
        memcpy(fake->document_head, head, 8);
        break;
    }
    case IPP_OP_GET_JOB_ATTRIBUTES:
        if (fake->job_missing) {
            ippSetStatusCode(response, IPP_STATUS_ERROR_NOT_FOUND);
        } else {
            ippAddInteger(response, IPP_TAG_JOB, IPP_TAG_ENUM, "job-state", fake->job_state);
            ippAddString(response, IPP_TAG_JOB, IPP_TAG_TEXT, "job-state-message", NULL, "stand-in cupsd");
        }
        break;
    case IPP_OP_CANCEL_JOB:
        attr = ippFindAttribute(request, "job-id", IPP_TAG_INTEGER);
        fake->canceled_job = attr ? ippGetInteger(attr, 0) : -1;
        break;
    case IPP_OP_GET_PRINTER_ATTRIBUTES: {
        /* What cupsGetNamedDest() and cupsCopyDestInfo() need from cupsd. */
        attr = ippFindAttribute(request, "printer-uri", IPP_TAG_URI);
        const char *uri = attr ? ippGetString(attr, 0, NULL) : "";
        const char *queue = strstr(uri, "/printers/");
        char supported[256];
        snprintf(supported, sizeof(supported), "ipp://localhost:%d/printers/%s", fake->port, queue ? queue + 10 : "");
        ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_NAME, "printer-name", NULL, queue ? queue + 10 : "");
        ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_URI, "printer-uri-supported", NULL, supported);
        /* libcups addresses a server other than its default by device-uri. */
        snprintf(supported, sizeof(supported), "ipp://127.0.0.1:%d/printers/%s", fake->port, queue ? queue + 10 : "");
        ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_URI, "device-uri", NULL, supported);
        ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_TEXT, "printer-make-and-model", NULL,
                     "HP LaserJet Professional P1600dn Series");
        ippAddInteger(response, IPP_TAG_PRINTER, IPP_TAG_ENUM, "printer-type", CUPS_PRINTER_LOCAL | CUPS_PRINTER_BW);
        ippAddInteger(response, IPP_TAG_PRINTER, IPP_TAG_ENUM, "printer-state", fake->printer_state);
        ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "printer-state-reasons", NULL,
                     fake->printer_state == IPP_PSTATE_STOPPED ? "media-empty-error" : "none");
        ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_TEXT, "printer-state-message", NULL,
                     fake->printer_state == IPP_PSTATE_STOPPED ? "Out of paper" : "");
        ippAddBoolean(response, IPP_TAG_PRINTER, "printer-is-accepting-jobs", fake->accepting);
        ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_TEXT, "printer-location", NULL, "Office 2");
        ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "media-default", NULL, "na_letter_8.5x11in");
        ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_NAME, "marker-names", NULL, "Black Print Cartridge");
        ippAddInteger(response, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "marker-levels", 42);
        ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "marker-types", NULL, "toner");
        ippAddString(response, IPP_TAG_PRINTER, IPP_TAG_NAME, "marker-colors", NULL, "#000000");
        ippAddInteger(response, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "marker-low-levels", 5);
        ippAddInteger(response, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "marker-high-levels", 100);
        break;
    }
    default:
        fprintf(stderr, "stand-in cupsd: unexpected %s\n", ippOpString(ippGetOperation(request)));
        ippSetStatusCode(response, IPP_STATUS_ERROR_OPERATION_NOT_SUPPORTED);
        break;
    }
    pthread_mutex_unlock(&fake->lock);
}

static void fake_connection(FakeCups *fake, http_t *http) {
    httpBlocking(http, 1);
    while (httpWait(http, 3000)) {
        char uri[1024];
        http_state_t state;
        while ((state = httpReadRequest(http, uri, sizeof(uri))) == HTTP_STATE_WAITING) usleep(1000);
        if (state != HTTP_STATE_POST) break;
        http_status_t status;
        while ((status = httpUpdate(http)) == HTTP_STATUS_CONTINUE) {
        }
        if (status != HTTP_STATUS_OK) break;
        if (httpGetExpect(http) == HTTP_STATUS_CONTINUE) httpWriteResponse(http, HTTP_STATUS_CONTINUE);
        ipp_t *request = ippNew();
        if (ippRead(http, request) != IPP_STATE_DATA) {
            ippDelete(request);
            break;
        }
        ipp_t *response = ippNewResponse(request);
        ippSetStatusCode(response, IPP_STATUS_OK);
        fake_handle(fake, http, request, response);
        char drain[4096];
        while (httpGetState(http) == HTTP_STATE_POST_RECV && httpRead2(http, drain, sizeof(drain)) > 0) {
        }
        httpClearFields(http);
        httpSetField(http, HTTP_FIELD_CONTENT_TYPE, "application/ipp");
        httpSetLength(http, ippLength(response));
        httpWriteResponse(http, HTTP_STATUS_OK);
        ippSetState(response, IPP_STATE_IDLE);
        ippWrite(http, response);
        httpFlushWrite(http);
        ippDelete(request);
        ippDelete(response);
    }
    httpClose(http);
}

static void *fake_main(void *argument) {
    FakeCups *fake = argument;
    while (!atomic_load(&fake->stop)) {
        struct pollfd channel = {.fd = fake->listener, .events = POLLIN};
        if (poll(&channel, 1, 100) <= 0) continue;
        http_t *http = httpAcceptConnection(fake->listener, 1);
        if (http) fake_connection(fake, http);
    }
    return NULL;
}

static void fake_start(FakeCups *fake) {
    memset(fake, 0, sizeof(*fake));
    pthread_mutex_init(&fake->lock, NULL);
    atomic_init(&fake->stop, false);
    fake->job_state = IPP_JSTATE_PROCESSING;
    fake->printer_state = IPP_PSTATE_STOPPED;
    fake->accepting = true;
    fake->listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address = {.sin_len = sizeof(address), .sin_family = AF_INET,
                                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t length = sizeof(address);
    CHECK(fake->listener >= 0 && bind(fake->listener, (struct sockaddr *)&address, sizeof(address)) == 0 &&
          listen(fake->listener, 16) == 0 && getsockname(fake->listener, (struct sockaddr *)&address, &length) == 0);
    fake->port = ntohs(address.sin_port);
    CHECK(pthread_create(&fake->thread, NULL, fake_main, fake) == 0);
}

static void fake_stop(FakeCups *fake) {
    atomic_store(&fake->stop, true);
    pthread_join(fake->thread, NULL);
    close(fake->listener);
    pthread_mutex_destroy(&fake->lock);
}

static bool fake_text(FakeCups *fake, const char *field, const char *expected) {
    pthread_mutex_lock(&fake->lock);
    const char *value = strcmp(field, "queue") == 0   ? fake->queue
                        : strcmp(field, "title") == 0 ? fake->title
                                                      : fake->format;
    bool same = strcmp(value, expected) == 0;
    if (!same) fprintf(stderr, "stand-in cupsd %s is \"%s\", not \"%s\"\n", field, value, expected);
    pthread_mutex_unlock(&fake->lock);
    return same;
}

static int fake_number(FakeCups *fake, const int *field) {
    pthread_mutex_lock(&fake->lock);
    int value = *field;
    pthread_mutex_unlock(&fake->lock);
    return value;
}

static bool fake_options(FakeCups *fake, const char *needle) {
    pthread_mutex_lock(&fake->lock);
    bool found = strstr(fake->options, needle) != NULL;
    if (!found) fprintf(stderr, "stand-in cupsd options lack %s: %s\n", needle, fake->options);
    pthread_mutex_unlock(&fake->lock);
    return found;
}

static void test_real_mode(void) {
    FakeCups fake;
    fake_start(&fake);
    P1600AirCups cups = {"127.0.0.1", fake.port};
    char error[256], path[1024];

    /* The CUPS client functions on their own. */
    ipp_t *printer = p1600_air_cups_printer(&cups, "Office");
    CHECK(printer && ippGetInteger(ippFindAttribute(printer, "marker-levels", IPP_TAG_INTEGER), 0) == 42);
    ippDelete(printer);
    int fd = temp_file("submit.pdf", path, sizeof(path));
    CHECK(fd >= 0 && write(fd, tiny_pdf, sizeof(tiny_pdf) - 1) == (ssize_t)sizeof(tiny_pdf) - 1);
    cups_option_t *options = NULL;
    int count = cupsAddOption("PageSize", "A5", 0, &options);
    count = cupsAddOption("copies", "2", count, &options);
    count = cupsAddOption("sides", "two-sided-long-edge", count, &options);
    int cups_id =
        p1600_air_cups_submit(&cups, "Office", "Title", "application/pdf", fd, count, options, error, sizeof(error));
    cupsFreeOptions(count, options);
    close(fd);
    if (cups_id < 0) fprintf(stderr, "submit: %s\n", error);
    CHECK(cups_id == 77 && fake_text(&fake, "queue", "Office") && fake_text(&fake, "title", "Title"));
    CHECK(fake_options(&fake, "PageSize=A5;") && fake_options(&fake, "copies=2;") &&
          fake_options(&fake, "sides=two-sided-long-edge;"));
    pthread_mutex_lock(&fake.lock);
    CHECK(fake.document_bytes == sizeof(tiny_pdf) - 1 && memcmp(fake.document_head, "%PDF-1.4", 8) == 0 &&
          strcmp(fake.format, "application/pdf") == 0);
    pthread_mutex_unlock(&fake.lock);
    ipp_jstate_t state = IPP_JSTATE_PENDING;
    char message[256];
    CHECK(p1600_air_cups_job_state(&cups, 77, &state, message, sizeof(message)) == 0 && state == IPP_JSTATE_PROCESSING &&
          strcmp(message, "stand-in cupsd") == 0);
    pthread_mutex_lock(&fake.lock);
    fake.job_missing = true;
    pthread_mutex_unlock(&fake.lock);
    CHECK(p1600_air_cups_job_state(&cups, 77, &state, message, sizeof(message)) == 1);
    pthread_mutex_lock(&fake.lock);
    fake.job_missing = false;
    pthread_mutex_unlock(&fake.lock);
    CHECK(p1600_air_cups_cancel(&cups, "Office", 77) == 0 && fake_number(&fake, &fake.canceled_job) == 77);
    P1600AirCups nowhere = {"127.0.0.1", 1};
    CHECK(p1600_air_cups_printer(&nowhere, "Office") == NULL);
    fd = open(path, O_RDONLY);
    CHECK(p1600_air_cups_submit(&nowhere, "Office", "T", "application/pdf", fd, 0, NULL, error, sizeof(error)) < 0);
    close(fd);
    puts("[PASS] Real mode: Create-Job/Send-Document with CUPS options, job state, cancel and queue state");
    puts("       on a stand-in cupsd");

    /* The whole server in real mode: state mirrored, jobs submitted and tracked. */
    P1600AirConfig config;
    memset(&config, 0, sizeof(config));
    config.loopback = true;
    config.mirror_cups = true;
    config.cups = cups;
    config.queue_count = 1;
    snprintf(config.queues[0].name, sizeof(config.queues[0].name), "TestQ");
    snprintf(config.queues[0].info, sizeof(config.queues[0].info), "Faculty printer");
    snprintf(config.queues[0].make_model, sizeof(config.queues[0].make_model), "HP LaserJet Professional P1606dn");
    RunningServer running;
    server_start(&running, &config);
    int port = p1600_air_server_port(running.server);
    ipp_t *request = new_request(IPP_OP_GET_PRINTER_ATTRIBUTES, port, "/ipp/print/TestQ");
    ipp_t *response = exchange(port, "/ipp/print/TestQ", request, NULL, 0);
    CHECK(ippGetInteger(ippFindAttribute(response, "printer-state", IPP_TAG_ENUM), 0) == IPP_PSTATE_STOPPED);
    CHECK(ippContainsString(ippFindAttribute(response, "printer-state-reasons", IPP_TAG_KEYWORD), "media-empty-error"));
    CHECK(strcmp(ippGetString(ippFindAttribute(response, "printer-state-message", IPP_TAG_TEXT), 0, NULL),
                 "Out of paper") == 0);
    CHECK(strcmp(ippGetString(ippFindAttribute(response, "printer-location", IPP_TAG_TEXT), 0, NULL), "Office 2") == 0);
    CHECK(strcmp(ippGetString(ippFindAttribute(response, "printer-info", IPP_TAG_TEXT), 0, NULL),
                 "Faculty printer") == 0);
    CHECK(strcmp(ippGetString(ippFindAttribute(response, "media-default", IPP_TAG_KEYWORD), 0, NULL),
                 "na_letter_8.5x11in") == 0);
    CHECK(ippGetInteger(ippFindAttribute(response, "marker-levels", IPP_TAG_INTEGER), 0) == 42);
    ipp_attribute_t *supply = ippFindAttribute(response, "printer-supply", IPP_TAG_STRING);
    int supply_length = 0;
    const char *supply_text = supply ? ippGetOctetString(supply, 0, &supply_length) : NULL;
    CHECK(supply_text && memmem(supply_text, (size_t)supply_length, "level=42;", 9));
    CHECK(strcmp(ippGetString(ippFindAttribute(response, "printer-supply-description", IPP_TAG_TEXT), 0, NULL),
                 "Black Print Cartridge") == 0);
    ippDelete(response);

    ipp_status_t status;
    int id = print_document(port, "application/pdf", NULL, tiny_pdf, sizeof(tiny_pdf) - 1, &status, add_quality_copies);
    CHECK(status == IPP_STATUS_OK && id > 0);
    bool submitted = false;
    for (int i = 0; i < 200 && !submitted; i++) {
        pthread_mutex_lock(&fake.lock);
        submitted = fake.jobs_created == 2 && fake.document_bytes == sizeof(tiny_pdf) - 1;
        pthread_mutex_unlock(&fake.lock);
        if (!submitted) usleep(25000);
    }
    CHECK(submitted && fake_options(&fake, "Resolution=600dpi;") && fake_options(&fake, "copies=2;"));
    CHECK(fake_text(&fake, "title", "unit test"));
    CHECK(wait_job(port, id, IPP_JSTATE_PROCESSING) == IPP_JSTATE_PROCESSING);
    pthread_mutex_lock(&fake.lock);
    fake.job_state = IPP_JSTATE_COMPLETED;
    pthread_mutex_unlock(&fake.lock);
    CHECK(wait_job(port, id, IPP_JSTATE_COMPLETED) == IPP_JSTATE_COMPLETED);

    /* Cancel-Job reaches the CUPS job. */
    pthread_mutex_lock(&fake.lock);
    fake.job_state = IPP_JSTATE_PROCESSING;
    fake.canceled_job = 0;
    pthread_mutex_unlock(&fake.lock);
    id = print_document(port, "application/pdf", NULL, tiny_pdf, sizeof(tiny_pdf) - 1, &status, NULL);
    for (int i = 0; i < 200 && fake_number(&fake, &fake.jobs_created) < 3; i++) usleep(25000);
    request = new_request(IPP_OP_CANCEL_JOB, port, "/ipp/print/TestQ");
    ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "job-id", id);
    response = exchange(port, "/ipp/print/TestQ", request, NULL, 0);
    CHECK(ippGetStatusCode(response) == IPP_STATUS_OK && job_state(port, id) == IPP_JSTATE_CANCELED);
    ippDelete(response);
    for (int i = 0; i < 200 && fake_number(&fake, &fake.canceled_job) != 79; i++) usleep(25000);
    CHECK(fake_number(&fake, &fake.canceled_job) == 79);
    pthread_mutex_lock(&fake.lock);
    /* A queue that stops accepting jobs refuses new ones. */
    fake.accepting = false;
    pthread_mutex_unlock(&fake.lock);
    usleep((P1600_AIR_STATE_CACHE_SECONDS + 1) * 1000000);
    print_document(port, "application/pdf", NULL, tiny_pdf, sizeof(tiny_pdf) - 1, &status, NULL);
    CHECK(status == IPP_STATUS_ERROR_NOT_ACCEPTING_JOBS);
    server_stop(&running);
    fake_stop(&fake);
    puts("[PASS] Real mode server: queue state/markers mirrored, job submitted with options, CUPS state mirrored,");
    puts("       Cancel-Job cancels the CUPS job, a queue not accepting jobs refuses Print-Job");
}

/* --- sample PWG raster for test_airprint.sh ------------------------------------------ */

static int write_pwg_sample(const char *path, const char *type, int dpi, int pages) {
    cups_page_header2_t header;
    if ((strcmp(type, "sgray_8") != 0 && strcmp(type, "srgb_8") != 0) || dpi < 150 || dpi > 600 || pages < 1 ||
        pages > 9) {
        fprintf(stderr, "usage: test_airprint --write-pwg OUT sgray_8|srgb_8 DPI PAGES\n");
        return 2;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return 1;
    cups_raster_t *raster = cupsRasterOpen(fd, CUPS_RASTER_WRITE_PWG);
    for (int page = 0; page < pages; page++) {
        if (!cupsRasterInitPWGHeader(&header, pwgMediaForPWG("iso_a4_210x297mm"), type, dpi, dpi, "one-sided", NULL)) {
            return 1;
        }
        unsigned components = header.cupsBitsPerPixel / 8;
        unsigned char *line = malloc(header.cupsBytesPerLine);
        cupsRasterWriteHeader2(raster, &header);
        for (unsigned y = 0; y < header.cupsHeight; y++) {
            memset(line, 0xff, header.cupsBytesPerLine);
            unsigned margin = (unsigned)dpi / 2;
            bool frame = y == margin || y == header.cupsHeight - margin;
            bool inside = y > margin && y < header.cupsHeight - margin;
            for (unsigned x = margin; inside && x < header.cupsWidth - margin; x++) {
                bool bar = (y / ((unsigned)dpi / 6)) % 4 == (unsigned)page % 4 && (x / ((unsigned)dpi / 4)) % 2 == 0;
                if (x == margin || x + 1 == header.cupsWidth - margin || bar) {
                    for (unsigned c = 0; c < components; c++) line[x * components + c] = (unsigned char)(c * 60);
                }
            }
            if (frame) memset(line + margin * components, 0, (header.cupsWidth - 2 * margin) * components);
            cupsRasterWritePixels(raster, line, header.cupsBytesPerLine);
        }
        free(line);
    }
    cupsRasterClose(raster);
    return close(fd) == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc == 6 && strcmp(argv[1], "--write-pwg") == 0) {
        return write_pwg_sample(argv[2], argv[3], atoi(argv[4]), atoi(argv[5]));
    }
    const char *ppd = argc > 1 ? argv[1] : "ppd/P1600dn-Series.ppd";
    signal(SIGPIPE, SIG_IGN);
    const char *tmp = getenv("TMPDIR");
    snprintf(temp_root, sizeof(temp_root), "%s/test_airprint.XXXXXX", tmp && *tmp ? tmp : "/tmp");
    CHECK(mkdtemp(temp_root) != NULL);
    puts("[*] AirPrint server and raster document tests");
    test_urf();
    test_libcups_urf();
    test_pwg();
    test_pdf();
    test_media(ppd);
    test_formats();
    test_options();
    test_bonjour();
    test_server_spool();
    test_real_mode();
    char command[1200];
    snprintf(command, sizeof(command), "/bin/rm -rf '%s'", temp_root);
    if (system(command) != 0) fprintf(stderr, "could not remove %s\n", temp_root);
    puts("[+] All AirPrint tests passed.");
    return 0;
}
