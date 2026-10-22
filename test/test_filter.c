/*
 * End-to-end smoke tests for the rastertop1600dn CUPS filter (argv[1]):
 * one 8-bit gray page through to the exact PJL/ZjStream job, the 600 dpi and
 * legacy 1200x600 formats, solid page screening, empty/truncated/corrupt
 * input, a canceled job, header rejections, paper codes, automatic and
 * manual duplex order, and argument errors.  test_filter_modes.c holds the exhaustive matrix.
 *
 * Usage: test_filter <filter>
 */
#include <assert.h>
#include <cups/raster.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define TITLE "Integration Test"
#define MAX_JOB_PAGES 8U

enum {
    CHUNK_START_DOC = 0,
    CHUNK_END_DOC = 1,
    CHUNK_START_PAGE = 2,
    CHUNK_END_PAGE = 3,
    CHUNK_JBIG_BIH = 4,
    CHUNK_JBIG_BID = 5,
    CHUNK_END_JBIG = 6,
    CHUNK_PAUSE = 11
};

static const char pjl_header[] =
    "\033%-12345X@PJL USTATUS JOB=ON\r\n"
    "@PJL USTATUS PAGE=ON\r\n"
    "@PJL USTATUS DEVICE=ON\r\n"
    "@PJL INFO STATUS\r\n"
    "@PJL JOB NAME=" TITLE "\r\n"
    "@PJL SET DENSITY = 3\r\n"
    "@PJL SET JAMRECOVERY = OFF\r\n"
    "@PJL ENTER LANGUAGE=ZJS\r\n"
    "JZJZ";
static const char pjl_trailer[] =
    "\033%-12345X@PJL EOJ\r\n\033%-12345X@PJL USTATUSOFF\r\n\033%-12345X";

typedef struct {
    unsigned char *data;
    size_t len;
} Blob;

typedef struct {
    int status;
    Blob output;
    Blob diagnostics;
} FilterResult;

typedef enum { FILL_WHITE, FILL_BLACK, FILL_NOISE } Fill;

typedef struct {
    unsigned int model;
    unsigned int resolution_x;
    unsigned int resolution_y;
    unsigned int width;
    unsigned int height;
    unsigned int bits_per_pixel;
    unsigned int bytes_per_line;
    unsigned int color_space;
    unsigned int num_colors;
    unsigned int compression;
    unsigned int page_size[2];
    unsigned int media_position;
    unsigned int cups_media_type;
    unsigned int duplex;
    unsigned int tumble;
    Fill fill;
    unsigned int seed;
    int rotated;
} RasterPage;

typedef struct {
    uint32_t items[14][2]; /* START_PAGE item ids and values, in order */
    uint32_t item_count;
    uint32_t video_x;
    uint32_t video_y;
    unsigned char *jbig;   /* BIH followed by every BID payload byte */
    size_t jbig_len;
    size_t start;
    size_t end;
} ZjsPage;

typedef struct {
    uint32_t collate;
    int pause_after;
    size_t page_count;
    ZjsPage pages[MAX_JOB_PAGES];
} ZjsJob;

static uint32_t read_be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t read_be16(const unsigned char *p) {
    return (uint16_t)(((unsigned int)p[0] << 8) | (unsigned int)p[1]);
}

static char *temp_path(const char *tag) {
    const char *dir = getenv("TMPDIR");
    if (!dir || *dir == '\0') dir = "/tmp";
    size_t size = strlen(dir) + strlen(tag) + 32U;
    char *path = malloc(size);
    assert(path != NULL);
    int length = snprintf(path, size, "%s/p1600dn-%s.XXXXXX", dir, tag);
    assert(length > 0 && (size_t)length < size);
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(close(fd) == 0);
    return path;
}

static void remove_path(char *path) {
    assert(unlink(path) == 0);
    free(path);
}

static Blob read_path(const char *path) {
    Blob blob = {0};
    FILE *fp = fopen(path, "rb");
    assert(fp != NULL);
    assert(fseek(fp, 0, SEEK_END) == 0);
    long size = ftell(fp);
    assert(size >= 0 && fseek(fp, 0, SEEK_SET) == 0);
    blob.len = (size_t)size;
    blob.data = malloc(blob.len == 0 ? 1U : blob.len);
    assert(blob.data != NULL);
    assert(fread(blob.data, 1, blob.len, fp) == blob.len);
    assert(fclose(fp) == 0);
    return blob;
}

static int contains(const Blob *blob, const char *text) {
    return blob->len > 0 && memmem(blob->data, blob->len, text, strlen(text)) != NULL;
}

static void free_result(FilterResult *result) {
    free(result->output.data);
    free(result->diagnostics.data);
    memset(result, 0, sizeof(*result));
}

