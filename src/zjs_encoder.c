#include "zjs_encoder.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "jbig.h"
#include "zjs.h"

_Static_assert(sizeof(ZJ_HEADER) == 16, "ZjStream chunk headers must be 16 bytes");
_Static_assert(sizeof(ZJ_ITEM_HEADER) == 8, "ZjStream item headers must be 8 bytes");
_Static_assert(sizeof(ZJ_ITEM_UINT32) == 12, "ZjStream uint32 items must be 12 bytes");

/* PJL Universal Exit Language. */
static const char pjl_uel[] = "\033%-12345X";
/* PJL limits JOB NAME to 80 characters. */
#define PJL_JOB_NAME_MAX 80U

/* 180 degree bit-mirror lookup table */
static const unsigned char Mirror1[256] = {
    0x00, 0x80, 0x40, 0xC0, 0x20, 0xA0, 0x60, 0xE0, 0x10, 0x90, 0x50, 0xD0, 0x30, 0xB0, 0x70, 0xF0,
    0x08, 0x88, 0x48, 0xC8, 0x28, 0xA8, 0x68, 0xE8, 0x18, 0x98, 0x58, 0xD8, 0x38, 0xB8, 0x78, 0xF8,
    0x04, 0x84, 0x44, 0xC4, 0x24, 0xA4, 0x64, 0xE4, 0x14, 0x94, 0x54, 0xD4, 0x34, 0xB4, 0x74, 0xF4,
    0x0C, 0x8C, 0x4C, 0xCC, 0x2C, 0xAC, 0x6C, 0xEC, 0x1C, 0x9C, 0x5C, 0xDC, 0x3C, 0xBC, 0x7C, 0xFC,
    0x02, 0x82, 0x42, 0xC2, 0x22, 0xA2, 0x62, 0xE2, 0x12, 0x92, 0x52, 0xD2, 0x32, 0xB2, 0x72, 0xF2,
    0x0A, 0x8A, 0x4A, 0xCA, 0x2A, 0xAA, 0x6A, 0xEA, 0x1A, 0x9A, 0x5A, 0xDA, 0x3A, 0xBA, 0x7A, 0xFA,
    0x06, 0x86, 0x46, 0xC6, 0x26, 0xA6, 0x66, 0xE6, 0x16, 0x96, 0x56, 0xD6, 0x36, 0xB6, 0x76, 0xF6,
    0x0E, 0x8E, 0x4E, 0xCE, 0x2E, 0xAE, 0x6E, 0xEE, 0x1E, 0x9E, 0x5E, 0xDE, 0x3E, 0xBE, 0x7E, 0xFE,
    0x01, 0x81, 0x41, 0xC1, 0x21, 0xA1, 0x61, 0xE1, 0x11, 0x91, 0x51, 0xD1, 0x31, 0xB1, 0x71, 0xF1,
    0x09, 0x89, 0x49, 0xC9, 0x29, 0xA9, 0x69, 0xE9, 0x19, 0x99, 0x59, 0xD9, 0x39, 0xB9, 0x79, 0xF9,
    0x05, 0x85, 0x45, 0xC5, 0x25, 0xA5, 0x65, 0xE5, 0x15, 0x95, 0x55, 0xD5, 0x35, 0xB5, 0x75, 0xF5,
    0x0D, 0x8D, 0x4D, 0xCD, 0x2D, 0xAD, 0x6D, 0xED, 0x1D, 0x9D, 0x5D, 0xDD, 0x3D, 0xBD, 0x7D, 0xFD,
    0x03, 0x83, 0x43, 0xC3, 0x23, 0xA3, 0x63, 0xE3, 0x13, 0x93, 0x53, 0xD3, 0x33, 0xB3, 0x73, 0xF3,
    0x0B, 0x8B, 0x4B, 0xCB, 0x2B, 0xAB, 0x6B, 0xEB, 0x1B, 0x9B, 0x5B, 0xDB, 0x3B, 0xBB, 0x7B, 0xFB,
    0x07, 0x87, 0x47, 0xC7, 0x27, 0xA7, 0x67, 0xE7, 0x17, 0x97, 0x57, 0xD7, 0x37, 0xB7, 0x77, 0xF7,
    0x0F, 0x8F, 0x4F, 0xCF, 0x2F, 0xAF, 0x6F, 0xEF, 0x1F, 0x9F, 0x5F, 0xDF, 0x3F, 0xBF, 0x7F, 0xFF
};

