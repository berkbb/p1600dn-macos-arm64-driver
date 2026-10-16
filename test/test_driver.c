/*
 * Unit tests for the page renderer: halftone screening against a reference
 * written from the screening formulas of the job specification, screen table
 * invariants, the DMPAPER table, raster geometry and page rendering.
 */
#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <cups/raster.h>

#include "p1600_raster.h"
#include "p1600_screen.h"
#include "p1600_screen_tables.h"
#include "zjs_encoder.h"

static unsigned int rng_state = 0x1606dU;

static unsigned int next_random(void) {
    rng_state = rng_state * 1103515245U + 12345U;
    return rng_state >> 8;
}

/* Reference screen from the formulas in reference/zjstream/ZJS_STREAM_SPEC.md
 * (section 3), one pixel at a time, MSB first; darkness g = 255 - gray. */
static void reference_screen(const uint8_t *gray, unsigned width, uint32_t bpp, uint32_t n, uint8_t *dst) {
    for (unsigned x = 0; x < width; x++) {
        unsigned g = 255U - gray[x];
        if (g == 0) continue;
        if (bpp == 2) {
            unsigned cell = p1600_screen_2bpp[g * 48U + (n % 12U) * 4U + (x / 4U) % 3U];
            unsigned level = (cell >> (6U - 2U * (x & 3U))) & 3U;
            dst[x / 4U] |= (uint8_t)(level << (6U - 2U * (x & 3U)));
        } else if (g >= p1600_screen_1bpp[(n % 16U) * 16U + x % 16U]) {
            dst[x / 8U] |= (uint8_t)(0x80U >> (x & 7U));
        }
    }
}

static void test_screen_against_reference(void) {
    static uint8_t gray[4096 + 8];
    static uint8_t expected[2048], actual[2048];
    unsigned long checks = 0;
    for (int iteration = 0; iteration < 12000; iteration++) {
        unsigned width = 1U + next_random() % 2500U;
        uint32_t remaining = 1U + next_random() % 8000U;
        int mode = iteration % 5;
        for (unsigned i = 0; i < width + 8U; i++) {
            unsigned r = next_random();
            gray[i] = mode == 0 ? (uint8_t)r
                    : mode == 1 ? ((r & 7U) ? 0xff : (uint8_t)(r >> 3))
                    : mode == 2 ? ((r & 1U) ? 0xff : 0x00)
                    : mode == 3 ? (uint8_t)(200U + r % 56U)
                                : (uint8_t)((i / 37U) * 29U);
        }
        for (unsigned i = width; i < width + 8U; i++) gray[i] = 0xff;
        for (uint32_t bpp = 1; bpp <= 2; bpp++) {
            memset(expected, 0, sizeof(expected));
            memset(actual, 0, sizeof(actual));
            reference_screen(gray, width, bpp, remaining, expected);
            p1600_screen_line(gray, width, bpp, remaining, actual);
            assert(memcmp(expected, actual, ((size_t)width * bpp + 7U) / 8U) == 0);
            checks++;
        }
    }
    printf("    [PASS] Screening vs. Reference Formulas (%lu lines)\n", checks);
}

/* The NEON screen against the scalar one: every darkness value at every
 * position of the 48-pixel FastRes and 16-pixel 600 dpi periods, every screen
 * row, line tails of every length, and OR-ing into existing bits. */
static void test_screen_simd_matches_scalar(void) {
#if defined(__ARM_NEON)
    assert(p1600_screen_simd_active());
#endif
    enum { FULL = 48 * 256 };
    static uint8_t gray[FULL], expected[FULL / 4 + 16], actual[FULL / 4 + 16], prefill[FULL / 4 + 16];
    for (unsigned value = 0; value < 256U; value++) {
        for (unsigned phase = 0; phase < 48U; phase++) gray[phase + 48U * value] = (uint8_t)value;
    }
    for (size_t i = 0; i < sizeof(prefill); i++) prefill[i] = (uint8_t)next_random();
    unsigned long checks = 0;
    for (uint32_t remaining = 0; remaining < 48U; remaining++) {
        for (uint32_t bpp = 1; bpp <= 2; bpp++) {
            for (unsigned cut = 0; cut <= 96U; cut++) {
                unsigned width = cut <= 48U ? FULL - cut : cut - 48U;
                for (int prefilled = 0; prefilled < 2; prefilled++) {
                    size_t bytes = ((size_t)width * bpp + 7U) / 8U;
                    if (prefilled) {
                        memcpy(expected, prefill, sizeof(expected));
                        memcpy(actual, prefill, sizeof(actual));
                    } else {
                        memset(expected, 0, sizeof(expected));
                        memset(actual, 0, sizeof(actual));
                    }
                    p1600_screen_line_scalar(gray, width, bpp, remaining, expected);
                    p1600_screen_line(gray, width, bpp, remaining, actual);
                    assert(memcmp(expected, actual, sizeof(expected)) == 0);
                    assert(bytes <= sizeof(expected));
                    checks++;
                }
            }
        }
    }
    p1600_screen_line_scalar(NULL, 10, 2, 1, actual);
    p1600_screen_line_scalar(gray, 10, 2, 1, NULL);
    printf("    [PASS] NEON Screening Identical to Scalar (%lu lines, all phases and darkness values)\n", checks);
}