static RasterPage gray_page(unsigned int width, unsigned int height) {
    RasterPage page;
    memset(&page, 0, sizeof(page));
    page.model = 1606;
    page.resolution_x = 600;
    page.resolution_y = 600;
    page.width = width;
    page.height = height;
    page.bits_per_pixel = 8;
    page.bytes_per_line = width;
    page.color_space = CUPS_CSPACE_W;
    page.num_colors = 1;
    page.compression = 1;
    page.page_size[0] = 595;
    page.page_size[1] = 842;
    page.media_position = 7;
    page.cups_media_type = 1;
    page.fill = FILL_NOISE;
    page.seed = width * 31U + height;
    return page;
}

static RasterPage legacy_page(unsigned int width, unsigned int height) {
    RasterPage page = gray_page(width, height);
    page.resolution_x = 1200;
    page.bits_per_pixel = 1;
    page.bytes_per_line = (width + 7U) / 8U;
    page.color_space = CUPS_CSPACE_K;
    page.compression = 0;
    return page;
}

/* 0 = white .. 255 = black. */
static unsigned int darkness(const RasterPage *page, unsigned int x, unsigned int y) {
    if (page->rotated) {
        x = page->width - 1U - x;
        y = page->height - 1U - y;
    }
    if (page->fill == FILL_WHITE) return 0U;
    if (page->fill == FILL_BLACK) return 255U;
    uint32_t value = page->seed * 0x9e3779b9U ^ x * 0x85ebca6bU ^ y * 0xc2b2ae35U;
    value ^= value >> 15;
    value *= 0x2c1b3c6dU;
    value ^= value >> 12;
    return value & 0xffU;
}

static void write_raster(const char *path, const RasterPage *pages, size_t count) {
    int fd = open(path, O_WRONLY | O_TRUNC);
    assert(fd >= 0);
    cups_raster_t *raster = cupsRasterOpen(fd, CUPS_RASTER_WRITE);
    assert(raster != NULL);
    for (size_t i = 0; i < count; i++) {
        const RasterPage *page = &pages[i];
        cups_page_header2_t header;
        memset(&header, 0, sizeof(header));
        header.HWResolution[0] = page->resolution_x;
        header.HWResolution[1] = page->resolution_y;
        header.cupsWidth = page->width;
        header.cupsHeight = page->height;
        header.cupsBitsPerColor = page->bits_per_pixel;
        header.cupsBitsPerPixel = page->bits_per_pixel;
        header.cupsBytesPerLine = page->bytes_per_line;
        header.cupsColorOrder = CUPS_ORDER_CHUNKED;
        header.cupsColorSpace = (cups_cspace_t)page->color_space;
        header.cupsNumColors = page->num_colors;
        header.cupsCompression = page->compression;
        header.cupsInteger[0] = page->model;
        header.PageSize[0] = page->page_size[0];
        header.PageSize[1] = page->page_size[1];
        header.NumCopies = 1;
        header.MediaPosition = page->media_position;
        header.cupsMediaType = page->cups_media_type;
        header.Duplex = page->duplex ? CUPS_TRUE : CUPS_FALSE;
        header.Tumble = page->tumble ? CUPS_TRUE : CUPS_FALSE;
        assert(cupsRasterWriteHeader2(raster, &header));

        unsigned char *row = calloc(page->bytes_per_line, 1);
        assert(row != NULL);
        for (unsigned int y = 0; y < page->height; y++) {
            memset(row, 0, page->bytes_per_line);
            for (unsigned int x = 0; x < page->width; x++) {
                unsigned int dark = darkness(page, x, y);
                if (page->bits_per_pixel == 8U) {
                    row[x] = (unsigned char)(255U - dark);
                } else if (dark >= 128U) {
                    row[x / 8U] |= (unsigned char)(0x80U >> (x & 7U));
                }
            }
            assert(cupsRasterWritePixels(raster, row, page->bytes_per_line) == page->bytes_per_line);
        }
        free(row);
    }
    cupsRasterClose(raster);
    assert(close(fd) == 0);
}

static FilterResult run_filter(const char *filter, const char *input, const char *copies,
                               const char *options) {
    char *output_path = temp_path("zjs");
    char *diagnostics_path = temp_path("diagnostics");
    fflush(stdout);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        int out_fd = open(output_path, O_WRONLY | O_TRUNC);
        int err_fd = open(diagnostics_path, O_WRONLY | O_TRUNC);
        if (out_fd < 0 || err_fd < 0 || dup2(out_fd, STDOUT_FILENO) < 0 ||
            dup2(err_fd, STDERR_FILENO) < 0 || unsetenv("PPD") != 0) _exit(126);
        close(out_fd);
        close(err_fd);
        alarm(120);
        execl(filter, filter, "1", "test-user", TITLE, copies, options, input, (char *)NULL);
        _exit(127);
    }

    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    FilterResult result;
    result.status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    result.output = read_path(output_path);
    result.diagnostics = read_path(diagnostics_path);
    remove_path(output_path);
    remove_path(diagnostics_path);
    if (result.status > 1 || contains(&result.diagnostics, "Sanitizer") ||
        contains(&result.diagnostics, "runtime error")) {
        fprintf(stderr, "filter status %d:\n%.*s\n", result.status, (int)result.diagnostics.len,
                (const char *)result.diagnostics.data);
        assert(0 && "filter crashed or reported a sanitizer error");
    }
    return result;
}