static void rotate_row_180(unsigned char *dst, const unsigned char *src,
                           int active_bytes, int trailing_bits) {
    for (int i = 0; i < active_bytes; i++) {
        dst[i] = Mirror1[src[active_bytes - 1 - i]];
    }

    if (trailing_bits != 0) {
        int padding_bits = 8 - trailing_bits;
        for (int i = 0; i < active_bytes; i++) {
            unsigned int current = (unsigned int)dst[i] << padding_bits;
            unsigned int next = i + 1 < active_bytes ? dst[i + 1] >> trailing_bits : 0;
            dst[i] = (unsigned char)(current | next);
        }
    }
}

int zjs_rotate_bitmap_180(unsigned char *buf, int width, int height, int stride) {
    if (!buf || width <= 0 || height <= 0 || stride <= 0 || width > INT_MAX - 7) return -1;

    int active_bytes = (width + 7) / 8;
    if (stride < active_bytes || (size_t)stride > SIZE_MAX / (size_t)height) return -1;

    unsigned char *scratch = malloc((size_t)active_bytes);
    if (!scratch) return -1;

    int trailing_bits = width & 7;
    for (int top = 0, bottom = height - 1; top <= bottom; top++, bottom--) {
        unsigned char *top_row = buf + (size_t)top * (size_t)stride;
        unsigned char *bottom_row = buf + (size_t)bottom * (size_t)stride;

        rotate_row_180(scratch, top_row, active_bytes, trailing_bits);
        if (top != bottom) {
            rotate_row_180(top_row, bottom_row, active_bytes, trailing_bits);
            memcpy(bottom_row, scratch, (size_t)active_bytes);
            memset(bottom_row + active_bytes, 0, (size_t)(stride - active_bytes));
        } else {
            memcpy(top_row, scratch, (size_t)active_bytes);
        }
        memset(top_row + active_bytes, 0, (size_t)(stride - active_bytes));
    }

    free(scratch);
    return 0;
}

static int write_bytes(const void *buf, size_t len, FILE *fp) {
    return fwrite(buf, 1, len, fp) == len ? 0 : -1;
}

/* Chunk header: size, type, item count, item bytes, 0x5A5A (big-endian). */
static int chunk_write(uint32_t type, uint32_t items, uint32_t data_size, FILE *fp) {
    uint32_t item_bytes = items * (uint32_t)sizeof(ZJ_ITEM_UINT32);
    if (items > UINT16_MAX / sizeof(ZJ_ITEM_UINT32) ||
        data_size > UINT32_MAX - (uint32_t)sizeof(ZJ_HEADER) - item_bytes) return -1;
    ZJ_HEADER chunk;
    chunk.size = be32((uint32_t)sizeof(ZJ_HEADER) + item_bytes + data_size);
    chunk.type = be32(type);
    chunk.items = be32(items);
    chunk.reserved = be16((uint16_t)item_bytes);
    chunk.signature = be16(ZJ_SIGNATURE);
    return write_bytes(&chunk, sizeof(chunk), fp);
}

static int item_write(uint16_t item, uint32_t value, FILE *fp) {
    ZJ_ITEM_UINT32 itm;
    itm.header.size = be32(sizeof(ZJ_ITEM_UINT32));
    itm.header.item = be16(item);
    itm.header.type = ZJIT_UINT32;
    itm.header.param = 0;
    itm.value = be32(value);
    return write_bytes(&itm, sizeof(itm), fp);
}

/* Control characters would end the PJL line early; long names are cut at
 * a UTF-8 character boundary within the PJL limit. */
