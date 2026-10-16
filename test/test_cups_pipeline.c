/*
 * test_cups_pipeline - validates the ZjStream rastertop1600dn wrote for a
 * real macOS CUPS raster job (cgtexttopdf -> cgpdftoraster -> filter).
 *
 * Usage: test_cups_pipeline RASTER ZJSTREAM key=value...
 *        test_cups_pipeline --trim RASTER_IN RASTER_OUT N
 *
 *   mode=native          ZjStream job layout (the only one)
 *   pages=N              raster pages CUPS must have produced (required)
 *   title=T              job title (PJL JOB NAME)
 *   duplex=none|long|short   raster Duplex/Tumble
 *   sheets=simplex|auto|manual   native sheet order
 *   padded=1             CUPS appended an all-white back side to an odd
 *                        duplex document: the last raster page must be blank
 *   bpp paper media source copies collate density jam econo rotate
 *
 * --trim keeps the first N pages of a raster, as a rasterizer that does not
 * pad odd duplex documents (or a one-page duplex job) would deliver them.
 *
 * Every raster page is rendered with the filters' own p1600_page_geometry() and
 * p1600_render_page() (rotate = TersKose); each JBIG image in the stream must
 * decode to exactly that bitmap, and blank duplex partners to all zeros. The
 * screening itself is unit-tested elsewhere: this test proves the macOS
 * raster -> filter -> ZjStream framing, sheet order and pixel integrity.
 */
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cups/raster.h>

#include "p1600_raster.h"
#include "jbig.h"
#include "zjs.h"
#include "zjs_encoder.h"

#define BIH_LENGTH 20U
#define CHUNK_HEADER 16U
#define UINT_ITEM 12U
#define MAX_ITEMS 24U

typedef enum { RASTER_SIMPLEX, RASTER_LONG_EDGE, RASTER_SHORT_EDGE } RasterDuplex;
typedef enum { SHEETS_SIMPLEX, SHEETS_AUTO, SHEETS_MANUAL } Sheets;

typedef struct {
    size_t pages;
    const char *title;
    RasterDuplex duplex;
    Sheets sheets;
    uint32_t bpp;
    uint32_t paper;
    uint32_t media;
    uint32_t source;
    uint32_t copies;
    uint32_t collate;
    uint32_t density;
    bool jam_recovery;
    bool econo;
    bool rotate;
    bool padded;
} Expect;

/* A raster page as the filters render it. */
typedef struct {
    P1600PageGeometry geometry;
    unsigned char *bitmap; /* geometry.height * geometry.line_bytes */
} Page;

/* One entry of the expected output: a raster page, a blank partner or the
 * manual-duplex pause. */
typedef enum { SLOT_PAGE, SLOT_BLANK, SLOT_PAUSE } SlotKind;

typedef struct {
    SlotKind kind;
    size_t page; /* raster page index (SLOT_PAGE) */
} Slot;

typedef struct {
    const unsigned char *data;
    size_t len;
    size_t pos;
} Stream;

typedef struct {
    uint32_t size;
    uint32_t type;
    uint32_t items;
    uint16_t reserved;
    const unsigned char *payload;
    size_t payload_len;
    size_t offset;
} Chunk;

typedef struct {
    uint16_t id;
    uint8_t type;
    uint32_t value;      /* ZJIT_UINT32 */
} ItemSpec;

static const char *context_label = "setup";

__attribute__((format(printf, 3, 4))) _Noreturn static void fail(int line, const char *condition,
                                                                  const char *format, ...) {
    va_list args;
    fprintf(stderr, "    [FAIL] test_cups_pipeline.c:%d [%s] ", line, context_label);
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr, " (%s)\n", condition);
    exit(1);
}

