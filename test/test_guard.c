/*
 * Tests for the page guard (src/zjs_guard.c): every geometry the filter can
 * produce passes with its padding forced white, and every way a page can
 * disagree with what the printer expects is caught before it is sent. The
 * black stripes the P1606dn printed across the bottom of pages came from such
 * disagreements (JBIG Yd padded past VIDEO_Y, VIDEO_Y not a 12-line multiple,
 * odd RASTER_X); each of them is reproduced here and must be refused.
 *
 * Sayfa koruması testleri: çizgi hatasına yol açan her uyumsuzluk yakalanmalı.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "p1600_encoder_pool.h"
#include "jbig.h"
#include "zjs.h"
#include "zjs_encoder.h"
#include "zjs_guard.h"

typedef struct {
    unsigned char *data;
    size_t length;
} Blob;

static uint32_t read32(const unsigned char *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void write32(unsigned char *p, uint32_t value) {
    p[0] = (unsigned char)(value >> 24);
    p[1] = (unsigned char)(value >> 16);
    p[2] = (unsigned char)(value >> 8);
    p[3] = (unsigned char)value;
}

static uint32_t next_random(uint32_t *state) {
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return *state;
}

static ZjsPageOptions page_options(uint32_t width, uint32_t content_height, uint32_t bpp, bool auto_duplex) {
    return (ZjsPageOptions){
        .paper = 9,
        .copies = 1,
        .source = 7,
        .media = 1,
        .width = width,
        .height = zjs_video_height(content_height),
        .content_height = content_height,
        .bpp = bpp,
        .auto_duplex = auto_duplex,
    };
}

/* Content with `density` in 256 black bits, the last content line black,
 * and garbage everywhere the printer must see white. */
static unsigned char *make_bitmap(const ZjsPageOptions *page, uint32_t seed, unsigned int density) {
    size_t line_bytes = zjs_line_bytes(page->width, page->bpp);
    unsigned char *bitmap = malloc(line_bytes * page->height);
    assert(bitmap != NULL);
    uint32_t state = seed | 1U;
    for (size_t i = 0; i < line_bytes * page->height; i++) {
        unsigned char value = 0;
        for (int bit = 0; bit < 8; bit++) {
            if ((next_random(&state) & 0xffU) < density) value |= (unsigned char)(0x80U >> bit);
        }
        bitmap[i] = value;
    }
    memset(bitmap + (size_t)(page->content_height - 1U) * line_bytes, 0xff, line_bytes);
    memset(bitmap + (size_t)page->content_height * line_bytes, 0xff,
           (size_t)(page->height - page->content_height) * line_bytes);
    return bitmap;
}

static int bit_at(const unsigned char *row, size_t bit) {
    return (row[bit / 8U] >> (7U - bit % 8U)) & 1U;
}

/* Bits a page may carry: the content lines, up to width * bpp. */
static void expect_white_padding(const ZjsPageOptions *page, const unsigned char *bitmap) {
    size_t line_bytes = zjs_line_bytes(page->width, page->bpp);
    size_t used_bits = (size_t)page->width * page->bpp;
    for (uint32_t y = 0; y < page->height; y++) {
        const unsigned char *row = bitmap + (size_t)y * line_bytes;
        for (size_t bit = y < page->content_height ? used_bits : 0; bit < line_bytes * 8U; bit++) {
            assert(bit_at(row, bit) == 0);
        }
    }
}

/* The page's JBIG image, decoded independently of the guard. */
static void expect_decodes_to(const Blob *page_bytes, const ZjsPageOptions *page, const unsigned char *bitmap) {
    size_t line_bytes = zjs_line_bytes(page->width, page->bpp);
    struct jbg_dec_state decoder;
    jbg_dec_init(&decoder);
    size_t position = 16U + (page->auto_duplex ? 14U : 13U) * 12U;
    int status = JBG_EAGAIN;
    while (position + 16U <= page_bytes->length) {
        uint32_t size = read32(page_bytes->data + position);
        uint32_t type = read32(page_bytes->data + position + 4U);
        if (type == ZJT_JBIG_BIH) {
            const unsigned char *bih = page_bytes->data + position + 16U;
            assert(read32(bih + 4) == line_bytes * 8U && read32(bih + 8) == page->height);
        }
        if ((type == ZJT_JBIG_BIH || type == ZJT_JBIG_BID) && status != JBG_EOK) {
            size_t consumed = 0;
            status = jbg_dec_in(&decoder, page_bytes->data + position + 16U, size - 16U, &consumed);
            assert(status == JBG_EOK || status == JBG_EAGAIN);
        }
        position += size;
    }
    assert(status == JBG_EOK);
    assert(jbg_dec_getheight(&decoder) == page->height && jbg_dec_getwidth(&decoder) == line_bytes * 8U);
    assert(memcmp(jbg_dec_getimage(&decoder, 0), bitmap, line_bytes * page->height) == 0);
    jbg_dec_free(&decoder);
}

