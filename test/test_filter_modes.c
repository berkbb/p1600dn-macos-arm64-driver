/*
 * Exhaustive mode matrix for the rastertop1600dn CUPS filter (argv[1]):
 * PJL job framing, START_PAGE items for every paper/media/source/copy and
 * EconoMode option, both accepted raster formats (600 dpi 8-bit gray and
 * the legacy 1200x600 1-bit format), screening and its line phase, 64 KiB
 * BID blocking, 180-degree rotation, automatic and manual duplex sheet order,
 * rejection of every invalid header/option/argument/input/output, and
 * deterministic corrupt-raster and header-mutation fuzzing.
 *
 * JBIG is never decoded here.  JBIG is lossless, so two pages carry the same
 * bitmap exactly when their BIH and BID bytes are identical; expected bitmaps
 * are fed through the legacy 1-bit format (copied bit for bit) or through a
 * pre-rotated raster, and the encoded pages are compared byte for byte.
 */
#include <assert.h>
#include <cups/raster.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* The halftone screens (src/p1600_screen_tables.h, tools/gen_screens.py). */
#include "p1600_screen_tables.h"

#define DEFAULT_TITLE "Mode Matrix"
#define FILTER_TIMEOUT_SECONDS 120U
#define MAX_JOB_PAGES 32U
#define MAX_TEST_PAGES 8U
#define BID_BLOCK_BYTES 65536U

/* ZjStream chunk types and item ids of the P1600dn job, spelled out here
 * instead of taken from src/zjs.h so the test pins the wire values. */
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

enum {
    ITEM_DMCOLLATE = 1,
    ITEM_DMDUPLEX = 2,
    ITEM_DMPAPER = 3,
    ITEM_DMCOPIES = 4,
    ITEM_DMDEFAULTSOURCE = 5,
    ITEM_DMMEDIATYPE = 6,
    ITEM_NBIE = 7,
    ITEM_RESOLUTION_X = 8,
    ITEM_RESOLUTION_Y = 9,
    ITEM_RASTER_X = 12,
    ITEM_RASTER_Y = 13,
    ITEM_VIDEO_BPP = 16,
    ITEM_VIDEO_X = 17,
    ITEM_VIDEO_Y = 18,
    ITEM_ECONOMODE = 23
};

/* Sheet tokens: input page numbers, the blank partner and the PAUSE. */
enum { TOKEN_PAUSE = -1, TOKEN_BLANK = 0 };

static const char pjl_trailer[] =
    "\033%-12345X@PJL EOJ\r\n\033%-12345X@PJL USTATUSOFF\r\n\033%-12345X";

typedef struct {
    unsigned char *data;
    size_t len;
} Blob;

typedef struct {
    int status; /* exit status, or 128 + signal number */
    Blob output;
    Blob diagnostics;
} FilterResult;

typedef enum { TRANSPORT_FILE, TRANSPORT_STDIN_FILE, TRANSPORT_STDIN_PIPE } Transport;
typedef enum { STDOUT_FILE, STDOUT_READ_ONLY, STDOUT_CLOSED } StdoutMode;

typedef struct {
    const char *title;   /* argv[3] */
    const char *copies;  /* argv[4] */
    const char *options; /* argv[5] */
    const char *ppd;     /* $PPD for the filter, NULL to unset it */
    Transport transport;
    StdoutMode stdout_mode;
} RunOptions;

typedef enum { FILL_WHITE, FILL_BLACK, FILL_RECT, FILL_NOISE, FILL_LEVEL, FILL_RAW } Fill;

typedef struct {
    unsigned int model;
    unsigned int resolution_x;
    unsigned int resolution_y;
    unsigned int width;
    unsigned int height;
    unsigned int bits_per_color;
    unsigned int bits_per_pixel;
    unsigned int bytes_per_line;
    unsigned int color_order;
    unsigned int color_space;
    unsigned int num_colors;
    unsigned int compression;
    unsigned int page_size[2];
    unsigned int num_copies;
    unsigned int media_position;
    unsigned int cups_media_type;
    const char *media_type;
    unsigned int duplex;
    unsigned int tumble;
    unsigned int collate;
    Fill fill;
    unsigned int rect[4];       /* FILL_RECT: x, y, width, height */
    unsigned int seed;          /* FILL_NOISE */
    unsigned int level;         /* FILL_LEVEL: 0 = black .. 255 = white */
    unsigned char padding;      /* row padding beyond cupsWidth */
    int rotated;                /* content turned by 180 degrees */
    const unsigned char *raw;   /* FILL_RAW: bytes_per_line * height bytes */
} RasterPage;

typedef struct {
    uint32_t type;
    uint32_t items;
    size_t item_at;
    size_t data_at;
    size_t data_len;
} Chunk;

typedef struct {
    uint32_t paper;
    uint32_t copies;
    uint32_t source;
    uint32_t media;
    uint32_t raster_x;
    uint32_t raster_y;
    uint32_t bpp;
    uint32_t video_x;
    uint32_t video_y;
    uint32_t econo;
    int has_duplex;
    uint32_t duplex;
    unsigned char *jbig; /* 20-byte BIH followed by every BID payload byte */
    size_t jbig_len;
    size_t bid_chunks;
    size_t start;        /* byte range of the page in the job output */
    size_t end;
} ZjsPage;

typedef struct {
    uint32_t collate;
    int pause_after; /* pages written before the manual-duplex PAUSE, or -1 */
    size_t page_count;
    ZjsPage pages[MAX_JOB_PAGES];
} ZjsJob;

typedef enum { SHEETS_SIMPLEX, SHEETS_AUTOMATIC, SHEETS_MANUAL } SheetMode;

static uint32_t read_be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t read_be16(const unsigned char *p) {
    return (uint16_t)(((unsigned int)p[0] << 8) | (unsigned int)p[1]);
}

static uint32_t round_up(uint32_t value, uint32_t multiple) {
    return (value + multiple - 1U) / multiple * multiple;
}

static uint32_t xorshift(uint32_t state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

static uint32_t mix32(uint32_t value) {
    value ^= value >> 16;
    value *= 0x7feb352dU;
    value ^= value >> 15;
    value *= 0x846ca68bU;
    value ^= value >> 16;
    return value;
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
    assert(size >= 0);
    assert(fseek(fp, 0, SEEK_SET) == 0);
    blob.len = (size_t)size;
    blob.data = malloc(blob.len == 0 ? 1U : blob.len);
    assert(blob.data != NULL);
    assert(fread(blob.data, 1, blob.len, fp) == blob.len);
    assert(fclose(fp) == 0);
    return blob;
}

static void write_path(const char *path, const unsigned char *data, size_t len) {
    int fd = open(path, O_WRONLY | O_TRUNC);
    assert(fd >= 0);
    size_t written = 0;
    while (written < len) {
        ssize_t amount = write(fd, data + written, len - written);
        assert(amount > 0);
        written += (size_t)amount;
    }
    assert(close(fd) == 0);
}

static void free_blob(Blob *blob) {
    free(blob->data);
    blob->data = NULL;
    blob->len = 0;
}

static int contains(const Blob *blob, const char *text) {
    return blob->len > 0 && memmem(blob->data, blob->len, text, strlen(text)) != NULL;
}

static void free_result(FilterResult *result) {
    free_blob(&result->output);
    free_blob(&result->diagnostics);
}

static void print_diagnostics(const FilterResult *result) {
    fprintf(stderr, "filter exit status %d, stderr:\n%.*s\n", result->status,
            (int)result->diagnostics.len, (const char *)result->diagnostics.data);
}

static RunOptions run_defaults(void) {
    RunOptions run;
    memset(&run, 0, sizeof(run));
    run.title = DEFAULT_TITLE;
    run.copies = "1";
    run.options = "";
    run.transport = TRANSPORT_FILE;
    run.stdout_mode = STDOUT_FILE;
    return run;
}

/* Odd-sized writes make the filter see short reads on its pipe. */
static void feed_pipe(int fd, const Blob *data) {
    size_t written = 0;
    while (written < data->len) {
        size_t amount = data->len - written;
        if (amount > 777U) amount = 777U;
        ssize_t result = write(fd, data->data + written, amount);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) break; /* the filter stopped reading (EPIPE) */
        written += (size_t)result;
    }
}

static FilterResult run_filter(const char *filter, const char *input, const RunOptions *run) {
    char *output_path = run->stdout_mode == STDOUT_FILE ? temp_path("mode-output") : NULL;
    char *diagnostics_path = temp_path("mode-stderr");
    int pipe_fds[2] = {-1, -1};
    Blob piped = {0};
    if (run->transport == TRANSPORT_STDIN_PIPE) {
        piped = read_path(input);
        assert(pipe(pipe_fds) == 0);
    }
    fflush(stdout);
    fflush(stderr);

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        signal(SIGPIPE, SIG_DFL);
        if ((run->ppd ? setenv("PPD", run->ppd, 1) : unsetenv("PPD")) != 0) _exit(126);
        int err = open(diagnostics_path, O_WRONLY | O_TRUNC);
        if (err < 0 || dup2(err, STDERR_FILENO) < 0) _exit(126);
        close(err);
        if (run->transport == TRANSPORT_STDIN_PIPE) {
            if (dup2(pipe_fds[0], STDIN_FILENO) < 0) _exit(126);
            close(pipe_fds[0]);
            close(pipe_fds[1]);
        } else if (run->transport == TRANSPORT_STDIN_FILE) {
            int in = open(input, O_RDONLY);
            if (in < 0 || dup2(in, STDIN_FILENO) < 0) _exit(126);
            close(in);
        }
        if (run->stdout_mode == STDOUT_CLOSED) {
            /* libmalloc's debugging variables (make test-stress) open a
             * descriptor at startup that would take the closed stdout. */
            static const char *const malloc_debug[] = {"MallocScribble", "MallocPreScribble",
                                                       "MallocGuardEdges", "MallocCheckHeapStart",
                                                       "MallocCheckHeapEach"};
            for (size_t i = 0; i < sizeof(malloc_debug) / sizeof(malloc_debug[0]); i++) {
                unsetenv(malloc_debug[i]);
            }
            close(STDOUT_FILENO);
        } else {
            int out = run->stdout_mode == STDOUT_FILE ? open(output_path, O_WRONLY | O_TRUNC)
                                                      : open("/dev/null", O_RDONLY);
            if (out < 0 || dup2(out, STDOUT_FILENO) < 0) _exit(126);
            close(out);
        }
        alarm(FILTER_TIMEOUT_SECONDS);
        if (run->transport == TRANSPORT_FILE) {
            execl(filter, filter, "42", "mode-user", run->title, run->copies, run->options, input,
                  (char *)NULL);
        } else {
            execl(filter, filter, "42", "mode-user", run->title, run->copies, run->options,
                  (char *)NULL);
        }
        _exit(127);
    }

    if (run->transport == TRANSPORT_STDIN_PIPE) {
        close(pipe_fds[0]);
        feed_pipe(pipe_fds[1], &piped);
        close(pipe_fds[1]);
        free_blob(&piped);
    }
    int wait_status = 0;
    assert(waitpid(pid, &wait_status, 0) == pid);

    FilterResult result;
    memset(&result, 0, sizeof(result));
    if (WIFEXITED(wait_status)) {
        result.status = WEXITSTATUS(wait_status);
    } else {
        result.status = 128 + (WIFSIGNALED(wait_status) ? WTERMSIG(wait_status) : 0);
    }
    if (output_path) {
        result.output = read_path(output_path);
        remove_path(output_path);
    } else {
        result.output.data = malloc(1);
        assert(result.output.data != NULL);
    }
    result.diagnostics = read_path(diagnostics_path);
    remove_path(diagnostics_path);

    /* No input may crash the filter, hang it or trip a sanitizer. */
    if (result.status >= 126 || contains(&result.diagnostics, "Sanitizer") ||
        contains(&result.diagnostics, "runtime error")) {
        print_diagnostics(&result);
        assert(0 && "filter crashed, timed out or reported a sanitizer error");
    }
    return result;
}

static RasterPage gray_page(unsigned int width, unsigned int height) {
    RasterPage page;
    memset(&page, 0, sizeof(page));
    page.model = 1606;
    page.resolution_x = 600;
    page.resolution_y = 600;
    page.width = width;
    page.height = height;
    page.bits_per_color = 8;
    page.bits_per_pixel = 8;
    page.bytes_per_line = width;
    page.color_order = CUPS_ORDER_CHUNKED;
    page.color_space = CUPS_CSPACE_W;
    page.num_colors = 1;
    page.compression = 1; /* FastRes 1200, the PPD default */
    page.page_size[0] = 595;
    page.page_size[1] = 842;
    page.num_copies = 1;
    page.media_position = 7;  /* InputSlot Auto */
    page.cups_media_type = 1; /* MediaType Paper */
    page.media_type = "Paper";
    page.fill = FILL_WHITE;
    return page;
}