static FilterResult run_pages(const char *filter, const RasterPage *pages, size_t count,
                              const char *copies, const char *options) {
    char *path = temp_path("raster");
    write_raster(path, pages, count);
    FilterResult result = run_filter(filter, path, copies, options);
    remove_path(path);
    return result;
}

/* Chunk header: u32 size, u32 type, u32 items, u16 item bytes, u16 0x5A5A. */
static uint32_t read_chunk(const Blob *blob, size_t *position, uint32_t *items, size_t *data_len) {
    assert(blob->len - *position >= 16U);
    const unsigned char *p = blob->data + *position;
    uint32_t size = read_be32(p);
    *items = read_be32(p + 8);
    assert(*items <= 14U && read_be16(p + 12) == *items * 12U && read_be16(p + 14) == 0x5a5aU);
    assert(size >= 16U + *items * 12U && size <= blob->len - *position);
    for (uint32_t i = 0; i < *items; i++) {
        const unsigned char *item = p + 16U + i * 12U;
        assert(read_be32(item) == 12U && item[6] == 1U && item[7] == 0U);
    }
    *data_len = size - 16U - *items * 12U;
    *position += size;
    return read_be32(p + 4);
}

static void parse_job(const Blob *blob, ZjsJob *job) {
    memset(job, 0, sizeof(*job));
    job->pause_after = -1;
    size_t position = sizeof(pjl_header) - 1U;
    assert(blob->len > position && memcmp(blob->data, pjl_header, position) == 0);
    uint32_t items = 0;
    size_t data_len = 0;
    size_t chunk = position;
    assert(read_chunk(blob, &position, &items, &data_len) == CHUNK_START_DOC);
    assert(items == 1U && data_len == 0U && read_be16(blob->data + chunk + 20U) == 1U);
    job->collate = read_be32(blob->data + chunk + 24U);

    for (;;) {
        chunk = position;
        uint32_t type = read_chunk(blob, &position, &items, &data_len);
        if (type == CHUNK_PAUSE) {
            assert(items == 0U && data_len == 0U && job->pause_after < 0);
            job->pause_after = (int)job->page_count;
            continue;
        }
        if (type == CHUNK_END_DOC) {
            assert(items == 0U && data_len == 0U);
            break;
        }
        assert(type == CHUNK_START_PAGE && data_len == 0U && job->page_count < MAX_JOB_PAGES);
        ZjsPage *page = &job->pages[job->page_count++];
        page->start = chunk;
        page->item_count = items;
        for (uint32_t i = 0; i < items; i++) {
            page->items[i][0] = read_be16(blob->data + chunk + 16U + i * 12U + 4U);
            page->items[i][1] = read_be32(blob->data + chunk + 16U + i * 12U + 8U);
        }
        page->video_x = page->items[10][1];
        page->video_y = page->items[11][1];

        size_t bih = position;
        assert(read_chunk(blob, &position, &items, &data_len) == CHUNK_JBIG_BIH);
        assert(items == 0U && data_len == 20U);
        page->jbig = malloc(blob->len);
        assert(page->jbig != NULL);
        memcpy(page->jbig, blob->data + bih + 16U, 20U);
        page->jbig_len = 20U;
        size_t previous = 0;
        while (blob->len - position >= 16U && read_be32(blob->data + position + 4U) == CHUNK_JBIG_BID) {
            size_t bid = position;
            assert(previous == 0U || previous == 65536U);
            assert(read_chunk(blob, &position, &items, &data_len) == CHUNK_JBIG_BID && items == 0U);
            memcpy(page->jbig + page->jbig_len, blob->data + bid + 16U, data_len);
            page->jbig_len += data_len;
            previous = data_len;
        }
        assert(previous > 0U && previous % 4U == 0U);
        assert(read_chunk(blob, &position, &items, &data_len) == CHUNK_END_JBIG && items == 0U && data_len == 0U);
        assert(read_chunk(blob, &position, &items, &data_len) == CHUNK_END_PAGE && items == 0U && data_len == 0U);
        page->end = position;
    }
    assert(blob->len - position == sizeof(pjl_trailer) - 1U);
    assert(memcmp(blob->data + position, pjl_trailer, sizeof(pjl_trailer) - 1U) == 0);
}

static void free_job(ZjsJob *job) {
    for (size_t i = 0; i < job->page_count; i++) free(job->pages[i].jbig);
    job->page_count = 0;
}

