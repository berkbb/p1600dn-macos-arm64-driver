/*
 * Tests for the driver's diagnostic page (src/p1600_diagnostic.c):
 *   - paper, sides and language handling;
 *   - the PDF for every PPD paper size, both languages and every sides
 *     setting: two pages of exactly the paper's size;
 *   - `p1600dn-utility page diagnostic --output` (argv[1]);
 *   - `--check-raster FILE`: the bottom edge test of a page as macOS
 *     rasterized it (used by test_diagnostic.sh): a black bar near the
 *     bottom, and nothing but the corner marks below it.
 *
 * Teşhis sayfası testleri.
 */
#include <CoreGraphics/CoreGraphics.h>
#include <assert.h>
#include <cups/raster.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "p1600_diagnostic.h"

extern char **environ;

static const char *const paper_names[] = {
    "Letter", "Legal", "A4", "Executive", "FanFoldGermanLegal", "A5", "A6", "B5", "195x270mm",
    "184x260mm", "197x273mm", "Postcard", "DoublePostcard", "Env10", "EnvMonarch", "EnvISOB5", "EnvC5", "EnvDL",
};

static char *temp_path(const char *suffix) {
    const char *directory = getenv("TMPDIR");
    char *path = NULL;
    assert(asprintf(&path, "%s/p1600dn-diagnostic-test-XXXXXX%s", directory && *directory ? directory : "/tmp",
                    suffix) > 0);
    int fd = mkstemps(path, (int)strlen(suffix));
    assert(fd >= 0);
    close(fd);
    return path;
}

/* Pages and media box of a PDF. */
static size_t pdf_pages(const char *path, CGRect *box) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    CGPDFDocumentRef document = CGPDFDocumentCreateWithURL(url);
    CFRelease(url);
    if (!document) return 0;
    size_t pages = CGPDFDocumentGetNumberOfPages(document);
    for (size_t i = 1; i <= pages; i++) {
        CGRect page = CGPDFPageGetBoxRect(CGPDFDocumentGetPage(document, i), kCGPDFMediaBox);
        if (i == 1) *box = page;
        assert(page.size.width == box->size.width && page.size.height == box->size.height);
    }
    CGPDFDocumentRelease(document);
    return pages;
}

