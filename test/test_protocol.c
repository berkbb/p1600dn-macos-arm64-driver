/*
 * ZjStream encoder protocol tests: exact byte layout of the job header,
 * START_DOC, START_PAGE, BIH/BID chunking, page and job trailers, plus JBIG
 * round trips and bitmap rotation.
 */
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jbig.h"
#include "zjs.h"
#include "zjs_encoder.h"

typedef struct {
    unsigned char *data;
    size_t len;
} Blob;

typedef struct {
    uint32_t size;
    uint32_t type;
    uint32_t items;
    uint16_t reserved;
    const unsigned char *payload; /* items followed by data */
    size_t payload_len;
} Chunk;

static const char job_header_prefix[] =
    "\033%-12345X@PJL USTATUS JOB=ON\r\n@PJL USTATUS PAGE=ON\r\n"
    "@PJL USTATUS DEVICE=ON\r\n@PJL INFO STATUS\r\n@PJL JOB NAME=";
static const char job_trailer[] =
    "\033%-12345X@PJL EOJ\r\n\033%-12345X@PJL USTATUSOFF\r\n\033%-12345X";

static uint32_t read_be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t read_be16(const unsigned char *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static Blob read_all(FILE *fp) {
    Blob blob = {0};
    assert(fflush(fp) == 0 && fseek(fp, 0, SEEK_END) == 0);
    long size = ftell(fp);
    assert(size >= 0 && fseek(fp, 0, SEEK_SET) == 0);
    blob.len = (size_t)size;
    blob.data = malloc(blob.len ? blob.len : 1U);
    assert(blob.data != NULL);
    assert(fread(blob.data, 1, blob.len, fp) == blob.len);
    return blob;
}

static size_t find_bytes(const Blob *blob, const void *needle, size_t needle_len, size_t from) {
    for (size_t i = from; i + needle_len <= blob->len; i++) {
        if (memcmp(blob->data + i, needle, needle_len) == 0) return i;
    }
    return SIZE_MAX;
}

static Chunk chunk_at(const Blob *blob, size_t offset) {
    assert(offset + 16U <= blob->len);
    Chunk chunk;
    chunk.size = read_be32(blob->data + offset);
    chunk.type = read_be32(blob->data + offset + 4U);
    chunk.items = read_be32(blob->data + offset + 8U);
    chunk.reserved = read_be16(blob->data + offset + 12U);
    assert(read_be16(blob->data + offset + 14U) == ZJ_SIGNATURE);
    assert(chunk.size >= 16U && offset + chunk.size <= blob->len);
    chunk.payload = blob->data + offset + 16U;
    chunk.payload_len = chunk.size - 16U;
    return chunk;
}

static uint32_t item_value(const Chunk *chunk, uint32_t index, uint16_t *id) {
    const unsigned char *item = chunk->payload + 12U * index;
    assert(read_be32(item) == 12U && item[6] == ZJIT_UINT32 && item[7] == 0);
    *id = read_be16(item + 4);
    return read_be32(item + 8);
}

static ZjsPageOptions page_options(uint32_t width, uint32_t height, uint32_t bpp) {
    ZjsPageOptions page = {
        .paper = DMPAPER_A4, .copies = 1, .source = DMBIN_MANUAL, .media = DMMEDIA_PLAIN,
        .width = width, .height = height, .bpp = bpp,
    };
    return page;
}

static unsigned char *make_bitmap(const ZjsPageOptions *page, unsigned int seed, int density) {
    size_t line_bytes = zjs_line_bytes(page->width, page->bpp);
    unsigned char *bitmap = calloc(page->height, line_bytes);
    assert(bitmap != NULL);
    uint32_t bits = page->width * page->bpp;
    for (uint32_t y = 0; y < page->height; y++) {
        for (uint32_t x = 0; x < bits; x++) {
            seed = seed * 1103515245U + 12345U;
            if ((int)((seed >> 16) % 100U) < density) {
                bitmap[(size_t)y * line_bytes + x / 8U] |= (unsigned char)(0x80U >> (x % 8U));
            }
        }
    }
    return bitmap;
}

/* Page chunk walk: START_PAGE .. END_PAGE starting at `offset`; returns the
 * offset after END_PAGE and the decoded BIE (BIH + BID data with padding). */
static size_t walk_page(const Blob *blob, size_t offset, Chunk *start_page, Blob *bie,
                        size_t *bid_count, size_t *last_bid) {
    *start_page = chunk_at(blob, offset);
    assert(start_page->type == ZJT_START_PAGE);
    offset += start_page->size;

    Chunk bih = chunk_at(blob, offset);
    assert(bih.type == ZJT_JBIG_BIH && bih.items == 0 && bih.reserved == 0 && bih.payload_len == 20U);
    bie->len = 0;
    bie->data = malloc(20U);
    assert(bie->data != NULL);
    memcpy(bie->data, bih.payload, 20U);
    bie->len = 20U;
    offset += bih.size;

    *bid_count = 0;
    for (;;) {
        Chunk chunk = chunk_at(blob, offset);
        offset += chunk.size;
        if (chunk.type == ZJT_END_JBIG) {
            assert(chunk.size == 16U && chunk.items == 0 && chunk.reserved == 0);
            break;
        }
        assert(chunk.type == ZJT_JBIG_BID && chunk.items == 0 && chunk.reserved == 0);
        assert(chunk.payload_len > 0);
        if (*bid_count > 0) assert(*last_bid == ZJ_BID_BLOCK_SIZE);
        *last_bid = chunk.payload_len;
        (*bid_count)++;
        unsigned char *grown = realloc(bie->data, bie->len + chunk.payload_len);
        assert(grown != NULL);
        bie->data = grown;
        memcpy(bie->data + bie->len, chunk.payload, chunk.payload_len);
        bie->len += chunk.payload_len;
    }
    assert(*bid_count > 0 && *last_bid <= ZJ_BID_BLOCK_SIZE && *last_bid % ZJ_BID_PADDING == 0U);

    Chunk end_page = chunk_at(blob, offset);
    assert(end_page.type == ZJT_END_PAGE && end_page.size == 16U && end_page.items == 0 &&
           end_page.reserved == 0);
    return offset + end_page.size;
}

static void decode_and_compare(const Blob *bie, const unsigned char *expected,
                               unsigned long width, unsigned long height) {
    struct jbg_dec_state decoder;
    jbg_dec_init(&decoder);
    size_t consumed = 0;
    int status = jbg_dec_in(&decoder, bie->data, bie->len, &consumed);
    assert(status == JBG_EOK || status == JBG_EOK_INTR);
    assert(jbg_dec_getwidth(&decoder) == width && jbg_dec_getheight(&decoder) == height);
    assert(jbg_dec_getplanes(&decoder) == 1);
    assert(jbg_dec_getsize(&decoder) == (width + 7U) / 8U * height);
    assert(memcmp(jbg_dec_getimage(&decoder, 0), expected, jbg_dec_getsize(&decoder)) == 0);
    jbg_dec_free(&decoder);
}

static Blob encode_job(const ZjsJobOptions *job, const ZjsPageOptions *pages,
                       unsigned char *const *bitmaps, size_t count, int pause_after) {
    FILE *fp = tmpfile();
    assert(fp != NULL);
    assert(zjs_start_document(fp, job) == 0);
    for (size_t i = 0; i < count; i++) {
        assert(zjs_write_page(fp, &pages[i], bitmaps[i]) == 0);
        if ((int)i == pause_after) assert(zjs_write_pause(fp) == 0);
    }
    assert(zjs_end_document(fp) == 0);
    Blob blob = read_all(fp);
    fclose(fp);
    return blob;
}

static void test_job_framing(void) {
    ZjsJobOptions job = {.job_name = "Report.pdf", .density = 3};
    ZjsPageOptions page = page_options(64, 12, 2);
    unsigned char *bitmap = make_bitmap(&page, 1, 10);
    Blob blob = encode_job(&job, &page, &bitmap, 1, -1);

    static const char expected_header[] =
        "\033%-12345X@PJL USTATUS JOB=ON\r\n@PJL USTATUS PAGE=ON\r\n"
        "@PJL USTATUS DEVICE=ON\r\n@PJL INFO STATUS\r\n@PJL JOB NAME=Report.pdf\r\n"
        "@PJL SET DENSITY = 3\r\n@PJL SET JAMRECOVERY = OFF\r\n@PJL ENTER LANGUAGE=ZJS\r\nJZJZ";
    size_t header_len = sizeof(expected_header) - 1U;
    assert(blob.len > header_len && memcmp(blob.data, expected_header, header_len) == 0);

    /* START_DOC: exactly one DMCOLLATE item (28 bytes). */
    static const unsigned char start_doc[] = {
        0, 0, 0, 28, 0, 0, 0, ZJT_START_DOC, 0, 0, 0, 1, 0, 12, 0x5a, 0x5a,
        0, 0, 0, 12, 0, ZJI_DMCOLLATE, ZJIT_UINT32, 0, 0, 0, 0, 0,
    };
    assert(memcmp(blob.data + header_len, start_doc, sizeof(start_doc)) == 0);

    /* END_DOC followed by the PJL trailer ends the job. */
    static const unsigned char end_doc[] = {0, 0, 0, 16, 0, 0, 0, ZJT_END_DOC, 0, 0, 0, 0, 0, 0, 0x5a, 0x5a};
    size_t trailer_len = sizeof(job_trailer) - 1U;
    assert(memcmp(blob.data + blob.len - trailer_len, job_trailer, trailer_len) == 0);
    assert(memcmp(blob.data + blob.len - trailer_len - sizeof(end_doc), end_doc, sizeof(end_doc)) == 0);
    free(blob.data);

    /* Collate, density, jam recovery. */
    job.collate = true;
    job.density = 5;
    job.jam_recovery = true;
    blob = encode_job(&job, &page, &bitmap, 1, -1);
    static const char pjl_settings[] = "@PJL SET DENSITY = 5\r\n@PJL SET JAMRECOVERY = AUTO\r\n";
    assert(find_bytes(&blob, pjl_settings, strlen(pjl_settings), 0) != SIZE_MAX);
    size_t doc = find_bytes(&blob, "JZJZ", 4, 0) + 4U;
    assert(read_be32(blob.data + doc + 24U) == 1U);
    free(blob.data);

    for (int density = -1; density <= 7; density++) {
        job.density = density;
        blob = encode_job(&job, &page, &bitmap, 1, -1);
        char line[32];
        snprintf(line, sizeof(line), "DENSITY = %d\r\n", density >= 1 && density <= 5 ? density : 3);
        assert(find_bytes(&blob, line, strlen(line), 0) != SIZE_MAX);
        free(blob.data);
    }
    free(bitmap);
    puts("    [PASS] PJL Job Header, START_DOC, END_DOC and PJL Trailer Bytes");
}

static void expect_job_name(const char *title, const char *expected) {
    ZjsJobOptions job = {.job_name = title, .density = 3};
    FILE *fp = tmpfile();
    assert(fp != NULL && zjs_start_document(fp, &job) == 0);
    Blob blob = read_all(fp);
    fclose(fp);
    size_t prefix = sizeof(job_header_prefix) - 1U;
    assert(memcmp(blob.data, job_header_prefix, prefix) == 0);
    static const char next_line[] = "\r\n@PJL SET DENSITY";
    size_t end = find_bytes(&blob, next_line, strlen(next_line), prefix);
    assert(end != SIZE_MAX && end - prefix == strlen(expected));
    assert(memcmp(blob.data + prefix, expected, strlen(expected)) == 0);
    free(blob.data);
}

static void test_job_name_sanitizing(void) {
    expect_job_name("Rapor", "Rapor");
    expect_job_name(NULL, "");
    expect_job_name("a\r\n@PJL EOJ\tb\x7f", "a  @PJL EOJ b ");
    char long_name[200];
    memset(long_name, 'x', sizeof(long_name) - 1U);
    long_name[sizeof(long_name) - 1U] = '\0';
    char expected[81];
    memset(expected, 'x', 80);
    expected[80] = '\0';
    expect_job_name(long_name, expected);
    /* 79 ASCII bytes + a two-byte UTF-8 letter: cut before the letter. */
    char utf8[100];
    memset(utf8, 'y', 79);
    memcpy(utf8 + 79, "\xc5\x9fz", 4);
    char utf8_expected[80];
    memset(utf8_expected, 'y', 79);
    utf8_expected[79] = '\0';
    expect_job_name(utf8, utf8_expected);
    expect_job_name("Ba\xc5\x9fvuru \xc4\xb0", "Ba\xc5\x9fvuru \xc4\xb0");
    puts("    [PASS] PJL JOB NAME Control-Character and 80-Byte UTF-8 Truncation");
}

static void test_page_items(void) {
    const uint32_t widths[] = {1, 63, 64, 4758, 4759, 5100};
    for (size_t w = 0; w < sizeof(widths) / sizeof(widths[0]); w++) {
        for (uint32_t bpp = 1; bpp <= 2; bpp++) {
            for (int variant = 0; variant < 4; variant++) {
                ZjsPageOptions page = page_options(widths[w], 24, bpp);
                page.auto_duplex = (variant & 1) != 0;
                page.econo_mode = (variant & 2) != 0;
                page.paper = 41;
                page.copies = 3;
                page.source = 7;
                page.media = 282;
                unsigned char *bitmap = make_bitmap(&page, (unsigned int)(widths[w] + bpp), 30);
                ZjsJobOptions job = {.job_name = "t", .density = 3};
                Blob blob = encode_job(&job, &page, &bitmap, 1, -1);
                size_t offset = find_bytes(&blob, "JZJZ", 4, 0) + 4U + 28U;

                Chunk start;
                Blob bie;
                size_t bids, last;
                walk_page(&blob, offset, &start, &bie, &bids, &last);
                uint32_t count = page.auto_duplex ? 14U : 13U;
                assert(start.items == count && start.reserved == 12U * count && start.size == 16U + 12U * count);

                const uint16_t ids[] = {ZJI_DMPAPER, ZJI_DMCOPIES, ZJI_DMDEFAULTSOURCE, ZJI_DMMEDIATYPE, ZJI_NBIE,
                                        ZJI_RESOLUTION_X, ZJI_RESOLUTION_Y, ZJI_RASTER_X, ZJI_RASTER_Y,
                                        ZJI_VIDEO_BPP, ZJI_VIDEO_X, ZJI_VIDEO_Y};
                const uint32_t values[] = {41, 3, 7, 282, 1, 600, 600, widths[w] * bpp, 24, bpp, widths[w], 24};
                for (uint32_t i = 0; i < 12; i++) {
                    uint16_t id = 0;
                    assert(item_value(&start, i, &id) == values[i] && id == ids[i]);
                }
                uint16_t id = 0;
                uint32_t index = 12;
                if (page.auto_duplex) {
                    assert(item_value(&start, index++, &id) == ZJ_DMDUPLEX_AUTOMATIC && id == ZJI_DMDUPLEX);
                }
                assert(item_value(&start, index, &id) == (page.econo_mode ? 1U : 0U) && id == ZJI_ECONOMODE);

                /* BIH: Xd = 128-bit aligned line, Yd, L0 128, MX 0, MY 0, order 3, options 0x5c. */
                unsigned long xd = (unsigned long)zjs_line_bytes(widths[w], bpp) * 8UL;
                assert(bie.data[0] == 0 && bie.data[1] == 0 && bie.data[2] == 1 && bie.data[3] == 0);
                assert(read_be32(bie.data + 4) == xd && read_be32(bie.data + 8) == 24U);
                assert(read_be32(bie.data + 12) == 128U);
                assert(bie.data[16] == 0 && bie.data[17] == 0 && bie.data[18] == 3 && bie.data[19] == 0x5c);
                decode_and_compare(&bie, bitmap, xd, 24);
                free(bie.data);
                free(blob.data);
                free(bitmap);
            }
        }
    }
    puts("    [PASS] START_PAGE Item Order/Values, BIH Fields and JBIG Round Trip (48 pages)");
}

static void test_bid_blocking(void) {
    /* Dense noise compresses poorly, so the stripe data spans several blocks. */
    ZjsPageOptions page = page_options(4800, 1200, 2);
    unsigned char *bitmap = make_bitmap(&page, 0x1606U, 50);
    ZjsJobOptions job = {.job_name = "noise", .density = 3};
    Blob blob = encode_job(&job, &page, &bitmap, 1, -1);
    Chunk start;
    Blob bie;
    size_t bids = 0, last = 0;
    walk_page(&blob, find_bytes(&blob, "JZJZ", 4, 0) + 4U + 28U, &start, &bie, &bids, &last);
    assert(bids >= 3);
    decode_and_compare(&bie, bitmap, zjs_line_bytes(4800, 2) * 8U, 1200);
    free(bie.data);
    free(blob.data);
    free(bitmap);
    printf("    [PASS] 64 KiB BID Blocking and Pad-to-4 Final Chunk (%zu BID chunks)\n", bids);
}

static void test_keep_stripe_zeros(void) {
    /* The output keeps the coder's trailing zero bytes before each SDNORM marker;
     * jbig-kit strips them by default. Both must decode identically. */
    ZjsPageOptions page = page_options(64, 600, 2);
    size_t line_bytes = zjs_line_bytes(page.width, page.bpp);
    unsigned char *bitmap = calloc(page.height, line_bytes);
    assert(bitmap != NULL);
    for (uint32_t y = 0; y < page.height; y++) {
        memset(bitmap + y * line_bytes, (y & 1) ? 0x55 : 0xaa, line_bytes);
    }
    ZjsJobOptions job = {.job_name = "pattern", .density = 3};
    Blob blob = encode_job(&job, &page, &bitmap, 1, -1);
    Chunk start;
    Blob bie;
    size_t bids = 0, last = 0;
    walk_page(&blob, find_bytes(&blob, "JZJZ", 4, 0) + 4U + 28U, &start, &bie, &bids, &last);
    size_t zero_before_marker = 0;
    for (size_t i = 21; i + 1 < bie.len; i++) {
        if (bie.data[i] == 0xff && bie.data[i + 1] == 0x02 && bie.data[i - 1] == 0x00) zero_before_marker++;
    }
    assert(zero_before_marker > 0);
    decode_and_compare(&bie, bitmap, line_bytes * 8U, page.height);
    free(bie.data);
    free(blob.data);
    free(bitmap);
    puts("    [PASS] Trailing Stripe Zero Bytes Kept Before SDNORM");
}

static void test_multi_page_and_pause(void) {
    ZjsPageOptions pages[3] = {page_options(100, 12, 2), page_options(200, 36, 1), page_options(4758, 6828, 2)};
    unsigned char *bitmaps[3];
    for (int i = 0; i < 3; i++) bitmaps[i] = make_bitmap(&pages[i], (unsigned int)i + 7U, i == 2 ? 1 : 20);
    ZjsJobOptions job = {.job_name = "multi", .density = 3};
    Blob blob = encode_job(&job, pages, bitmaps, 3, 1);
    size_t offset = find_bytes(&blob, "JZJZ", 4, 0) + 4U + 28U;
    for (int i = 0; i < 3; i++) {
        Chunk start;
        Blob bie;
        size_t bids = 0, last = 0;
        offset = walk_page(&blob, offset, &start, &bie, &bids, &last);
        decode_and_compare(&bie, bitmaps[i], zjs_line_bytes(pages[i].width, pages[i].bpp) * 8U, pages[i].height);
        free(bie.data);
        if (i == 1) {
            static const unsigned char pause[] = {0, 0, 0, 16, 0, 0, 0, ZJT_PAUSE, 0, 0, 0, 0, 0, 0, 0x5a, 0x5a};
            assert(memcmp(blob.data + offset, pause, sizeof(pause)) == 0);
            offset += sizeof(pause);
        }
    }
    assert(chunk_at(&blob, offset).type == ZJT_END_DOC);
    assert(offset + 16U + sizeof(job_trailer) - 1U == blob.len);
    for (int i = 0; i < 3; i++) free(bitmaps[i]);
    free(blob.data);
    puts("    [PASS] Multi-Page Job with Manual-Duplex PAUSE Chunk and A4 6828-Line Page");
}

static void test_invalid_arguments(void) {
    ZjsPageOptions page = page_options(64, 12, 2);
    unsigned char *bitmap = make_bitmap(&page, 3, 10);
    FILE *fp = tmpfile();
    assert(fp != NULL);
    ZjsJobOptions job = {.job_name = "x", .density = 3};
    assert(zjs_start_document(NULL, &job) == -1);
    assert(zjs_start_document(fp, NULL) == -1);
    assert(zjs_write_page(NULL, &page, bitmap) == -1);
    assert(zjs_write_page(fp, NULL, bitmap) == -1);
    assert(zjs_write_page(fp, &page, NULL) == -1);
    ZjsPageOptions bad = page;
    bad.width = 0;
    assert(zjs_write_page(fp, &bad, bitmap) == -1);
    bad = page;
    bad.height = 13;
    assert(zjs_write_page(fp, &bad, bitmap) == -1);
    bad = page;
    bad.height = 0;
    assert(zjs_write_page(fp, &bad, bitmap) == -1);
    bad = page;
    bad.bpp = 3;
    assert(zjs_write_page(fp, &bad, bitmap) == -1);
    bad = page;
    bad.width = UINT32_MAX;
    assert(zjs_write_page(fp, &bad, bitmap) == -1);
    assert(ftell(fp) == 0);
    assert(zjs_write_pause(NULL) == -1);
    assert(zjs_end_document(NULL) == -1);
    fclose(fp);

    FILE *read_only = fopen("/dev/null", "r");
    assert(read_only != NULL);
    assert(zjs_start_document(read_only, &job) == -1);
    assert(zjs_write_page(read_only, &page, bitmap) == -1);
    assert(zjs_end_document(read_only) == -1);
    fclose(read_only);
    free(bitmap);

    assert(zjs_video_height(1) == 12U && zjs_video_height(12) == 12U && zjs_video_height(13) == 24U);
    assert(zjs_video_height(6817) == 6828U && zjs_video_height(6816) == 6816U);
    assert(zjs_line_bytes(4758, 2) == 1200U && zjs_line_bytes(4758, 1) == 608U && zjs_line_bytes(1, 1) == 16U);
    puts("    [PASS] Invalid Argument, Unwritable Stream and Geometry Helper Matrix");
}

static void test_rotation_exhaustive(void) {
    for (int width = 1; width <= 257; width++) {
        for (int height = 1; height <= 7; height++) {
            int stride = (width + 7) / 8 + (width % 5);
            size_t data_size = (size_t)stride * (size_t)height;
            unsigned char *data = malloc(data_size);
            unsigned char *original = malloc(data_size);
            assert(data != NULL && original != NULL);
            for (size_t i = 0; i < data_size; i++) data[i] = (unsigned char)(i * 37U + (size_t)width);
            /* Clear the padding (beyond width) first: rotation keeps it zero. */
            for (int y = 0; y < height; y++) {
                unsigned char *row = data + (size_t)y * (size_t)stride;
                for (int x = width; x < stride * 8; x++) row[x / 8] &= (unsigned char)~(0x80U >> (x % 8));
            }
            memcpy(original, data, data_size);
            assert(zjs_rotate_bitmap_180(data, width, height, stride) == 0);
            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) {
                    int from = (original[(size_t)(height - 1 - y) * (size_t)stride + (size_t)(width - 1 - x) / 8U] >>
                                (7 - (width - 1 - x) % 8)) & 1;
                    int to = (data[(size_t)y * (size_t)stride + (size_t)x / 8U] >> (7 - x % 8)) & 1;
                    assert(from == to);
                }
            }
            assert(zjs_rotate_bitmap_180(data, width, height, stride) == 0);
            assert(memcmp(data, original, data_size) == 0);
            free(data);
            free(original);
        }
    }
    unsigned char byte = 0;
    assert(zjs_rotate_bitmap_180(NULL, 1, 1, 1) == -1);
    assert(zjs_rotate_bitmap_180(&byte, 0, 1, 1) == -1);
    assert(zjs_rotate_bitmap_180(&byte, 1, 0, 1) == -1);
    assert(zjs_rotate_bitmap_180(&byte, 9, 1, 1) == -1);
    assert(zjs_rotate_bitmap_180(&byte, INT_MAX, 1, 1) == -1);
    puts("    [PASS] Exhaustive Bit-Exact 180-Degree Bitmap Rotation (widths 1..257)");
}

int main(void) {
    puts("[*] Starting ZjStream protocol tests...");
    test_job_framing();
    test_job_name_sanitizing();
    test_page_items();
    test_bid_blocking();
    test_keep_stripe_zeros();
    test_multi_page_and_pause();
    test_invalid_arguments();
    test_rotation_exhaustive();
    puts("[+] ZjStream protocol tests passed.");
    return 0;
}