static void pjl_job_name(const char *title, char *name, size_t size) {
    size_t length = 0;
    if (title) {
        while (title[length] != '\0' && length < PJL_JOB_NAME_MAX && length + 1 < size) length++;
        if (title[length] != '\0') {
            while (length > 0 && ((unsigned char)title[length] & 0xc0U) == 0x80U) length--;
        }
        for (size_t i = 0; i < length; i++) {
            unsigned char c = (unsigned char)title[i];
            name[i] = (c < 0x20U || c == 0x7fU) ? ' ' : (char)c;
        }
    }
    name[length] = '\0';
}

int zjs_start_document(FILE *out, const ZjsJobOptions *job) {
    if (!out || !job) return -1;
    char name[PJL_JOB_NAME_MAX + 1];
    pjl_job_name(job->job_name, name, sizeof(name));
    int density = job->density >= 1 && job->density <= 5 ? job->density : 3;

    if (fputs(pjl_uel, out) == EOF ||
        fprintf(out,
                "@PJL USTATUS JOB=ON\r\n"
                "@PJL USTATUS PAGE=ON\r\n"
                "@PJL USTATUS DEVICE=ON\r\n"
                "@PJL INFO STATUS\r\n"
                "@PJL JOB NAME=%s\r\n"
                "@PJL SET DENSITY = %d\r\n"
                "@PJL SET JAMRECOVERY = %s\r\n"
                "@PJL ENTER LANGUAGE=ZJS\r\n",
                name, density, job->jam_recovery ? "AUTO" : "OFF") < 0) return -1;
    if (write_bytes(ZJ_STREAM_SIGNATURE, 4, out) < 0 ||
        chunk_write(ZJT_START_DOC, 1, 0, out) < 0 ||
        item_write(ZJI_DMCOLLATE, job->collate ? 1U : 0U, out) < 0) return -1;
    return ferror(out) ? -1 : 0;
}

/* JBIG output: the 20-byte BIH, then the stripe data streamed into chunks of
 * exactly ZJ_BID_BLOCK_SIZE bytes; only the last one may be shorter. */
typedef struct {
    FILE *out;
    unsigned char bih[20];
    size_t bih_len;
    unsigned char *block;
    size_t block_len;
    int failed;
} BidWriter;

static int flush_bid(BidWriter *writer, int last) {
    size_t padding = last ? (ZJ_BID_PADDING - writer->block_len % ZJ_BID_PADDING) % ZJ_BID_PADDING : 0;
    static const unsigned char zeros[ZJ_BID_PADDING] = {0};
    if (chunk_write(ZJT_JBIG_BID, 0, (uint32_t)(writer->block_len + padding), writer->out) < 0 ||
        write_bytes(writer->block, writer->block_len, writer->out) < 0 ||
        (padding && write_bytes(zeros, padding, writer->out) < 0)) return -1;
    writer->block_len = 0;
    return 0;
}

static void jbig_output(unsigned char *start, size_t len, void *context) {
    BidWriter *writer = context;
    while (!writer->failed && len > 0) {
        if (writer->bih_len < sizeof(writer->bih)) {
            size_t amount = sizeof(writer->bih) - writer->bih_len;
            if (amount > len) amount = len;
            memcpy(writer->bih + writer->bih_len, start, amount);
            writer->bih_len += amount;
            start += amount;
            len -= amount;
            if (writer->bih_len == sizeof(writer->bih) &&
                (chunk_write(ZJT_JBIG_BIH, 0, sizeof(writer->bih), writer->out) < 0 ||
                 write_bytes(writer->bih, sizeof(writer->bih), writer->out) < 0)) writer->failed = 1;
            continue;
        }
        /* A full block is written only once more data follows it. */
        if (writer->block_len == ZJ_BID_BLOCK_SIZE && flush_bid(writer, 0) < 0) {
            writer->failed = 1;
            return;
        }
        size_t amount = ZJ_BID_BLOCK_SIZE - writer->block_len;
        if (amount > len) amount = len;
        memcpy(writer->block + writer->block_len, start, amount);
        writer->block_len += amount;
        start += amount;
        len -= amount;
    }
}