static RasterPage legacy_page(unsigned int width, unsigned int height) {
    RasterPage page = gray_page(width, height);
    page.resolution_x = 1200;
    page.bits_per_color = 1;
    page.bits_per_pixel = 1;
    page.bytes_per_line = (width + 7U) / 8U;
    page.color_space = CUPS_CSPACE_K;
    page.compression = 0;
    page.media_position = 0;
    page.cups_media_type = 0;
    page.media_type = "Plain";
    return page;
}

static void set_rect(RasterPage *page, unsigned int x, unsigned int y, unsigned int width,
                     unsigned int height) {
    page->fill = FILL_RECT;
    page->rect[0] = x;
    page->rect[1] = y;
    page->rect[2] = width;
    page->rect[3] = height;
}

/* Darkness of content pixel (x, y): 0 = white .. 255 = black. */
static unsigned int content_darkness(const RasterPage *page, unsigned int x, unsigned int y) {
    if (page->rotated) {
        x = page->width - 1U - x;
        y = page->height - 1U - y;
    }
    switch (page->fill) {
    case FILL_BLACK:
        return 255U;
    case FILL_RECT:
        return x >= page->rect[0] && x - page->rect[0] < page->rect[2] && y >= page->rect[1] &&
                       y - page->rect[1] < page->rect[3]
                   ? 255U
                   : 0U;
    case FILL_NOISE:
        return mix32(page->seed * 0x9e3779b9U ^ mix32(x * 0x85ebca6bU ^ mix32(y + 0x1606U))) & 0xffU;
    case FILL_LEVEL:
        return 255U - (page->level & 0xffU);
    case FILL_WHITE:
    case FILL_RAW:
    default:
        return 0U;
    }
}

static void build_row(const RasterPage *page, unsigned int y, unsigned char *row) {
    size_t bytes = page->bytes_per_line;
    if (page->fill == FILL_RAW) {
        memcpy(row, page->raw + (size_t)y * bytes, bytes);
        return;
    }
    memset(row, page->padding, bytes);
    if (page->bits_per_pixel == 8U) {
        for (unsigned int x = 0; x < page->width && x < bytes; x++) {
            row[x] = (unsigned char)(255U - content_darkness(page, x, y));
        }
    } else if (page->bits_per_pixel == 1U) {
        for (unsigned int x = 0; x < page->width && x / 8U < bytes; x++) {
            unsigned char mask = (unsigned char)(0x80U >> (x & 7U));
            if (content_darkness(page, x, y) >= 128U) {
                row[x / 8U] |= mask;
            } else {
                row[x / 8U] &= (unsigned char)~mask;
            }
        }
    }
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
        header.cupsBitsPerColor = page->bits_per_color;
        header.cupsBitsPerPixel = page->bits_per_pixel;
        header.cupsBytesPerLine = page->bytes_per_line;
        header.cupsColorOrder = (cups_order_t)page->color_order;
        header.cupsColorSpace = (cups_cspace_t)page->color_space;
        header.cupsNumColors = page->num_colors;
        header.cupsCompression = page->compression;
        header.cupsInteger[0] = page->model;
        header.PageSize[0] = page->page_size[0];
        header.PageSize[1] = page->page_size[1];
        header.NumCopies = page->num_copies;
        header.MediaPosition = page->media_position;
        header.cupsMediaType = page->cups_media_type;
        header.Duplex = page->duplex ? CUPS_TRUE : CUPS_FALSE;
        header.Tumble = page->tumble ? CUPS_TRUE : CUPS_FALSE;
        header.Collate = page->collate ? CUPS_TRUE : CUPS_FALSE;
        if (page->media_type) {
            snprintf(header.MediaType, sizeof(header.MediaType), "%s", page->media_type);
        }
        assert(cupsRasterWriteHeader2(raster, &header));
        if (page->bytes_per_line == 0U || page->height == 0U) continue;

        unsigned char *row = malloc(page->bytes_per_line);
        assert(row != NULL);
        for (unsigned int y = 0; y < page->height; y++) {
            build_row(page, y, row);
            assert(cupsRasterWritePixels(raster, row, page->bytes_per_line) == page->bytes_per_line);
        }
        free(row);
    }
    cupsRasterClose(raster);
    assert(close(fd) == 0);
}

static FilterResult run_pages(const char *filter, const RasterPage *pages, size_t count,
                              const RunOptions *run) {
    RunOptions defaults = run_defaults();
    char *path = temp_path("mode-raster");
    write_raster(path, pages, count);
    FilterResult result = run_filter(filter, path, run ? run : &defaults);
    remove_path(path);
    return result;
}

static void expected_pjl(char *buffer, size_t size, const char *job_name, int density, int jam_auto) {
    int length = snprintf(buffer, size,
                          "\033%%-12345X@PJL USTATUS JOB=ON\r\n"
                          "@PJL USTATUS PAGE=ON\r\n"
                          "@PJL USTATUS DEVICE=ON\r\n"
                          "@PJL INFO STATUS\r\n"
                          "@PJL JOB NAME=%s\r\n"
                          "@PJL SET DENSITY = %d\r\n"
                          "@PJL SET JAMRECOVERY = %s\r\n"
                          "@PJL ENTER LANGUAGE=ZJS\r\n",
                          job_name, density, jam_auto ? "AUTO" : "OFF");
    assert(length > 0 && (size_t)length < size);
}

/* Reads one chunk header (16 bytes, big-endian) and its uint32 items. */
static Chunk read_chunk(const Blob *blob, size_t *position) {
    assert(*position <= blob->len && blob->len - *position >= 16U);
    const unsigned char *p = blob->data + *position;
    uint32_t size = read_be32(p);
    Chunk chunk;
    chunk.type = read_be32(p + 4);
    chunk.items = read_be32(p + 8);
    assert(read_be16(p + 14) == 0x5a5aU);
    assert(chunk.items <= 32U);
    assert(read_be16(p + 12) == chunk.items * 12U);
    assert(size >= 16U + chunk.items * 12U && size <= blob->len - *position);
    chunk.item_at = *position + 16U;
    chunk.data_at = chunk.item_at + (size_t)chunk.items * 12U;
    chunk.data_len = size - 16U - chunk.items * 12U;
    for (uint32_t i = 0; i < chunk.items; i++) {
        const unsigned char *item = blob->data + chunk.item_at + (size_t)i * 12U;
        assert(read_be32(item) == 12U && item[6] == 1U && item[7] == 0U);
    }
    *position += size;
    return chunk;
}

static uint16_t item_id(const Blob *blob, const Chunk *chunk, uint32_t index) {
    assert(index < chunk->items);
    return read_be16(blob->data + chunk->item_at + (size_t)index * 12U + 4U);
}

static uint32_t item_value(const Blob *blob, const Chunk *chunk, uint32_t index) {
    assert(index < chunk->items);
    return read_be32(blob->data + chunk->item_at + (size_t)index * 12U + 8U);
}

static void expect_empty_chunk(const Blob *blob, size_t *position, uint32_t type) {
    Chunk chunk = read_chunk(blob, position);
    assert(chunk.type == type && chunk.items == 0U && chunk.data_len == 0U);
}

/* START_PAGE (fixed item order), BIH, 64 KiB BID blocks, END_JBIG, END_PAGE. */
static void parse_page(const Blob *blob, const Chunk *start, size_t page_start, size_t *position,
                       ZjsPage *page) {
    static const uint16_t order[] = {
        ITEM_DMPAPER, ITEM_DMCOPIES, ITEM_DMDEFAULTSOURCE, ITEM_DMMEDIATYPE,
        ITEM_NBIE, ITEM_RESOLUTION_X, ITEM_RESOLUTION_Y, ITEM_RASTER_X,
        ITEM_RASTER_Y, ITEM_VIDEO_BPP, ITEM_VIDEO_X, ITEM_VIDEO_Y
    };
    uint32_t values[12];
    assert(start->data_len == 0U && (start->items == 13U || start->items == 14U));
    for (uint32_t i = 0; i < 12U; i++) {
        assert(item_id(blob, start, i) == order[i]);
        values[i] = item_value(blob, start, i);
    }
    memset(page, 0, sizeof(*page));
    page->paper = values[0];
    page->copies = values[1];
    page->source = values[2];
    page->media = values[3];
    page->raster_x = values[7];
    page->raster_y = values[8];
    page->bpp = values[9];
    page->video_x = values[10];
    page->video_y = values[11];
    uint32_t next = 12U;
    if (start->items == 14U) {
        assert(item_id(blob, start, 12U) == ITEM_DMDUPLEX);
        page->has_duplex = 1;
        page->duplex = item_value(blob, start, 12U);
        assert(page->duplex == 2U);
        next = 13U;
    }
    assert(item_id(blob, start, next) == ITEM_ECONOMODE);
    page->econo = item_value(blob, start, next);
    assert(page->econo <= 1U);
    assert(values[4] == 1U && values[5] == 600U && values[6] == 600U);
    assert(page->bpp == 1U || page->bpp == 2U);
    assert(page->video_x > 0U && page->video_y > 0U && page->video_y % 12U == 0U);
    assert(page->raster_x == page->video_x * page->bpp && page->raster_y == page->video_y);

    Chunk bih = read_chunk(blob, position);
    assert(bih.type == CHUNK_JBIG_BIH && bih.items == 0U && bih.data_len == 20U);
    const unsigned char *b = blob->data + bih.data_at;
    assert(b[0] == 0U && b[1] == 0U && b[2] == 1U && b[3] == 0U);
    assert(read_be32(b + 4) == round_up(page->raster_x, 128U));
    assert(read_be32(b + 8) == page->video_y);
    assert(read_be32(b + 12) == 128U);
    assert(b[16] == 0U && b[17] == 0U && b[18] == 3U && b[19] == 0x5cU);

    size_t capacity = 20U;
    page->jbig = malloc(capacity);
    assert(page->jbig != NULL);
    memcpy(page->jbig, b, 20U);
    page->jbig_len = 20U;
    size_t previous = 0;
    while (blob->len - *position >= 16U && read_be32(blob->data + *position + 4U) == CHUNK_JBIG_BID) {
        Chunk bid = read_chunk(blob, position);
        assert(bid.items == 0U);
        /* Every BID chunk but the last carries exactly 64 KiB. */
        assert(page->bid_chunks == 0U || previous == BID_BLOCK_BYTES);
        if (page->jbig_len + bid.data_len > capacity) {
            capacity = (page->jbig_len + bid.data_len) * 2U;
            unsigned char *grown = realloc(page->jbig, capacity);
            assert(grown != NULL);
            page->jbig = grown;
        }
        memcpy(page->jbig + page->jbig_len, blob->data + bid.data_at, bid.data_len);
        page->jbig_len += bid.data_len;
        previous = bid.data_len;
        page->bid_chunks++;
    }
    /* The last one is zero-padded to a multiple of four bytes: at most three
     * zero bytes follow the final stripe's SDNORM marker (FF 02). */
    assert(page->bid_chunks > 0U && previous > 0U && previous <= BID_BLOCK_BYTES && previous % 4U == 0U);
    size_t padding = 0;
    while (padding < page->jbig_len && page->jbig[page->jbig_len - 1U - padding] == 0U) padding++;
    assert(padding <= 3U && page->jbig_len - padding >= 22U);
    assert(page->jbig[page->jbig_len - padding - 2U] == 0xffU && page->jbig[page->jbig_len - padding - 1U] == 0x02U);
    expect_empty_chunk(blob, position, CHUNK_END_JBIG);
    expect_empty_chunk(blob, position, CHUNK_END_PAGE);
    page->start = page_start;
    page->end = *position;
}

static void parse_job(const Blob *blob, const char *pjl, ZjsJob *job) {
    memset(job, 0, sizeof(*job));
    job->pause_after = -1;
    size_t pjl_len = strlen(pjl);
    if (blob->len < pjl_len + 4U || memcmp(blob->data, pjl, pjl_len) != 0) {
        fprintf(stderr, "expected PJL header:\n%s\nactual output starts with:\n%.*s\n", pjl,
                (int)(blob->len < pjl_len ? blob->len : pjl_len), (const char *)blob->data);
        assert(0 && "PJL job header mismatch");
    }
    size_t position = pjl_len;
    assert(memcmp(blob->data + position, "JZJZ", 4) == 0);
    position += 4U;

    Chunk doc = read_chunk(blob, &position);
    assert(doc.type == CHUNK_START_DOC && doc.items == 1U && doc.data_len == 0U);
    assert(item_id(blob, &doc, 0) == ITEM_DMCOLLATE);
    job->collate = item_value(blob, &doc, 0);
    assert(job->collate <= 1U);

    for (;;) {
        size_t chunk_start = position;
        Chunk chunk = read_chunk(blob, &position);
        if (chunk.type == CHUNK_START_PAGE) {
            assert(job->page_count < MAX_JOB_PAGES);
            parse_page(blob, &chunk, chunk_start, &position, &job->pages[job->page_count++]);
        } else if (chunk.type == CHUNK_PAUSE) {
            assert(chunk.items == 0U && chunk.data_len == 0U && job->pause_after < 0);
            job->pause_after = (int)job->page_count;
        } else {
            assert(chunk.type == CHUNK_END_DOC && chunk.items == 0U && chunk.data_len == 0U);
            break;
        }
    }
    size_t trailer_len = sizeof(pjl_trailer) - 1U;
    assert(blob->len - position == trailer_len);
    assert(memcmp(blob->data + position, pjl_trailer, trailer_len) == 0);
}

