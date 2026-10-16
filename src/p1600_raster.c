#include "p1600_raster.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "p1600_screen.h"
#include "zjs.h"
#include "zjs_encoder.h"

/* Largest page edge accepted from CUPS, in inches (P1600dn maximum is 14). */
#define P1600_RASTER_MAX_INCHES 18U

/* The paper sizes of the PPD, and their landscape forms where Windows has a
 * code for them, in points, with their Windows DMPAPER codes (wingdi.h). The
 * 16K sizes have no wingdi.h name; 264, 263 and 257 are the printer's codes
 * for them. */
static const struct {
    uint16_t width;
    uint16_t height;
    uint16_t code;
} paper_sizes[] = {
    {612, 792, DMPAPER_LETTER},
    {612, 1008, DMPAPER_LEGAL},
    {595, 842, DMPAPER_A4},
    {522, 756, DMPAPER_EXECUTIVE},
    {612, 936, DMPAPER_FOLIO_8_5X13},
    {420, 595, DMPAPER_A5},
    {297, 420, DMPAPER_A6},
    {516, 729, DMPAPER_B5},
    {284, 419, DMPAPER_POSTCARD},
    {567, 420, DMPAPER_DOUBLE_POSTCARD},
    {297, 684, DMPAPER_ENV_10},
    {279, 540, DMPAPER_ENV_MONARCH},
    {499, 709, DMPAPER_ENV_B5},
    {459, 649, DMPAPER_ENV_C5},
    {312, 624, DMPAPER_ENV_DL},
    {792, 612, DMPAPER_LETTER_ROTATED},
    {842, 595, DMPAPER_A4_ROTATED},
    {595, 420, DMPAPER_A5_ROTATED},
    {420, 297, DMPAPER_A6_ROTATED},
    {729, 516, DMPAPER_B5_ROTATED},
    {419, 284, DMPAPER_POSTCARD_ROTATED},
    {420, 567, DMPAPER_DOUBLE_POSTCARD_ROTATED},
    {553, 765, DMPAPER_16K_195X270},
    {522, 737, DMPAPER_16K_184X260},
    {558, 774, DMPAPER_16K_197X273},
};

static bool within_a_point(uint32_t a, uint32_t b) {
    return a + 1U >= b && b + 1U >= a;
}

uint32_t p1600_paper_code(uint32_t width_points, uint32_t height_points) {
    /* PageSize is whole points, and applications round A4 (595.28 x 841.89)
     * either way (macOS truncates, so B5 arrives as 515 x 728): an exact
     * match first, then one point either way. */
    for (size_t i = 0; i < sizeof(paper_sizes) / sizeof(paper_sizes[0]); i++) {
        if (paper_sizes[i].width == width_points && paper_sizes[i].height == height_points) {
            return paper_sizes[i].code;
        }
    }
    for (size_t i = 0; i < sizeof(paper_sizes) / sizeof(paper_sizes[0]); i++) {
        if (within_a_point(paper_sizes[i].width, width_points) &&
            within_a_point(paper_sizes[i].height, height_points)) {
            return paper_sizes[i].code;
        }
    }
    return DMPAPER_USER;
}

static int fail(char *error, size_t error_size, const char *reason) {
    if (error && error_size > 0) snprintf(error, error_size, "%s", reason);
    return -1;
}

ssize_t p1600_raster_input_read(void *context, unsigned char *buffer, size_t length) {
    P1600RasterInput *input = (P1600RasterInput *)context;
    if (!input || !buffer) return -1;

    size_t total = 0;
    while (total < length) {
        ssize_t amount = read(input->fd, buffer + total, length - total);
        if (amount > 0) {
            for (size_t i = 0; i < (size_t)amount && input->delivered + i < sizeof(input->sync); i++) {
                input->sync[input->delivered + i] = buffer[total + i];
            }
            input->delivered += (size_t)amount;
            total += (size_t)amount;
        } else if (amount == 0) {
            if (total > 0) input->short_read = true;
            else input->clean_eof = true;
            break;
        } else if (errno != EINTR) {
            input->io_error = errno;
            return total > 0 ? (ssize_t)total : -1;
        }
    }
    return (ssize_t)total;
}