static void test_settings(void) {
    assert(strcmp(p1600_diagnostic_language("tr"), "tr") == 0);
    assert(strcmp(p1600_diagnostic_language("tr_TR"), "tr") == 0);
    assert(strcmp(p1600_diagnostic_language("tr-TR"), "tr") == 0);
    assert(strcmp(p1600_diagnostic_language("tr_TR.UTF-8"), "tr") == 0);
    assert(strcmp(p1600_diagnostic_language("trx"), "en") == 0);
    assert(strcmp(p1600_diagnostic_language("de"), "en") == 0);
    assert(strcmp(p1600_diagnostic_language(NULL), "en") == 0);

    P1600DiagnosticInfo info;
    p1600_diagnostic_defaults(&info, "tr");
    assert(strcmp(info.language, "tr") == 0 && strcmp(info.paper, "A4") == 0);
    assert(info.width == 595 && info.height == 842 && info.sides == P1600_SIDES_LONG_EDGE);
    assert(strncmp(info.system, "macOS", 5) == 0 && info.date[0] != '\0');

    for (size_t i = 0; i < sizeof(paper_names) / sizeof(paper_names[0]); i++) {
        assert(p1600_diagnostic_set_paper(&info, paper_names[i]) == 0 && strcmp(info.paper, paper_names[i]) == 0);
        double width = info.width, height = info.height;
        /* The same paper by its size, rounded the way macOS rounds it. */
        assert(p1600_diagnostic_set_paper(&info, "A4") == 0);
        assert(p1600_diagnostic_set_paper_size(&info, (double)(int)width, (double)(int)height) == 0);
        assert(strcmp(info.paper, paper_names[i]) == 0);
    }
    assert(p1600_diagnostic_set_paper(&info, "iso_a5_148x210mm") == 0 && strcmp(info.paper, "A5") == 0);
    assert(p1600_diagnostic_set_paper(&info, "na_letter_8.5x11in") == 0 && strcmp(info.paper, "Letter") == 0);
    assert(p1600_diagnostic_set_paper(&info, "Tabloid") == -1 && strcmp(info.paper, "Letter") == 0);
    assert(p1600_diagnostic_set_paper(&info, "") == -1 && p1600_diagnostic_set_paper(&info, NULL) == -1);
    assert(p1600_diagnostic_set_paper_size(&info, 792, 1224) == -1);

    static const struct {
        const char *text;
        P1600Sides sides;
    } sides[] = {{"one", P1600_SIDES_ONE}, {"long", P1600_SIDES_LONG_EDGE}, {"short", P1600_SIDES_SHORT_EDGE},
                 {"one-sided", P1600_SIDES_ONE}, {"two-sided-long-edge", P1600_SIDES_LONG_EDGE},
                 {"two-sided-short-edge", P1600_SIDES_SHORT_EDGE}, {"LONG", P1600_SIDES_LONG_EDGE}};
    for (size_t i = 0; i < sizeof(sides) / sizeof(sides[0]); i++) {
        P1600Sides parsed = P1600_SIDES_ONE;
        assert(p1600_sides_parse(sides[i].text, &parsed) == 0 && parsed == sides[i].sides);
    }
    P1600Sides parsed = P1600_SIDES_LONG_EDGE;
    assert(p1600_sides_parse("both", &parsed) == -1 && p1600_sides_parse(NULL, &parsed) == -1);
    assert(strcmp(p1600_sides_keyword(P1600_SIDES_ONE), "one-sided") == 0);
    assert(strcmp(p1600_sides_keyword(P1600_SIDES_LONG_EDGE), "two-sided-long-edge") == 0);
    assert(strcmp(p1600_sides_keyword(P1600_SIDES_SHORT_EDGE), "two-sided-short-edge") == 0);

    /* The resolution the page names and is printed at: the queue's option,
     * else the PPD default cupsd reports (not in a destination's options). */
    assert(strcmp(p1600_diagnostic_resolution(NULL, 600, 600), "600 dpi") == 0);
    assert(strcmp(p1600_diagnostic_resolution(NULL, 1200, 600), "FastRes 1200") == 0);
    assert(strcmp(p1600_diagnostic_resolution(NULL, 0, 0), "FastRes 1200") == 0);
    assert(strcmp(p1600_diagnostic_resolution("600dpi", 1200, 600), "600 dpi") == 0);
    assert(strcmp(p1600_diagnostic_resolution("1200x600dpi", 600, 600), "FastRes 1200") == 0);
    p1600_diagnostic_defaults(&info, "en");
    assert(strcmp(info.resolution, "FastRes 1200") == 0);
    assert(strcmp(p1600_diagnostic_resolution_option(&info), "1200x600dpi") == 0);
    snprintf(info.resolution, sizeof(info.resolution), "%s", p1600_diagnostic_resolution(NULL, 600, 600));
    assert(strcmp(p1600_diagnostic_resolution_option(&info), "600dpi") == 0);

    char error[256];
    assert(p1600_diagnostic_info_for_queue("", &info, error, sizeof(error)) == -1);
    assert(p1600_diagnostic_info_for_queue("p1600dn-no-such-queue", &info, error, sizeof(error)) == -1);
    info.queue[0] = '\0';
    assert(p1600_diagnostic_print(&info, NULL, error, sizeof(error)) == -1 && strstr(error, "queue"));
    printf("    [PASS] Diagnostic Page Settings (languages, 18 papers by name/PWG/size, sides, resolution, queue errors)\n");
}

static void test_pdfs(void) {
    char *path = temp_path(".pdf");
    size_t written = 0;
    for (size_t i = 0; i < sizeof(paper_names) / sizeof(paper_names[0]); i++) {
        for (int language = 0; language < 2; language++) {
            for (int sides = P1600_SIDES_ONE; sides <= P1600_SIDES_SHORT_EDGE; sides++) {
                P1600DiagnosticInfo info;
                char error[256] = "";
                p1600_diagnostic_defaults(&info, language ? "en" : "tr");
                assert(p1600_diagnostic_set_paper(&info, paper_names[i]) == 0);
                info.sides = (P1600Sides)sides;
                snprintf(info.queue, sizeof(info.queue), "HP_LaserJet_P1606dn");
                snprintf(info.connection, sizeof(info.connection), "USB");
                assert(p1600_diagnostic_write_pdf(path, &info, error, sizeof(error)) == 0);
                CGRect box = CGRectZero;
                assert(pdf_pages(path, &box) == 2);
                assert(box.size.width > info.width - 0.01 && box.size.width < info.width + 0.01);
                assert(box.size.height > info.height - 0.01 && box.size.height < info.height + 0.01);
                written++;
            }
        }
    }
    P1600DiagnosticInfo info;
    char error[256];
    p1600_diagnostic_defaults(&info, "en");
    info.width = 100;
    assert(p1600_diagnostic_write_pdf(path, &info, error, sizeof(error)) == -1 && strstr(error, "Unsupported"));
    p1600_diagnostic_defaults(&info, "en");
    assert(p1600_diagnostic_write_pdf("/nonexistent-directory/page.pdf", &info, error, sizeof(error)) == -1);
    assert(p1600_diagnostic_write_pdf(NULL, &info, error, sizeof(error)) == -1);
    unlink(path);
    free(path);
    printf("    [PASS] Diagnostic Page PDFs (%zu: 18 papers, Turkish and English, 1-sided/long/short, 2 pages each)\n",
           written);
}

