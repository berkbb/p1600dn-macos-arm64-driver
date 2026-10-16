/*
 * Throughput benchmark for rastertop1600dn (argv[1]): A4 pages at 600 dpi
 * 8-bit gray, as macOS cgpdftoraster sends them, in three kinds of content
 * (sparse text, a dense photo-like gradient, solid black) for FastRes 1200
 * and 600 dpi. Reports wall-clock and CPU time per page, and peak memory.
 *
 * Usage: bench_filter <filter> [pages per run]
 * Timing only; nothing is asserted (`make bench`).
 */
#include <cups/raster.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define A4_WIDTH 4960U
#define A4_HEIGHT 7016U

typedef enum { CONTENT_TEXT, CONTENT_PHOTO, CONTENT_BLACK } Content;

static const char *content_name(Content content) {
    return content == CONTENT_TEXT ? "text" : content == CONTENT_PHOTO ? "photo" : "black";
}

static unsigned char pixel(Content content, unsigned int x, unsigned int y) {
    switch (content) {
    case CONTENT_TEXT: {
        /* Lines of 40-pixel "words" with 90-pixel line pitch inside margins. */
        if (x < 300 || x >= A4_WIDTH - 300 || y < 400 || y >= A4_HEIGHT - 400) return 255;
        unsigned int line = (y - 400) % 90U, word = (x - 300) % 52U;
        return line < 46U && word < 40U && ((x / 7U + y / 5U) & 3U) != 0 ? 0 : 255;
    }
    case CONTENT_PHOTO: {
        uint32_t noise = x * 0x9e3779b9U ^ y * 0x85ebca6bU;
        noise ^= noise >> 15;
        return (unsigned char)(((x * 255U) / A4_WIDTH + (y * 255U) / A4_HEIGHT) / 2U + (noise & 15U));
    }
    case CONTENT_BLACK:
        break;
    }
    return 0;
}

static void write_raster(const char *path, Content content, unsigned int compression, unsigned int pages) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    cups_raster_t *raster = fd >= 0 ? cupsRasterOpen(fd, CUPS_RASTER_WRITE) : NULL;
    unsigned char *row = malloc(A4_WIDTH);
    if (!raster || !row) {
        perror("raster");
        exit(1);
    }
    for (unsigned int page = 0; page < pages; page++) {
        cups_page_header2_t header;
        memset(&header, 0, sizeof(header));
        header.HWResolution[0] = header.HWResolution[1] = 600;
        header.cupsWidth = A4_WIDTH;
        header.cupsHeight = A4_HEIGHT;
        header.cupsBitsPerColor = header.cupsBitsPerPixel = 8;
        header.cupsBytesPerLine = A4_WIDTH;
        header.cupsColorOrder = CUPS_ORDER_CHUNKED;
        header.cupsColorSpace = CUPS_CSPACE_W;
        header.cupsNumColors = 1;
        header.cupsCompression = compression;
        header.cupsInteger[0] = 1606;
        header.PageSize[0] = 595;
        header.PageSize[1] = 842;
        header.NumCopies = 1;
        header.MediaPosition = 7;
        header.cupsMediaType = 1;
        cupsRasterWriteHeader2(raster, &header);
        for (unsigned int y = 0; y < A4_HEIGHT; y++) {
            for (unsigned int x = 0; x < A4_WIDTH; x++) row[x] = pixel(content, x, y);
            cupsRasterWritePixels(raster, row, A4_WIDTH);
        }
    }
    cupsRasterClose(raster);
    close(fd);
    free(row);
}

static double seconds(struct timeval value) {
    return (double)value.tv_sec + (double)value.tv_usec / 1e6;
}

static void run(const char *filter, const char *raster, unsigned int pages, const char *label) {
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    pid_t pid = fork();
    if (pid == 0) {
        int input = open(raster, O_RDONLY), output = open("/dev/null", O_WRONLY);
        if (input < 0 || output < 0 || dup2(input, 0) < 0 || dup2(output, 1) < 0 || dup2(output, 2) < 0) _exit(126);
        unsetenv("DEVICE_URI");
        unsetenv("PPD");
        execl(filter, "bench", "1", "bench", "Benchmark", "1", "", (char *)NULL);
        _exit(127);
    }
    int status = 0;
    struct rusage usage;
    if (pid < 0 || wait4(pid, &status, 0, &usage) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "filter failed for %s\n", label);
        exit(1);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    double wall = (double)(end.tv_sec - start.tv_sec) + (double)(end.tv_nsec - start.tv_nsec) / 1e9;
    double cpu = seconds(usage.ru_utime) + seconds(usage.ru_stime);
    printf("%-18s %7.1f ms/page wall  %7.1f ms/page cpu  %6.1f pages/s  %6.1f MiB peak\n", label,
           wall * 1000.0 / pages, cpu * 1000.0 / pages, pages / wall, (double)usage.ru_maxrss / (1024.0 * 1024.0));
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Usage: %s <filter> [pages]\n", argv[0]);
        return 2;
    }
    unsigned int pages = argc == 3 ? (unsigned int)strtoul(argv[2], NULL, 10) : 4U;
    if (pages == 0) pages = 1;
    const char *directory = getenv("TMPDIR");
    char path[1024];
    snprintf(path, sizeof(path), "%s/p1600dn-bench-XXXXXX", directory && *directory ? directory : "/tmp");
    int fd = mkstemp(path);
    if (fd < 0) {
        perror("mkstemp");
        return 1;
    }
    close(fd);

    printf("A4 600 dpi 8-bit gray, %u pages per run\n", pages);
    static const Content contents[] = {CONTENT_TEXT, CONTENT_PHOTO, CONTENT_BLACK};
    for (unsigned int mode = 0; mode < 2; mode++) {
        for (size_t i = 0; i < sizeof(contents) / sizeof(contents[0]); i++) {
            char label[64];
            snprintf(label, sizeof(label), "%s %s", mode == 0 ? "FastRes" : "600dpi", content_name(contents[i]));
            write_raster(path, contents[i], mode == 0 ? 1U : 0U, pages);
            run(argv[1], path, pages, label);
        }
    }
    unlink(path);
    return 0;
}