int p1600_raster_input_end(const P1600RasterInput *input, char *error, size_t error_size) {
    if (!input) return fail(error, error_size, "Missing raster input");
    if (input->io_error != 0) {
        if (error && error_size > 0) {
            snprintf(error, error_size, "Unable to read raster input: %s", strerror(input->io_error));
        }
        return -1;
    }
    bool compressed = memcmp(input->sync, "RaS2", 4) == 0 || memcmp(input->sync, "2SaR", 4) == 0;
    if (compressed) return 0;
    if (input->short_read) return fail(error, error_size, "Truncated CUPS raster stream");
    if (!input->clean_eof) {
        const char *reason = cupsRasterErrorString();
        if (error && error_size > 0) {
            snprintf(error, error_size, "Invalid CUPS raster page header%s%s",
                     reason && *reason ? ": " : "", reason && *reason ? reason : "");
        }
        return -1;
    }
    return 0;
}

int p1600_prepare_standard_descriptors(void) {
    bool stdout_open = fcntl(STDOUT_FILENO, F_GETFD) != -1;
    for (int fd = STDIN_FILENO; fd <= STDERR_FILENO; fd++) {
        if (fcntl(fd, F_GETFD) == -1 && errno == EBADF) {
            int opened = open("/dev/null", fd == STDIN_FILENO ? O_RDONLY : O_WRONLY);
            if (opened != fd) {
                if (opened >= 0) close(opened);
                return -1;
            }
        }
    }
    return stdout_open ? 0 : -1;
}

int p1600_page_geometry(const cups_page_header2_t *header, P1600PageGeometry *geometry,
                     char *error, size_t error_size) {
    if (!header || !geometry) return fail(error, error_size, "Missing raster header");
    if (header->cupsInteger[0] != P1600_RASTER_MODEL_ID) {
        return fail(error, error_size, "Raster was not prepared by the HP LaserJet P1600dn PPD");
    }
    if (header->cupsNumColors != 1 || header->cupsColorOrder != CUPS_ORDER_CHUNKED) {
        return fail(error, error_size, "Unsupported raster color layout");
    }

    uint32_t x_dpi = header->HWResolution[0];
    uint32_t y_dpi = header->HWResolution[1];
    uint32_t width = header->cupsWidth;
    uint32_t height = header->cupsHeight;
    if (y_dpi != 600U || width == 0 || height == 0 ||
        width > x_dpi * P1600_RASTER_MAX_INCHES || height > y_dpi * P1600_RASTER_MAX_INCHES) {
        return fail(error, error_size, "Unsupported raster resolution or page size");
    }

    P1600PageGeometry result = {0};
    size_t minimum_bpl;
    size_t maximum_bpl;
    if (x_dpi == 600U && header->cupsBitsPerColor == 8 && header->cupsBitsPerPixel == 8 &&
        header->cupsColorSpace == CUPS_CSPACE_W) {
        result.kind = P1600_RASTER_GRAY;
        result.width = width;
        result.bpp = (header->cupsCompression & 1U) + 1U;
        minimum_bpl = width;
        maximum_bpl = ((size_t)width + 127U) & ~(size_t)127U;
    } else if (x_dpi == 1200U && header->cupsBitsPerColor == 1 && header->cupsBitsPerPixel == 1 &&
               header->cupsColorSpace == CUPS_CSPACE_K) {
        result.kind = P1600_RASTER_LEGACY_1BIT;
        result.width = (width + 1U) / 2U;
        result.bpp = 2;
        minimum_bpl = ((size_t)width + 7U) / 8U;
        maximum_bpl = ((size_t)width + 127U) / 128U * 16U;
    } else {
        return fail(error, error_size, "Unsupported raster format (expected 600 dpi 8-bit gray)");
    }
    if (header->cupsBytesPerLine < minimum_bpl || header->cupsBytesPerLine > maximum_bpl) {
        return fail(error, error_size, "Invalid raster line length");
    }

    result.content_height = height;
    result.height = zjs_video_height(height);
    result.line_bytes = zjs_line_bytes(result.width, result.bpp);
    *geometry = result;
    return 0;
}