static int run(char *const argv[], const char *output_path) {
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, output_path ? output_path : "/dev/null",
                                     O_WRONLY | O_CREAT | O_TRUNC, 0600);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t pid;
    assert(posix_spawn(&pid, argv[0], &actions, NULL, argv, environ) == 0);
    posix_spawn_file_actions_destroy(&actions);
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void test_tool(const char *tool) {
    char *pdf = temp_path(".pdf");
    char *out = temp_path(".txt");
    char *const ok[] = {(char *)tool, "--media", "Letter", "--sides", "short", "--output", pdf, "page", "diagnostic",
                        "en", NULL};
    assert(run(ok, out) == 0);
    CGRect box = CGRectZero;
    assert(pdf_pages(pdf, &box) == 2 && box.size.width == 612 && box.size.height == 792);
    char *const turkish[] = {(char *)tool, "--output", pdf, "page", "diagnostic", NULL};
    assert(run(turkish, NULL) == 0 && pdf_pages(pdf, &box) == 2 && box.size.width == 595);
    char *const bad_media[] = {(char *)tool, "--media", "Tabloid", "--output", pdf, "page", "diagnostic", NULL};
    assert(run(bad_media, NULL) == 2);
    char *const bad_sides[] = {(char *)tool, "--sides", "both", "--output", pdf, "page", "diagnostic", NULL};
    assert(run(bad_sides, NULL) == 2);
    char *const no_queue[] = {(char *)tool, "--queue", "p1600dn-no-such-queue", "page", "diagnostic", NULL};
    assert(run(no_queue, NULL) == 1);
    unlink(pdf);
    unlink(out);
    free(pdf);
    free(out);
    printf("    [PASS] p1600dn-utility page diagnostic (--output, --media, --sides, errors)\n");
}

/* The bottom edge test as rasterized: the last rows with a long black run
 * are the bar; below it, in the bar's own columns, only white (the corner
 * marks lie beside it). */
static int check_raster(const char *path) {
    int fd = open(path, O_RDONLY);
    cups_raster_t *raster = fd >= 0 ? cupsRasterOpen(fd, CUPS_RASTER_READ) : NULL;
    cups_page_header2_t header;
    if (!raster || !cupsRasterReadHeader2(raster, &header) || header.cupsBitsPerPixel != 8) {
        fprintf(stderr, "check-raster: not an 8-bit raster: %s\n", path);
        return 1;
    }
    unsigned char *row = malloc(header.cupsBytesPerLine);
    long bar_end = -1, marks_below = 0;
    unsigned int bar_left = 0, bar_right = 0;
    for (unsigned int y = 0; y < header.cupsHeight; y++) {
        if (cupsRasterReadPixels(raster, row, header.cupsBytesPerLine) != header.cupsBytesPerLine) return 1;
        /* The longest dark run of the row. */
        unsigned int run_start = 0, best_start = 0, best_length = 0;
        for (unsigned int x = 0; x <= header.cupsWidth; x++) {
            if (x < header.cupsWidth && row[x] < 128) continue;
            if (x > run_start && x - run_start > best_length) {
                best_start = run_start;
                best_length = x - run_start;
            }
            run_start = x + 1;
        }
        if (best_length > header.cupsWidth / 2) {
            bar_end = y;
            bar_left = best_start;
            bar_right = best_start + best_length;
            marks_below = 0;
        } else if (bar_end >= 0) {
            unsigned int dark = 0;
            for (unsigned int x = bar_left; x < bar_right; x++) dark += row[x] < 128;
            if (dark > 0) marks_below++;
        }
    }
    free(row);
    cupsRasterClose(raster);
    close(fd);
    unsigned int bar_rows_from_bottom = bar_end >= 0 ? header.cupsHeight - 1U - (unsigned int)bar_end : 0;
    if (bar_end < 0 || marks_below != 0 || bar_rows_from_bottom > header.HWResolution[1] / 3U) {
        fprintf(stderr, "check-raster: bar ends at row %ld of %u, %ld marked rows below it\n", bar_end,
                header.cupsHeight, marks_below);
        return 1;
    }
    printf("bar ends %u rows above the bottom; nothing below it\n", bar_rows_from_bottom);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--check-raster") == 0) return check_raster(argv[2]);
    if (argc != 2) {
        fprintf(stderr, "Usage: test_diagnostic <p1600dn-utility> | --check-raster FILE\n");
        return 2;
    }
    printf("[*] Starting diagnostic page tests...\n");
    test_settings();
    test_pdfs();
    test_tool(argv[1]);
    printf("[+] Diagnostic page tests passed.\n");
    return 0;
}
