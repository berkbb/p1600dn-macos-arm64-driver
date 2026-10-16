/*
 * Tests for parallel page encoding (src/p1600_encoder_pool.c) and for how
 * rastertop1600dn (argv[1]) uses it: pages come back in order with exactly
 * the bytes zjs_write_page() writes for the page with its padding forced
 * white (the page guard), for every worker count and depth; failed
 * pages, close and teardown; the P1600DN_ENCODER_THREADS override; filter
 * output identical for 0-4 encoder threads; and pages (and automatic-duplex
 * sheets) reaching the printer while the next raster page has not arrived.
 *
 * Usage: test_encoder_pool <rastertop1600dn>
 */
#include <assert.h>
#include <cups/raster.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "p1600_encoder_pool.h"
#include "zjs_encoder.h"
#include "zjs_guard.h"

static uint32_t random_state = 0x1606u;

static uint32_t next_random(void) {
    random_state = random_state * 1664525u + 1013904223u;
    return random_state;
}

typedef struct {
    ZjsPageOptions page;
    unsigned char *bitmap;
    unsigned char *expected;
    size_t expected_length;
    bool valid;
} TestPage;

static TestPage make_page(unsigned index, bool valid) {
    TestPage test = {.valid = valid};
    uint32_t width = 64U + (next_random() % 400U), height = 12U * (1U + next_random() % 40U);
    test.page = (ZjsPageOptions){.paper = 9, .copies = 1 + index % 3U, .source = 7, .media = 1,
                                 .width = width, .height = valid ? height : height + 1U,
                                 .bpp = 1U + index % 2U, .econo_mode = index % 4U == 0};
    size_t bytes = zjs_line_bytes(width, test.page.bpp) * (height + 1U);
    test.bitmap = malloc(bytes);
    assert(test.bitmap != NULL);
    for (size_t i = 0; i < bytes; i++) test.bitmap[i] = (uint8_t)(index % 3U == 0 ? next_random() : (i % 97U < 3U ? 0xffU : 0));
    if (valid) {
        /* The pool sends the page with its padding forced white (page guard). */
        unsigned char *cleared = malloc(bytes);
        assert(cleared != NULL);
        memcpy(cleared, test.bitmap, bytes);
        zjs_guard_clear_padding(&test.page, cleared);
        char *buffer = NULL;
        size_t length = 0;
        FILE *stream = open_memstream(&buffer, &length);
        assert(stream != NULL);
        assert(zjs_write_page(stream, &test.page, cleared) == 0);
        free(cleared);
        assert(fclose(stream) == 0);
        test.expected = (unsigned char *)buffer;
        test.expected_length = length;
    }
    return test;
}

typedef struct {
    P1600EncoderPool *pool;
    TestPage *pages;
    size_t count;
    size_t taken;
} Consumer;

static void *consume(void *argument) {
    Consumer *consumer = argument;
    unsigned char *data = NULL;
    size_t length = 0;
    int result;
    while ((result = p1600_encoder_pool_next(consumer->pool, &data, &length)) != 0) {
        assert(consumer->taken < consumer->count);
        const TestPage *expected = &consumer->pages[consumer->taken++];
        if (expected->valid) {
            assert(result == 1);
            assert(length == expected->expected_length);
            assert(memcmp(data, expected->expected, length) == 0);
        } else {
            assert(result == -1 && data == NULL && length == 0);
        }
        free(data);
    }
    return NULL;
}

static void test_pool_order_and_bytes(void) {
    enum { PAGES = 9 };
    unsigned runs = 0;
    for (unsigned workers = 0; workers <= P1600_ENCODER_MAX_WORKERS; workers++) {
        for (unsigned depth = 1; depth <= 6; depth += 2) {
            TestPage pages[PAGES];
            for (unsigned i = 0; i < PAGES; i++) pages[i] = make_page(i, i != 4U || depth != 3U);
            P1600EncoderPool *pool = p1600_encoder_pool_create(workers, depth);
            assert(pool != NULL);
            Consumer consumer = {pool, pages, PAGES, 0};
            pthread_t thread;
            assert(pthread_create(&thread, NULL, consume, &consumer) == 0);
            for (unsigned i = 0; i < PAGES; i++) {
                assert(p1600_encoder_pool_submit(pool, &pages[i].page, pages[i].bitmap) == 0);
                pages[i].bitmap = NULL;  /* owned by the pool */
            }
            p1600_encoder_pool_close(pool);
            assert(pthread_join(thread, NULL) == 0);
            assert(consumer.taken == PAGES);
            unsigned char *data = (unsigned char *)"x";
            size_t length = 1;
            assert(p1600_encoder_pool_next(pool, &data, &length) == 0 && data == NULL && length == 0);
            p1600_encoder_pool_destroy(pool);
            for (unsigned i = 0; i < PAGES; i++) free(pages[i].expected);
            runs++;
        }
    }
    printf("[PASS] Encoder pool: %u worker/depth combinations, submission order, exact bytes, failed page\n", runs);
}