static void test_screen_tables_and_extremes(void) {
    for (unsigned row = 0; row < 12; row++) {
        for (unsigned x = 0; x < 12; x++) {
            unsigned previous = 0;
            for (unsigned darkness = 0; darkness < 256; darkness++) {
                unsigned cell = p1600_screen_2bpp[darkness * 48U + row * 4U + x / 4U];
                unsigned level = (cell >> (6U - 2U * (x % 4U))) & 3U;
                assert(level >= previous);
                previous = level;
                if (darkness == 0) assert(level == 0);
                if (darkness == 255) assert(level == 3);
            }
        }
    }
    for (unsigned darkness = 0; darkness < 256; darkness++) {
        for (unsigned row = 0; row < 12; row++) assert(p1600_screen_2bpp[darkness * 48U + row * 4U + 3U] == 0);
    }

    uint8_t white[600], black[600], out[300];
    memset(white, 0xff, sizeof(white));
    memset(black, 0x00, sizeof(black));
    for (uint32_t bpp = 1; bpp <= 2; bpp++) {
        for (uint32_t remaining = 1; remaining <= 48; remaining++) {
            memset(out, 0, sizeof(out));
            p1600_screen_line(white, 600, bpp, remaining, out);
            for (size_t i = 0; i < sizeof(out); i++) assert(out[i] == 0);
            memset(out, 0, sizeof(out));
            p1600_screen_line(black, 600, bpp, remaining, out);
            for (size_t i = 0; i < 600U * bpp / 8U; i++) assert(out[i] == 0xff);
        }
    }

    /* The screen row is the descending line counter modulo the screen size. */
    uint8_t mid[96], a[24], b[24];
    memset(mid, 0x80, sizeof(mid));
    for (uint32_t remaining = 1; remaining <= 40; remaining++) {
        memset(a, 0, sizeof(a));
        memset(b, 0, sizeof(b));
        p1600_screen_line(mid, 96, 2, remaining, a);
        p1600_screen_line(mid, 96, 2, remaining + 12U, b);
        assert(memcmp(a, b, sizeof(a)) == 0);
        memset(a, 0, sizeof(a));
        memset(b, 0, sizeof(b));
        p1600_screen_line(mid, 96, 1, remaining, a);
        p1600_screen_line(mid, 96, 1, remaining + 16U, b);
        assert(memcmp(a, b, 12) == 0);
    }
    p1600_screen_line(NULL, 10, 2, 1, out);
    p1600_screen_line(white, 10, 2, 1, NULL);
    puts("    [PASS] Screen Table Monotonicity, White/Black Extremes and Row Phase");
}

static void test_paper_codes(void) {
    static const struct { uint32_t w, h, code; } sizes[] = {
        {612, 792, 1}, {612, 1008, 5}, {595, 842, 9}, {522, 756, 7}, {612, 936, 41},
        {420, 595, 11}, {297, 420, 70}, {516, 729, 13}, {284, 419, 43}, {567, 420, 69},
        {297, 684, 20}, {279, 540, 37}, {499, 709, 34}, {459, 649, 28}, {312, 624, 27},
        {842, 595, 77}, {792, 612, 75}, {420, 567, 82}, {729, 516, 79},
        /* 16K sizes get codes 257, 263 and 264; other sizes 256 (custom). */
        {553, 765, 264}, {522, 737, 263}, {558, 774, 257}, {552, 766, 264}, {557, 773, 257},
        {600, 800, 256}, {0, 0, 256}, {792, 1224, 256}, {842, 1191, 256},
        /* Rounded A4/Letter still name their size. */
        {595, 841, 9}, {596, 843, 9}, {594, 842, 9}, {611, 792, 1}, {613, 1007, 5}, {593, 842, 256},
    };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        assert(p1600_paper_code(sizes[i].w, sizes[i].h) == sizes[i].code);
    }
    puts("    [PASS] DMPAPER Mapping (PPD sizes and landscape forms, 1-point rounding, 16K 257/263/264, custom 256)");
}