static Blob encode_checked(const ZjsPageOptions *page, unsigned char *bitmap) {
    Blob blob = {0};
    char reason[192] = "";
    int status = zjs_encode_page_checked(page, bitmap, &blob.data, &blob.length, reason, sizeof(reason));
    if (status != 0) fprintf(stderr, "page %ux%u (%u lines) bpp %u refused: %s\n", page->width, page->height,
                             page->content_height, page->bpp, reason);
    assert(status == 0 && blob.data != NULL && blob.length > 0);
    return blob;
}

static void expect_refused(const Blob *blob, const ZjsPageOptions *page, const unsigned char *bitmap,
                           const char *needle) {
    char reason[192] = "";
    assert(zjs_guard_check(blob->data, blob->length, page, bitmap, reason, sizeof(reason)) == -1);
    if (!strstr(reason, needle)) {
        fprintf(stderr, "expected a reason with \"%s\", got \"%s\"\n", needle, reason);
        assert(0 && "wrong guard reason");
    }
}

/* Every residue of the width (bytes, 128-bit JBIG lines) and of the height
 * (12-line VIDEO_Y groups, 128-line JBIG stripes), both resolutions, simplex
 * and automatic duplex, real page sizes included. */
static void test_geometry_sweep(void) {
    static const uint32_t widths[] = {1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 63, 64, 65, 127, 128, 129, 255, 256, 257, 1001};
    static const uint32_t heights[] = {1, 2, 11, 12, 13, 23, 24, 25, 117, 127, 128, 129, 131, 140, 255, 256, 257, 383, 385};
    size_t pages = 0;
    for (size_t w = 0; w < sizeof(widths) / sizeof(widths[0]); w++) {
        for (size_t h = 0; h < sizeof(heights) / sizeof(heights[0]); h++) {
            for (uint32_t bpp = 1; bpp <= 2; bpp++) {
                ZjsPageOptions page = page_options(widths[w], heights[h], bpp, (w + h) % 2U == 0);
                unsigned char *bitmap = make_bitmap(&page, (uint32_t)(w * 131U + h * 7U + bpp), (w + h) % 5U * 60U);
                Blob blob = encode_checked(&page, bitmap);
                expect_white_padding(&page, bitmap);
                expect_decodes_to(&blob, &page, bitmap);
                free(blob.data);
                free(bitmap);
                pages++;
            }
        }
    }
    /* Full pages as cgpdftoraster renders them (imageable area at 600 dpi):
     * A4 4758 x 6817, Letter 4900 x 6400, Legal 4900 x 8200, A5 3310 x 4762. */
    static const uint32_t full[][2] = {{4758, 6817}, {4900, 6400}, {4900, 8200}, {3310, 4762}, {4959, 7017}};
    for (size_t i = 0; i < sizeof(full) / sizeof(full[0]); i++) {
        for (uint32_t bpp = 1; bpp <= 2; bpp++) {
            ZjsPageOptions page = page_options(full[i][0], full[i][1], bpp, i % 2U == 0);
            unsigned char *bitmap = make_bitmap(&page, (uint32_t)(i + 99U), 3U);
            Blob blob = encode_checked(&page, bitmap);
            expect_white_padding(&page, bitmap);
            expect_decodes_to(&blob, &page, bitmap);
            free(blob.data);
            free(bitmap);
            pages++;
        }
    }
    printf("    [PASS] Page Guard Geometry Sweep (%zu pages: every width and height residue, 600 dpi and FastRes)\n",
           pages);
}

/* Black ink below the content and right of RASTER_X never reaches the
 * printer, and the content itself is untouched. */
static void test_padding_forced_white(void) {
    ZjsPageOptions page = page_options(37, 6817, 2, false);
    size_t line_bytes = zjs_line_bytes(page.width, page.bpp);
    unsigned char *bitmap = malloc(line_bytes * page.height);
    assert(bitmap != NULL);
    memset(bitmap, 0xff, line_bytes * page.height);
    Blob blob = encode_checked(&page, bitmap);
    expect_white_padding(&page, bitmap);
    for (uint32_t y = 0; y < page.content_height; y++) {
        const unsigned char *row = bitmap + (size_t)y * line_bytes;
        for (size_t bit = 0; bit < (size_t)page.width * page.bpp; bit++) assert(bit_at(row, bit) == 1);
    }
    expect_decodes_to(&blob, &page, bitmap);
    free(blob.data);
    free(bitmap);
    printf("    [PASS] Page Guard White Padding Test (11 bottom lines and the right edge forced white)\n");
}