static void free_job(ZjsJob *job) {
    for (size_t i = 0; i < job->page_count; i++) free(job->pages[i].jbig);
    job->page_count = 0;
}

static void expect_job(const FilterResult *result, const char *pjl, ZjsJob *job) {
    char fallback[512];
    if (!pjl) {
        expected_pjl(fallback, sizeof(fallback), DEFAULT_TITLE, 3, 0);
        pjl = fallback;
    }
    if (result->status != 0 || contains(&result->diagnostics, "ERROR:")) {
        print_diagnostics(result);
        assert(0 && "filter job failed");
    }
    parse_job(&result->output, pjl, job);
}

static void expect_rejection(const FilterResult *result, const char *diagnostic) {
    if (result->status != 1 || !contains(&result->diagnostics, "ERROR:") ||
        (diagnostic && !contains(&result->diagnostics, diagnostic))) {
        fprintf(stderr, "expected exit 1 with \"%s\"\n", diagnostic ? diagnostic : "ERROR:");
        print_diagnostics(result);
        assert(0 && "filter did not reject the job as expected");
    }
}

static ZjsJob run_ok(const char *filter, const RasterPage *pages, size_t count, const RunOptions *run) {
    FilterResult result = run_pages(filter, pages, count, run);
    ZjsJob job;
    expect_job(&result, NULL, &job);
    assert(job.page_count >= count);
    free_result(&result);
    return job;
}

static void expect_rejected_pages(const char *filter, const RasterPage *pages, size_t count,
                                  const RunOptions *run, const char *diagnostic) {
    FilterResult result = run_pages(filter, pages, count, run);
    expect_rejection(&result, diagnostic);
    free_result(&result);
}

static int has_page_line(const FilterResult *result, unsigned int page, unsigned int copies) {
    char line[64];
    snprintf(line, sizeof(line), "PAGE: %u %u\n", page, copies);
    return contains(&result->diagnostics, line);
}

static int same_jbig(const ZjsPage *a, const ZjsPage *b) {
    return a->jbig_len == b->jbig_len && memcmp(a->jbig, b->jbig, a->jbig_len) == 0;
}

static int same_settings(const ZjsPage *a, const ZjsPage *b) {
    return a->paper == b->paper && a->copies == b->copies && a->source == b->source &&
           a->media == b->media && a->bpp == b->bpp && a->video_x == b->video_x &&
           a->video_y == b->video_y && a->econo == b->econo;
}

static int same_page_bytes(const Blob *a_output, const ZjsPage *a, const Blob *b_output, const ZjsPage *b) {
    return a->end - a->start == b->end - b->start &&
           memcmp(a_output->data + a->start, b_output->data + b->start, a->end - a->start) == 0;
}

/* Expected page bytes of two jobs must match page for page. */
static void expect_same_pages(const char *filter, const RasterPage *actual, const RunOptions *actual_run,
                              const RasterPage *expected, const RunOptions *expected_run, size_t count) {
    FilterResult a = run_pages(filter, actual, count, actual_run);
    FilterResult b = run_pages(filter, expected, count, expected_run);
    ZjsJob a_job;
    ZjsJob b_job;
    expect_job(&a, NULL, &a_job);
    expect_job(&b, NULL, &b_job);
    assert(a_job.page_count == count && b_job.page_count == count);
    for (size_t i = 0; i < count; i++) {
        if (!same_page_bytes(&a.output, &a_job.pages[i], &b.output, &b_job.pages[i])) {
            fprintf(stderr, "page %zu differs from its reference\n", i + 1U);
            assert(0 && "page bytes differ");
        }
    }
    free_job(&a_job);
    free_job(&b_job);
    free_result(&a);
    free_result(&b);
}

static char *repeated(char c, size_t count, const char *suffix) {
    size_t suffix_len = strlen(suffix);
    char *text = malloc(count + suffix_len + 1U);
    assert(text != NULL);
    memset(text, c, count);
    memcpy(text + count, suffix, suffix_len + 1U);
    return text;
}

static void test_job_framing_matrix(const char *filter) {
    RasterPage page = gray_page(48, 12);
    set_rect(&page, 4, 2, 20, 6);
    char pjl[512];
    unsigned int jobs = 0;

    /* JOB NAME: control characters become spaces; at most 80 bytes, cut on a UTF-8 boundary. */
    static const char *const fixed_titles[][2] = {
        {DEFAULT_TITLE, DEFAULT_TITLE},
        {"", ""},
        {"Tab\there", "Tab here"},
        {"Line\r\nBreak", "Line  Break"},
        {"\033%-12345X@PJL EOJ", " %-12345X@PJL EOJ"},
        {"Del\177char", "Del char"},
        {"\001\037", "  "},
        {"T\303\274rk\303\247e ba\305\237l\304\261k", "T\303\274rk\303\247e ba\305\237l\304\261k"},
        {"100% \"quoted\" = name", "100% \"quoted\" = name"}
    };
    for (size_t i = 0; i < sizeof(fixed_titles) / sizeof(fixed_titles[0]); i++) {
        RunOptions run = run_defaults();
        run.title = fixed_titles[i][0];
        FilterResult result = run_pages(filter, &page, 1, &run);
        expected_pjl(pjl, sizeof(pjl), fixed_titles[i][1], 3, 0);
        ZjsJob job;
        expect_job(&result, pjl, &job);
        assert(job.page_count == 1U && job.collate == 0U && job.pause_after < 0);
        free_job(&job);
        free_result(&result);
        jobs++;
    }

    static const struct {
        size_t count;
        const char *suffix;
        size_t kept;
        const char *kept_suffix;
    } generated[] = {
        {80, "", 80, ""},
        {81, "", 80, ""},
        {300, "", 80, ""},
        {79, "\303\251", 79, ""},
        {78, "\303\251x", 78, "\303\251"},
        {77, "\342\202\254x", 77, "\342\202\254"},
        {78, "\342\202\254", 78, ""},
        {79, "\nbbb", 79, " "}
    };
    for (size_t i = 0; i < sizeof(generated) / sizeof(generated[0]); i++) {
        char *title = repeated('a', generated[i].count, generated[i].suffix);
        char *name = repeated('a', generated[i].kept, generated[i].kept_suffix);
        RunOptions run = run_defaults();
        run.title = title;
        FilterResult result = run_pages(filter, &page, 1, &run);
        expected_pjl(pjl, sizeof(pjl), name, 3, 0);
        ZjsJob job;
        expect_job(&result, pjl, &job);
        free_job(&job);
        free_result(&result);
        free(title);
        free(name);
        jobs++;
    }

    /* Density: pjlDensity, PrintDensity, Density; the first present wins, invalid -> 3. */
    static const struct {
        const char *options;
        int density;
    } densities[] = {
        {"", 3}, {"pjlDensity=1", 1}, {"pjlDensity=2", 2}, {"pjlDensity=3", 3},
        {"pjlDensity=4", 4}, {"pjlDensity=5", 5}, {"PrintDensity=4", 4}, {"Density=2", 2},
        {"pjlDensity=2 PrintDensity=5 Density=4", 2}, {"PrintDensity=4 Density=1", 4},
        {"pjlDensity=0", 3}, {"pjlDensity=6", 3}, {"PrintDensity=abc", 3}, {"Density=+2", 3},
        {"pjlDensity=-1", 3}, {"pjlDensity=2x", 3}, {"pjlDensity=99999999999999999999", 3},
        {"pjlDensity=9 PrintDensity=5", 3}, {"Density=", 3}
    };
    for (size_t i = 0; i < sizeof(densities) / sizeof(densities[0]); i++) {
        RunOptions run = run_defaults();
        run.options = densities[i].options;
        FilterResult result = run_pages(filter, &page, 1, &run);
        expected_pjl(pjl, sizeof(pjl), DEFAULT_TITLE, densities[i].density, 0);
        ZjsJob job;
        expect_job(&result, pjl, &job);
        free_job(&job);
        free_result(&result);
        jobs++;
    }

    /* JAMRECOVERY AUTO for pjlJamRecovery / JamRecovery True, Yes, On or 1. */
    static const struct {
        const char *options;
        int jam_auto;
    } jams[] = {
        {"", 0}, {"pjlJamRecovery=True", 1}, {"pjlJamRecovery=yes", 1}, {"pjlJamRecovery=ON", 1},
        {"pjlJamRecovery=1", 1}, {"JamRecovery=True", 1}, {"JamRecovery=On", 1},
        {"pjlJamRecovery=False", 0}, {"pjlJamRecovery=2", 0}, {"JamRecovery=Maybe", 0},
        {"pjlJamRecovery=False JamRecovery=True", 0}, {"pjlDensity=5 pjlJamRecovery=True", 1}
    };
    for (size_t i = 0; i < sizeof(jams) / sizeof(jams[0]); i++) {
        RunOptions run = run_defaults();
        run.options = jams[i].options;
        FilterResult result = run_pages(filter, &page, 1, &run);
        expected_pjl(pjl, sizeof(pjl), DEFAULT_TITLE, i + 1U == sizeof(jams) / sizeof(jams[0]) ? 5 : 3,
                     jams[i].jam_auto);
        ZjsJob job;
        expect_job(&result, pjl, &job);
        free_job(&job);
        free_result(&result);
        jobs++;
    }

    /* START_DOC DMCOLLATE follows page 1's Collate. */
    static const unsigned int collates[][3] = {{0, 0, 0}, {1, 1, 1}, {1, 0, 1}, {0, 1, 0}};
    for (size_t i = 0; i < sizeof(collates) / sizeof(collates[0]); i++) {
        RasterPage pages[2] = {page, page};
        pages[0].collate = collates[i][0];
        pages[1].collate = collates[i][1];
        ZjsJob job = run_ok(filter, pages, 2, NULL);
        assert(job.page_count == 2U && job.collate == collates[i][2]);
        free_job(&job);
        jobs++;
    }

    /* A raster stream without pages writes nothing and succeeds. */
    char *empty = temp_path("mode-empty");
    write_raster(empty, NULL, 0);
    for (int transport = TRANSPORT_FILE; transport <= TRANSPORT_STDIN_PIPE; transport++) {
        RunOptions run = run_defaults();
        run.transport = (Transport)transport;
        FilterResult result = run_filter(filter, empty, &run);
        assert(result.status == 0 && result.output.len == 0U);
        assert(!contains(&result.diagnostics, "ERROR:") && !contains(&result.diagnostics, "PAGE:"));
        free_result(&result);
        jobs++;
    }
    remove_path(empty);

    /* File, stdin and pipe input produce the same job bytes. */
    RasterPage pages[3] = {page, gray_page(33, 25), legacy_page(70, 13)};
    pages[1].fill = FILL_NOISE;
    pages[2].fill = FILL_NOISE;
    FilterResult reference = run_pages(filter, pages, 3, NULL);
    ZjsJob reference_job;
    expect_job(&reference, NULL, &reference_job);
    assert(reference_job.page_count == 3U);
    for (int transport = TRANSPORT_STDIN_FILE; transport <= TRANSPORT_STDIN_PIPE; transport++) {
        RunOptions run = run_defaults();
        run.transport = (Transport)transport;
        FilterResult result = run_pages(filter, pages, 3, &run);
        assert(result.status == 0 && result.output.len == reference.output.len);
        assert(memcmp(result.output.data, reference.output.data, reference.output.len) == 0);
        free_result(&result);
        jobs++;
    }
    free_job(&reference_job);
    free_result(&reference);
    printf("    [PASS] PJL JOB NAME/DENSITY/JAMRECOVERY, DMCOLLATE, Empty Job and Transport Matrix (%u jobs)\n",
           jobs + 1U);
}