static cups_page_header2_t gray_header(uint32_t width, uint32_t height, uint32_t compression) {
    cups_page_header2_t header;
    memset(&header, 0, sizeof(header));
    header.HWResolution[0] = 600;
    header.HWResolution[1] = 600;
    header.cupsWidth = width;
    header.cupsHeight = height;
    header.cupsBitsPerColor = 8;
    header.cupsBitsPerPixel = 8;
    header.cupsBytesPerLine = width;
    header.cupsColorOrder = CUPS_ORDER_CHUNKED;
    header.cupsColorSpace = CUPS_CSPACE_W;
    header.cupsNumColors = 1;
    header.cupsCompression = compression;
    header.cupsInteger[0] = P1600_RASTER_MODEL_ID;
    return header;
}

static cups_page_header2_t legacy_header(uint32_t width, uint32_t height) {
    cups_page_header2_t header = gray_header(width, height, 0);
    header.HWResolution[0] = 1200;
    header.cupsBitsPerColor = 1;
    header.cupsBitsPerPixel = 1;
    header.cupsBytesPerLine = (width + 7U) / 8U;
    header.cupsColorSpace = CUPS_CSPACE_K;
    return header;
}

static void test_geometry(void) {
    P1600PageGeometry geometry;
    char error[160];
    cups_page_header2_t header = gray_header(4758, 6817, 1);
    assert(p1600_page_geometry(&header, &geometry, error, sizeof(error)) == 0);
    assert(geometry.kind == P1600_RASTER_GRAY && geometry.width == 4758 && geometry.bpp == 2);
    assert(geometry.height == 6828 && geometry.content_height == 6817 && geometry.line_bytes == 1200);
    header.cupsCompression = 0;
    assert(p1600_page_geometry(&header, &geometry, error, sizeof(error)) == 0 && geometry.bpp == 1);
    assert(geometry.line_bytes == 608);

    header = legacy_header(9517, 6817);
    assert(p1600_page_geometry(&header, &geometry, error, sizeof(error)) == 0);
    assert(geometry.kind == P1600_RASTER_LEGACY_1BIT && geometry.width == 4759 && geometry.bpp == 2);
    assert(geometry.height == 6828 && geometry.line_bytes == 1200);

    cups_page_header2_t bad[12];
    for (int i = 0; i < 12; i++) bad[i] = gray_header(100, 100, 1);
    bad[0].cupsInteger[0] = 0;
    bad[1].HWResolution[0] = 300;
    bad[2].HWResolution[1] = 1200;
    bad[3].cupsBitsPerColor = 16;
    bad[4].cupsColorSpace = CUPS_CSPACE_K;
    bad[5].cupsNumColors = 3;
    bad[6].cupsColorOrder = CUPS_ORDER_BANDED;
    bad[7].cupsWidth = 0;
    bad[8].cupsHeight = 600U * 18U + 1U;
    bad[9].cupsBytesPerLine = 99;
    bad[10].cupsBytesPerLine = 129;
    bad[11] = legacy_header(100, 100);
    bad[11].cupsBytesPerLine = 3;
    for (int i = 0; i < 12; i++) {
        error[0] = '\0';
        assert(p1600_page_geometry(&bad[i], &geometry, error, sizeof(error)) == -1 && error[0] != '\0');
    }
    assert(p1600_page_geometry(NULL, &geometry, error, sizeof(error)) == -1);
    puts("    [PASS] Raster Geometry: 8-Bit Gray, Legacy 1-Bit and 12 Rejection Cases");
}