int zjs_write_page(FILE *out, const ZjsPageOptions *page, const unsigned char *bitmap) {
    if (!out || !page || !bitmap || page->width == 0 || page->height == 0 ||
        page->height % ZJS_VIDEO_LINE_MULTIPLE != 0 || (page->bpp != 1 && page->bpp != 2) ||
        page->width > UINT32_MAX / 2U - 127U || page->height > INT32_MAX) return -1;
    size_t line_bytes = zjs_line_bytes(page->width, page->bpp);
    if (line_bytes > SIZE_MAX / page->height || line_bytes > UINT32_MAX / 8U) return -1;

    uint32_t items = page->auto_duplex ? 14U : 13U;
    if (chunk_write(ZJT_START_PAGE, items, 0, out) < 0 ||
        item_write(ZJI_DMPAPER, page->paper, out) < 0 ||
        item_write(ZJI_DMCOPIES, page->copies, out) < 0 ||
        item_write(ZJI_DMDEFAULTSOURCE, page->source, out) < 0 ||
        item_write(ZJI_DMMEDIATYPE, page->media, out) < 0 ||
        item_write(ZJI_NBIE, 1, out) < 0 ||
        item_write(ZJI_RESOLUTION_X, 600, out) < 0 ||
        item_write(ZJI_RESOLUTION_Y, 600, out) < 0 ||
        item_write(ZJI_RASTER_X, page->width * page->bpp, out) < 0 ||
        item_write(ZJI_RASTER_Y, page->height, out) < 0 ||
        item_write(ZJI_VIDEO_BPP, page->bpp, out) < 0 ||
        item_write(ZJI_VIDEO_X, page->width, out) < 0 ||
        item_write(ZJI_VIDEO_Y, page->height, out) < 0 ||
        (page->auto_duplex && item_write(ZJI_DMDUPLEX, ZJ_DMDUPLEX_AUTOMATIC, out) < 0) ||
        item_write(ZJI_ECONOMODE, page->econo_mode ? 1U : 0U, out) < 0) return -1;

    BidWriter writer = {.out = out, .block = malloc(ZJ_BID_BLOCK_SIZE)};
    if (!writer.block) return -1;

    /* jbig-kit reads the bitmap but takes a non-const pointer. */
    unsigned char *planes[1];
    memcpy(&planes[0], &bitmap, sizeof(planes[0]));
    struct jbg_enc_state encoder;
    jbg_enc_init(&encoder, (unsigned long)line_bytes * 8UL, page->height, 1, planes,
                 jbig_output, &writer);
    jbg_enc_options(&encoder, JBG_ILEAVE | JBG_SMID,
                    JBG_LRLTWO | JBG_TPDON | JBG_TPBON | JBG_DPON | JBG_KEEP_ZEROS,
                    128, 0, 0);
    jbg_enc_out(&encoder);
    jbg_enc_free(&encoder);

    int status = writer.failed || writer.bih_len != sizeof(writer.bih) ||
                 writer.block_len == 0 || flush_bid(&writer, 1) < 0 ? -1 : 0;
    free(writer.block);
    if (status < 0 ||
        chunk_write(ZJT_END_JBIG, 0, 0, out) < 0 ||
        chunk_write(ZJT_END_PAGE, 0, 0, out) < 0) return -1;
    return ferror(out) ? -1 : 0;
}

int zjs_write_pause(FILE *out) {
    if (!out || chunk_write(ZJT_PAUSE, 0, 0, out) < 0) return -1;
    return ferror(out) ? -1 : 0;
}

int zjs_end_job(FILE *out) {
    if (!out || chunk_write(ZJT_END_DOC, 0, 0, out) < 0 ||
        fputs(pjl_uel, out) == EOF || fputs("@PJL EOJ\r\n", out) == EOF ||
        fputs(pjl_uel, out) == EOF) return -1;
    return fflush(out) == 0 && !ferror(out) ? 0 : -1;
}

int zjs_end_status(FILE *out) {
    if (!out || fputs("@PJL USTATUSOFF\r\n", out) == EOF || fputs(pjl_uel, out) == EOF) return -1;
    return fflush(out) == 0 && !ferror(out) ? 0 : -1;
}

int zjs_end_document(FILE *out) {
    return zjs_end_job(out) == 0 && zjs_end_status(out) == 0 ? 0 : -1;
}