static void expect_job(const FilterResult *result, ZjsJob *job) {
    if (result->status != 0 || contains(&result->diagnostics, "ERROR:")) {
        fprintf(stderr, "%.*s\n", (int)result->diagnostics.len, (const char *)result->diagnostics.data);
        assert(0 && "filter job failed");
    }
    parse_job(&result->output, job);
}

static void expect_rejection(const FilterResult *result, const char *diagnostic) {
    if (result->status != 1 || !contains(&result->diagnostics, "ERROR:") ||
        !contains(&result->diagnostics, diagnostic)) {
        fprintf(stderr, "expected \"%s\", got status %d:\n%.*s\n", diagnostic, result->status,
                (int)result->diagnostics.len, (const char *)result->diagnostics.data);
        assert(0 && "filter did not reject the job");
    }
}

static int same_jbig(const ZjsPage *a, const ZjsPage *b) {
    return a->jbig_len == b->jbig_len && memcmp(a->jbig, b->jbig, a->jbig_len) == 0;
}

static int has_item(const ZjsPage *page, uint32_t id, uint32_t value) {
    for (uint32_t i = 0; i < page->item_count; i++) {
        if (page->items[i][0] == id) return page->items[i][1] == value;
    }
    return 0;
}

static void test_end_to_end_gray(const char *filter) {
    RasterPage page = gray_page(48, 10);
    FilterResult result = run_pages(filter, &page, 1, "1", "PageSize=A4 Resolution=1200x600dpi");
    ZjsJob job;
    expect_job(&result, &job);
    assert(job.page_count == 1U && job.collate == 0U && job.pause_after < 0);
    assert(contains(&result.diagnostics, "PAGE: 1 1\n"));

    /* START_DOC carries DMCOLLATE only; END_DOC is a bare header. */
    static const unsigned char start_doc[] = {
        0, 0, 0, 28, 0, 0, 0, 0, 0, 0, 0, 1, 0, 12, 0x5a, 0x5a, 0, 0, 0, 12, 0, 1, 1, 0, 0, 0, 0, 0
    };
    static const unsigned char end_doc[] = {0, 0, 0, 16, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0x5a, 0x5a};
    size_t header_len = sizeof(pjl_header) - 1U;
    size_t trailer_at = result.output.len - (sizeof(pjl_trailer) - 1U);
    assert(memcmp(result.output.data + header_len, start_doc, sizeof(start_doc)) == 0);
    assert(memcmp(result.output.data + trailer_at - sizeof(end_doc), end_doc, sizeof(end_doc)) == 0);

    /* START_PAGE items in the protocol's order; VIDEO_Y is rounded up to 12 lines. */
    static const uint32_t items[13][2] = {
        {3, 9}, {4, 1}, {5, 7}, {6, 1}, {7, 1}, {8, 600}, {9, 600},
        {12, 96}, {13, 12}, {16, 2}, {17, 48}, {18, 12}, {23, 0}
    };
    const ZjsPage *zjs = &job.pages[0];
    assert(zjs->item_count == 13U && memcmp(zjs->items, items, sizeof(items)) == 0);
    assert(read_be32(result.output.data + zjs->start) == 16U + 13U * 12U);

    /* BIH: one plane, Xd = 96 bits rounded to 128, Yd 12, L0 128, order 3, options 0x5c. */
    static const unsigned char bih[] = {0, 0, 1, 0, 0, 0, 0, 128, 0, 0, 0, 12, 0, 0, 0, 128, 0, 0, 3, 0x5c};
    assert(memcmp(zjs->jbig, bih, sizeof(bih)) == 0 && zjs->jbig_len > sizeof(bih));
    free_job(&job);
    free_result(&result);
    printf("    [PASS] End-to-End 8-Bit Gray Raster to PJL/ZjStream Job Test\n");
}

static void test_formats(const char *filter) {
    RasterPage pages[3] = {gray_page(100, 25), gray_page(37, 12), legacy_page(61, 13)};
    pages[0].compression = 0;
    FilterResult result = run_pages(filter, pages, 3, "3", "");
    ZjsJob job;
    expect_job(&result, &job);
    assert(job.page_count == 3U);
    /* 600 dpi: one bit per pixel. */
    assert(has_item(&job.pages[0], 16, 1) && has_item(&job.pages[0], 12, 100) && job.pages[0].video_y == 36U);
    /* FastRes 1200: two bits per pixel; gray copies come from NumCopies. */
    assert(has_item(&job.pages[1], 16, 2) && has_item(&job.pages[1], 12, 74) && has_item(&job.pages[1], 4, 1));
    /* Legacy 1200x600 1-bit: VIDEO_X = ceil(61 / 2), copies from argv[4]. */
    assert(has_item(&job.pages[2], 16, 2) && job.pages[2].video_x == 31U && job.pages[2].video_y == 24U);
    assert(has_item(&job.pages[2], 4, 3) && contains(&result.diagnostics, "PAGE: 3 3\n"));
    free_job(&job);
    free_result(&result);
    printf("    [PASS] 600 dpi Gray, FastRes 1200 Gray and Legacy 1200x600 1-bit Format Test\n");
}