/* Offsets inside one encoded page. */
typedef struct {
    size_t bih;          /* BIH chunk */
    size_t first_bid;
    size_t last_bid;
    size_t end_jbig;
    size_t bids;
} Layout;

static Layout layout(const Blob *blob) {
    Layout l = {0};
    size_t position = 0;
    while (position + 16U <= blob->length) {
        uint32_t type = read32(blob->data + position + 4U);
        if (type == ZJT_JBIG_BIH) l.bih = position;
        if (type == ZJT_JBIG_BID) {
            if (l.bids++ == 0) l.first_bid = position;
            l.last_bid = position;
        }
        if (type == ZJT_END_JBIG) l.end_jbig = position;
        position += read32(blob->data + position);
    }
    assert(position == blob->length && l.bih > 0 && l.bids > 0 && l.end_jbig > l.last_bid);
    return l;
}

static Blob copy_blob(const Blob *blob, size_t extra) {
    Blob copy = {malloc(blob->length + extra), blob->length};
    assert(copy.data != NULL);
    memcpy(copy.data, blob->data, blob->length);
    return copy;
}

/* `bytes` more bytes in the last BID chunk (value `fill`), before END_JBIG. */
static Blob grow_last_bid(const Blob *blob, const Layout *l, size_t bytes, unsigned char fill) {
    Blob grown = copy_blob(blob, bytes);
    memmove(grown.data + l->end_jbig + bytes, grown.data + l->end_jbig, blob->length - l->end_jbig);
    memset(grown.data + l->end_jbig, fill, bytes);
    write32(grown.data + l->last_bid, read32(grown.data + l->last_bid) + (uint32_t)bytes);
    grown.length += bytes;
    return grown;
}