static void test_page_items_matrix(const char *filter) {
    unsigned int jobs = 0;

    /* DMPAPER from PageSize via the paper size table (within a point),
     * 257/263/264 for the 16K sizes, 256 for custom sizes. */
    static const struct {
        unsigned int width;
        unsigned int height;
        uint32_t code;
    } papers[] = {
        {612, 792, 1}, {612, 1008, 5}, {595, 842, 9}, {522, 756, 7}, {420, 595, 11},
        {297, 420, 70}, {516, 729, 13}, {612, 936, 41}, {284, 419, 43}, {567, 420, 69},
        {297, 684, 20}, {279, 540, 37}, {499, 709, 34}, {459, 649, 28}, {312, 624, 27},
        {553, 765, 264}, {522, 737, 263}, {558, 774, 257}, {595, 841, 9}, {200, 300, 256}, {0, 0, 256}
    };
    const size_t paper_count = sizeof(papers) / sizeof(papers[0]);
    RasterPage paper_pages[sizeof(papers) / sizeof(papers[0])];
    for (size_t i = 0; i < paper_count; i++) {
        paper_pages[i] = i % 2U ? legacy_page(20, 3) : gray_page(10, 3);
        paper_pages[i].page_size[0] = papers[i].width;
        paper_pages[i].page_size[1] = papers[i].height;
    }
    ZjsJob paper_job = run_ok(filter, paper_pages, paper_count, NULL);
    assert(paper_job.page_count == paper_count);
    for (size_t i = 0; i < paper_count; i++) assert(paper_job.pages[i].paper == papers[i].code);
    free_job(&paper_job);
    jobs++;

    /* DMMEDIATYPE: cupsMediaType passes through when non-zero. */
    static const uint32_t media_codes[] = {1, 2, 258, 260, 262, 263, 265, 267, 273, 282, 283,
                                           512, 513, 514, 515, 516, 0x12345678U};
    const size_t media_count = sizeof(media_codes) / sizeof(media_codes[0]);
    RasterPage media_pages[sizeof(media_codes) / sizeof(media_codes[0])];
    for (size_t i = 0; i < media_count; i++) {
        media_pages[i] = gray_page(10, 3);
        media_pages[i].cups_media_type = media_codes[i];
        media_pages[i].media_type = "Labels";
    }
    RunOptions media_run = run_defaults();
    media_run.options = "MediaType=Envelope";
    ZjsJob media_job = run_ok(filter, media_pages, media_count, &media_run);
    for (size_t i = 0; i < media_count; i++) assert(media_job.pages[i].media == media_codes[i]);
    free_job(&media_job);
    jobs++;

    /* ... otherwise the MediaType name (header first, then the job option) is mapped. */
    static const struct {
        const char *name;
        uint32_t code;
    } media_names[] = {
        {"Standard", 1}, {"Plain", 1}, {"Paper", 1}, {"plain", 1}, {"Transparency", 2},
        {"Glossy", 3}, {"Thick", 0x105}, {"Cardstock", 0x105}, {"Postcard", 0x106},
        {"Labels", 265}, {"LABELS", 265}, {"Envelope", 267}, {"Letterhead", 513},
        {"UnknownMedia", 1}, {NULL, 1}
    };
    const size_t name_count = sizeof(media_names) / sizeof(media_names[0]);
    RasterPage name_pages[sizeof(media_names) / sizeof(media_names[0])];
    for (size_t i = 0; i < name_count; i++) {
        name_pages[i] = i % 2U ? gray_page(10, 3) : legacy_page(20, 3);
        name_pages[i].cups_media_type = 0;
        name_pages[i].media_type = media_names[i].name;
    }
    ZjsJob name_job = run_ok(filter, name_pages, name_count, NULL);
    for (size_t i = 0; i < name_count; i++) assert(name_job.pages[i].media == media_names[i].code);
    free_job(&name_job);
    jobs++;

    static const struct {
        const char *header_name;
        const char *options;
        uint32_t code;
    } media_options[] = {
        {NULL, "MediaType=Envelope", 267}, {NULL, "MediaType=Glossy", 3},
        {NULL, "MediaType=letterhead", 513}, {NULL, "MediaType=Bogus", 1},
        {"Labels", "MediaType=Envelope", 265}
    };
    for (size_t i = 0; i < sizeof(media_options) / sizeof(media_options[0]); i++) {
        RasterPage page = legacy_page(20, 3);
        page.media_type = media_options[i].header_name;
        RunOptions run = run_defaults();
        run.options = media_options[i].options;
        ZjsJob job = run_ok(filter, &page, 1, &run);
        assert(job.pages[0].media == media_options[i].code);
        free_job(&job);
        jobs++;
    }

    /* DMDEFAULTSOURCE: MediaPosition passes through when non-zero. */
    static const uint32_t positions[] = {1, 2, 3, 4, 7, 14, 255};
    const size_t position_count = sizeof(positions) / sizeof(positions[0]);
    RasterPage position_pages[sizeof(positions) / sizeof(positions[0])];
    for (size_t i = 0; i < position_count; i++) {
        position_pages[i] = i % 2U ? legacy_page(20, 3) : gray_page(10, 3);
        position_pages[i].media_position = positions[i];
    }
    RunOptions position_run = run_defaults();
    position_run.options = "InputSlot=Cassette";
    ZjsJob position_job = run_ok(filter, position_pages, position_count, &position_run);
    for (size_t i = 0; i < position_count; i++) assert(position_job.pages[i].source == positions[i]);
    free_job(&position_job);
    jobs++;

    /* ... otherwise InputSlot, then MediaSource, is mapped by name. */
    static const struct {
        const char *options;
        uint32_t code;
    } sources[] = {
        {"", 7}, {"InputSlot=Manual", 4}, {"InputSlot=Tray1", 4}, {"InputSlot=manual", 4},
        {"InputSlot=Tray2", 2}, {"InputSlot=Lower", 2}, {"InputSlot=Cassette", 14},
        {"InputSlot=Auto", 7}, {"InputSlot=Upper", 7}, {"MediaSource=Cassette", 14},
        {"MediaSource=Tray2", 2}, {"InputSlot=Tray1 MediaSource=Tray2", 4}
    };
    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
        RasterPage pages[2] = {gray_page(10, 3), legacy_page(20, 3)};
        pages[0].media_position = 0;
        RunOptions run = run_defaults();
        run.options = sources[i].options;
        ZjsJob job = run_ok(filter, pages, 2, &run);
        assert(job.pages[0].source == sources[i].code && job.pages[1].source == sources[i].code);
        free_job(&job);
        jobs++;
    }

    /* DMCOPIES: header NumCopies (minimum 1) for gray pages, argv[4] for legacy pages. */
    static const unsigned int gray_copies[] = {0, 1, 2, 99, 65535};
    RasterPage copy_pages[sizeof(gray_copies) / sizeof(gray_copies[0])];
    for (size_t i = 0; i < sizeof(gray_copies) / sizeof(gray_copies[0]); i++) {
        copy_pages[i] = gray_page(10, 3);
        copy_pages[i].num_copies = gray_copies[i];
    }
    RunOptions copy_run = run_defaults();
    copy_run.copies = "7";
    FilterResult copy_result = run_pages(filter, copy_pages, 5, &copy_run);
    ZjsJob copy_job;
    expect_job(&copy_result, NULL, &copy_job);
    for (size_t i = 0; i < 5U; i++) {
        unsigned int expected = gray_copies[i] ? gray_copies[i] : 1U;
        assert(copy_job.pages[i].copies == expected);
        assert(has_page_line(&copy_result, (unsigned int)i + 1U, expected));
    }
    free_job(&copy_job);
    free_result(&copy_result);
    jobs++;

    static const struct {
        const char *text;
        uint32_t value;
    } legacy_copies[] = {{"1", 1}, {"2", 2}, {"255", 255}, {"65535", 65535}, {"2147483647", 2147483647U}};
    for (size_t i = 0; i < sizeof(legacy_copies) / sizeof(legacy_copies[0]); i++) {
        RasterPage page = legacy_page(20, 3);
        page.num_copies = 9;
        RunOptions run = run_defaults();
        run.copies = legacy_copies[i].text;
        FilterResult result = run_pages(filter, &page, 1, &run);
        ZjsJob job;
        expect_job(&result, NULL, &job);
        assert(job.pages[0].copies == legacy_copies[i].value);
        assert(has_page_line(&result, 1, legacy_copies[i].value));
        free_job(&job);
        free_result(&result);
        jobs++;
    }

    /* ECONOMODE is always present: 1 for EconoMode / TonerSave True, Yes, On or 1. */
    static const struct {
        const char *options;
        uint32_t value;
    } econo[] = {
        {"", 0}, {"EconoMode=True", 1}, {"EconoMode=Yes", 1}, {"EconoMode=On", 1},
        {"EconoMode=1", 1}, {"EconoMode=true", 1}, {"EconoMode=YES", 1}, {"TonerSave=On", 1},
        {"TonerSave=1", 1}, {"EconoMode=False", 0}, {"EconoMode=No", 0}, {"EconoMode=Off", 0},
        {"EconoMode=0", 0}, {"EconoMode=2", 0}, {"EconoMode=False TonerSave=On", 0}
    };
    for (size_t i = 0; i < sizeof(econo) / sizeof(econo[0]); i++) {
        RasterPage pages[2] = {gray_page(10, 3), legacy_page(20, 3)};
        RunOptions run = run_defaults();
        run.options = econo[i].options;
        ZjsJob job = run_ok(filter, pages, 2, &run);
        assert(job.pages[0].econo == econo[i].value && job.pages[1].econo == econo[i].value);
        free_job(&job);
        jobs++;
    }
    printf("    [PASS] DMPAPER Table, DMMEDIATYPE, DMDEFAULTSOURCE, DMCOPIES and ECONOMODE Matrix (%u jobs)\n",
           jobs);
}

static void test_raster_format_matrix(const char *filter) {
    unsigned int jobs = 0;

    /* Gray: VIDEO_X = cupsWidth, VIDEO_BPP = (cupsCompression & 1) + 1. */
    static const unsigned int widths[] = {1, 3, 64, 100, 127, 128, 129, 1000};
    const size_t width_count = sizeof(widths) / sizeof(widths[0]);
    for (unsigned int compression = 0; compression < 4U; compression++) {
        RasterPage pages[sizeof(widths) / sizeof(widths[0])];
        for (size_t i = 0; i < width_count; i++) {
            pages[i] = gray_page(widths[i], 5U + (unsigned int)i);
            pages[i].compression = compression;
            pages[i].fill = FILL_NOISE;
            pages[i].seed = compression * 16U + (unsigned int)i;
        }
        ZjsJob job = run_ok(filter, pages, width_count, NULL);
        for (size_t i = 0; i < width_count; i++) {
            uint32_t bpp = (compression & 1U) + 1U;
            assert(job.pages[i].video_x == widths[i] && job.pages[i].bpp == bpp);
            assert(job.pages[i].raster_x == widths[i] * bpp && job.pages[i].video_y == 12U);
        }
        free_job(&job);
        jobs++;
    }

    /* VIDEO_Y = cupsHeight rounded up to 12 lines, for both formats. */
    static const unsigned int heights[] = {1, 11, 12, 13, 23, 24, 25, 100, 101};
    const size_t height_count = sizeof(heights) / sizeof(heights[0]);
    for (int legacy = 0; legacy <= 1; legacy++) {
        RasterPage pages[sizeof(heights) / sizeof(heights[0])];
        for (size_t i = 0; i < height_count; i++) {
            pages[i] = legacy ? legacy_page(30, heights[i]) : gray_page(15, heights[i]);
            pages[i].fill = FILL_BLACK;
        }
        ZjsJob job = run_ok(filter, pages, height_count, NULL);
        for (size_t i = 0; i < height_count; i++) {
            assert(job.pages[i].video_y == round_up(heights[i], 12U) && job.pages[i].video_x == 15U);
            assert(job.pages[i].bpp == 2U);
        }
        free_job(&job);
        jobs++;
    }

    /* Legacy: VIDEO_X = ceil(cupsWidth / 2), two bits per pixel. */
    static const unsigned int legacy_widths[] = {1, 2, 7, 8, 9, 15, 16, 17, 61, 127, 128, 129, 255, 256, 257};
    const size_t legacy_count = sizeof(legacy_widths) / sizeof(legacy_widths[0]);
    RasterPage legacy_pages[sizeof(legacy_widths) / sizeof(legacy_widths[0])];
    for (size_t i = 0; i < legacy_count; i++) {
        legacy_pages[i] = legacy_page(legacy_widths[i], 4);
        legacy_pages[i].fill = FILL_NOISE;
        legacy_pages[i].seed = (unsigned int)i;
    }
    ZjsJob legacy_job = run_ok(filter, legacy_pages, legacy_count, NULL);
    for (size_t i = 0; i < legacy_count; i++) {
        uint32_t video_x = (legacy_widths[i] + 1U) / 2U;
        assert(legacy_job.pages[i].video_x == video_x && legacy_job.pages[i].bpp == 2U);
        assert(legacy_job.pages[i].raster_x == 2U * video_x);
    }
    free_job(&legacy_job);
    jobs++;

    /* cupsBytesPerLine: gray [w, roundup(w, 128)], legacy [ceil(w / 8), roundup(w, 128) / 8]. */
    static const struct {
        int legacy;
        unsigned int width;
        unsigned int bytes_per_line;
        int accepted;
    } lines[] = {
        {0, 100, 100, 1}, {0, 100, 128, 1}, {0, 100, 99, 0}, {0, 100, 129, 0},
        {0, 128, 128, 1}, {0, 128, 129, 0}, {0, 129, 129, 1}, {0, 129, 256, 1},
        {0, 129, 257, 0}, {0, 1, 1, 1}, {0, 1, 128, 1}, {0, 1, 129, 0},
        {1, 61, 8, 1}, {1, 61, 16, 1}, {1, 61, 7, 0}, {1, 61, 17, 0},
        {1, 128, 16, 1}, {1, 128, 17, 0}, {1, 129, 17, 1}, {1, 129, 32, 1},
        {1, 129, 33, 0}, {1, 1, 1, 1}, {1, 1, 16, 1}, {1, 1, 17, 0}
    };
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        RasterPage page = lines[i].legacy ? legacy_page(lines[i].width, 5) : gray_page(lines[i].width, 5);
        page.bytes_per_line = lines[i].bytes_per_line;
        page.fill = FILL_NOISE;
        FilterResult result = run_pages(filter, &page, 1, NULL);
        if (lines[i].accepted) {
            ZjsJob job;
            expect_job(&result, NULL, &job);
            free_job(&job);
        } else {
            expect_rejection(&result, "Invalid raster line length on page 1");
            assert(result.output.len == 0U);
        }
        free_result(&result);
        jobs++;
    }

    /* Row padding is not image content, whatever its value. */
    RasterPage gray_padded[2] = {gray_page(100, 30), gray_page(100, 30)};
    gray_padded[0].fill = gray_padded[1].fill = FILL_NOISE;
    gray_padded[0].bytes_per_line = 128;
    gray_padded[0].padding = 0x00;
    expect_same_pages(filter, &gray_padded[0], NULL, &gray_padded[1], NULL, 1);
    RasterPage legacy_padded[3] = {legacy_page(61, 30), legacy_page(61, 30), legacy_page(61, 30)};
    for (size_t i = 0; i < 3U; i++) legacy_padded[i].fill = FILL_NOISE;
    legacy_padded[0].bytes_per_line = 16;
    legacy_padded[0].padding = 0xff;
    legacy_padded[1].padding = 0xff;
    RasterPage legacy_clean[2] = {legacy_padded[2], legacy_padded[2]};
    expect_same_pages(filter, legacy_padded, NULL, legacy_clean, NULL, 2);
    jobs += 4U;

    /* 18 inches is the largest accepted edge; zero sizes are rejected. */
    RasterPage largest[4] = {gray_page(10800, 1), gray_page(1, 10800), legacy_page(21600, 1), legacy_page(1, 10800)};
    ZjsJob largest_job = run_ok(filter, largest, 4, NULL);
    assert(largest_job.pages[0].video_x == 10800U && largest_job.pages[0].raster_x == 21600U);
    assert(largest_job.pages[1].video_y == 10800U);
    assert(largest_job.pages[2].video_x == 10800U && largest_job.pages[2].raster_x == 21600U);
    assert(largest_job.pages[3].video_y == 10800U && largest_job.pages[3].video_x == 1U);
    free_job(&largest_job);
    jobs++;

    RasterPage sizes[8] = {gray_page(10801, 1), gray_page(1, 10801), legacy_page(21601, 1), legacy_page(1, 10801),
                           gray_page(0, 1), gray_page(1, 0), legacy_page(0, 1), legacy_page(1, 0)};
    sizes[4].bytes_per_line = 1;
    sizes[6].bytes_per_line = 1;
    for (size_t i = 0; i < 8U; i++) {
        FilterResult result = run_pages(filter, &sizes[i], 1, NULL);
        expect_rejection(&result, "Unsupported raster resolution or page size on page 1");
        assert(result.output.len == 0U);
        free_result(&result);
        jobs++;
    }
    printf("    [PASS] Gray/Legacy Geometry, VIDEO_Y Rounding, Line Length and Size Limit Matrix (%u jobs)\n",
           jobs);
}