static void test_pool_edges(void) {
    assert(p1600_encoder_pool_create(P1600_ENCODER_MAX_WORKERS + 1U, 2) == NULL);
    assert(p1600_encoder_pool_create(1, 0) == NULL);

    /* Closed pools refuse pages and free them. */
    P1600EncoderPool *pool = p1600_encoder_pool_create(2, 2);
    assert(pool != NULL);
    TestPage page = make_page(1, true);
    p1600_encoder_pool_close(pool);
    assert(p1600_encoder_pool_submit(pool, &page.page, page.bitmap) == -1);
    p1600_encoder_pool_destroy(pool);
    free(page.expected);

    /* Teardown with pages never taken back (leaks show up under ASan). */
    for (unsigned workers = 0; workers <= 2; workers++) {
        pool = p1600_encoder_pool_create(workers, 3);
        assert(pool != NULL);
        for (unsigned i = 0; i < 3; i++) {
            TestPage held = make_page(i, true);
            assert(p1600_encoder_pool_submit(pool, &held.page, held.bitmap) == 0);
            free(held.expected);
        }
        p1600_encoder_pool_destroy(pool);
    }

    pool = p1600_encoder_pool_create(0, 1);
    unsigned char *bitmap = malloc(16);
    assert(p1600_encoder_pool_submit(NULL, &page.page, bitmap) == -1);
    bitmap = malloc(16);
    assert(p1600_encoder_pool_submit(pool, NULL, bitmap) == -1);
    assert(p1600_encoder_pool_submit(pool, &page.page, NULL) == -1);
    unsigned char *data = NULL;
    size_t length = 0;
    assert(p1600_encoder_pool_next(NULL, &data, &length) == -1);
    assert(p1600_encoder_pool_next(pool, NULL, &length) == -1);
    assert(p1600_encoder_pool_next(pool, &data, NULL) == -1);
    p1600_encoder_pool_close(NULL);
    p1600_encoder_pool_destroy(NULL);
    p1600_encoder_pool_destroy(pool);
    puts("[PASS] Encoder pool: limits, closed pool, teardown with pending pages, invalid arguments");
}

static void test_default_workers(void) {
    unsetenv("P1600DN_ENCODER_THREADS");
    unsigned automatic = p1600_encoder_default_workers();
    assert(automatic >= 1U && automatic <= P1600_ENCODER_MAX_WORKERS);
    static const struct {
        const char *value;
        int expected; /* -1: automatic */
    } cases[] = {{"0", 0}, {"1", 1}, {"3", 3}, {"4", 4}, {"16", 4}, {"17", -1}, {"-1", -1},
                 {"x", -1}, {"2x", -1}, {"", -1}, {"99999999999999999999", -1}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        setenv("P1600DN_ENCODER_THREADS", cases[i].value, 1);
        unsigned workers = p1600_encoder_default_workers();
        assert(workers == (cases[i].expected < 0 ? automatic : (unsigned)cases[i].expected));
    }
    unsetenv("P1600DN_ENCODER_THREADS");
    printf("[PASS] Encoder threads: %u on this Mac (performance cores, at most %u), environment override\n",
           automatic, P1600_ENCODER_MAX_WORKERS);
}

/* --- rastertop1600dn ---------------------------------------------------- */

typedef struct {
    unsigned width, height, pages, compression;
    bool duplex, tumble, legacy;
    unsigned page_width, page_height;
} RasterJob;