static void test_solid_pages(const char *filter) {
    /* Gray black is all ones and white all zeros in the active VIDEO_X * 2
     * bits: the same bitmaps as legacy 1-bit pages of twice the width. */
    RasterPage gray[2] = {gray_page(37, 29), gray_page(37, 29)};
    RasterPage legacy[2] = {legacy_page(74, 29), legacy_page(74, 29)};
    gray[0].fill = legacy[0].fill = FILL_BLACK;
    gray[1].fill = legacy[1].fill = FILL_WHITE;
    FilterResult a = run_pages(filter, gray, 2, "1", "");
    FilterResult b = run_pages(filter, legacy, 2, "1", "");
    ZjsJob a_job;
    ZjsJob b_job;
    expect_job(&a, &a_job);
    expect_job(&b, &b_job);
    for (size_t i = 0; i < 2U; i++) {
        size_t len = a_job.pages[i].end - a_job.pages[i].start;
        assert(len == b_job.pages[i].end - b_job.pages[i].start);
        assert(memcmp(a.output.data + a_job.pages[i].start, b.output.data + b_job.pages[i].start, len) == 0);
    }
    assert(!same_jbig(&a_job.pages[0], &a_job.pages[1]));
    free_job(&a_job);
    free_job(&b_job);
    free_result(&a);
    free_result(&b);
    printf("    [PASS] Solid Black/White Page Screening Test\n");
}

static void test_empty_and_truncated(const char *filter) {
    char *path = temp_path("raster");
    write_raster(path, NULL, 0);
    FilterResult result = run_filter(filter, path, "1", "");
    assert(result.status == 0 && result.output.len == 0U && !contains(&result.diagnostics, "ERROR:"));
    free_result(&result);
    printf("    [PASS] Zero-Page CUPS Raster Job Test\n");

    RasterPage page = gray_page(40, 13);
    write_raster(path, &page, 1);
    struct stat st;
    assert(stat(path, &st) == 0 && st.st_size == (off_t)(4U + sizeof(cups_page_header2_t) + 40U * 13U));
    assert(truncate(path, st.st_size - 1) == 0);
    result = run_filter(filter, path, "1", "");
    expect_rejection(&result, "Truncated raster data on page 1");
    /* The job header was sent, so the job is closed. */
    ZjsJob closed;
    parse_job(&result.output, &closed);
    assert(closed.page_count == 0U);
    free_result(&result);
    printf("    [PASS] Truncated CUPS Raster Rejection Test (job closed with END_DOC, EOJ, USTATUSOFF)\n");

    assert(truncate(path, 4 + (off_t)sizeof(cups_page_header2_t) / 2) == 0);
    result = run_filter(filter, path, "1", "");
    expect_rejection(&result, "Truncated CUPS raster stream");
    free_result(&result);
    printf("    [PASS] Truncated CUPS Raster Header Rejection Test\n");

    assert(truncate(path, 1) == 0);
    result = run_filter(filter, path, "1", "");
    expect_rejection(&result, "Unable to open CUPS raster stream");
    free_result(&result);
    assert(truncate(path, 0) == 0);
    result = run_filter(filter, path, "1", "");
    expect_rejection(&result, "Unable to open CUPS raster stream");
    free_result(&result);
    printf("    [PASS] Corrupt and Empty CUPS Raster Rejection Test\n");
    remove_path(path);
}

/* A job that fails on its last page still ends cleanly: whole pages and
 * whole automatic-duplex sheets that were sent, then END_DOC and the PJL
 * trailer, never part of a page. Pages still being encoded when the error
 * comes are dropped, so up to `pages_sent` pages arrive, in order. Manual
 * duplex sends nothing until the end, so its failed job is empty. */
static void test_failed_job_closed(const char *filter) {
    static const struct {
        unsigned int duplex;
        const char *options;
        size_t pages_sent;
    } cases[] = {{0, "", 2}, {1, "", 2}, {1, "DupUnit=False", 0}};
    char *path = temp_path("raster");
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        RasterPage pages[3];
        for (unsigned int i = 0; i < 3U; i++) {
            pages[i] = gray_page(64, 12U * (i + 1U));
            pages[i].duplex = cases[c].duplex;
            pages[i].fill = FILL_NOISE;
            pages[i].seed = i + 1U;
        }
        write_raster(path, pages, 3);
        struct stat st;
        assert(stat(path, &st) == 0 && truncate(path, st.st_size - 1) == 0);
        FilterResult result = run_filter(filter, path, "1", cases[c].options);
        expect_rejection(&result, "Truncated raster data on page 3");
        ZjsJob job;
        parse_job(&result.output, &job);
        assert(job.page_count <= cases[c].pages_sent && job.pause_after < 0);
        if (cases[c].duplex && cases[c].options[0] == '\0') assert(job.page_count % 2U == 0U);
        /* Simplex 1, 2; automatic duplex sheet 2, 1 (VIDEO_Y 24, then 12). */
        for (size_t i = 0; i < job.page_count; i++) {
            assert(job.pages[i].video_y == 12U * (cases[c].duplex ? 2U - i : i + 1U));
        }
        free_job(&job);
        free_result(&result);
    }
    remove_path(path);
    printf("    [PASS] Failed Job Closed at a Page Boundary (simplex, automatic and manual duplex)\n");
}