/* The screen of a gray page, computed here from the screen tables: the screen
 * row is selected by the descending line counter VIDEO_Y - y. */
static unsigned char *screen_page(const RasterPage *gray, unsigned int bpp, size_t *row_bytes) {
    uint32_t video_y = round_up(gray->height, 12U);
    size_t bytes = ((size_t)gray->width * bpp + 7U) / 8U;
    unsigned char *rows = calloc(gray->height, bytes);
    assert(rows != NULL);
    for (unsigned int y = 0; y < gray->height; y++) {
        unsigned int remaining = video_y - y;
        unsigned char *row = rows + (size_t)y * bytes;
        for (unsigned int x = 0; x < gray->width; x++) {
            unsigned int darkness = content_darkness(gray, x, y);
            if (darkness == 0U) continue;
            if (bpp == 2U) {
                unsigned int shift = 6U - 2U * (x & 3U);
                unsigned int cell = p1600_screen_2bpp[darkness * 48U + (remaining % 12U) * 4U + (x / 4U) % 3U];
                row[x / 4U] |= (unsigned char)(((cell >> shift) & 3U) << shift);
            } else if (darkness >= p1600_screen_1bpp[(remaining % 16U) * 16U + x % 16U]) {
                row[x / 8U] |= (unsigned char)(0x80U >> (x & 7U));
            }
        }
    }
    *row_bytes = bytes;
    return rows;
}

/* Legacy 1-bit page whose bits are the expected video bitmap of `gray`. */
static RasterPage screened_reference(const RasterPage *gray, unsigned int bpp, unsigned char **raw) {
    size_t row_bytes = 0;
    *raw = screen_page(gray, bpp, &row_bytes);
    RasterPage page = legacy_page(gray->width * bpp, gray->height);
    page.bytes_per_line = (unsigned int)row_bytes;
    page.fill = FILL_RAW;
    page.raw = *raw;
    page.page_size[0] = gray->page_size[0];
    page.page_size[1] = gray->page_size[1];
    page.media_position = gray->media_position;
    page.cups_media_type = gray->cups_media_type;
    return page;
}

static void compare_with_references(const char *filter, const RasterPage *gray, const RasterPage *legacy,
                                    size_t count, unsigned int bpp, const RunOptions *gray_run) {
    FilterResult a = run_pages(filter, gray, count, gray_run);
    FilterResult b = run_pages(filter, legacy, count, NULL);
    ZjsJob a_job;
    ZjsJob b_job;
    expect_job(&a, NULL, &a_job);
    expect_job(&b, NULL, &b_job);
    for (size_t i = 0; i < count; i++) {
        const ZjsPage *page = &a_job.pages[i];
        assert(page->bpp == bpp && page->video_x == gray[i].width);
        /* With two bits per pixel the legacy page is the same page byte for byte;
         * with one bit only the JBIG image is (VIDEO_X and VIDEO_BPP differ). */
        int same = bpp == 2U ? same_page_bytes(&a.output, page, &b.output, &b_job.pages[i])
                             : same_jbig(page, &b_job.pages[i]);
        if (!same) {
            fprintf(stderr, "screened page %zu (%ux%u, %u bpp) differs from the expected screen\n", i + 1U,
                    gray[i].width, gray[i].height, bpp);
            assert(0 && "screened page differs");
        }
    }
    free_job(&a_job);
    free_job(&b_job);
    free_result(&a);
    free_result(&b);
}

static void test_screening_matrix(const char *filter) {
    static const unsigned int levels[] = {0, 1, 64, 127, 128, 200, 254, 255};
    RasterPage gray[16];
    size_t count = 0;
    for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        gray[count] = gray_page(37, 29);
        gray[count].fill = FILL_LEVEL;
        gray[count++].level = levels[i];
    }
    static const unsigned int noise_sizes[][2] = {{37, 29}, {64, 1}, {5, 13}, {130, 40}, {1, 24}, {97, 12}};
    for (size_t i = 0; i < sizeof(noise_sizes) / sizeof(noise_sizes[0]); i++) {
        gray[count] = gray_page(noise_sizes[i][0], noise_sizes[i][1]);
        gray[count].fill = FILL_NOISE;
        gray[count++].seed = 100U + (unsigned int)i;
    }
    gray[count] = gray_page(45, 30);
    set_rect(&gray[count++], 3, 4, 17, 9);

    unsigned int jobs = 0;
    for (unsigned int bpp = 1; bpp <= 2U; bpp++) {
        RasterPage legacy[16];
        unsigned char *raws[16];
        for (size_t i = 0; i < count; i++) {
            gray[i].compression = bpp - 1U;
            legacy[i] = screened_reference(&gray[i], bpp, &raws[i]);
        }
        compare_with_references(filter, gray, legacy, count, bpp, NULL);
        for (size_t i = 0; i < count; i++) free(raws[i]);
        jobs += 2U;

        /* A turned page is screened at its output position. */
        RasterPage turned[16];
        for (size_t i = 0; i < count; i++) {
            turned[i] = gray[i];
            turned[i].rotated = 1;
            legacy[i] = screened_reference(&turned[i], bpp, &raws[i]);
        }
        RunOptions rotate = run_defaults();
        rotate.options = "TersKose=True";
        compare_with_references(filter, gray, legacy, count, bpp, &rotate);
        for (size_t i = 0; i < count; i++) free(raws[i]);
        jobs += 2U;
    }

    /* Table-independent anchors: black is all ones in the active VIDEO_X * bpp
     * bits (padding zero), white is an all-zero plane. */
    for (unsigned int bpp = 1; bpp <= 2U; bpp++) {
        RasterPage solid[2] = {gray_page(37, 29), gray_page(37, 29)};
        solid[0].fill = FILL_BLACK;
        solid[1].fill = FILL_WHITE;
        RasterPage bits[2] = {legacy_page(37U * bpp, 29), legacy_page(37U * bpp, 29)};
        bits[0].fill = FILL_BLACK;
        bits[1].fill = FILL_WHITE;
        for (size_t i = 0; i < 2U; i++) {
            solid[i].compression = bpp - 1U;
            bits[i].media_position = solid[i].media_position;
            bits[i].cups_media_type = solid[i].cups_media_type;
        }
        compare_with_references(filter, solid, bits, 2, bpp, NULL);
        jobs += 2U;
    }
    printf("    [PASS] Screen Tables, Line Phase, Rotated Phase and Solid Black/White Bitmaps (%u jobs, %zu pages each)\n",
           jobs, count);
}

static void test_bid_chunking(const char *filter) {
    RasterPage pages[3] = {legacy_page(3000, 708), gray_page(1500, 708), gray_page(16, 12)};
    pages[0].fill = FILL_NOISE;
    pages[0].seed = 99;
    pages[1].fill = FILL_NOISE;
    pages[1].seed = 98;
    set_rect(&pages[2], 2, 2, 5, 5);
    FilterResult file_result = run_pages(filter, pages, 3, NULL);
    ZjsJob job;
    expect_job(&file_result, NULL, &job);
    assert(job.pages[0].bid_chunks >= 4U);
    assert(job.pages[1].bid_chunks >= 2U);
    assert(job.pages[2].bid_chunks == 1U);
    size_t chunks = job.pages[0].bid_chunks + job.pages[1].bid_chunks + job.pages[2].bid_chunks;
    for (int transport = TRANSPORT_STDIN_FILE; transport <= TRANSPORT_STDIN_PIPE; transport++) {
        RunOptions run = run_defaults();
        run.transport = (Transport)transport;
        FilterResult result = run_pages(filter, pages, 3, &run);
        assert(result.status == 0 && result.output.len == file_result.output.len);
        assert(memcmp(result.output.data, file_result.output.data, file_result.output.len) == 0);
        free_result(&result);
    }
    free_job(&job);
    free_result(&file_result);
    printf("    [PASS] 64 KiB JBIG_BID Blocking with Pad-to-4 Final Chunk over File/Stdin/Pipe (%zu chunks)\n",
           chunks);
}

static void test_rotation_matrix(const char *filter) {
    RasterPage pages[4] = {gray_page(45, 29), gray_page(45, 29), legacy_page(61, 29), gray_page(40, 26)};
    pages[0].fill = FILL_NOISE;
    pages[0].seed = 11;
    pages[1].fill = FILL_NOISE;
    pages[1].seed = 12;
    pages[1].compression = 0;
    pages[2].fill = FILL_NOISE;
    pages[2].seed = 13;
    pages[2].padding = 0xff;
    set_rect(&pages[3], 1, 2, 9, 5);
    RasterPage turned[4];
    for (size_t i = 0; i < 4U; i++) {
        turned[i] = pages[i];
        turned[i].rotated = 1;
    }

    static const char *const rotate_options[] = {
        "TersKose=True", "TersKose=On", "TersKose=1", "TersKose=yes",
        "ReverseLandscape=True", "ReverseOrientation=Yes", "TersKose=True EconoMode=False"
    };
    static const char *const keep_options[] = {
        "", "TersKose=False", "TersKose=No", "TersKose=Off", "TersKose=2",
        "TersKose=False ReverseLandscape=True", "ReverseOrientation=Off"
    };
    unsigned int jobs = 0;
    for (size_t i = 0; i < sizeof(rotate_options) / sizeof(rotate_options[0]); i++) {
        RunOptions run = run_defaults();
        run.options = rotate_options[i];
        expect_same_pages(filter, pages, &run, turned, NULL, 4);
        jobs += 2U;
    }
    for (size_t i = 0; i < sizeof(keep_options) / sizeof(keep_options[0]); i++) {
        RunOptions run = run_defaults();
        run.options = keep_options[i];
        expect_same_pages(filter, pages, &run, pages, NULL, 4);
        jobs += 2U;
    }
    printf("    [PASS] TersKose/ReverseLandscape/ReverseOrientation 180-Degree Rotation Matrix (%u jobs)\n", jobs);
}

/* Sheet order: automatic 2,1,4,3,... and manual n..2, PAUSE, 1..n-1, with
 * a blank partner for an odd last page (none for a one-page manual job). */