static void write_page(cups_raster_t *raster, const RasterJob *job, unsigned index) {
    cups_page_header2_t header;
    memset(&header, 0, sizeof(header));
    header.HWResolution[0] = job->legacy ? 1200 : 600;
    header.HWResolution[1] = 600;
    header.cupsWidth = job->width;
    header.cupsHeight = job->height;
    header.cupsBitsPerColor = header.cupsBitsPerPixel = job->legacy ? 1 : 8;
    header.cupsBytesPerLine = job->legacy ? (job->width + 7U) / 8U : job->width;
    header.cupsColorOrder = CUPS_ORDER_CHUNKED;
    header.cupsColorSpace = job->legacy ? CUPS_CSPACE_K : CUPS_CSPACE_W;
    header.cupsNumColors = 1;
    header.cupsCompression = job->compression;
    header.cupsInteger[0] = 1606;
    header.PageSize[0] = job->page_width;
    header.PageSize[1] = job->page_height;
    header.NumCopies = 1;
    header.MediaPosition = 7;
    header.cupsMediaType = 1;
    header.Duplex = job->duplex;
    header.Tumble = job->tumble;
    assert(cupsRasterWriteHeader2(raster, &header));
    unsigned char *row = malloc(header.cupsBytesPerLine);
    assert(row != NULL);
    for (unsigned y = 0; y < job->height; y++) {
        for (unsigned x = 0; x < header.cupsBytesPerLine; x++) {
            uint32_t noise = (x + index * 131U) * 0x9e3779b9U ^ y * 0x85ebca6bU;
            row[x] = (unsigned char)((noise >> 13) ^ (x * 255U / header.cupsBytesPerLine));
        }
        assert(cupsRasterWritePixels(raster, row, header.cupsBytesPerLine) == header.cupsBytesPerLine);
    }
    free(row);
}

static char *temp_path(void) {
    const char *directory = getenv("TMPDIR");
    char *path = NULL;
    assert(asprintf(&path, "%s/p1600dn-pool-XXXXXX", directory && *directory ? directory : "/tmp") > 0);
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    return path;
}

static unsigned char *read_file(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    assert(fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file);
    assert(size >= 0);
    rewind(file);
    unsigned char *data = malloc((size_t)size + 1U);
    assert(data != NULL);
    assert(fread(data, 1, (size_t)size, file) == (size_t)size);
    fclose(file);
    *length = (size_t)size;
    return data;
}