/* Canceling a job: cupsd sends SIGTERM and stops the filter before this one,
 * so the raster input ends in the middle of a page. The job is closed after
 * the last whole page (END_DOC and the job trailer, no part of a page,
 * no ERROR) instead of dying mid-page. Automatic duplex holds page 1 for its
 * back side, so nothing of that sheet is sent. */
static void test_canceled_job_closed(const char *filter) {
    static const struct {
        unsigned int duplex;
        size_t pages_sent;
    } cases[] = {{0, 1}, {1, 0}};
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        RasterPage pages[2];
        for (unsigned int i = 0; i < 2U; i++) {
            pages[i] = gray_page(64, 24);
            pages[i].duplex = cases[c].duplex;
            pages[i].fill = FILL_NOISE;
            pages[i].seed = i + 1U;
        }
        char *raster_path = temp_path("raster");
        write_raster(raster_path, pages, 2);
        Blob raster = read_path(raster_path);
        remove_path(raster_path);
        const size_t page_bytes = sizeof(cups_page_header2_t) + 64U * 24U;
        assert(raster.len == 4U + 2U * page_bytes);
        /* Page 1 whole, then page 2's header and half of its lines. */
        const size_t sent = 4U + page_bytes + sizeof(cups_page_header2_t) + 64U * 12U;

        char *output_path = temp_path("zjs");
        char *diagnostics_path = temp_path("diagnostics");
        int input[2];
        assert(pipe(input) == 0);
        fflush(stdout);
        pid_t pid = fork();
        assert(pid >= 0);
        if (pid == 0) {
            int out_fd = open(output_path, O_WRONLY | O_TRUNC);
            int err_fd = open(diagnostics_path, O_WRONLY | O_TRUNC);
            if (out_fd < 0 || err_fd < 0 || dup2(out_fd, STDOUT_FILENO) < 0 ||
                dup2(err_fd, STDERR_FILENO) < 0 || dup2(input[0], STDIN_FILENO) < 0 || unsetenv("PPD") != 0) {
                _exit(126);
            }
            close(out_fd);
            close(err_fd);
            close(input[0]);
            close(input[1]);
            alarm(120);
            execl(filter, filter, "1", "test-user", TITLE, "1", "", (char *)NULL);
            _exit(127);
        }
        close(input[0]);
        assert(write(input[1], raster.data, sent) == (ssize_t)sent);
        /* Page 1 is rendered (its PAGE: line); the filter waits for page 2. */
        int rendered = 0;
        for (int tries = 0; tries < 6000 && !rendered; tries++) {
            Blob diagnostics = read_path(diagnostics_path);
            rendered = contains(&diagnostics, "PAGE: 1 ");
            free(diagnostics.data);
            if (!rendered) usleep(10000);
        }
        assert(rendered);
        assert(kill(pid, SIGTERM) == 0);
        usleep(200000);
        close(input[1]);  /* cupsd stopped the filter before this one */
        int status = 0;
        assert(waitpid(pid, &status, 0) == pid);
        FilterResult result = {
            .status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status),
            .output = read_path(output_path),
            .diagnostics = read_path(diagnostics_path),
        };
        remove_path(output_path);
        remove_path(diagnostics_path);
        if (result.status > 1 || contains(&result.diagnostics, "ERROR:") ||
            !contains(&result.diagnostics, "Job canceled") || contains(&result.diagnostics, "Sanitizer") ||
            contains(&result.diagnostics, "runtime error")) {
            fprintf(stderr, "canceled filter status %d:\n%.*s\n", result.status, (int)result.diagnostics.len,
                    (const char *)result.diagnostics.data);
            assert(0 && "the canceled job was not closed");
        }
        ZjsJob job;
        parse_job(&result.output, &job);
        assert(job.page_count <= cases[c].pages_sent && job.pause_after < 0);
        for (size_t i = 0; i < job.page_count; i++) assert(job.pages[i].video_y == 24U);
        free_job(&job);
        free_result(&result);
        free(raster.data);
    }
    printf("    [PASS] Canceled Job (SIGTERM) Closed at a Page Boundary (simplex, automatic duplex)\n");
}