static size_t sheet_tokens(unsigned int count, SheetMode mode, int *tokens) {
    size_t n = 0;
    unsigned int padded = count + (count & 1U);
    if (mode == SHEETS_SIMPLEX || (mode == SHEETS_MANUAL && count == 1U)) {
        for (unsigned int page = 1; page <= count; page++) tokens[n++] = (int)page;
        return n;
    }
    if (mode == SHEETS_AUTOMATIC) {
        for (unsigned int front = 1; front <= padded; front += 2U) {
            tokens[n++] = front + 1U > count ? TOKEN_BLANK : (int)(front + 1U);
            tokens[n++] = (int)front;
        }
        return n;
    }
    for (unsigned int back = padded; back >= 2U; back -= 2U) {
        tokens[n++] = back > count ? TOKEN_BLANK : (int)back;
    }
    tokens[n++] = TOKEN_PAUSE;
    for (unsigned int front = 1; front <= padded; front += 2U) tokens[n++] = (int)front;
    return n;
}

static void expect_tokens(unsigned int count, SheetMode mode, const int *want, size_t want_count) {
    int tokens[2U * MAX_TEST_PAGES + 2U];
    size_t n = sheet_tokens(count, mode, tokens);
    assert(n == want_count && memcmp(tokens, want, n * sizeof(tokens[0])) == 0);
}

static void expect_sequence(const ZjsJob *job, const ZjsJob *expected, const ZjsPage *blank,
                            const int *tokens, size_t token_count, int automatic) {
    size_t index = 0;
    int pause_seen = 0;
    for (size_t i = 0; i < token_count; i++) {
        if (tokens[i] == TOKEN_PAUSE) {
            assert(job->pause_after == (int)index);
            pause_seen = 1;
            continue;
        }
        assert(index < job->page_count);
        const ZjsPage *page = &job->pages[index++];
        const ZjsPage *want = tokens[i] == TOKEN_BLANK ? blank : &expected->pages[tokens[i] - 1];
        if (!same_jbig(page, want) || !same_settings(page, want)) {
            fprintf(stderr, "sheet %zu: expected %s %d\n", index,
                    tokens[i] == TOKEN_BLANK ? "the blank partner of page" : "page",
                    tokens[i] == TOKEN_BLANK ? (int)expected->page_count : tokens[i]);
            assert(0 && "duplex sheet order or page content mismatch");
        }
        assert(page->has_duplex == automatic);
    }
    assert(index == job->page_count);
    assert(pause_seen || job->pause_after < 0);
}

static RasterPage sheet_page(int legacy, unsigned int number, unsigned int duplex, unsigned int tumble) {
    RasterPage page = legacy ? legacy_page(50U + 6U * number, 12U * number - 3U)
                             : gray_page(40U + 3U * number, 12U * number - 3U);
    page.fill = FILL_NOISE;
    page.seed = 0x1606U + number;
    page.duplex = duplex;
    page.tumble = tumble;
    if (!legacy) page.num_copies = number;
    return page;
}

static void check_sheet_order(const char *filter, int legacy, unsigned int count, unsigned int duplex,
                              unsigned int tumble, int reverse_corner, const char *options, SheetMode mode) {
    RasterPage pages[MAX_TEST_PAGES];
    RasterPage reference[MAX_TEST_PAGES];
    assert(count >= 1U && count <= MAX_TEST_PAGES);
    for (unsigned int number = 1; number <= count; number++) {
        pages[number - 1U] = sheet_page(legacy, number, duplex, tumble);
        reference[number - 1U] = sheet_page(legacy, number, 0, 0);
        int turned = reverse_corner;
        /* Legacy (PPD 3.x) long-edge back sides are turned by the filter; 8-bit
         * gray pages arrive already turned by CUPS (cupsBackSide Rotated). */
        if (legacy && duplex && !tumble && number % 2U == 0U) turned = !turned;
        reference[number - 1U].rotated = turned;
    }

    RunOptions run = run_defaults();
    run.copies = "2";
    run.options = options;
    FilterResult result = run_pages(filter, pages, count, &run);
    ZjsJob job;
    expect_job(&result, NULL, &job);
    for (unsigned int number = 1; number <= count; number++) {
        assert(has_page_line(&result, number, legacy ? 2U : number));
    }
    char extra[32];
    snprintf(extra, sizeof(extra), "PAGE: %u ", count + 1U);
    assert(!contains(&result.diagnostics, extra));

    RunOptions plain = run_defaults();
    plain.copies = "2";
    ZjsJob expected = run_ok(filter, reference, count, &plain);
    RasterPage white = reference[count - 1U];
    white.fill = FILL_WHITE;
    ZjsJob blank = run_ok(filter, &white, 1, &plain);

    int tokens[2U * MAX_TEST_PAGES + 2U];
    size_t token_count = sheet_tokens(count, mode, tokens);
    expect_sequence(&job, &expected, &blank.pages[0], tokens, token_count, mode == SHEETS_AUTOMATIC);
    free_job(&job);
    free_job(&expected);
    free_job(&blank);
    free_result(&result);
}

static void test_sheet_order_matrix(const char *filter) {
    /* The model above, pinned to explicit sequences. */
    static const int auto1[] = {TOKEN_BLANK, 1};
    static const int auto3[] = {2, 1, TOKEN_BLANK, 3};
    static const int auto4[] = {2, 1, 4, 3};
    static const int manual1[] = {1};
    static const int manual3[] = {TOKEN_BLANK, 2, TOKEN_PAUSE, 1, 3};
    static const int manual4[] = {4, 2, TOKEN_PAUSE, 1, 3};
    static const int manual5[] = {TOKEN_BLANK, 4, 2, TOKEN_PAUSE, 1, 3, 5};
    expect_tokens(1, SHEETS_AUTOMATIC, auto1, 2);
    expect_tokens(3, SHEETS_AUTOMATIC, auto3, 4);
    expect_tokens(4, SHEETS_AUTOMATIC, auto4, 4);
    expect_tokens(1, SHEETS_MANUAL, manual1, 1);
    expect_tokens(3, SHEETS_MANUAL, manual3, 5);
    expect_tokens(4, SHEETS_MANUAL, manual4, 5);
    expect_tokens(5, SHEETS_MANUAL, manual5, 7);

    static const struct {
        unsigned int duplex;
        unsigned int tumble;
        const char *options;
        const char *rotated_options;
        SheetMode mode;
    } modes[] = {
        {0, 0, "", "TersKose=True", SHEETS_SIMPLEX},
        {1, 0, "", "TersKose=True", SHEETS_AUTOMATIC},
        {1, 1, "", "ReverseOrientation=Yes", SHEETS_AUTOMATIC},
        {1, 0, "DupUnit=False", "DupUnit=False TersKose=True", SHEETS_MANUAL},
        {1, 1, "DupUnit=False", "DupUnit=No TersKose=On", SHEETS_MANUAL}
    };
    unsigned int scenarios = 0;
    for (int legacy = 0; legacy <= 1; legacy++) {
        for (size_t mode = 0; mode < sizeof(modes) / sizeof(modes[0]); mode++) {
            for (int reverse = 0; reverse <= 1; reverse++) {
                for (unsigned int count = 1; count <= 5U; count++) {
                    check_sheet_order(filter, legacy, count, modes[mode].duplex, modes[mode].tumble, reverse,
                                      reverse ? modes[mode].rotated_options : modes[mode].options,
                                      modes[mode].mode);
                    scenarios++;
                }
            }
        }
    }
    printf("    [PASS] Simplex/Automatic/Manual Duplex Sheet Order, Blank Partner and Back-Side Rotation (%u jobs)\n",
           scenarios * 3U);
}

/* Mode of a two-page long-edge duplex job: P2 always comes first. */
static SheetMode two_page_mode(const char *filter, const RasterPage *base, const char *options, const char *ppd) {
    RasterPage pages[2] = {*base, *base};
    pages[0].height = 9;
    pages[1].height = 21;
    pages[0].duplex = pages[1].duplex = 1;
    RunOptions run = run_defaults();
    run.options = options;
    run.ppd = ppd;
    ZjsJob job = run_ok(filter, pages, 2, &run);
    assert(job.page_count == 2U && job.pages[0].video_y == 24U && job.pages[1].video_y == 12U);
    SheetMode mode;
    if (job.pause_after == 1 && !job.pages[0].has_duplex && !job.pages[1].has_duplex) {
        mode = SHEETS_MANUAL;
    } else {
        assert(job.pause_after < 0 && job.pages[0].has_duplex && job.pages[1].has_duplex);
        mode = SHEETS_AUTOMATIC;
    }
    free_job(&job);
    return mode;
}

static char *write_ppd(const char *text) {
    char *path = temp_path("mode-ppd");
    write_path(path, (const unsigned char *)text, strlen(text));
    return path;
}

static void test_automatic_duplex_eligibility(const char *filter) {
    unsigned int jobs = 0;
    RasterPage base = gray_page(30, 9);
    set_rect(&base, 2, 2, 10, 4);

    /* Automatic only for Letter, Legal, A4 and 8.5x13 ... */
    static const struct {
        unsigned int width;
        unsigned int height;
        int automatic;
    } papers[] = {
        {612, 792, 1}, {612, 1008, 1}, {595, 842, 1}, {612, 936, 1}, {522, 756, 0}, {420, 595, 0},
        {297, 420, 0}, {516, 729, 0}, {284, 419, 0}, {567, 420, 0}, {297, 684, 0}, {279, 540, 0},
        {499, 709, 0}, {459, 649, 0}, {312, 624, 0}, {553, 765, 0}, {792, 1224, 0}, {595, 841, 1},
        {611, 793, 1}, {600, 800, 0}
    };
    for (size_t i = 0; i < sizeof(papers) / sizeof(papers[0]); i++) {
        RasterPage page = base;
        page.page_size[0] = papers[i].width;
        page.page_size[1] = papers[i].height;
        SheetMode mode = two_page_mode(filter, &page, "", NULL);
        assert(mode == (papers[i].automatic ? SHEETS_AUTOMATIC : SHEETS_MANUAL));
        jobs++;
    }

    /* ... on plain, light or mid-weight media. */
    static const struct {
        uint32_t code;
        int automatic;
    } media[] = {
        {1, 1}, {258, 1}, {282, 1}, {2, 0}, {260, 0}, {262, 0}, {263, 0}, {265, 0}, {267, 0},
        {273, 0}, {283, 0}, {512, 0}, {513, 0}, {514, 0}, {515, 0}, {516, 0}
    };
    for (size_t i = 0; i < sizeof(media) / sizeof(media[0]); i++) {
        RasterPage page = base;
        page.cups_media_type = media[i].code;
        SheetMode mode = two_page_mode(filter, &page, "", NULL);
        assert(mode == (media[i].automatic ? SHEETS_AUTOMATIC : SHEETS_MANUAL));
        jobs++;
    }
    static const struct {
        const char *name;
        const char *options;
        int automatic;
    } names[] = {
        {"Plain", "", 1}, {"Standard", "", 1}, {"Paper", "", 1}, {"UnknownMedia", "", 1},
        {"Transparency", "", 0}, {"Labels", "", 0}, {"Envelope", "", 0}, {"Thick", "", 0},
        {NULL, "MediaType=Labels", 0}, {NULL, "MediaType=Plain", 1}
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        RasterPage page = legacy_page(40, 9);
        page.media_type = names[i].name;
        SheetMode mode = two_page_mode(filter, &page, names[i].options, NULL);
        assert(mode == (names[i].automatic ? SHEETS_AUTOMATIC : SHEETS_MANUAL));
        jobs++;
    }
    RasterPage manual_tray = base;
    manual_tray.media_position = 4;
    assert(two_page_mode(filter, &manual_tray, "", NULL) == SHEETS_AUTOMATIC);
    jobs++;

    /* DupUnit: job option first, then the queue PPD's *DefaultDupUnit, else installed. */
    char *ppd_false = write_ppd("*PPD-Adobe: \"4.3\"\n*OpenUI *DupUnit/Duplex Unit: Boolean\n"
                                "*OrderDependency: 10 AnySetup *DupUnit\n*DefaultDupUnit: False\n"
                                "*DupUnit True/Yes: \"\"\n*DupUnit False/No: \"\"\n*CloseUI: *DupUnit\n");
    char *ppd_no = write_ppd("*PPD-Adobe: \"4.3\"\n*DefaultDupUnit:   No\n");
    char *ppd_true = write_ppd("*PPD-Adobe: \"4.3\"\n*OpenUI *DupUnit/Duplex Unit: Boolean\n"
                               "*DefaultDupUnit: True\n*CloseUI: *DupUnit\n");
    char *ppd_none = write_ppd("*PPD-Adobe: \"4.3\"\n*ModelName: \"HP LaserJet Professional P1606dn\"\n");
    char *ppd_missing = temp_path("mode-ppd-missing");
    assert(unlink(ppd_missing) == 0);
    const struct {
        const char *options;
        const char *ppd;
        SheetMode mode;
    } units[] = {
        {"", NULL, SHEETS_AUTOMATIC}, {"", ppd_false, SHEETS_MANUAL}, {"", ppd_no, SHEETS_MANUAL},
        {"", ppd_true, SHEETS_AUTOMATIC}, {"", ppd_none, SHEETS_AUTOMATIC},
        {"", ppd_missing, SHEETS_AUTOMATIC}, {"DupUnit=False", ppd_true, SHEETS_MANUAL},
        {"DupUnit=No", NULL, SHEETS_MANUAL}, {"DupUnit=Off", NULL, SHEETS_MANUAL},
        {"DupUnit=0", NULL, SHEETS_MANUAL}, {"DupUnit=True", ppd_false, SHEETS_AUTOMATIC},
        {"DupUnit=Yes", ppd_false, SHEETS_AUTOMATIC}, {"DupUnit=Installed", ppd_no, SHEETS_AUTOMATIC}
    };
    for (size_t i = 0; i < sizeof(units) / sizeof(units[0]); i++) {
        assert(two_page_mode(filter, &base, units[i].options, units[i].ppd) == units[i].mode);
        jobs++;
    }
    remove_path(ppd_false);
    remove_path(ppd_no);
    remove_path(ppd_true);
    remove_path(ppd_none);
    free(ppd_missing);
    printf("    [PASS] Automatic Duplex Paper/Media Eligibility and DupUnit Option/PPD Matrix (%u jobs)\n", jobs);
}

