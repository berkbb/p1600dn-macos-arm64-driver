#include "zjs_guard.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jbig.h"
#include "zjs.h"

#define CHUNK_HEADER_BYTES 16U
#define ITEM_BYTES 12U
#define BIH_BYTES 20U

static int fail(char *reason, size_t size, const char *format, ...) {
    if (reason && size > 0) {
        va_list arguments;
        va_start(arguments, format);
        vsnprintf(reason, size, format, arguments);
        va_end(arguments);
    }
    return -1;
}

static uint32_t read32(const unsigned char *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint16_t read16(const unsigned char *p) {
    return (uint16_t)(p[0] << 8 | p[1]);
}

/* Lines that carry the page; the rest of VIDEO_Y is white padding. */
static uint32_t content_lines(const ZjsPageOptions *page) {
    return page->content_height != 0 && page->content_height < page->height ? page->content_height : page->height;
}

void zjs_guard_clear_padding(const ZjsPageOptions *page, unsigned char *bitmap) {
    if (!page || !bitmap || page->width == 0 || (page->bpp != 1 && page->bpp != 2)) return;
    size_t line_bytes = zjs_line_bytes(page->width, page->bpp);
    size_t used_bits = (size_t)page->width * page->bpp;
    size_t whole = used_bits / 8U;
    unsigned int partial = (unsigned int)(used_bits % 8U);
    uint32_t content = content_lines(page);
    for (uint32_t y = 0; y < content; y++) {
        unsigned char *row = bitmap + (size_t)y * line_bytes;
        size_t start = whole;
        if (partial != 0) row[start++] &= (unsigned char)(0xffU << (8U - partial));
        memset(row + start, 0, line_bytes - start);
    }
    memset(bitmap + (size_t)content * line_bytes, 0, (size_t)(page->height - content) * line_bytes);
}

typedef struct {
    uint32_t type;
    uint32_t items;
    const unsigned char *item_data;
    const unsigned char *payload;
    size_t payload_length;
} Chunk;

/* Next chunk header and its items; -1 when the bytes are not a whole chunk. */
static int next_chunk(const unsigned char *data, size_t length, size_t *position, Chunk *chunk) {
    if (length - *position < CHUNK_HEADER_BYTES) return -1;
    const unsigned char *p = data + *position;
    uint32_t size = read32(p);
    chunk->type = read32(p + 4);
    chunk->items = read32(p + 8);
    if (read16(p + 14) != ZJ_SIGNATURE || chunk->items > UINT16_MAX / ITEM_BYTES ||
        read16(p + 12) != chunk->items * ITEM_BYTES || size < CHUNK_HEADER_BYTES + chunk->items * ITEM_BYTES ||
        size > length - *position) return -1;
    chunk->item_data = p + CHUNK_HEADER_BYTES;
    chunk->payload = chunk->item_data + chunk->items * ITEM_BYTES;
    chunk->payload_length = size - CHUNK_HEADER_BYTES - chunk->items * ITEM_BYTES;
    *position += size;
    return 0;
}

static int check_page_items(const Chunk *chunk, const ZjsPageOptions *page, char *reason, size_t size) {
    const uint32_t expected[][2] = {
        {ZJI_DMPAPER, page->paper},
        {ZJI_DMCOPIES, page->copies},
        {ZJI_DMDEFAULTSOURCE, page->source},
        {ZJI_DMMEDIATYPE, page->media},
        {ZJI_NBIE, 1},
        {ZJI_RESOLUTION_X, 600},
        {ZJI_RESOLUTION_Y, 600},
        {ZJI_RASTER_X, page->width * page->bpp},
        {ZJI_RASTER_Y, page->height},
        {ZJI_VIDEO_BPP, page->bpp},
        {ZJI_VIDEO_X, page->width},
        {ZJI_VIDEO_Y, page->height},
        {ZJI_DMDUPLEX, ZJ_DMDUPLEX_AUTOMATIC},
        {ZJI_ECONOMODE, page->econo_mode ? 1U : 0U},
    };
    uint32_t count = page->auto_duplex ? 14U : 13U;
    if (chunk->type != ZJT_START_PAGE || chunk->items != count || chunk->payload_length != 0) {
        return fail(reason, size, "START_PAGE has %u items, expected %u", chunk->items, count);
    }
    for (uint32_t i = 0, e = 0; i < count; i++, e++) {
        if (!page->auto_duplex && expected[e][0] == ZJI_DMDUPLEX) e++;
        const unsigned char *item = chunk->item_data + (size_t)i * ITEM_BYTES;
        if (read32(item) != ITEM_BYTES || item[6] != ZJIT_UINT32 || item[7] != 0 ||
            read16(item + 4) != expected[e][0] || read32(item + 8) != expected[e][1]) {
            return fail(reason, size, "START_PAGE item %u is %u = %u, expected %u = %u", i + 1, read16(item + 4),
                        read32(item + 8), expected[e][0], expected[e][1]);
        }
    }
    return 0;
}

static int check_bih(const Chunk *chunk, uint32_t xd, uint32_t yd, char *reason, size_t size) {
    const unsigned char *b = chunk->payload;
    if (chunk->type != ZJT_JBIG_BIH || chunk->items != 0 || chunk->payload_length != BIH_BYTES) {
        return fail(reason, size, "the JBIG header chunk is missing");
    }
    if (b[0] != 0 || b[1] != 0 || b[2] != 1 || b[3] != 0 || read32(b + 12) != 128U || b[16] != 0 || b[17] != 0 ||
        b[18] != (JBG_ILEAVE | JBG_SMID) || b[19] != (JBG_LRLTWO | JBG_TPDON | JBG_TPBON | JBG_DPON)) {
        return fail(reason, size, "the JBIG header does not use the printer's parameters");
    }
    if (read32(b + 4) != xd || read32(b + 8) != yd) {
        return fail(reason, size, "JBIG image is %ux%u, the page is %ux%u", read32(b + 4), read32(b + 8), xd, yd);
    }
    return 0;
}

/* Feeds `length` bytes to the decoder. Bytes after the end of the image
 * are counted in *extra and must be zero (BID padding). */
static int decode_bytes(struct jbg_dec_state *decoder, const unsigned char *data, size_t length, int *status,
                        size_t *extra, char *reason, size_t size) {
    size_t position = 0;
    while (position < length) {
        if (*status == JBG_EOK) {
            if (data[position] != 0) return fail(reason, size, "data follows the end of the JBIG image");
            (*extra)++;
            position++;
            continue;
        }
        size_t consumed = 0;
        /* jbig-kit reads its input but takes a non-const pointer. */
        unsigned char *input;
        const unsigned char *source = data + position;
        memcpy(&input, &source, sizeof(input));
        *status = jbg_dec_in(decoder, input, length - position, &consumed);
        if (consumed > length - position) return fail(reason, size, "the JBIG decoder overran its input");
        position += consumed;
        if (*status != JBG_EOK && *status != JBG_EAGAIN && *status != JBG_EOK_INTR) {
            return fail(reason, size, "the JBIG data does not decode (%s)", jbg_strerror(*status));
        }
        if (consumed == 0 && *status != JBG_EOK) return fail(reason, size, "the JBIG decoder stalled");
    }
    return 0;
}

int zjs_guard_check(const unsigned char *data, size_t length, const ZjsPageOptions *page,
                    const unsigned char *bitmap, char *reason, size_t reason_size) {
    if (!data || !page || !bitmap) return fail(reason, reason_size, "nothing to check");
    if (page->width == 0 || page->height == 0 || (page->bpp != 1 && page->bpp != 2)) {
        return fail(reason, reason_size, "invalid page geometry %ux%u at %u bits", page->width, page->height, page->bpp);
    }
    if (page->height % ZJS_VIDEO_LINE_MULTIPLE != 0) {
        return fail(reason, reason_size, "VIDEO_Y %u is not a multiple of %u lines", page->height,
                    ZJS_VIDEO_LINE_MULTIPLE);
    }
    if (page->content_height > page->height || page->height - content_lines(page) >= ZJS_VIDEO_LINE_MULTIPLE) {
        return fail(reason, reason_size, "VIDEO_Y %u does not end within %u lines of the %u content lines",
                    page->height, ZJS_VIDEO_LINE_MULTIPLE, page->content_height);
    }
    size_t line_bytes = zjs_line_bytes(page->width, page->bpp);
    uint32_t xd = (uint32_t)(line_bytes * 8U);

    size_t position = 0;
    Chunk chunk;
    if (next_chunk(data, length, &position, &chunk) != 0) return fail(reason, reason_size, "START_PAGE is damaged");
    if (check_page_items(&chunk, page, reason, reason_size) != 0) return -1;
    if (next_chunk(data, length, &position, &chunk) != 0) return fail(reason, reason_size, "the JBIG header is damaged");
    if (check_bih(&chunk, xd, page->height, reason, reason_size) != 0) return -1;

    struct jbg_dec_state decoder;
    jbg_dec_init(&decoder);
    int status = JBG_EAGAIN;
    size_t extra = 0;
    int result = decode_bytes(&decoder, chunk.payload, BIH_BYTES, &status, &extra, reason, reason_size);
    size_t blocks = 0;
    size_t last_block = 0;
    while (result == 0 && position < length) {
        size_t before = position;
        if (next_chunk(data, length, &position, &chunk) != 0) {
            result = fail(reason, reason_size, "a chunk after the JBIG data is damaged");
            break;
        }
        if (chunk.type != ZJT_JBIG_BID) {
            position = before;
            break;
        }
        if (chunk.items != 0 || chunk.payload_length == 0 || chunk.payload_length > ZJ_BID_BLOCK_SIZE) {
            result = fail(reason, reason_size, "JBIG data chunk %zu carries %zu bytes", blocks + 1, chunk.payload_length);
        } else if (blocks > 0 && last_block != ZJ_BID_BLOCK_SIZE) {
            result = fail(reason, reason_size, "JBIG data chunk %zu is short but not the last", blocks);
        } else if (extra != 0) {
            result = fail(reason, reason_size, "JBIG data continues after the end of the image");
        } else {
            result = decode_bytes(&decoder, chunk.payload, chunk.payload_length, &status, &extra, reason,
                                  reason_size);
        }
        last_block = chunk.payload_length;
        blocks++;
    }
    if (result == 0 && blocks == 0) result = fail(reason, reason_size, "the page has no JBIG data");
    if (result == 0 && last_block % ZJ_BID_PADDING != 0) {
        result = fail(reason, reason_size, "the last JBIG data chunk is not padded to %u bytes", ZJ_BID_PADDING);
    }
    if (result == 0 && status != JBG_EOK) {
        result = fail(reason, reason_size, "the JBIG image ends early (%s)", jbg_strerror(status));
    }
    if (result == 0 && extra >= ZJ_BID_PADDING) {
        result = fail(reason, reason_size, "%zu bytes follow the end of the JBIG image", extra);
    }
    if (result == 0 && (jbg_dec_getwidth(&decoder) != xd || jbg_dec_getheight(&decoder) != page->height ||
                        jbg_dec_getplanes(&decoder) != 1 ||
                        jbg_dec_getsize(&decoder) != line_bytes * page->height)) {
        result = fail(reason, reason_size, "the JBIG image decodes to %lux%lu", jbg_dec_getwidth(&decoder),
                      jbg_dec_getheight(&decoder));
    }
    if (result == 0) {
        const unsigned char *image = jbg_dec_getimage(&decoder, 0);
        if (!image || memcmp(image, bitmap, line_bytes * page->height) != 0) {
            size_t line = 0;
            while (image && line < page->height &&
                   memcmp(image + line * line_bytes, bitmap + line * line_bytes, line_bytes) == 0) line++;
            result = fail(reason, reason_size, "the JBIG image differs from the page at line %zu", line);
        }
    }
    jbg_dec_free(&decoder);
    if (result != 0) return -1;

    if (next_chunk(data, length, &position, &chunk) != 0 || chunk.type != ZJT_END_JBIG || chunk.items != 0 ||
        chunk.payload_length != 0 || next_chunk(data, length, &position, &chunk) != 0 ||
        chunk.type != ZJT_END_PAGE || chunk.items != 0 || chunk.payload_length != 0) {
        return fail(reason, reason_size, "END_JBIG and END_PAGE do not close the page");
    }
    if (position != length) return fail(reason, reason_size, "%zu bytes follow END_PAGE", length - position);
    return 0;
}

int zjs_encode_page_checked(const ZjsPageOptions *page, unsigned char *bitmap,
                            unsigned char **data, size_t *length, char *reason, size_t reason_size) {
    if (reason && reason_size > 0) reason[0] = '\0';
    if (!page || !bitmap || !data || !length) return fail(reason, reason_size, "nothing to encode");
    *data = NULL;
    *length = 0;
    if (page->width == 0 || page->height % ZJS_VIDEO_LINE_MULTIPLE != 0 || page->content_height > page->height ||
        (page->bpp != 1 && page->bpp != 2)) {
        return fail(reason, reason_size, "invalid page geometry %ux%u (%u content lines) at %u bits", page->width,
                    page->height, page->content_height, page->bpp);
    }
    zjs_guard_clear_padding(page, bitmap);

    char *buffer = NULL;
    size_t size = 0;
    FILE *stream = open_memstream(&buffer, &size);
    if (!stream) return fail(reason, reason_size, "out of memory");
    int status = zjs_write_page(stream, page, bitmap);
    if (fclose(stream) != 0) status = -1;
    if (status != 0) {
        free(buffer);
        return fail(reason, reason_size, "the page could not be encoded");
    }
    if (zjs_guard_check((const unsigned char *)buffer, size, page, bitmap, reason, reason_size) != 0) {
        free(buffer);
        return -1;
    }
    *data = (unsigned char *)buffer;
    *length = size;
    return 0;
}