static void test_header_rejections(const char *filter) {
    static const unsigned int models[] = {0, 1010};
    for (size_t i = 0; i < 2U; i++) {
        RasterPage page = gray_page(20, 5);
        page.model = models[i];
        FilterResult result = run_pages(filter, &page, 1, "1", "");
        expect_rejection(&result, "Raster was not prepared by the HP LaserJet P1600dn PPD on page 1");
        assert(result.output.len == 0U);
        free_result(&result);
    }
    RasterPage pages[2] = {gray_page(20, 5), gray_page(20, 5)};
    pages[1].model = 1010;
    FilterResult later = run_pages(filter, pages, 2, "1", "");
    expect_rejection(&later, "on page 2");
    free_result(&later);
    printf("    [PASS] HP P1600dn Model Number Rejection Test\n");

    RasterPage invalid[4] = {gray_page(20, 5), gray_page(20, 5), gray_page(20, 5), legacy_page(20, 5)};
    invalid[0].resolution_y = 1200;
    invalid[1].resolution_x = 300;
    invalid[2].num_colors = 3;
    invalid[3].color_space = CUPS_CSPACE_W;
    static const char *const reasons[] = {
        "Unsupported raster resolution or page size", "Unsupported raster format",
        "Unsupported raster color layout", "Unsupported raster format"
    };
    for (size_t i = 0; i < 4U; i++) {
        FilterResult result = run_pages(filter, &invalid[i], 1, "1", "");
        expect_rejection(&result, reasons[i]);
        free_result(&result);
    }
    RasterPage line = gray_page(100, 5);
    line.bytes_per_line = 129;
    FilterResult result = run_pages(filter, &line, 1, "1", "");
    expect_rejection(&result, "Invalid raster line length");
    free_result(&result);
    printf("    [PASS] Unsupported Resolution, Format, Color Layout and Line Length Rejection Test\n");
}

static void test_paper_codes(const char *filter) {
    static const unsigned int sizes[][3] = {{595, 842, 9}, {612, 792, 1}, {612, 1008, 5}, {553, 765, 264}, {300, 500, 256}};
    RasterPage pages[5];
    for (size_t i = 0; i < 5U; i++) {
        pages[i] = gray_page(20, 5);
        pages[i].page_size[0] = sizes[i][0];
        pages[i].page_size[1] = sizes[i][1];
    }
    FilterResult result = run_pages(filter, pages, 5, "1", "");
    ZjsJob job;
    expect_job(&result, &job);
    for (size_t i = 0; i < 5U; i++) assert(has_item(&job.pages[i], 3, sizes[i][2]));
    free_job(&job);
    free_result(&result);
    printf("    [PASS] DMPAPER Mapping Test (A4, Letter, Legal, 16K 264 and Custom 256)\n");
}

static void expect_order(const ZjsJob *job, const uint32_t *heights, size_t count, int pause_after, int automatic) {
    assert(job->page_count == count && job->pause_after == pause_after);
    for (size_t i = 0; i < count; i++) {
        assert(job->pages[i].video_y == heights[i]);
        assert(job->pages[i].item_count == (automatic ? 14U : 13U));
        assert(!automatic || (job->pages[i].items[12][0] == 2U && job->pages[i].items[12][1] == 2U));
    }
}