/* The queue's settings in its PPD (*Default<keyword>: what lpadmin -o and
 * the printer's settings change; cupsd does not repeat them in the job
 * options) apply when the job does not name the option; the job wins. */
static void test_ppd_defaults_matrix(const char *filter) {
    char *ppds[] = {
        write_ppd("*PPD-Adobe: \"4.3\"\n*DefaultpjlDensity: 5\n*DefaultpjlJamRecovery: True\n"
                  "*DefaultTersKose: True\n*DefaultEconoMode: True\n"),
        write_ppd("*PPD-Adobe: \"4.3\"\n*DefaultpjlDensity: 3\n*DefaultpjlJamRecovery: False\n"
                  "*DefaultTersKose: False\n*DefaultEconoMode: False\n"),
        write_ppd("*PPD-Adobe: \"4.3\"\n*DefaultpjlDensity:  2\n"),
        write_ppd("*PPD-Adobe: \"4.3\"\n*DefaultpjlDensity: 9\n*DefaultEconoMode:\n*DefaultTersKose: Maybe\n"),
    };
    enum { PPD_SET, PPD_PLAIN, PPD_DENSITY, PPD_BAD, PPD_NONE };
    static const struct {
        const char *options;
        int ppd;
        int density;
        int jam;
        uint32_t econo;
        int rotated;
    } cases[] = {
        {"", PPD_SET, 5, 1, 1, 1},
        {"", PPD_PLAIN, 3, 0, 0, 0},
        {"", PPD_DENSITY, 2, 0, 0, 0},
        {"", PPD_BAD, 3, 0, 0, 0},
        {"", PPD_NONE, 3, 0, 0, 0},
        {"pjlDensity=1 pjlJamRecovery=False TersKose=False EconoMode=False", PPD_SET, 1, 0, 0, 0},
        {"PrintDensity=4 JamRecovery=Off ReverseLandscape=Off TonerSave=Off", PPD_SET, 4, 0, 0, 0},
        {"EconoMode=True TersKose=True", PPD_PLAIN, 3, 0, 1, 1},
        {"pjlDensity=4", PPD_DENSITY, 4, 0, 0, 0},
    };
    RasterPage page = gray_page(45, 29);
    page.fill = FILL_NOISE;
    page.seed = 21;
    RasterPage turned = page;
    turned.rotated = 1;
    char pjl[512];
    unsigned int jobs = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        RunOptions run = run_defaults();
        run.options = cases[i].options;
        run.ppd = cases[i].ppd == PPD_NONE ? NULL : ppds[cases[i].ppd];
        FilterResult result = run_pages(filter, &page, 1, &run);
        expected_pjl(pjl, sizeof(pjl), DEFAULT_TITLE, cases[i].density, cases[i].jam);
        ZjsJob job;
        expect_job(&result, pjl, &job);
        assert(job.page_count == 1U && job.pages[0].econo == cases[i].econo);
        /* The page itself: turned 180 degrees when TersKose applies. */
        RunOptions reference_run = run_defaults();
        reference_run.options = cases[i].econo ? "EconoMode=True" : "";
        FilterResult reference = run_pages(filter, cases[i].rotated ? &turned : &page, 1, &reference_run);
        ZjsJob reference_job;
        expect_job(&reference, NULL, &reference_job);
        assert(reference_job.page_count == 1U);
        assert(same_page_bytes(&result.output, &job.pages[0], &reference.output, &reference_job.pages[0]));
        free_job(&reference_job);
        free_result(&reference);
        free_job(&job);
        free_result(&result);
        jobs += 2U;
    }
    for (size_t i = 0; i < sizeof(ppds) / sizeof(ppds[0]); i++) remove_path(ppds[i]);
    printf("    [PASS] Queue PPD Defaults for Density, Jam Recovery, EconoMode and TersKose; Job Options Win (%u jobs)\n",
           jobs);
}

static void test_duplex_transition_matrix(const char *filter) {
    static const unsigned int states[][2] = {{0, 0}, {0, 1}, {1, 0}, {1, 1}};
    const size_t state_count = sizeof(states) / sizeof(states[0]);
    unsigned int jobs = 0;
    for (int legacy = 0; legacy <= 1; legacy++) {
        for (size_t first = 0; first < state_count; first++) {
            for (size_t second = 0; second < state_count; second++) {
                RasterPage pages[2] = {sheet_page(legacy, 1, states[first][0], states[first][1]),
                                       sheet_page(legacy, 2, states[second][0], states[second][1])};
                int compatible = states[first][0] == states[second][0] &&
                                 (states[first][0] == 0U || states[first][1] == states[second][1]);
                FilterResult result = run_pages(filter, pages, 2, NULL);
                if (compatible) {
                    ZjsJob job;
                    expect_job(&result, NULL, &job);
                    assert(job.page_count == 2U);
                    /* Simplex keeps the order; automatic duplex sends page 2 first. */
                    assert(job.pages[0].video_y == (states[first][0] ? 24U : 12U));
                    free_job(&job);
                } else {
                    expect_rejection(&result, "Duplex mode changed inside the raster job on page 2");
                }
                free_result(&result);
                jobs++;
            }
        }
    }
    RasterPage three[3] = {sheet_page(0, 1, 1, 0), sheet_page(0, 2, 1, 0), sheet_page(0, 3, 1, 1)};
    expect_rejected_pages(filter, three, 3, NULL, "Duplex mode changed inside the raster job on page 3");
    three[2].duplex = 0;
    three[2].tumble = 0;
    expect_rejected_pages(filter, three, 3, NULL, "Duplex mode changed inside the raster job on page 3");
    printf("    [PASS] Duplex/Tumble Transition Matrix (%u jobs)\n", jobs + 2U);
}

static FilterResult run_arity(const char *filter, int argc) {
    char *diagnostics_path = temp_path("mode-arity");
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        int null_fd = open("/dev/null", O_RDWR);
        int err = open(diagnostics_path, O_WRONLY | O_TRUNC);
        if (null_fd < 0 || err < 0 || dup2(null_fd, STDIN_FILENO) < 0 || dup2(null_fd, STDOUT_FILENO) < 0 ||
            dup2(err, STDERR_FILENO) < 0) _exit(126);
        close(null_fd);
        close(err);
        alarm(FILTER_TIMEOUT_SECONDS);
        switch (argc) {
        case 1:
            execl(filter, filter, (char *)NULL);
            break;
        case 2:
            execl(filter, filter, "1", (char *)NULL);
            break;
        case 5:
            execl(filter, filter, "1", "user", "title", "1", (char *)NULL);
            break;
        default:
            execl(filter, filter, "1", "user", "title", "1", "", "/nonexistent/p1600dn.ras", "extra",
                  (char *)NULL);
            break;
        }
        _exit(127);
    }
    int wait_status = 0;
    assert(waitpid(pid, &wait_status, 0) == pid);
    FilterResult result;
    memset(&result, 0, sizeof(result));
    result.status = WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : 128 + WTERMSIG(wait_status);
    result.output.data = malloc(1);
    assert(result.output.data != NULL);
    result.diagnostics = read_path(diagnostics_path);
    remove_path(diagnostics_path);
    return result;
}

static void test_rejection_matrix(const char *filter) {
    unsigned int jobs = 0;
    static const char model_reason[] = "Raster was not prepared by the HP LaserJet P1600dn PPD on page 1";
    static const char layout_reason[] = "Unsupported raster color layout on page 1";
    static const char size_reason[] = "Unsupported raster resolution or page size on page 1";
    static const char format_reason[] = "Unsupported raster format (expected 600 dpi 8-bit gray) on page 1";

    RasterPage invalid[32];
    const char *reasons[32];
    size_t n = 0;
    static const unsigned int models[] = {0, 1010, 1605, 1607, 0x10000U + 1606U};
    for (size_t i = 0; i < sizeof(models) / sizeof(models[0]); i++) {
        invalid[n] = gray_page(20, 5);
        invalid[n].model = models[i];
        reasons[n++] = model_reason;
    }
    invalid[n] = legacy_page(20, 5);
    invalid[n].model = 1010;
    reasons[n++] = model_reason;

    invalid[n] = gray_page(20, 5);
    invalid[n].num_colors = 3;
    reasons[n++] = layout_reason;
    invalid[n] = gray_page(20, 5);
    invalid[n].color_order = CUPS_ORDER_BANDED;
    reasons[n++] = layout_reason;
    invalid[n] = gray_page(20, 5);
    invalid[n].color_order = CUPS_ORDER_PLANAR;
    reasons[n++] = layout_reason;
    invalid[n] = legacy_page(20, 5);
    invalid[n].num_colors = 2;
    reasons[n++] = layout_reason;

    static const unsigned int bad_y[][2] = {{600, 300}, {600, 1200}, {1200, 1200}, {600, 0}};
    for (size_t i = 0; i < sizeof(bad_y) / sizeof(bad_y[0]); i++) {
        invalid[n] = gray_page(20, 5);
        invalid[n].resolution_x = bad_y[i][0];
        invalid[n].resolution_y = bad_y[i][1];
        reasons[n++] = size_reason;
    }

    static const unsigned int gray_x[] = {300, 1200, 2400};
    for (size_t i = 0; i < sizeof(gray_x) / sizeof(gray_x[0]); i++) {
        invalid[n] = gray_page(20, 5);
        invalid[n].resolution_x = gray_x[i];
        reasons[n++] = format_reason;
    }
    static const unsigned int legacy_x[] = {600, 2400};
    for (size_t i = 0; i < sizeof(legacy_x) / sizeof(legacy_x[0]); i++) {
        invalid[n] = legacy_page(20, 5);
        invalid[n].resolution_x = legacy_x[i];
        reasons[n++] = format_reason;
    }
    static const unsigned int gray_spaces[] = {CUPS_CSPACE_K, CUPS_CSPACE_SW, CUPS_CSPACE_RGB, CUPS_CSPACE_CMYK};
    for (size_t i = 0; i < sizeof(gray_spaces) / sizeof(gray_spaces[0]); i++) {
        invalid[n] = gray_page(20, 5);
        invalid[n].color_space = gray_spaces[i];
        reasons[n++] = format_reason;
    }
    invalid[n] = legacy_page(20, 5);
    invalid[n].color_space = CUPS_CSPACE_W;
    reasons[n++] = format_reason;
    invalid[n] = gray_page(20, 5);
    invalid[n].bits_per_color = 16;
    invalid[n].bits_per_pixel = 16;
    invalid[n].bytes_per_line = 40;
    reasons[n++] = format_reason;
    invalid[n] = gray_page(20, 5);
    invalid[n].bits_per_color = 1;
    invalid[n].bits_per_pixel = 1;
    invalid[n].bytes_per_line = 3;
    reasons[n++] = format_reason;
    invalid[n] = legacy_page(20, 5);
    invalid[n].bits_per_color = 2;
    invalid[n].bits_per_pixel = 2;
    invalid[n].bytes_per_line = 5;
    reasons[n++] = format_reason;
    invalid[n] = legacy_page(20, 5);
    invalid[n].bits_per_color = 8;
    invalid[n].bits_per_pixel = 8;
    invalid[n].bytes_per_line = 20;
    reasons[n++] = format_reason;
    assert(n <= sizeof(invalid) / sizeof(invalid[0]));

    for (size_t i = 0; i < n; i++) {
        FilterResult result = run_pages(filter, &invalid[i], 1, NULL);
        expect_rejection(&result, reasons[i]);
        assert(result.output.len == 0U);
        free_result(&result);
        jobs++;
    }

    /* A bad later page fails the job after the earlier pages. */
    RasterPage later[2] = {gray_page(20, 5), gray_page(20, 5)};
    later[1].model = 1010;
    expect_rejected_pages(filter, later, 2, NULL,
                          "Raster was not prepared by the HP LaserJet P1600dn PPD on page 2");
    jobs++;

    /* argv[4] must be a plain positive decimal, for both formats. */
    static const char *const bad_copies[] = {"0", "-1", "+1", " 1", "1 ", "1x", "", "2147483648",
                                             "999999999999999999999999", "0x10", "1.0"};
    for (int legacy = 0; legacy <= 1; legacy++) {
        RasterPage page = legacy ? legacy_page(20, 5) : gray_page(20, 5);
        for (size_t i = 0; i < sizeof(bad_copies) / sizeof(bad_copies[0]); i++) {
            RunOptions run = run_defaults();
            run.copies = bad_copies[i];
            FilterResult result = run_pages(filter, &page, 1, &run);
            expect_rejection(&result, "Invalid copy count");
            assert(result.output.len == 0U);
            free_result(&result);
            jobs++;
        }
    }

    static const int arities[] = {1, 2, 5, 8};
    for (size_t i = 0; i < sizeof(arities) / sizeof(arities[0]); i++) {
        FilterResult result = run_arity(filter, arities[i]);
        expect_rejection(&result, "ERROR: Usage:");
        free_result(&result);
        jobs++;
    }

    char *missing = temp_path("mode-missing");
    assert(unlink(missing) == 0);
    FilterResult missing_result = run_filter(filter, missing, &(RunOptions){
        .title = DEFAULT_TITLE, .copies = "1", .options = "", .transport = TRANSPORT_FILE});
    expect_rejection(&missing_result, "Unable to open raster file");
    free_result(&missing_result);
    free(missing);
    const char *dir = getenv("TMPDIR");
    FilterResult directory_result = run_filter(filter, dir && *dir ? dir : "/tmp", &(RunOptions){
        .title = DEFAULT_TITLE, .copies = "1", .options = "", .transport = TRANSPORT_FILE});
    expect_rejection(&directory_result, NULL);
    assert(directory_result.output.len == 0U);
    free_result(&directory_result);
    jobs += 2U;
    printf("    [PASS] Invalid Header/Model/Format/Copies/Arity/Input Rejection Matrix (%u jobs)\n", jobs);
}