static unsigned char *run_filter(const char *filter, const char *raster, const char *threads, size_t *length) {
    char *output = temp_path();
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        int out = open(output, O_WRONLY | O_TRUNC), null = open("/dev/null", O_WRONLY);
        if (out < 0 || null < 0 || dup2(out, STDOUT_FILENO) < 0 || dup2(null, STDERR_FILENO) < 0) _exit(126);
        if (threads) setenv("P1600DN_ENCODER_THREADS", threads, 1);
        unsetenv("DEVICE_URI");
        unsetenv("PPD");
        alarm(120);
        execl(filter, "HP_P1606dn", "1", "user", "Pool", "1", "", raster, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    unsigned char *data = read_file(output, length);
    unlink(output);
    free(output);
    return data;
}

static void test_filter_thread_counts(const char *filter) {
    static const RasterJob jobs[] = {
        {900, 1300, 5, 1, false, false, false, 595, 842},
        {900, 1300, 5, 0, true, false, false, 595, 842},   /* automatic duplex, odd */
        {900, 1300, 4, 1, true, true, false, 612, 792},    /* automatic duplex, short edge */
        {900, 1300, 5, 1, true, false, false, 420, 595},   /* A5: manual duplex */
        {1802, 1201, 3, 0, true, false, true, 595, 842},   /* PPD 3.x 1-bit */
    };
    static const char *const threads[] = {"0", "1", "2", "4", NULL};
    for (size_t j = 0; j < sizeof(jobs) / sizeof(jobs[0]); j++) {
        char *raster = temp_path();
        int fd = open(raster, O_WRONLY | O_TRUNC);
        assert(fd >= 0);
        cups_raster_t *stream = cupsRasterOpen(fd, CUPS_RASTER_WRITE);
        for (unsigned p = 0; p < jobs[j].pages; p++) write_page(stream, &jobs[j], p);
        cupsRasterClose(stream);
        close(fd);

        size_t reference_length = 0;
        unsigned char *reference = run_filter(filter, raster, "0", &reference_length);
        for (size_t t = 1; t < sizeof(threads) / sizeof(threads[0]); t++) {
            size_t length = 0;
            unsigned char *output = run_filter(filter, raster, threads[t], &length);
            assert(length == reference_length && memcmp(output, reference, length) == 0);
            free(output);
        }
        free(reference);
        unlink(raster);
        free(raster);
    }
    puts("[PASS] Filter output identical with 0, 1, 2, 4 and the default number of encoder threads");
}

static double now_seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

/* END_PAGE chunks (type 3) in a ZjStream read so far. */
static unsigned end_pages(const unsigned char *data, size_t length) {
    const unsigned char *start = memmem(data, length, "JZJZ", 4);
    if (!start) return 0;
    size_t position = (size_t)(start - data) + 4U;
    unsigned count = 0;
    while (position + 16U <= length) {
        uint32_t size = (uint32_t)data[position] << 24 | (uint32_t)data[position + 1] << 16 |
                        (uint32_t)data[position + 2] << 8 | data[position + 3];
        uint32_t type = (uint32_t)data[position + 4] << 24 | (uint32_t)data[position + 5] << 16 |
                        (uint32_t)data[position + 6] << 8 | data[position + 7];
        if (size < 16U || position + size > length) break;
        if (type == 3U) count++;
        position += size;
    }
    return count;
}

/* Drains the filter's output so it never waits on a full pipe. */
typedef struct {
    int fd;
    pthread_mutex_t lock;
    unsigned char *data;
    size_t length;
    size_t capacity;
} OutputDrain;

static void *drain_output(void *argument) {
    OutputDrain *drain = argument;
    unsigned char buffer[65536];
    ssize_t received;
    while ((received = read(drain->fd, buffer, sizeof(buffer))) != 0) {
        if (received < 0) {
            assert(errno == EINTR);
            continue;
        }
        pthread_mutex_lock(&drain->lock);
        while (drain->length + (size_t)received > drain->capacity) {
            drain->capacity = drain->capacity ? drain->capacity * 2U : 1U << 20;
            drain->data = realloc(drain->data, drain->capacity);
            assert(drain->data != NULL);
        }
        memcpy(drain->data + drain->length, buffer, (size_t)received);
        drain->length += (size_t)received;
        pthread_mutex_unlock(&drain->lock);
    }
    return NULL;
}

static unsigned drained_end_pages(OutputDrain *drain) {
    pthread_mutex_lock(&drain->lock);
    unsigned count = drain->data ? end_pages(drain->data, drain->length) : 0U;
    pthread_mutex_unlock(&drain->lock);
    return count;
}

/* Pages are sent while the raster input is still open: all but the last
 * page are written, and `early` pages must come out before the last one is
 * sent. Returns false when they did not within 20 s. */
static bool expect_streaming(const char *filter, const RasterJob *job, unsigned early, unsigned total) {
    int input[2], output[2];
    assert(pipe(input) == 0 && pipe(output) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        int null = open("/dev/null", O_WRONLY);
        if (null < 0 || dup2(input[0], STDIN_FILENO) < 0 || dup2(output[1], STDOUT_FILENO) < 0 ||
            dup2(null, STDERR_FILENO) < 0) _exit(126);
        close(input[0]);
        close(input[1]);
        close(output[0]);
        close(output[1]);
        unsetenv("DEVICE_URI");
        unsetenv("PPD");
        alarm(120);
        execl(filter, "HP_P1606dn", "1", "user", "Streaming", "1", "", (char *)NULL);
        _exit(127);
    }
    close(input[0]);
    close(output[1]);
    OutputDrain drain = {.fd = output[0]};
    assert(pthread_mutex_init(&drain.lock, NULL) == 0);
    pthread_t reader;
    assert(pthread_create(&reader, NULL, drain_output, &drain) == 0);

    cups_raster_t *raster = cupsRasterOpen(input[1], CUPS_RASTER_WRITE);
    assert(raster != NULL);
    for (unsigned p = 0; p + 1U < job->pages; p++) write_page(raster, job, p);
    double deadline = now_seconds() + 20.0;
    while (drained_end_pages(&drain) < early && now_seconds() < deadline) {
        struct timespec pause = {0, 20000000L};
        nanosleep(&pause, NULL);
    }
    bool streamed = drained_end_pages(&drain) >= early;

    write_page(raster, job, job->pages - 1U);
    cupsRasterClose(raster);
    close(input[1]);
    assert(pthread_join(reader, NULL) == 0);
    close(output[0]);
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(end_pages(drain.data, drain.length) == total);
    free(drain.data);
    pthread_mutex_destroy(&drain.lock);
    return streamed;
}

static void test_streaming(const char *filter) {
    const RasterJob simplex = {900, 1300, 3, 1, false, false, false, 595, 842};
    const RasterJob automatic = {900, 1300, 3, 1, true, false, false, 595, 842};
    assert(expect_streaming(filter, &simplex, 2, 3));
    /* Sheet 1 (pages 2, 1) before page 3; then the blank back and page 3. */
    assert(expect_streaming(filter, &automatic, 2, 4));
    puts("[PASS] Pages and automatic-duplex sheets reach the printer before the next raster page arrives");
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <rastertop1600dn>\n", argv[0]);
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    test_pool_order_and_bytes();
    test_pool_edges();
    test_default_workers();
    test_filter_thread_counts(argv[1]);
    test_streaming(argv[1]);
    puts("[PASS] Parallel encoding suite");
    return 0;
}