static void test_duplex(const char *filter) {
    /* Page n is 12 * n video lines high, so the order shows in VIDEO_Y. */
    RasterPage pages[4];
    for (unsigned int i = 0; i < 4U; i++) {
        pages[i] = gray_page(30, 12U * (i + 1U) - 2U);
        pages[i].duplex = 1;
    }
    RasterPage white = pages[2];
    white.fill = FILL_WHITE;
    white.duplex = 0;
    FilterResult blank_result = run_pages(filter, &white, 1, "1", "");
    ZjsJob blank;
    expect_job(&blank_result, &blank);

    /* Automatic: 2, 1, blank partner, 3 with DMDUPLEX 2 on every page. */
    FilterResult result = run_pages(filter, pages, 3, "1", "");
    ZjsJob job;
    expect_job(&result, &job);
    static const uint32_t automatic[] = {24, 12, 36, 36};
    expect_order(&job, automatic, 4, -1, 1);
    assert(same_jbig(&job.pages[2], &blank.pages[0]) && !same_jbig(&job.pages[3], &blank.pages[0]));
    assert(contains(&result.diagnostics, "PAGE: 3 1\n") && !contains(&result.diagnostics, "PAGE: 4"));
    free_job(&job);
    free_result(&result);

    /* Manual (no duplex unit): even pages descending, PAUSE, odd pages. */
    result = run_pages(filter, pages, 4, "1", "DupUnit=False");
    expect_job(&result, &job);
    static const uint32_t manual4[] = {48, 24, 12, 36};
    expect_order(&job, manual4, 4, 2, 0);
    free_job(&job);
    free_result(&result);
    result = run_pages(filter, pages, 3, "1", "DupUnit=False");
    expect_job(&result, &job);
    static const uint32_t manual3[] = {36, 24, 12, 36};
    expect_order(&job, manual3, 4, 2, 0);
    assert(same_jbig(&job.pages[0], &blank.pages[0]));
    free_job(&job);
    free_result(&result);
    result = run_pages(filter, pages, 1, "1", "DupUnit=False");
    expect_job(&result, &job);
    static const uint32_t manual1[] = {12};
    expect_order(&job, manual1, 1, -1, 0);
    free_job(&job);
    free_result(&result);

    /* Envelopes cannot use the duplex unit. */
    for (size_t i = 0; i < 2U; i++) {
        pages[i].page_size[0] = 297;
        pages[i].page_size[1] = 684;
    }
    result = run_pages(filter, pages, 2, "1", "");
    expect_job(&result, &job);
    static const uint32_t envelope[] = {24, 12};
    expect_order(&job, envelope, 2, 1, 0);
    free_job(&job);
    free_result(&result);
    free_job(&blank);
    free_result(&blank_result);
    printf("    [PASS] Automatic and Manual Duplex Sheet Order with Blank Partner Test\n");

    /* Legacy long-edge back sides are turned by the filter. */
    RasterPage legacy[2] = {legacy_page(50, 9), legacy_page(56, 21)};
    legacy[0].duplex = legacy[1].duplex = 1;
    RasterPage reference[2] = {legacy[0], legacy[1]};
    reference[0].duplex = reference[1].duplex = 0;
    reference[1].rotated = 1;
    result = run_pages(filter, legacy, 2, "1", "");
    expect_job(&result, &job);
    FilterResult reference_result = run_pages(filter, reference, 2, "1", "");
    ZjsJob reference_job;
    expect_job(&reference_result, &reference_job);
    assert(job.page_count == 2U && job.pages[0].video_x == 28U && job.pages[1].video_x == 25U);
    assert(same_jbig(&job.pages[0], &reference_job.pages[1]) && same_jbig(&job.pages[1], &reference_job.pages[0]));
    free_job(&job);
    free_job(&reference_job);
    free_result(&result);
    free_result(&reference_result);
    printf("    [PASS] Legacy Long-Edge Back-Side Rotation Test\n");
}

static void test_duplex_change(const char *filter) {
    const char *reason = "Duplex mode changed inside the raster job on page 2";
    RasterPage changed[2] = {gray_page(20, 5), gray_page(20, 5)};
    changed[0].duplex = changed[1].duplex = 1;
    changed[1].tumble = 1;
    FilterResult result = run_pages(filter, changed, 2, "1", "");
    expect_rejection(&result, reason);
    free_result(&result);
    changed[1].duplex = 0;
    changed[1].tumble = 0;
    result = run_pages(filter, changed, 2, "1", "");
    expect_rejection(&result, reason);
    free_result(&result);
    printf("    [PASS] Duplex Mode Change Rejection Test\n");
}

static void test_arguments(const char *filter) {
    RasterPage page = legacy_page(20, 5);
    static const char *const copies[] = {"not-a-number", "0", "+1", "", "999999999999999999999999"};
    for (size_t i = 0; i < sizeof(copies) / sizeof(copies[0]); i++) {
        FilterResult result = run_pages(filter, &page, 1, copies[i], "");
        expect_rejection(&result, "Invalid copy count");
        free_result(&result);
    }
    printf("    [PASS] Invalid Copy Count Rejection Test\n");

    char *missing = temp_path("missing");
    assert(unlink(missing) == 0);
    FilterResult result = run_filter(filter, missing, "1", "");
    expect_rejection(&result, "Unable to open raster file");
    free_result(&result);
    free(missing);

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        int null_fd = open("/dev/null", O_RDWR);
        if (null_fd < 0 || dup2(null_fd, STDOUT_FILENO) < 0 || dup2(null_fd, STDERR_FILENO) < 0) _exit(126);
        execl(filter, filter, "1", "test-user", TITLE, "1", (char *)NULL);
        _exit(127);
    }
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 1);
    printf("    [PASS] Missing Input and Argument Count Rejection Test\n");
}

int main(int argc, char **argv) {
    assert(argc == 2);
    test_end_to_end_gray(argv[1]);
    test_formats(argv[1]);
    test_solid_pages(argv[1]);
    test_paper_codes(argv[1]);
    test_duplex(argv[1]);
    test_empty_and_truncated(argv[1]);
    test_failed_job_closed(argv[1]);
    test_canceled_job_closed(argv[1]);
    test_header_rejections(argv[1]);
    test_duplex_change(argv[1]);
    test_arguments(argv[1]);
    return 0;
}