/* Writes one raster page to a temporary file and renders it back. */
static unsigned char *render(const cups_page_header2_t *header, const unsigned char *pixels, bool rotate,
                             P1600PageGeometry *geometry) {
    char path[] = "/tmp/p1600dn-render-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    cups_raster_t *writer = cupsRasterOpen(fd, CUPS_RASTER_WRITE);
    cups_page_header2_t copy = *header;
    assert(writer && cupsRasterWriteHeader2(writer, &copy));
    size_t bytes = (size_t)header->cupsBytesPerLine * header->cupsHeight;
    assert(cupsRasterWritePixels(writer, (unsigned char *)pixels, (unsigned)bytes) == bytes);
    cupsRasterClose(writer);
    assert(lseek(fd, 0, SEEK_SET) == 0);

    cups_raster_t *reader = cupsRasterOpen(fd, CUPS_RASTER_READ);
    cups_page_header2_t read_header;
    char error[160];
    assert(reader && cupsRasterReadHeader2(reader, &read_header));
    assert(p1600_page_geometry(&read_header, geometry, error, sizeof(error)) == 0);
    unsigned char *bitmap = calloc(geometry->height, geometry->line_bytes);
    assert(bitmap != NULL);
    assert(p1600_render_page(reader, &read_header, geometry, rotate, bitmap) == 0);
    cupsRasterClose(reader);
    close(fd);
    unlink(path);
    return bitmap;
}

static void test_render_and_rotation(void) {
    const uint32_t widths[] = {1, 13, 64, 131};
    for (size_t w = 0; w < sizeof(widths) / sizeof(widths[0]); w++) {
        for (uint32_t height = 1; height <= 30; height += 7) {
            for (uint32_t compression = 0; compression <= 1; compression++) {
                uint32_t width = widths[w];
                cups_page_header2_t header = gray_header(width, height, compression);
                unsigned char *pixels = malloc((size_t)width * height);
                unsigned char *turned = malloc((size_t)width * height);
                assert(pixels && turned);
                for (size_t i = 0; i < (size_t)width * height; i++) pixels[i] = (unsigned char)next_random();
                for (size_t i = 0; i < (size_t)width * height; i++) turned[i] = pixels[(size_t)width * height - 1U - i];

                P1600PageGeometry geometry;
                unsigned char *plain = render(&header, pixels, false, &geometry);
                unsigned char *rotated = render(&header, pixels, true, &geometry);
                unsigned char *from_turned = render(&header, turned, false, &geometry);
                /* Turning the gray page equals screening the turned page. */
                assert(memcmp(rotated, from_turned, geometry.height * geometry.line_bytes) == 0);
                for (uint32_t y = 0; y < height; y++) {
                    uint8_t line[160] = {0};
                    p1600_screen_line(pixels + (size_t)y * width, width, geometry.bpp, geometry.height - y, line);
                    assert(memcmp(plain + (size_t)y * geometry.line_bytes, line, geometry.line_bytes) == 0);
                }
                for (size_t i = (size_t)height * geometry.line_bytes; i < geometry.height * geometry.line_bytes; i++) {
                    assert(plain[i] == 0 && rotated[i] == 0);
                }
                free(plain);
                free(rotated);
                free(from_turned);
                free(pixels);
                free(turned);
            }
        }
    }

    /* Legacy 1-bit pages: bits copied, row padding masked, bit-exact turn. */
    cups_page_header2_t header = legacy_header(21, 5);
    header.cupsBytesPerLine = 4;
    unsigned char pixels[20];
    for (size_t i = 0; i < sizeof(pixels); i++) pixels[i] = (unsigned char)next_random();
    P1600PageGeometry geometry;
    unsigned char *plain = render(&header, pixels, false, &geometry);
    unsigned char *rotated = render(&header, pixels, true, &geometry);
    assert(geometry.width == 11 && geometry.height == 12 && geometry.line_bytes == 16);
    for (uint32_t y = 0; y < 5; y++) {
        for (uint32_t x = 0; x < 32; x++) {
            int bit = (pixels[y * 4U + x / 8U] >> (7U - x % 8U)) & 1;
            int got = (plain[y * 16U + x / 8U] >> (7U - x % 8U)) & 1;
            assert(got == (x < 21U ? bit : 0));
            if (x < 21U) {
                int turned = (rotated[(4U - y) * 16U + (20U - x) / 8U] >> (7U - (20U - x) % 8U)) & 1;
                assert(turned == bit);
            }
        }
    }
    free(plain);
    free(rotated);
    puts("    [PASS] Page Rendering: Screened Lines, White Padding, 180-Degree Turns (Gray + Legacy)");
}

int main(void) {
    puts("[*] Starting page renderer unit tests...");
    test_screen_against_reference();
    test_screen_simd_matches_scalar();
    test_screen_tables_and_extremes();
    test_paper_codes();
    test_geometry();
    test_render_and_rotation();
    puts("[+] Page renderer unit tests passed.");
    return 0;
}