static int render_gray(cups_raster_t *raster, const cups_page_header2_t *header,
                       const P1600PageGeometry *geometry, bool rotate, unsigned char *bitmap,
                       P1600LineProgress progress, void *context) {
    unsigned char *line = malloc(header->cupsBytesPerLine);
    unsigned char *reversed = rotate ? malloc(geometry->width) : NULL;
    if (!line || (rotate && !reversed)) {
        free(line);
        free(reversed);
        return -1;
    }

    int status = 0;
    for (uint32_t y = 0; y < geometry->content_height; y++) {
        if (cupsRasterReadPixels(raster, line, header->cupsBytesPerLine) != header->cupsBytesPerLine) {
            status = -1;
            break;
        }
        uint32_t output_y = rotate ? geometry->content_height - 1U - y : y;
        const unsigned char *source = line;
        if (rotate) {
            for (uint32_t x = 0; x < geometry->width; x++) {
                reversed[x] = line[geometry->width - 1U - x];
            }
            source = reversed;
        }
        /* Screen at the output row so a turned page keeps the screen phase. */
        p1600_screen_line(source, geometry->width, geometry->bpp, geometry->height - output_y,
                       bitmap + (size_t)output_y * geometry->line_bytes);
        if (progress) progress(context, y, geometry->height);
    }

    free(line);
    free(reversed);
    return status;
}

static int render_legacy(cups_raster_t *raster, const cups_page_header2_t *header,
                         const P1600PageGeometry *geometry, bool rotate, unsigned char *bitmap,
                         P1600LineProgress progress, void *context) {
    unsigned char *line = malloc(header->cupsBytesPerLine);
    if (!line) return -1;

    size_t active_bytes = ((size_t)header->cupsWidth + 7U) / 8U;
    unsigned int trailing_bits = header->cupsWidth & 7U;
    int status = 0;
    for (uint32_t y = 0; y < geometry->content_height; y++) {
        if (cupsRasterReadPixels(raster, line, header->cupsBytesPerLine) != header->cupsBytesPerLine) {
            status = -1;
            break;
        }
        /* CUPS row padding is not image content and may be non-zero. */
        unsigned char *row = bitmap + (size_t)y * geometry->line_bytes;
        memcpy(row, line, active_bytes);
        if (trailing_bits != 0U) {
            row[active_bytes - 1U] &= (unsigned char)(0xffU << (8U - trailing_bits));
        }
        if (progress) progress(context, y, geometry->height);
    }
    free(line);

    if (status == 0 && rotate &&
        zjs_rotate_bitmap_180(bitmap, (int)header->cupsWidth, (int)geometry->content_height,
                              (int)geometry->line_bytes) != 0) {
        status = -1;
    }
    return status;
}

int p1600_render_page_progress(cups_raster_t *raster, const cups_page_header2_t *header,
                            const P1600PageGeometry *geometry, bool rotate, unsigned char *bitmap,
                            P1600LineProgress progress, void *context) {
    if (!raster || !header || !geometry || !bitmap) return -1;
    int status = geometry->kind == P1600_RASTER_GRAY
                     ? render_gray(raster, header, geometry, rotate, bitmap, progress, context)
                     : render_legacy(raster, header, geometry, rotate, bitmap, progress, context);
    for (uint32_t y = geometry->content_height; status == 0 && progress && y < geometry->height; y++) {
        progress(context, y, geometry->height);
    }
    return status;
}

int p1600_render_page(cups_raster_t *raster, const cups_page_header2_t *header,
                   const P1600PageGeometry *geometry, bool rotate, unsigned char *bitmap) {
    return p1600_render_page_progress(raster, header, geometry, rotate, bitmap, NULL, NULL);
}