#define CHECK(condition, ...) \
    do { \
        if (!(condition)) fail(__LINE__, #condition, __VA_ARGS__); \
    } while (0)

static uint32_t read_be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t read_be16(const unsigned char *p) {
    return (uint16_t)(((unsigned int)p[0] << 8) | (unsigned int)p[1]);
}

static void *checked_calloc(size_t count, size_t size) {
    void *memory = calloc(count ? count : 1U, size ? size : 1U);
    CHECK(memory != NULL, "out of memory (%zu x %zu bytes)", count, size);
    return memory;
}

/* ---------------------------------------------------------------- options */

static uint32_t parse_uint(const char *key, const char *value) {
    char *end = NULL;
    CHECK(value[0] >= '0' && value[0] <= '9', "%s needs a number, got '%s'", key, value);
    unsigned long parsed = strtoul(value, &end, 10);
    CHECK(end != value && *end == '\0' && parsed <= UINT32_MAX, "%s needs a number, got '%s'", key, value);
    return (uint32_t)parsed;
}

static bool parse_bool(const char *key, const char *value) {
    uint32_t parsed = parse_uint(key, value);
    CHECK(parsed <= 1U, "%s must be 0 or 1, got '%s'", key, value);
    return parsed == 1U;
}

static Expect parse_expect(int argc, char **argv) {
    Expect e = {
        .title = "", .duplex = RASTER_SIMPLEX,
        .sheets = SHEETS_SIMPLEX, .bpp = 2, .paper = DMPAPER_A4, .media = DMMEDIA_PLAIN,
        .source = DMBIN_AUTO, .copies = 1, .collate = 0, .density = 3,
    };
    for (int i = 0; i < argc; i++) {
        const char *equals = strchr(argv[i], '=');
        CHECK(equals != NULL && equals != argv[i], "expected key=value, got '%s'", argv[i]);
        size_t key_len = (size_t)(equals - argv[i]);
        char key[32];
        CHECK(key_len < sizeof(key), "option name too long: '%s'", argv[i]);
        memcpy(key, argv[i], key_len);
        key[key_len] = '\0';
        const char *value = equals + 1;

        if (strcmp(key, "mode") == 0) {
            CHECK(strcmp(value, "native") == 0, "bad mode '%s'", value);
        } else if (strcmp(key, "pages") == 0) {
            e.pages = parse_uint(key, value);
        } else if (strcmp(key, "title") == 0) {
            e.title = value;
        } else if (strcmp(key, "duplex") == 0) {
            if (strcmp(value, "none") == 0) e.duplex = RASTER_SIMPLEX;
            else if (strcmp(value, "long") == 0) e.duplex = RASTER_LONG_EDGE;
            else if (strcmp(value, "short") == 0) e.duplex = RASTER_SHORT_EDGE;
            else CHECK(false, "bad duplex '%s'", value);
        } else if (strcmp(key, "sheets") == 0) {
            if (strcmp(value, "simplex") == 0) e.sheets = SHEETS_SIMPLEX;
            else if (strcmp(value, "auto") == 0) e.sheets = SHEETS_AUTO;
            else if (strcmp(value, "manual") == 0) e.sheets = SHEETS_MANUAL;
            else CHECK(false, "bad sheets '%s'", value);
        } else if (strcmp(key, "bpp") == 0) {
            e.bpp = parse_uint(key, value);
        } else if (strcmp(key, "paper") == 0) {
            e.paper = parse_uint(key, value);
        } else if (strcmp(key, "media") == 0) {
            e.media = parse_uint(key, value);
        } else if (strcmp(key, "source") == 0) {
            e.source = parse_uint(key, value);
        } else if (strcmp(key, "copies") == 0) {
            e.copies = parse_uint(key, value);
        } else if (strcmp(key, "collate") == 0) {
            e.collate = parse_bool(key, value) ? 1U : 0U;
        } else if (strcmp(key, "density") == 0) {
            e.density = parse_uint(key, value);
        } else if (strcmp(key, "jam") == 0) {
            e.jam_recovery = parse_bool(key, value);
        } else if (strcmp(key, "econo") == 0) {
            e.econo = parse_bool(key, value);
        } else if (strcmp(key, "rotate") == 0) {
            e.rotate = parse_bool(key, value);
        } else if (strcmp(key, "padded") == 0) {
            e.padded = parse_bool(key, value);
        } else {
            CHECK(false, "unknown option '%s'", key);
        }
    }

    CHECK(e.pages > 0, "pages=N is required");
    CHECK(e.bpp == 1U || e.bpp == 2U, "bpp must be 1 or 2");
    CHECK(e.density >= 1U && e.density <= 5U, "density must be 1..5");
    CHECK(e.copies >= 1U, "copies must be positive");
    CHECK(!e.padded || (e.duplex != RASTER_SIMPLEX && e.pages % 2U == 0U),
          "padded=1 needs an even duplex raster");
    CHECK((e.duplex == RASTER_SIMPLEX) == (e.sheets == SHEETS_SIMPLEX),
          "sheets=simplex exactly when duplex=none");
    size_t title_len = strlen(e.title);
    CHECK(title_len > 0 && title_len <= 80, "title must be 1..80 characters");
    for (size_t i = 0; i < title_len; i++) {
        CHECK(e.title[i] > 0x20 && e.title[i] < 0x7f, "title must be printable ASCII without spaces");
    }
    return e;
}

/* ----------------------------------------------------------------- raster */

static Page *read_raster(const char *path, const Expect *e, size_t *page_count) {
    context_label = "raster";
    int fd = open(path, O_RDONLY);
    CHECK(fd >= 0, "cannot open raster %s", path);
    cups_raster_t *raster = cupsRasterOpen(fd, CUPS_RASTER_READ);
    CHECK(raster != NULL, "cannot read CUPS raster %s", path);

    Page *pages = NULL;
    size_t count = 0;
    cups_page_header2_t header;
    while (cupsRasterReadHeader2(raster, &header)) {
        size_t n = count + 1U;
        CHECK(n <= e->pages, "CUPS produced more than %zu raster pages", e->pages);
        Page *grown = realloc(pages, n * sizeof(*pages));
        CHECK(grown != NULL, "out of memory");
        pages = grown;
        Page *page = &pages[count];
        memset(page, 0, sizeof(*page));

        CHECK(header.cupsInteger[0] == P1600_RASTER_MODEL_ID, "page %zu: cupsInteger0 %u", n, header.cupsInteger[0]);
        CHECK(header.HWResolution[1] == 600U, "page %zu: %u dpi vertical", n, header.HWResolution[1]);
        /* The PPD contract: 600x600 8-bit CUPS_CSPACE_W, bpp chosen by
         * cupsCompression bit 0. */
        CHECK(header.HWResolution[0] == 600U && header.cupsBitsPerColor == 8U &&
                  header.cupsBitsPerPixel == 8U && header.cupsColorSpace == CUPS_CSPACE_W,
              "page %zu: expected 600x600 8-bit gray raster, got %ux%u %u-bit space %u", n,
              header.HWResolution[0], header.HWResolution[1], header.cupsBitsPerPixel,
              header.cupsColorSpace);
        CHECK(header.cupsCompression == e->bpp - 1U, "page %zu: cupsCompression %u for bpp %u",
              n, header.cupsCompression, e->bpp);
        CHECK(header.NumCopies == e->copies, "page %zu: NumCopies %u, expected %u", n,
              header.NumCopies, e->copies);
        CHECK(header.Collate == e->collate, "page %zu: Collate %u, expected %u", n,
              header.Collate, e->collate);
        RasterDuplex duplex = !header.Duplex ? RASTER_SIMPLEX
                              : header.Tumble ? RASTER_SHORT_EDGE : RASTER_LONG_EDGE;
        CHECK(duplex == e->duplex, "page %zu: raster Duplex %u Tumble %u", n, header.Duplex, header.Tumble);
        CHECK(p1600_paper_code(header.PageSize[0], header.PageSize[1]) == e->paper,
              "page %zu: PageSize %ux%u is not DMPAPER %u", n, header.PageSize[0], header.PageSize[1], e->paper);
        CHECK(header.cupsMediaType == e->media, "page %zu: cupsMediaType %u, expected %u", n,
              header.cupsMediaType, e->media);
        CHECK(header.MediaPosition == e->source, "page %zu: MediaPosition %u, expected %u", n,
              header.MediaPosition, e->source);

        char reason[160] = "";
        CHECK(p1600_page_geometry(&header, &page->geometry, reason, sizeof(reason)) == 0,
              "page %zu: %s", n, reason);
        CHECK(page->geometry.kind == P1600_RASTER_GRAY,
              "page %zu: unexpected raster kind", n);
        CHECK(page->geometry.bpp == e->bpp, "page %zu: %u video bits per pixel, expected %u", n,
              page->geometry.bpp, e->bpp);
        CHECK(page->geometry.width == header.cupsWidth,
              "page %zu: VIDEO_X %u for cupsWidth %u", n, page->geometry.width, header.cupsWidth);
        CHECK(page->geometry.height == zjs_video_height(header.cupsHeight) &&
                  page->geometry.height % 12U == 0U && page->geometry.height >= header.cupsHeight &&
                  page->geometry.height < header.cupsHeight + 12U,
              "page %zu: VIDEO_Y %u for cupsHeight %u", n, page->geometry.height, header.cupsHeight);
        CHECK(page->geometry.line_bytes == ((size_t)page->geometry.width * e->bpp + 127U) / 128U * 16U,
              "page %zu: line bytes %zu", n, page->geometry.line_bytes);

        page->bitmap = checked_calloc(page->geometry.height, page->geometry.line_bytes);
        CHECK(p1600_render_page(raster, &header, &page->geometry, e->rotate, page->bitmap) == 0,
              "page %zu: truncated raster", n);
        count = n;
    }
    cupsRasterClose(raster);
    close(fd);
    CHECK(count == e->pages && pages != NULL, "CUPS produced %zu raster pages, expected %zu", count, e->pages);
    if (e->padded) {
        const Page *last = &pages[count - 1U];
        size_t size = (size_t)last->geometry.height * last->geometry.line_bytes;
        for (size_t i = 0; i < size; i++) {
            CHECK(last->bitmap[i] == 0, "the back side CUPS added to the odd duplex document is not blank");
        }
    }
    *page_count = count;
    return pages;
}

/* ------------------------------------------------------------ sheet order */

static size_t add_slot(Slot *slots, size_t used, SlotKind kind, size_t page) {
    slots[used] = (Slot){.kind = kind, .page = page};
    return used + 1U;
}

/* Raster page `index`, or the blank partner of an odd last page. */
static size_t add_sheet_side(Slot *slots, size_t used, size_t index, size_t count) {
    return index < count ? add_slot(slots, used, SLOT_PAGE, index) : add_slot(slots, used, SLOT_BLANK, 0);
}

static size_t expected_slots(const Expect *e, size_t count, Slot *slots) {
    size_t used = 0;
    size_t sides = count + (count % 2U);
    bool paired = e->sheets != SHEETS_SIMPLEX;
    bool manual = e->sheets == SHEETS_MANUAL;

    if (!paired || (manual && count == 1U)) {
        /* Simplex, and a one-page manual duplex job (no second pass). */
        for (size_t i = 0; i < count; i++) used = add_slot(slots, used, SLOT_PAGE, i);
    } else if (!manual) {
        /* Automatic duplex: 2, 1, 4, 3, ..., blank, n. */
        for (size_t front = 0; front < sides; front += 2U) {
            used = add_sheet_side(slots, used, front + 1U, count);
            used = add_sheet_side(slots, used, front, count);
        }
    } else {
        /* Manual duplex: even pages descending, pause, odd pages ascending. */
        for (size_t back = sides; back >= 2U; back -= 2U) used = add_sheet_side(slots, used, back - 1U, count);
        used = add_slot(slots, used, SLOT_PAUSE, 0);
        for (size_t front = 0; front < sides; front += 2U) used = add_sheet_side(slots, used, front, count);
    }
    return used;
}

/* -------------------------------------------------------------- ZjStream */

static void expect_bytes(Stream *s, const void *bytes, size_t len, const char *what) {
    CHECK(s->len - s->pos >= len && memcmp(s->data + s->pos, bytes, len) == 0,
          "%s does not match at offset %zu", what, s->pos);
    s->pos += len;
}

static Chunk read_chunk(Stream *s) {
    Chunk chunk = {0};
    CHECK(s->len - s->pos >= CHUNK_HEADER, "truncated chunk header at offset %zu", s->pos);
    const unsigned char *h = s->data + s->pos;
    chunk.offset = s->pos;
    chunk.size = read_be32(h);
    chunk.type = read_be32(h + 4);
    chunk.items = read_be32(h + 8);
    chunk.reserved = read_be16(h + 12);
    CHECK(read_be16(h + 14) == ZJ_SIGNATURE, "chunk at offset %zu lacks the 0x5A5A signature", s->pos);
    CHECK(chunk.size >= CHUNK_HEADER && chunk.size <= s->len - s->pos,
          "chunk at offset %zu has size %u", s->pos, chunk.size);
    chunk.payload = h + CHUNK_HEADER;
    chunk.payload_len = chunk.size - CHUNK_HEADER;
    s->pos += chunk.size;
    return chunk;
}

static void check_chunk_header(const Chunk *chunk, uint32_t type, uint32_t items, uint16_t reserved,
                               const char *what) {
    CHECK(chunk->type == type, "%s: chunk type %u at offset %zu, expected %u", what, chunk->type,
          chunk->offset, type);
    CHECK(chunk->items == items, "%s: %u items, expected %u", what, chunk->items, items);
    CHECK(chunk->reserved == reserved, "%s: reserved field %u, expected %u", what, chunk->reserved, reserved);
}

/* Items in exact order and encoding. */
static void check_items(const Chunk *chunk, const ItemSpec *specs, size_t count, const char *what) {
    size_t pos = 0;
    for (size_t i = 0; i < count; i++) {
        CHECK(chunk->payload_len - pos >= 8U, "%s: item %zu is truncated", what, i);
        const unsigned char *item = chunk->payload + pos;
        uint32_t size = read_be32(item);
        uint16_t id = read_be16(item + 4);
        CHECK(id == specs[i].id, "%s: item %zu has id %u, expected %u", what, i, id, specs[i].id);
        CHECK(item[6] == specs[i].type && item[7] == 0, "%s: item %u has type %u param %u", what, id,
              item[6], item[7]);
        CHECK(size == UINT_ITEM && size <= chunk->payload_len - pos, "%s: item %u has size %u", what, id, size);
        uint32_t value = read_be32(item + 8);
        CHECK(value == specs[i].value, "%s: item %u = %u, expected %u", what, id, value, specs[i].value);
        pos += size;
    }
    CHECK(pos == chunk->payload_len, "%s: %zu bytes after the last item", what, chunk->payload_len - pos);
}

static ItemSpec uint_item(uint16_t id, uint32_t value) {
    return (ItemSpec){.id = id, .type = ZJIT_UINT32, .value = value};
}

/* 20-byte BIH: DL 0, D 0, one plane, Xd, Yd, L0 128, MX 0, MY 0, order
 * ILEAVE|SMID, options LRLTWO|TPDON|TPBON|DPON. */
static void check_bih(const Chunk *chunk, uint32_t xd, uint32_t yd, const char *what) {
    CHECK(chunk->payload_len == BIH_LENGTH, "%s: BIH chunk carries %zu bytes", what, chunk->payload_len);
    const unsigned char *b = chunk->payload;
    CHECK(b[0] == 0 && b[1] == 0 && b[2] == 1 && b[3] == 0, "%s: BIH DL/D/P %u/%u/%u", what, b[0], b[1], b[2]);
    CHECK(read_be32(b + 4) == xd, "%s: BIH Xd %u, expected %u", what, read_be32(b + 4), xd);
    CHECK(read_be32(b + 8) == yd, "%s: BIH Yd %u, expected %u", what, read_be32(b + 8), yd);
    CHECK(read_be32(b + 12) == 128U, "%s: BIH L0 %u", what, read_be32(b + 12));
    CHECK(b[16] == 0 && b[17] == 0, "%s: BIH MX %u MY %u", what, b[16], b[17]);
    CHECK(b[18] == (JBG_ILEAVE | JBG_SMID), "%s: BIH order 0x%02x", what, b[18]);
    CHECK(b[19] == (JBG_LRLTWO | JBG_TPDON | JBG_TPBON | JBG_DPON), "%s: BIH options 0x%02x", what, b[19]);
}

/* Decodes BIH + BID data and compares it with `expected` (NULL = blank).
 * Returns the number of unconsumed bytes after the final stripe. */
static size_t decode_and_compare(unsigned char *bie, size_t bie_len, const unsigned char *expected,
                                 size_t line_bytes, uint32_t height, const char *what) {
    struct jbg_dec_state decoder;
    jbg_dec_init(&decoder);
    size_t position = 0;
    int status = JBG_EAGAIN;
    while (position < bie_len) {
        size_t consumed = 0;
        status = jbg_dec_in(&decoder, bie + position, bie_len - position, &consumed);
        CHECK(consumed <= bie_len - position, "%s: JBIG decoder overran its input", what);
        position += consumed;
        if (status == JBG_EOK) break;
        CHECK(status == JBG_EAGAIN || status == JBG_EOK_INTR, "%s: JBIG decode failed: %s", what,
              jbg_strerror(status));
        CHECK(consumed > 0 || status == JBG_EOK_INTR, "%s: JBIG decoder stalled", what);
    }
    CHECK(status == JBG_EOK, "%s: JBIG image ended early (%s)", what, jbg_strerror(status));
    for (size_t i = position; i < bie_len; i++) {
        CHECK(bie[i] == 0, "%s: non-zero byte after the final JBIG stripe", what);
    }

    CHECK(jbg_dec_getwidth(&decoder) == line_bytes * 8U && jbg_dec_getheight(&decoder) == height &&
              jbg_dec_getplanes(&decoder) == 1,
          "%s: decoded %lux%lu, expected %zux%u", what, jbg_dec_getwidth(&decoder),
          jbg_dec_getheight(&decoder), line_bytes * 8U, height);
    CHECK(jbg_dec_getsize(&decoder) == line_bytes * height, "%s: decoded plane size %lu", what,
          jbg_dec_getsize(&decoder));
    const unsigned char *image = jbg_dec_getimage(&decoder, 0);
    for (uint32_t y = 0; y < height; y++) {
        const unsigned char *row = image + (size_t)y * line_bytes;
        for (size_t x = 0; x < line_bytes; x++) {
            unsigned char want = expected ? expected[(size_t)y * line_bytes + x] : 0;
            CHECK(row[x] == want, "%s: pixel byte mismatch at line %u byte %zu (0x%02x, expected 0x%02x)",
                  what, y, x, row[x], want);
        }
    }
    jbg_dec_free(&decoder);
    return bie_len - position;
}

/* START_PAGE item lists. */
static size_t native_page_items(const Expect *e, const P1600PageGeometry *g, ItemSpec *items) {
    size_t n = 0;
    items[n++] = uint_item(ZJI_DMPAPER, e->paper);
    items[n++] = uint_item(ZJI_DMCOPIES, e->copies);
    items[n++] = uint_item(ZJI_DMDEFAULTSOURCE, e->source);
    items[n++] = uint_item(ZJI_DMMEDIATYPE, e->media);
    items[n++] = uint_item(ZJI_NBIE, 1);
    items[n++] = uint_item(ZJI_RESOLUTION_X, 600);
    items[n++] = uint_item(ZJI_RESOLUTION_Y, 600);
    items[n++] = uint_item(ZJI_RASTER_X, g->width * g->bpp);
    items[n++] = uint_item(ZJI_RASTER_Y, g->height);
    items[n++] = uint_item(ZJI_VIDEO_BPP, g->bpp);
    items[n++] = uint_item(ZJI_VIDEO_X, g->width);
    items[n++] = uint_item(ZJI_VIDEO_Y, g->height);
    if (e->sheets == SHEETS_AUTO) items[n++] = uint_item(ZJI_DMDUPLEX, ZJ_DMDUPLEX_AUTOMATIC);
    items[n++] = uint_item(ZJI_ECONOMODE, e->econo ? 1U : 0U);
    return n;
}

/* END_JBIG and END_PAGE carry no items. */
static void check_closing_chunk(Stream *s, uint32_t type, const char *what) {
    Chunk chunk = read_chunk(s);
    check_chunk_header(&chunk, type, 0, 0, what);
    CHECK(chunk.payload_len == 0, "%s carries %zu bytes", what, chunk.payload_len);
}

static void check_page(Stream *s, const Expect *e, const Page *page, const unsigned char *expected,
                       const char *what) {
    const P1600PageGeometry *g = &page->geometry;
    ItemSpec items[MAX_ITEMS];
    size_t count = native_page_items(e, g, items);

    Chunk start = read_chunk(s);
    check_chunk_header(&start, ZJT_START_PAGE, (uint32_t)count, (uint16_t)(count * UINT_ITEM), what);
    check_items(&start, items, count, what);

    Chunk bih = read_chunk(s);
    check_chunk_header(&bih, ZJT_JBIG_BIH, 0, 0, what);
    check_bih(&bih, (uint32_t)(g->line_bytes * 8U), g->height, what);

    size_t bie_len = BIH_LENGTH;
    unsigned char *bie = checked_calloc(1, BIH_LENGTH);
    memcpy(bie, bih.payload, BIH_LENGTH);
    size_t bid_count = 0;
    size_t last_bid = 0;
    while (s->len - s->pos >= CHUNK_HEADER && read_be32(s->data + s->pos + 4) == ZJT_JBIG_BID) {
        Chunk bid = read_chunk(s);
        check_chunk_header(&bid, ZJT_JBIG_BID, 0, 0, what);
        /* JBIG data comes in exactly 64 KiB; only the last chunk is
         * shorter (and padded). */
        CHECK(bid_count == 0 || last_bid == ZJ_BID_BLOCK_SIZE, "%s: BID chunk %zu carries %zu bytes",
              what, bid_count - 1U, last_bid);
        CHECK(bid.payload_len > 0 && bid.payload_len % ZJ_BID_PADDING == 0,
              "%s: BID chunk %zu carries %zu bytes", what, bid_count, bid.payload_len);
        unsigned char *grown = realloc(bie, bie_len + bid.payload_len);
        CHECK(grown != NULL, "out of memory");
        bie = grown;
        memcpy(bie + bie_len, bid.payload, bid.payload_len);
        bie_len += bid.payload_len;
        last_bid = bid.payload_len;
        bid_count++;
    }
    CHECK(bid_count > 0, "%s: no BID chunk", what);

    size_t trailing = decode_and_compare(bie, bie_len, expected, g->line_bytes, g->height, what);
    CHECK(trailing <= last_bid, "%s: padding spans BID chunks", what);
    size_t data_in_last = last_bid - trailing;
    /* Only the last chunk is padded, with zeros to a multiple of four. */
    CHECK(last_bid <= ZJ_BID_BLOCK_SIZE && trailing == (4U - data_in_last % 4U) % 4U,
          "%s: last BID chunk %zu bytes with %zu padding bytes", what, last_bid, trailing);
    free(bie);

    check_closing_chunk(s, ZJT_END_JBIG, what);
    check_closing_chunk(s, ZJT_END_PAGE, what);
}

static void check_native_header(Stream *s, const Expect *e) {
    char pjl[512];
    int length = snprintf(pjl, sizeof(pjl),
                          "\033%%-12345X@PJL USTATUS JOB=ON\r\n"
                          "@PJL USTATUS PAGE=ON\r\n"
                          "@PJL USTATUS DEVICE=ON\r\n"
                          "@PJL INFO STATUS\r\n"
                          "@PJL JOB NAME=%s\r\n"
                          "@PJL SET DENSITY = %u\r\n"
                          "@PJL SET JAMRECOVERY = %s\r\n"
                          "@PJL ENTER LANGUAGE=ZJS\r\n"
                          "JZJZ",
                          e->title, e->density, e->jam_recovery ? "AUTO" : "OFF");
    CHECK(length > 0 && (size_t)length < sizeof(pjl), "PJL header too long");
    expect_bytes(s, pjl, (size_t)length, "HP PJL job header");

    Chunk doc = read_chunk(s);
    check_chunk_header(&doc, ZJT_START_DOC, 1, UINT_ITEM, "START_DOC");
    ItemSpec collate = uint_item(ZJI_DMCOLLATE, e->collate);
    check_items(&doc, &collate, 1, "START_DOC");
}

static unsigned char *read_file(const char *path, size_t *length) {
    FILE *fp = fopen(path, "rb");
    CHECK(fp != NULL, "cannot open %s", path);
    CHECK(fseeko(fp, 0, SEEK_END) == 0, "cannot seek %s", path);
    off_t size = ftello(fp);
    CHECK(size > 0, "%s is empty", path);
    CHECK(fseeko(fp, 0, SEEK_SET) == 0, "cannot seek %s", path);
    unsigned char *data = checked_calloc((size_t)size, 1);
    CHECK(fread(data, 1, (size_t)size, fp) == (size_t)size, "cannot read %s", path);
    fclose(fp);
    *length = (size_t)size;
    return data;
}

static size_t validate_stream(const char *path, const Expect *e, const Page *pages, size_t count) {
    context_label = "zjstream";
    Stream s = {0};
    s.data = read_file(path, &s.len);
    check_native_header(&s, e);

    Slot *slots = checked_calloc(count * 2U + 2U, sizeof(*slots));
    size_t slot_count = expected_slots(e, count, slots);
    const Page *last = &pages[count - 1U];
    size_t images = 0;
    for (size_t i = 0; i < slot_count; i++) {
        char what[64];
        if (slots[i].kind == SLOT_PAUSE) {
            snprintf(what, sizeof(what), "slot %zu (pause)", i + 1U);
            Chunk pause = read_chunk(&s);
            check_chunk_header(&pause, ZJT_PAUSE, 0, 0, what);
            CHECK(pause.payload_len == 0, "pause chunk carries %zu bytes", pause.payload_len);
            continue;
        }
        if (slots[i].kind == SLOT_BLANK) {
            snprintf(what, sizeof(what), "slot %zu (blank partner)", i + 1U);
            check_page(&s, e, last, NULL, what);
        } else {
            const Page *page = &pages[slots[i].page];
            snprintf(what, sizeof(what), "slot %zu (raster page %zu)", i + 1U, slots[i].page + 1U);
            check_page(&s, e, page, page->bitmap, what);
        }
        images++;
    }

    context_label = "document end";
    Chunk end = read_chunk(&s);
    check_chunk_header(&end, ZJT_END_DOC, 0, 0, "END_DOC");
    CHECK(end.payload_len == 0, "END_DOC carries %zu bytes", end.payload_len);
    static const char native_trailer[] = "\033%-12345X@PJL EOJ\r\n\033%-12345X@PJL USTATUSOFF\r\n\033%-12345X";
    expect_bytes(&s, native_trailer, sizeof(native_trailer) - 1U, "PJL trailer");
    CHECK(s.pos == s.len, "%zu bytes after the PJL trailer", s.len - s.pos);

    free(slots);
    free((void *)s.data);
    return images;
}

static void trim_raster(const char *in_path, const char *out_path, const char *count_text) {
    context_label = "trim";
    uint32_t keep = parse_uint("N", count_text);
    CHECK(keep > 0, "--trim needs N > 0");
    int in_fd = open(in_path, O_RDONLY);
    CHECK(in_fd >= 0, "cannot open raster %s", in_path);
    int out_fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(out_fd >= 0, "cannot create raster %s", out_path);
    cups_raster_t *in = cupsRasterOpen(in_fd, CUPS_RASTER_READ);
    cups_raster_t *out = cupsRasterOpen(out_fd, CUPS_RASTER_WRITE);
    CHECK(in != NULL && out != NULL, "cannot open the rasters");

    cups_page_header2_t header;
    uint32_t copied = 0;
    while (copied < keep && cupsRasterReadHeader2(in, &header)) {
        CHECK(cupsRasterWriteHeader2(out, &header), "cannot write page %u header", copied + 1U);
        unsigned char *line = checked_calloc(header.cupsBytesPerLine, 1);
        for (uint32_t y = 0; y < header.cupsHeight; y++) {
            CHECK(cupsRasterReadPixels(in, line, header.cupsBytesPerLine) == header.cupsBytesPerLine,
                  "page %u is truncated", copied + 1U);
            CHECK(cupsRasterWritePixels(out, line, header.cupsBytesPerLine) == header.cupsBytesPerLine,
                  "cannot write page %u", copied + 1U);
        }
        free(line);
        copied++;
    }
    CHECK(copied == keep, "the raster has only %u pages", copied);
    cupsRasterClose(in);
    cupsRasterClose(out);
    close(in_fd);
    CHECK(close(out_fd) == 0, "cannot write %s", out_path);
}

int main(int argc, char **argv) {
    if (argc == 5 && strcmp(argv[1], "--trim") == 0) {
        trim_raster(argv[2], argv[3], argv[4]);
        return 0;
    }
    if (argc < 3) {
        fprintf(stderr, "Usage: %s RASTER ZJSTREAM key=value...\n"
                        "       %s --trim RASTER_IN RASTER_OUT N\n", argv[0], argv[0]);
        return 2;
    }
    Expect e = parse_expect(argc - 3, argv + 3);
    size_t count = 0;
    Page *pages = read_raster(argv[1], &e, &count);
    size_t images = validate_stream(argv[2], &e, pages, count);

    for (size_t i = 0; i < count; i++) free(pages[i].bitmap);
    free(pages);
    printf("    [PASS] %s: %zu raster page%s -> %zu %s JBIG image%s, framing and pixels exact\n", e.title,
           count, count == 1U ? "" : "s", images, "ZjStream",
           images == 1U ? "" : "s");
    return 0;
}