static void test_truncation_and_output_failures(const char *filter) {
    unsigned int jobs = 0;
    RasterPage pages[2] = {gray_page(40, 13), gray_page(24, 5)};
    pages[0].fill = FILL_NOISE;
    pages[1].fill = FILL_NOISE;
    pages[1].seed = 5;
    char *path = temp_path("mode-truncated");
    write_raster(path, pages, 2);
    Blob full = read_path(path);
    const size_t pixels0 = 40U * 13U;
    const size_t pixels1 = 24U * 5U;
    const size_t header = sizeof(cups_page_header2_t);
    assert(full.len == 4U + 2U * header + pixels0 + pixels1);
    const size_t page2 = 4U + header + pixels0;
    const size_t cuts[] = {
        1, 3, 5, 4U + header / 2U, 4U + header - 1U, 4U + header, 4U + header + 1U,
        4U + header + pixels0 / 2U, page2 - 1U, page2 + 1U, page2 + header / 2U, page2 + header,
        page2 + header + 1U, full.len - 1U
    };
    for (size_t i = 0; i < sizeof(cuts) / sizeof(cuts[0]); i++) {
        write_path(path, full.data, cuts[i]);
        RunOptions run = run_defaults();
        run.transport = (Transport)(i % 3U);
        FilterResult result = run_filter(filter, path, &run);
        expect_rejection(&result, NULL);
        free_result(&result);
        jobs++;
    }
    /* Cuts on stream boundaries are complete (shorter) jobs. */
    write_path(path, full.data, 4U);
    FilterResult sync_only = run_filter(filter, path, &(RunOptions){
        .title = DEFAULT_TITLE, .copies = "1", .options = "", .transport = TRANSPORT_STDIN_PIPE});
    assert(sync_only.status == 0 && sync_only.output.len == 0U);
    free_result(&sync_only);
    write_path(path, full.data, page2);
    FilterResult first_page = run_filter(filter, path, &(RunOptions){
        .title = DEFAULT_TITLE, .copies = "1", .options = "", .transport = TRANSPORT_FILE});
    ZjsJob job;
    expect_job(&first_page, NULL, &job);
    assert(job.page_count == 1U && job.pages[0].video_x == 40U);
    free_job(&job);
    free_result(&first_page);
    jobs += 2U;

    /* Unwritable stdout: read-only or closed, simplex and spooled duplex. */
    write_path(path, full.data, full.len);
    static const struct {
        StdoutMode stdout_mode;
        Transport transport;
    } outputs[] = {
        {STDOUT_READ_ONLY, TRANSPORT_FILE}, {STDOUT_READ_ONLY, TRANSPORT_STDIN_PIPE},
        {STDOUT_CLOSED, TRANSPORT_STDIN_FILE}, {STDOUT_CLOSED, TRANSPORT_FILE}
    };
    RasterPage duplex_pages[3] = {sheet_page(0, 1, 1, 0), sheet_page(0, 2, 1, 0), sheet_page(0, 3, 1, 0)};
    char *duplex_path = temp_path("mode-duplex");
    write_raster(duplex_path, duplex_pages, 3);
    RasterPage large = legacy_page(3000, 708);
    large.fill = FILL_NOISE;
    char *large_path = temp_path("mode-large");
    write_raster(large_path, &large, 1);
    const char *const inputs[] = {path, duplex_path, large_path};
    for (size_t input = 0; input < 3U; input++) {
        Blob before = read_path(inputs[input]);
        for (size_t i = 0; i < sizeof(outputs) / sizeof(outputs[0]); i++) {
            RunOptions run = run_defaults();
            run.stdout_mode = outputs[i].stdout_mode;
            run.transport = outputs[i].transport;
            FilterResult result = run_filter(filter, inputs[input], &run);
            if (result.status == 0 || !contains(&result.diagnostics, "ERROR:")) {
                print_diagnostics(&result);
                assert(0 && "unwritable stdout must fail the job");
            }
            free_result(&result);
            jobs++;
        }
        /* The input stays untouched even when it lands on descriptor 1. */
        Blob after = read_path(inputs[input]);
        assert(after.len == before.len && memcmp(after.data, before.data, before.len) == 0);
        free_blob(&before);
        free_blob(&after);
    }
    remove_path(duplex_path);
    remove_path(large_path);
    remove_path(path);
    free_blob(&full);
    printf("    [PASS] Truncated Raster Sweep and Read-Only/Closed Stdout Matrix (%u jobs)\n", jobs);
}

static void test_corrupt_raster_fuzz(const char *filter) {
    static const unsigned char sync_prefixes[][4] = {
        {'R', 'a', 'S', '3'}, {'3', 'S', 'a', 'R'},
        {'R', 'a', 'S', '2'}, {'2', 'S', 'a', 'R'},
        {'R', 'a', 'S', 't'}, {'t', 'S', 'a', 'R'},
        {'U', 'N', 'I', 'R'}, {'R', 'I', 'N', 'U'}
    };
    char *path = temp_path("corrupt-fuzz");
    uint32_t state = 0x1606d00dU;
    unsigned int accepted = 0;

    for (unsigned int test_case = 0; test_case < 256U; test_case++) {
        size_t length = (size_t)((test_case * 977U) % 4097U);
        unsigned char *data = malloc(length == 0 ? 1U : length);
        assert(data != NULL);
        for (size_t i = 0; i < length; i++) {
            state = xorshift(state);
            data[i] = (unsigned char)(state >> 24);
        }
        if (length >= 4U) {
            size_t prefix = test_case % (sizeof(sync_prefixes) / sizeof(sync_prefixes[0]));
            memcpy(data, sync_prefixes[prefix], 4U);
        }
        write_path(path, data, length);
        free(data);

        RunOptions run = run_defaults();
        run.transport = test_case & 1U ? TRANSPORT_STDIN_FILE : TRANSPORT_FILE;
        FilterResult result = run_filter(filter, path, &run);
        assert(result.status == 0 || result.status == 1);
        if (result.status == 1) {
            assert(contains(&result.diagnostics, "ERROR:"));
        } else if (result.output.len > 0U) {
            ZjsJob job;
            expect_job(&result, NULL, &job);
            free_job(&job);
        }
        accepted += result.status == 0 ? 1U : 0U;
        free_result(&result);
    }
    remove_path(path);
    printf("    [PASS] Deterministic Corrupt-Raster Fuzz Matrix (256 isolated processes, %u empty jobs)\n",
           accepted);
}

static void test_header_mutation_fuzz(const char *filter) {
    RasterPage pages[2] = {gray_page(40, 13), legacy_page(61, 5)};
    pages[0].fill = FILL_NOISE;
    pages[1].fill = FILL_NOISE;
    pages[1].seed = 3;
    char *path = temp_path("mutation-fuzz");
    write_raster(path, pages, 2);
    Blob base = read_path(path);
    const size_t header = sizeof(cups_page_header2_t);
    assert(base.len == 4U + 2U * header + 40U * 13U + 8U * 5U);
    const size_t header_at[2] = {4U, 4U + header + 40U * 13U};
    static const size_t fields[] = {
        offsetof(cups_page_header2_t, HWResolution), offsetof(cups_page_header2_t, HWResolution) + 4U,
        offsetof(cups_page_header2_t, cupsWidth), offsetof(cups_page_header2_t, cupsHeight),
        offsetof(cups_page_header2_t, cupsBitsPerColor), offsetof(cups_page_header2_t, cupsBitsPerPixel),
        offsetof(cups_page_header2_t, cupsBytesPerLine), offsetof(cups_page_header2_t, cupsColorOrder),
        offsetof(cups_page_header2_t, cupsColorSpace), offsetof(cups_page_header2_t, cupsCompression),
        offsetof(cups_page_header2_t, cupsNumColors), offsetof(cups_page_header2_t, cupsInteger),
        offsetof(cups_page_header2_t, PageSize), offsetof(cups_page_header2_t, PageSize) + 4U,
        offsetof(cups_page_header2_t, Duplex), offsetof(cups_page_header2_t, Tumble),
        offsetof(cups_page_header2_t, Collate), offsetof(cups_page_header2_t, NumCopies),
        offsetof(cups_page_header2_t, MediaPosition), offsetof(cups_page_header2_t, cupsMediaType),
        offsetof(cups_page_header2_t, MediaType)
    };
    static const uint32_t interesting[] = {0, 1, 2, 3, 8, 12, 600, 1200, 1606, 10800, 10801, 21600,
                                           0x7fffffffU, 0x80000000U, 0xffffffffU};
    unsigned char *data = malloc(base.len);
    assert(data != NULL);
    uint32_t state = 0x5eed1606U;
    unsigned int accepted = 0;
    unsigned int empty = 0;
    for (unsigned int test_case = 0; test_case < 128U; test_case++) {
        memcpy(data, base.data, base.len);
        for (unsigned int mutation = 0; mutation <= test_case % 3U; mutation++) {
            state = xorshift(state);
            size_t at = header_at[(state >> 8) & 1U] + fields[state % (sizeof(fields) / sizeof(fields[0]))];
            state = xorshift(state);
            uint32_t value;
            if (state & 1U) {
                value = interesting[(state >> 1) % (sizeof(interesting) / sizeof(interesting[0]))];
            } else {
                memcpy(&value, data + at, sizeof(value));
                value ^= 1U << ((state >> 1) % 32U);
            }
            memcpy(data + at, &value, sizeof(value));
        }
        write_path(path, data, base.len);
        RunOptions run = run_defaults();
        run.transport = test_case & 1U ? TRANSPORT_STDIN_PIPE : TRANSPORT_FILE;
        FilterResult result = run_filter(filter, path, &run);
        assert(result.status == 0 || result.status == 1);
        if (result.status == 1) {
            assert(contains(&result.diagnostics, "ERROR:"));
        } else if (result.output.len == 0U) {
            empty++;
        } else {
            ZjsJob job;
            expect_job(&result, NULL, &job);
            free_job(&job);
            accepted++;
        }
        free_result(&result);
    }
    free(data);
    free_blob(&base);
    remove_path(path);
    printf("    [PASS] Deterministic Header-Mutation Fuzz Matrix (128 isolated processes, %u valid, %u empty jobs)\n",
           accepted, empty);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    signal(SIGPIPE, SIG_IGN);
    puts("[*] Starting exhaustive CUPS filter mode tests...");
    test_job_framing_matrix(argv[1]);
    test_page_items_matrix(argv[1]);
    test_raster_format_matrix(argv[1]);
    test_screening_matrix(argv[1]);
    test_bid_chunking(argv[1]);
    test_rotation_matrix(argv[1]);
    test_sheet_order_matrix(argv[1]);
    test_automatic_duplex_eligibility(argv[1]);
    test_ppd_defaults_matrix(argv[1]);
    test_duplex_transition_matrix(argv[1]);
    test_rejection_matrix(argv[1]);
    test_truncation_and_output_failures(argv[1]);
    test_corrupt_raster_fuzz(argv[1]);
    test_header_mutation_fuzz(argv[1]);
    puts("[+] Exhaustive CUPS filter mode tests passed.");
    return 0;
}