static void test_stripe_defects_refused(void) {
    ZjsPageOptions page = page_options(700, 6817, 2, false);
    unsigned char *bitmap = make_bitmap(&page, 7U, 128U);
    Blob good = encode_checked(&page, bitmap);
    Layout l = layout(&good);
    assert(l.bids >= 2);
    assert(zjs_guard_check(good.data, good.length, &page, bitmap, NULL, 0) == 0);

    /* The original defect: JBIG height rounded up to 128 lines past VIDEO_Y. */
    Blob bad = copy_blob(&good, 0);
    write32(bad.data + l.bih + 16U + 8U, (page.height + 127U) & ~127U);
    expect_refused(&bad, &page, bitmap, "JBIG image is");
    free(bad.data);

    /* VIDEO_Y that is not a 12-line multiple (A4 6817 instead of 6828). */
    ZjsPageOptions uneven = page;
    uneven.height = 6817;
    expect_refused(&good, &uneven, bitmap, "not a multiple of 12");
    unsigned char *data = NULL;
    size_t length = 0;
    char reason[192];
    assert(zjs_encode_page_checked(&uneven, bitmap, &data, &length, reason, sizeof(reason)) == -1 && !data);

    /* VIDEO_Y further than 12 lines past the content. */
    ZjsPageOptions tall = page;
    tall.content_height = page.height - 12U;
    expect_refused(&good, &tall, bitmap, "does not end within 12 lines");

    /* Odd RASTER_X (9517 for VIDEO_X 4759), and VIDEO_Y changed in the header. */
    bad = copy_blob(&good, 0);
    write32(bad.data + 16U + 7U * 12U + 8U, page.width * page.bpp - 1U);
    expect_refused(&bad, &page, bitmap, "START_PAGE item 8");
    free(bad.data);
    bad = copy_blob(&good, 0);
    write32(bad.data + 16U + 11U * 12U + 8U, page.height + 12U);
    expect_refused(&bad, &page, bitmap, "START_PAGE item 12");
    free(bad.data);

    /* An automatic-duplex page without DMDUPLEX. */
    ZjsPageOptions duplex = page;
    duplex.auto_duplex = true;
    expect_refused(&good, &duplex, bitmap, "START_PAGE has 13 items, expected 14");

    /* JBIG parameters other than the ones the printer requires. */
    bad = copy_blob(&good, 0);
    write32(bad.data + l.bih + 16U + 12U, 64U);
    expect_refused(&bad, &page, bitmap, "the printer's parameters");
    free(bad.data);

    /* Damaged image data, and a bitmap that differs in one pixel. */
    bad = copy_blob(&good, 0);
    bad.data[l.first_bid + 16U + 4000U] ^= 0x5aU;
    expect_refused(&bad, &page, bitmap, "JBIG");
    free(bad.data);
    unsigned char *other = malloc(zjs_line_bytes(page.width, page.bpp) * page.height);
    assert(other != NULL);
    memcpy(other, bitmap, zjs_line_bytes(page.width, page.bpp) * page.height);
    other[zjs_line_bytes(page.width, page.bpp) * 6000U + 3U] ^= 0x10U;
    expect_refused(&good, &page, other, "differs from the page at line 6000");
    free(other);

    /* A short BID chunk before the last one. */
    bad = copy_blob(&good, 16U);
    size_t split = l.first_bid + 16U + 100U;
    memmove(bad.data + split + 16U, bad.data + split, good.length - split);
    memcpy(bad.data + split, bad.data + l.first_bid, 16U);
    write32(bad.data + l.first_bid, 16U + 100U);
    write32(bad.data + split, 16U + ZJ_BID_BLOCK_SIZE - 100U);
    bad.length += 16U;
    expect_refused(&bad, &page, bitmap, "is short but not the last");
    free(bad.data);

    /* Padding: unaligned, too long, or not zero. */
    bad = grow_last_bid(&good, &l, 1U, 0);
    expect_refused(&bad, &page, bitmap, "not padded to 4 bytes");
    free(bad.data);
    bad = grow_last_bid(&good, &l, 4U, 0);
    expect_refused(&bad, &page, bitmap, "follow the end of the JBIG image");
    free(bad.data);
    bad = grow_last_bid(&good, &l, 4U, 0xff);
    expect_refused(&bad, &page, bitmap, "data follows the end of the JBIG image");
    free(bad.data);

    /* A cut page and trailing bytes. */
    Blob cut = {good.data, good.length - 16U};
    expect_refused(&cut, &page, bitmap, "END_JBIG and END_PAGE");
    cut.length = l.last_bid;
    expect_refused(&cut, &page, bitmap, "JBIG image ends early");
    bad = copy_blob(&good, 1U);
    bad.data[bad.length++] = 0;
    expect_refused(&bad, &page, bitmap, "bytes follow END_PAGE");
    free(bad.data);
    expect_refused(&good, &page, NULL, "nothing to check");

    free(good.data);
    free(bitmap);
    printf("    [PASS] Page Guard Stripe Defect Test (JBIG Yd, VIDEO_Y, RASTER_X, items, data, BID framing refused)\n");
}

/* The encoder pool hands back a refused page as a failure with its reason,
 * and keeps encoding the pages after it. */
static void test_pool_refuses(void) {
    P1600EncoderPool *pool = p1600_encoder_pool_create(2, 3);
    assert(pool != NULL);
    for (uint32_t i = 0; i < 3U; i++) {
        ZjsPageOptions page = page_options(64, 24, 2, false);
        if (i == 1U) page.height = 25;  /* not a 12-line multiple */
        size_t bytes = zjs_line_bytes(page.width, page.bpp) * (page.height > 24U ? page.height : 24U);
        unsigned char *bitmap = calloc(1, bytes);
        assert(bitmap != NULL && p1600_encoder_pool_submit(pool, &page, bitmap) == 0);
    }
    p1600_encoder_pool_close(pool);
    unsigned char *data = NULL;
    size_t length = 0;
    assert(p1600_encoder_pool_next(pool, &data, &length) == 1 && data != NULL);
    free(data);
    assert(strcmp(p1600_encoder_pool_error(pool), "") == 0);
    assert(p1600_encoder_pool_next(pool, &data, &length) == -1 && data == NULL);
    assert(strstr(p1600_encoder_pool_error(pool), "invalid page geometry") != NULL);
    assert(p1600_encoder_pool_next(pool, &data, &length) == 1 && data != NULL);
    free(data);
    assert(p1600_encoder_pool_next(pool, &data, &length) == 0);
    p1600_encoder_pool_destroy(pool);
    printf("    [PASS] Page Guard Encoder Pool Test (refused page reported, later pages kept)\n");
}

int main(void) {
    printf("[*] Starting page guard tests...\n");
    test_geometry_sweep();
    test_padding_forced_white();
    test_stripe_defects_refused();
    test_pool_refuses();
    printf("[+] Page guard tests passed.\n");
    return 0;
}
