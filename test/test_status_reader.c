/*
 * Tests for the HP printer status support (src/p1600_status.c): job language,
 * PPD cupsIPPReason texts, the status code table and messages, page
 * progress lines, PJL USTATUS framing, the backchannel reader thread, and
 * rastertop1600dn (argv[1]) talking to a simulated printer on fd 3.
 *
 * Usage: test_status_reader <rastertop1600dn> <ppd>
 */
#include <assert.h>
#include <cups/raster.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <mach/mach.h>

#include "p1600_status.h"

static const char *ppd_path;

static double now_seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static void sleep_ms(long milliseconds) {
    struct timespec pause = {milliseconds / 1000, (milliseconds % 1000) * 1000000L};
    nanosleep(&pause, NULL);
}

static char *temp_path(const char *tag) {
    const char *directory = getenv("TMPDIR");
    char *path = NULL;
    assert(asprintf(&path, "%s/p1600dn-%s-XXXXXX", directory && *directory ? directory : "/tmp", tag) > 0);
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    return path;
}

static void write_file(const char *path, const char *text) {
    FILE *file = fopen(path, "w");
    assert(file != NULL);
    assert(fputs(text, file) >= 0);
    assert(fclose(file) == 0);
}

static char *read_stream(FILE *file) {
    assert(fflush(file) == 0);
    long length = ftell(file);
    assert(length >= 0);
    rewind(file);
    char *text = calloc((size_t)length + 1U, 1);
    assert(text != NULL);
    assert(fread(text, 1, (size_t)length, file) == (size_t)length);
    return text;
}

static void clear_language(void) {
    unsetenv("LC_ALL");
    unsetenv("LC_MESSAGES");
    unsetenv("LANG");
}

static void expect_language(const char *variable, const char *value, const char *expected) {
    clear_language();
    if (variable) setenv(variable, value, 1);
    char language[16] = "unchanged";
    p1600_status_language(language, sizeof(language));
    if (strcmp(language, expected) != 0) {
        fprintf(stderr, "%s=%s: expected \"%s\", got \"%s\"\n", variable, value, expected, language);
        assert(0);
    }
}

static void test_language(void) {
    expect_language(NULL, NULL, "");
    expect_language("LANG", "tr_TR.UTF-8", "tr_TR");
    expect_language("LANG", "tr", "tr");
    expect_language("LANG", "TR_tr", "tr_TR");
    expect_language("LANG", "pt-BR", "pt_BR");
    expect_language("LANG", "zh_TW.Big5", "zh_TW");
    expect_language("LC_MESSAGES", "de_DE@euro", "de_DE");
    expect_language("LANG", "en_US.UTF-8", "");
    expect_language("LANG", "en", "");
    expect_language("LANG", "C", "");
    expect_language("LANG", "POSIX", "");
    expect_language("LANG", "C.UTF-8", "");
    expect_language("LANG", "123", "");
    expect_language("LANG", "tr_TRX", "");
    expect_language("LANG", "", "");

    clear_language();
    setenv("LANG", "fr_FR.UTF-8", 1);
    setenv("LC_MESSAGES", "de_DE.UTF-8", 1);
    setenv("LC_ALL", "", 1);
    char language[16];
    p1600_status_language(language, sizeof(language));
    assert(strcmp(language, "de_DE") == 0);
    setenv("LC_ALL", "tr_TR.UTF-8", 1);
    p1600_status_language(language, sizeof(language));
    assert(strcmp(language, "tr_TR") == 0);

    char small[3] = "xx";
    p1600_status_language(small, sizeof(small));
    assert(small[0] == '\0');
    p1600_status_language(NULL, 16);
    p1600_status_language(language, 0);
    clear_language();
    puts("[PASS] Job language from LC_ALL, LC_MESSAGES and LANG");
}

static void test_repository_texts(void) {
    P1600StatusTexts texts;
    assert(p1600_status_texts_load(&texts, ppd_path, "tr_TR") == 0);
    assert(texts.entries[P1600_TEXT_MANUAL_DUPLEX].rank == 2);
    assert(strncmp(texts.entries[P1600_TEXT_MANUAL_DUPLEX].value, "Elle Çift Taraflı: İkinci yüzler için", strlen("Elle Çift Taraflı: İkinci yüzler için")) == 0);
    assert(strcmp(texts.entries[P1600_TEXT_PRINTER_READY].value, "Yazıcı Hazır") == 0);
    assert(strcmp(texts.entries[P1600_TEXT_PRINTING_PAGE].text, "Sayfa Yazdırılıyor") == 0);
    assert(strcmp(texts.entries[P1600_TEXT_COMPLETE].text, "Tamamlandı") == 0);
    assert(texts.entries[P1600_TEXT_DOOR_OPEN].rank == 0);
    for (int key = 0; key < P1600_TEXT_COUNT; key++) {
        if (key != P1600_TEXT_DOOR_OPEN) assert(texts.entries[key].rank == 2);
    }

    assert(p1600_status_texts_load(&texts, ppd_path, "") == 0);
    assert(texts.entries[P1600_TEXT_MANUAL_DUPLEX].rank == 1);
    assert(strcmp(texts.entries[P1600_TEXT_MANUAL_DUPLEX].value,
                  "Manual Duplex: For the second sides, move the printed sheets from the output bin to Tray 1, printed side down, then press Go on the printer.") == 0);
    assert(strcmp(texts.entries[P1600_TEXT_PRINTING_PAGE].text, "Printing Page") == 0);
    assert(strcmp(texts.entries[P1600_TEXT_PRINTING_PAGE].value, "") == 0);

    /* The project's own translations: German, Danish, Korean, Chinese;
     * Norwegian Bokmål maps to "no". */
    assert(p1600_status_texts_load(&texts, ppd_path, "de_DE") == 0);
    assert(texts.entries[P1600_TEXT_OUT_OF_PAPER].rank == 2);
    assert(strncmp(texts.entries[P1600_TEXT_MANUAL_DUPLEX].value, "Manueller Duplexdruck: ", 23) == 0);
    assert(strcmp(texts.entries[P1600_TEXT_PRINTING_PAGE].text, "Seite wird gedruckt") == 0);
    assert(p1600_status_texts_load(&texts, ppd_path, "da_DK") == 0);
    const char *danish = texts.entries[P1600_TEXT_MANUAL_DUPLEX].value;
    assert(strncmp(danish, "Manuel tosidet udskrivning: Til bagsiderne", strlen("Manuel tosidet udskrivning: Til bagsiderne")) == 0);
    assert(strstr(danish, " knappen Go på printeren.") != NULL && strchr(danish, '\n') == NULL);
    assert(strcmp(texts.entries[P1600_TEXT_MANUAL_FEED].text, "Manuel indføring") == 0);
    assert(p1600_status_texts_load(&texts, ppd_path, "ko_KR") == 0);
    assert(strcmp(texts.entries[P1600_TEXT_COMPLETE].text, "완료") == 0);
    assert(p1600_status_texts_load(&texts, ppd_path, "zh_TW") == 0);
    assert(texts.entries[P1600_TEXT_COMPLETE].rank == 3);
    char language[16];
    clear_language();
    setenv("LANG", "nb_NO.UTF-8", 1);
    p1600_status_language(language, sizeof(language));
    assert(strcmp(language, "no_NO") == 0);
    assert(p1600_status_texts_load(&texts, ppd_path, language) == 0);
    assert(texts.entries[P1600_TEXT_OUT_OF_PAPER].rank == 2);
    clear_language();
    assert(p1600_status_texts_load(&texts, ppd_path, "ja_JP") == 0);
    assert(texts.entries[P1600_TEXT_OUT_OF_PAPER].rank == 1);
    assert(p1600_status_texts_load(&texts, ppd_path, NULL) == 0);
    assert(texts.entries[P1600_TEXT_OUT_OF_PAPER].rank == 1);

    assert(p1600_status_texts_load(&texts, "/nonexistent/p1600dn.ppd", "tr") == -1);
    assert(texts.entries[P1600_TEXT_PRINTER_READY].rank == 0);
    assert(p1600_status_texts_load(&texts, NULL, "tr") == -1);
    assert(p1600_status_texts_load(NULL, ppd_path, "tr") == -1);
    puts("[PASS] Repository PPD status texts: 15 languages, English fallback");
}

static void test_synthetic_texts(void) {
    char long_value[700];
    memset(long_value, 'a', sizeof(long_value));
    /* "ç" (2 bytes) straddles the 511-byte limit. */
    memcpy(long_value + 510, "\xc3\xa7", 2);
    long_value[sizeof(long_value) - 1U] = '\0';

    char *path = temp_path("ppd");
    char *ppd = NULL;
    assert(asprintf(&ppd,
                    "*PPD-Adobe: \"4.3\"\n"
                    "*cupsIPPReason PrinterReady/Ready: \"base ready\"\n"
                    "*tr_TR.cupsIPPReason PrinterReady/Hazır TR: \"tr_TR ready\"\n"
                    "*tr.cupsIPPReason PrinterReady/Hazır: \"tr ready\"\n"
                    "*tr.cupsIPPReason CoverOpen/Kapak: \"first line\n"
                    "second\tline\"\n"
                    "*de.cupsIPPReason OutOfPaper/Papier: \"german\"\n"
                    "*xcupsIPPReason OutOfPaper/Bad: \"no dot\"\n"
                    "*trcupsIPPReason OutOfPaper/Bad: \"no dot either\"\n"
                    "*cupsIPPReason Unknown/Unknown: \"ignored\"\n"
                    "*cupsIPPReason InputJam/No quote: missing\n"
                    "*cupsIPPReason OutputJam/Long: \"%s\"\n"
                    "*cupsIPPReason EngineComm: \"no translation\"\n"
                    "*zh_TW.cupsIPPReason EngineFuser/zh: \"zh_TW value\"\n"
                    "*cupsIPPReason TonerMissing/Never closed: \"open value\n"
                    "still open\n",
                    long_value) > 0);
    write_file(path, ppd);
    free(ppd);

    P1600StatusTexts texts;
    assert(p1600_status_texts_load(&texts, path, "tr_TR") == 0);
    assert(texts.entries[P1600_TEXT_PRINTER_READY].rank == 3);
    assert(strcmp(texts.entries[P1600_TEXT_PRINTER_READY].value, "tr_TR ready") == 0);
    assert(strcmp(texts.entries[P1600_TEXT_PRINTER_READY].text, "Hazır TR") == 0);
    assert(texts.entries[P1600_TEXT_COVER_OPEN].rank == 2);
    assert(strcmp(texts.entries[P1600_TEXT_COVER_OPEN].value, "first line second line") == 0);
    assert(texts.entries[P1600_TEXT_OUT_OF_PAPER].rank == 0);
    assert(texts.entries[P1600_TEXT_INPUT_JAM].rank == 0);
    assert(texts.entries[P1600_TEXT_OUTPUT_JAM].rank == 1);
    assert(strlen(texts.entries[P1600_TEXT_OUTPUT_JAM].value) == 510U);
    assert(texts.entries[P1600_TEXT_ENGINE_COMM].rank == 1);
    assert(strcmp(texts.entries[P1600_TEXT_ENGINE_COMM].text, "") == 0);
    assert(strcmp(texts.entries[P1600_TEXT_ENGINE_COMM].value, "no translation") == 0);
    assert(texts.entries[P1600_TEXT_ENGINE_FUSER].rank == 0);
    assert(texts.entries[P1600_TEXT_TONER_MISSING].rank == 0);

    assert(p1600_status_texts_load(&texts, path, "tr") == 0);
    assert(strcmp(texts.entries[P1600_TEXT_PRINTER_READY].value, "tr ready") == 0);
    assert(p1600_status_texts_load(&texts, path, "de") == 0);
    assert(strcmp(texts.entries[P1600_TEXT_OUT_OF_PAPER].value, "german") == 0);
    assert(p1600_status_texts_load(&texts, path, "zh_TW") == 0);
    assert(texts.entries[P1600_TEXT_ENGINE_FUSER].rank == 3);
    assert(p1600_status_texts_load(&texts, path, "zh_CN") == 0);
    assert(texts.entries[P1600_TEXT_ENGINE_FUSER].rank == 0);
    unlink(path);
    free(path);
    puts("[PASS] PPD text parsing: language ranks, continuation lines, truncation, malformed entries");
}

static void test_code_table(void) {
    static const struct {
        int code;
        P1600TextKey key;
        const char *reason;
        const char *description;
    } expected[] = {
        {10001, P1600_TEXT_PRINTER_READY, NULL, NULL},
        {10003, P1600_TEXT_PRINTER_READY, NULL, NULL},
        {10023, P1600_TEXT_PRINTER_READY, NULL, NULL},
        {40021, P1600_TEXT_COVER_OPEN, "cover-open", "The cartridge door is open"},
        {40120, P1600_TEXT_MANUAL_DUPLEX, "media-needed", "Second sides next: the printed sheets will need reloading"},
        {40121, P1600_TEXT_MANUAL_DUPLEX, "media-needed", "Reload the printed sheets for their second side"},
        {40131, P1600_TEXT_MANUAL_DUPLEX, "media-needed", "Reload the printed sheets for their second side (other paper path)"},
        {40600, P1600_TEXT_TONER_MISSING, "toner-empty", "No toner cartridge"},
        {41100, P1600_TEXT_MANUAL_FEED, "media-needed", "Load paper in the manual feed slot"},
        {41150, P1600_TEXT_MANUAL_FEED, "media-needed", "Load paper in the manual feed slot"},
        {41199, P1600_TEXT_MANUAL_FEED, "media-needed", "Load paper in the manual feed slot"},
        {41002, P1600_TEXT_OUT_OF_PAPER, "media-empty", "No paper"},
        {41003, P1600_TEXT_OUT_OF_PAPER, "media-empty", "No paper"},
        {41004, P1600_TEXT_OUT_OF_PAPER, "media-empty", "No paper"},
        {42000, P1600_TEXT_INPUT_JAM, "media-jam", "Paper stuck while feeding"},
        {42001, P1600_TEXT_OUTPUT_JAM, "media-jam", "Paper stopped after feeding"},
        {42002, P1600_TEXT_INPUT_JAM, "media-jam", "Paper late at the fuser"},
        {42003, P1600_TEXT_OUTPUT_JAM, "media-jam", "Paper stopped in the fuser"},
        {42004, P1600_TEXT_INPUT_JAM, "media-jam", "Paper stuck in the input path"},
        {42005, P1600_TEXT_DOOR_OPEN, "cover-open", "A door is open after a paper jam"},
        {50003, P1600_TEXT_ENGINE_FUSER, "connecting-to-device", "Fuser error"},
        {50005, P1600_TEXT_ENGINE_SCANNER, "connecting-to-device", "Laser scanner error"},
        {50007, P1600_TEXT_ENGINE_COMM, "connecting-to-device", "Engine communication error"},
    };
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        const P1600StatusCode *entry = p1600_status_code_lookup(expected[i].code);
        assert(entry != NULL);
        assert(entry->key == expected[i].key);
        assert((entry->reason == NULL) == (expected[i].reason == NULL));
        if (entry->reason) assert(strcmp(entry->reason, expected[i].reason) == 0);
        assert((entry->description == NULL) == (expected[i].description == NULL));
        if (entry->description) assert(strcmp(entry->description, expected[i].description) == 0);
    }
    static const int unknown[] = {0, -1, 10000, 10002, 10006, 40022, 40122, 41001, 41005, 41099,
                                  41200, 41999, 42006, 50004, 50006, 99999};
    for (size_t i = 0; i < sizeof(unknown) / sizeof(unknown[0]); i++) {
        assert(p1600_status_code_lookup(unknown[i]) == NULL);
    }
    puts("[PASS] Status code table: 19 entries, ranges and unknown codes");
}

static void test_code_messages(void) {
    P1600StatusTexts turkish, english;
    assert(p1600_status_texts_load(&turkish, ppd_path, "tr_TR") == 0);
    assert(p1600_status_texts_load(&english, ppd_path, "") == 0);
    char out[1024];

    size_t length = p1600_status_format_code(&turkish, 10001, out, sizeof(out));
    const char *ready =
        "STATE: -cover-open-error,media-needed-error,toner-empty-error,media-empty-error,media-jam-error,"
        "connecting-to-device-error\nINFO: Yazıcı Hazır\n";
    assert(strcmp(out, ready) == 0 && length == strlen(ready));

    p1600_status_format_code(&english, 42000, out, sizeof(out));
    assert(strcmp(out,
                  "STATE: -cover-open-error,media-needed-error,toner-empty-error,media-empty-error,"
                  "connecting-to-device-error\nINFO: Rear Door Jam: Open the rear door, pull out the stuck sheet "
                  "and close the door again.\nSTATE: +media-jam-error\n") == 0);

    p1600_status_format_code(&turkish, 40121, out, sizeof(out));
    assert(strstr(out, "STATE: -cover-open-error,toner-empty-error,") == out);
    assert(strstr(out, "\nINFO: Elle Çift Taraflı: ") != NULL);
    assert(strstr(out, "\nSTATE: +media-needed-error\n") != NULL);

    /* DoorOpen has no PPD text: the fallback text, still a reason. */
    p1600_status_format_code(&english, 42005, out, sizeof(out));
    assert(strstr(out, "\nINFO: A door is open after a paper jam\nSTATE: +cover-open-error\n") != NULL);
    p1600_status_format_code(NULL, 50005, out, sizeof(out));
    assert(strstr(out, "\nINFO: Laser scanner error\nSTATE: +connecting-to-device-error\n") != NULL);
    p1600_status_format_code(NULL, 10023, out, sizeof(out));
    assert(strstr(out, "\nINFO: \n") != NULL && strstr(out, "STATE: +") == NULL);

    for (int code = 10000; code <= 60000; code++) {
        size_t produced = p1600_status_format_code(&turkish, code, out, sizeof(out));
        assert((produced > 0) == (p1600_status_code_lookup(code) != NULL));
        assert(produced == strlen(out));
    }

    strcpy(out, "stale");
    assert(p1600_status_format_code(&english, 10002, out, sizeof(out)) == 0 && out[0] == '\0');
    char small[40] = "stale";
    assert(p1600_status_format_code(&english, 40021, small, sizeof(small)) == 0 && small[0] == '\0');
    assert(p1600_status_format_code(&english, 40021, NULL, 10) == 0);
    assert(p1600_status_format_code(&english, 40021, out, 0) == 0);
    puts("[PASS] Status messages: STATE/INFO lines for every code, fallbacks, buffer limits");
}

static void test_progress(void) {
    unsigned int percent = 0, due = 0, last = 0;
    for (uint32_t line = 0; line < 7020U; line++) {
        if (p1600_progress_due(line, 7020U, &percent)) {
            assert((7020U - line - 1U) % 256U == 0);
            assert(percent >= last);
            last = percent;
            due++;
        }
    }
    assert(due == 28U && last == 100U);
    assert(p1600_progress_due(107U, 7020U, &percent) && percent == 1U);
    assert(p1600_progress_due(299U, 300U, &percent) && percent == 100U);
    assert(p1600_progress_due(43U, 300U, &percent) && percent == 14U);
    assert(!p1600_progress_due(0U, 300U, &percent));
    assert(p1600_progress_due(0U, 1U, NULL));
    assert(!p1600_progress_due(0U, 0U, &percent));
    assert(!p1600_progress_due(300U, 300U, &percent));

    P1600StatusTexts turkish, english, empty;
    assert(p1600_status_texts_load(&turkish, ppd_path, "tr") == 0);
    assert(p1600_status_texts_load(&english, ppd_path, "") == 0);
    memset(&empty, 0, sizeof(empty));
    char out[256];
    assert(p1600_progress_format(&turkish, 3, 50, out, sizeof(out)) > 0);
    assert(strcmp(out, "INFO: Sayfa Yazdırılıyor 3, 50% Tamamlandı...\n") == 0);
    p1600_progress_format(&english, 12, 100, out, sizeof(out));
    assert(strcmp(out, "INFO: Printing Page 12, 100% Complete...\n") == 0);
    assert(p1600_progress_format(&empty, 1, 1, out, sizeof(out)) == 0 && out[0] == '\0');
    assert(p1600_progress_format(NULL, 1, 1, out, sizeof(out)) == 0);
    assert(p1600_progress_format(&english, 1, 1, out, 10) == 0 && out[0] == '\0');
    assert(p1600_progress_format(&english, 1, 1, NULL, 10) == 0);
    english.entries[P1600_TEXT_COMPLETE].text[0] = '\0';
    assert(p1600_progress_format(&english, 1, 1, out, sizeof(out)) == 0);
    puts("[PASS] Page progress: every 256 lines, last line 100%, localized line format");
}

static void test_classify(void) {
    int code = -1;
    assert(p1600_ustatus_classify("@PJL USTATUS DEVICE\r\nCODE=40121\r\nDISPLAY=\"Manual Duplex\"\r\nONLINE=FALSE\r\n", &code) ==
           P1600_USTATUS_DEVICE && code == 40121);
    assert(p1600_ustatus_classify("\r\n@PJL INFO STATUS\r\nCODE=10001\r\nDISPLAY=\"Ready\"\r\nONLINE=TRUE\r\n", &code) ==
           P1600_USTATUS_DEVICE && code == 10001);
    assert(p1600_ustatus_classify("@PJL  USTATUS\tDEVICE\nCODE=41002", &code) == P1600_USTATUS_DEVICE && code == 41002);
    assert(p1600_ustatus_classify("@PJL USTATUS DEVICE\r\nCODE=abc", &code) == P1600_USTATUS_OTHER);
    assert(p1600_ustatus_classify("@PJL USTATUS DEVICE", &code) == P1600_USTATUS_OTHER);
    assert(p1600_ustatus_classify("@PJL USTATUS DEVICE\r\nCODE=123456", &code) == P1600_USTATUS_OTHER);
    assert(p1600_ustatus_classify("@PJL USTATUS DEVICE\r\nCODE=-5", &code) == P1600_USTATUS_OTHER);
    assert(p1600_ustatus_classify("@PJL USTATUS DEVICES\r\nCODE=10001", &code) == P1600_USTATUS_OTHER);
    assert(p1600_ustatus_classify("@PJL USTATUS PAGE\r\n1\r\n", &code) == P1600_USTATUS_PAGE);
    assert(p1600_ustatus_classify("@PJL USTATUS PAGE", &code) == P1600_USTATUS_PAGE);
    assert(p1600_ustatus_classify("@PJL USTATUS PAGES", &code) == P1600_USTATUS_OTHER);
    assert(p1600_ustatus_classify("@PJL USTATUS JOB\r\nSTART\r\nNAME=\"x\"\r\n", &code) == P1600_USTATUS_JOB_START);
    assert(p1600_ustatus_classify("@PJL USTATUS JOB\r\nEND\r\nNAME=\"x\"\r\nPAGES=2\r\n", &code) == P1600_USTATUS_JOB_END);
    assert(p1600_ustatus_classify("@PJL USTATUS JOB\r\nCANCELED\r\nNAME=\"x\"\r\n", &code) == P1600_USTATUS_JOB_CANCELED);
    assert(p1600_ustatus_classify("@PJL USTATUS JOB\r\nENDED\r\n", &code) == P1600_USTATUS_OTHER);
    assert(p1600_ustatus_classify("@PJL USTATUS JOB", &code) == P1600_USTATUS_OTHER);
    assert(p1600_ustatus_classify("@PJL USTATUS TIMED\r\n", &code) == P1600_USTATUS_OTHER);
    assert(p1600_ustatus_classify("PJL USTATUS DEVICE\r\nCODE=10001", &code) == P1600_USTATUS_OTHER);
    assert(p1600_ustatus_classify("", &code) == P1600_USTATUS_OTHER);
    assert(p1600_ustatus_classify(NULL, &code) == P1600_USTATUS_NONE);
    assert(p1600_ustatus_classify("@PJL INFO STATUS\r\nCODE=10001", NULL) == P1600_USTATUS_DEVICE);
    puts("[PASS] USTATUS grammar: DEVICE, INFO STATUS, PAGE, JOB START/END/CANCELED, malformed messages");
}

typedef struct {
    P1600UstatusKind kinds[16];
    int codes[16];
    size_t count;
} Messages;

static void collect(void *context, P1600UstatusKind kind, int code) {
    Messages *messages = context;
    assert(messages->count < 16U);
    messages->kinds[messages->count] = kind;
    messages->codes[messages->count] = code;
    messages->count++;
}

static void feed_text(P1600UstatusParser *parser, const char *data, size_t length, Messages *messages) {
    p1600_ustatus_feed(parser, (const unsigned char *)data, length, collect, messages);
}

static void test_framing(void) {
    static const char two[] = "@PJL USTATUS DEVICE\r\nCODE=10001\r\n\f@PJL USTATUS JOB\r\nEND\r\n\f";
    P1600UstatusParser parser;
    Messages messages;

    memset(&parser, 0, sizeof(parser));
    memset(&messages, 0, sizeof(messages));
    feed_text(&parser, two, sizeof(two) - 1U, &messages);
    assert(messages.count == 2 && messages.kinds[0] == P1600_USTATUS_DEVICE && messages.codes[0] == 10001 &&
           messages.kinds[1] == P1600_USTATUS_JOB_END);

    memset(&parser, 0, sizeof(parser));
    memset(&messages, 0, sizeof(messages));
    for (size_t i = 0; i < sizeof(two) - 1U; i++) feed_text(&parser, two + i, 1, &messages);
    assert(messages.count == 2 && messages.codes[0] == 10001 && messages.kinds[1] == P1600_USTATUS_JOB_END);

    /* A NUL drops the partial message before it. */
    static const char nul[] = "@PJL USTATUS DEVICE\r\nCODE=40021\0@PJL USTATUS DEVICE\r\nCODE=41002\r\n\f";
    memset(&parser, 0, sizeof(parser));
    memset(&messages, 0, sizeof(messages));
    feed_text(&parser, nul, sizeof(nul) - 1U, &messages);
    assert(messages.count == 1 && messages.codes[0] == 41002);

    /* NUL 0xAC starts a 16-byte block that may contain form feeds. */
    static const char block[] = "\0\xac\f\f\f\f\f\f\f\f\f\f\f\f\f\f@PJL INFO STATUS\r\nCODE=10023\r\n\f";
    memset(&parser, 0, sizeof(parser));
    memset(&messages, 0, sizeof(messages));
    feed_text(&parser, block, sizeof(block) - 1U, &messages);
    assert(messages.count == 1 && messages.codes[0] == 10023);
    memset(&parser, 0, sizeof(parser));
    memset(&messages, 0, sizeof(messages));
    feed_text(&parser, block, 1, &messages);
    feed_text(&parser, block + 1, 5, &messages);
    feed_text(&parser, block + 6, sizeof(block) - 7U, &messages);
    assert(messages.count == 1 && messages.codes[0] == 10023);

    /* An oversized message is dropped up to its form feed. */
    char *big = malloc(5000);
    assert(big != NULL);
    memset(big, 'x', 5000);
    memcpy(big, "@PJL USTATUS DEVICE\r\nCODE=40021\r\n", 33);
    big[4999] = '\f';
    memset(&parser, 0, sizeof(parser));
    memset(&messages, 0, sizeof(messages));
    feed_text(&parser, big, 5000, &messages);
    feed_text(&parser, "@PJL USTATUS DEVICE\r\nCODE=42001\r\n\f", 34, &messages);
    assert(messages.count == 1 && messages.codes[0] == 42001);
    free(big);

    /* An exactly full message still fits (2047 bytes + terminator). */
    char *full = malloc(P1600_USTATUS_BUFFER);
    assert(full != NULL);
    memset(full, ' ', P1600_USTATUS_BUFFER);
    memcpy(full, "@PJL USTATUS DEVICE\r\nCODE=40600\r\n", 33);
    full[P1600_USTATUS_BUFFER - 1U] = '\f';
    memset(&parser, 0, sizeof(parser));
    memset(&messages, 0, sizeof(messages));
    feed_text(&parser, full, P1600_USTATUS_BUFFER, &messages);
    assert(messages.count == 1 && messages.codes[0] == 40600);
    free(full);

    memset(&parser, 0, sizeof(parser));
    memset(&messages, 0, sizeof(messages));
    feed_text(&parser, "\f", 1, &messages);
    assert(messages.count == 1 && messages.kinds[0] == P1600_USTATUS_OTHER);
    p1600_ustatus_feed(&parser, (const unsigned char *)"\f", 1, NULL, NULL);
    p1600_ustatus_feed(NULL, (const unsigned char *)"\f", 1, collect, &messages);
    p1600_ustatus_feed(&parser, NULL, 1, collect, &messages);
    p1600_ustatus_feed(&parser, NULL, 0, collect, &messages);
    assert(messages.count == 1);
    puts("[PASS] USTATUS framing: form feeds, split reads, NUL resets, NUL 0xAC blocks, oversized messages");
}

static void test_backchannel_detection(void) {
    int channel[2];
    assert(pipe(channel) == 0);
    assert(p1600_status_backchannel_available("usb://HP/LaserJet%20Professional%20P1606dn?serial=X", channel[0]));
    assert(p1600_status_backchannel_available("USB://HP/P1606dn", channel[0]));
    assert(p1600_status_backchannel_available("socket://192.0.2.10:9100", channel[0]));
    assert(p1600_status_backchannel_available("dnssd://HP._pdl-datastream._tcp.local./", channel[0]));
    assert(p1600_status_backchannel_available("mdns://HP._pdl-datastream._tcp.local./", channel[0]));
    assert(!p1600_status_backchannel_available("ipp://192.0.2.10/ipp/print", channel[0]));
    assert(!p1600_status_backchannel_available("lpd://192.0.2.10/queue", channel[0]));
    assert(!p1600_status_backchannel_available("usbx://HP/P1606dn", channel[0]));
    assert(!p1600_status_backchannel_available("usb", channel[0]));
    assert(!p1600_status_backchannel_available(NULL, channel[0]));
    assert(!p1600_status_backchannel_available("usb://HP/P1606dn", channel[1]));
    assert(!p1600_status_backchannel_available("usb://HP/P1606dn", 4000));
    close(channel[0]);
    close(channel[1]);

    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    assert(p1600_status_backchannel_available("socket://192.0.2.10", pair[0]));
    close(pair[0]);
    close(pair[1]);

    char *path = temp_path("regular");
    int regular = open(path, O_RDONLY);
    assert(regular >= 0);
    assert(!p1600_status_backchannel_available("usb://HP/P1606dn", regular));
    close(regular);
    unlink(path);
    free(path);
    puts("[PASS] Backchannel detection: usb, socket, dnssd and mdns schemes, pipes and sockets only");
}

typedef struct {
    int fd;
    long delay_ms;
    const char *text;
} DelayedWrite;

static void *delayed_write(void *argument) {
    DelayedWrite *write_job = argument;
    sleep_ms(write_job->delay_ms);
    assert(write(write_job->fd, write_job->text, strlen(write_job->text)) == (ssize_t)strlen(write_job->text));
    return NULL;
}

static void test_reader_thread(void) {
    P1600StatusTexts texts;
    assert(p1600_status_texts_load(&texts, ppd_path, "") == 0);
    P1600StatusReader reader;

    /* Status and job end. */
    int channel[2];
    assert(pipe(channel) == 0);
    FILE *log = tmpfile();
    assert(log != NULL);
    assert(p1600_status_reader_start(&reader, channel[0], &texts, log) == 0);
    static const char printer[] =
        "@PJL INFO STATUS\r\nCODE=10001\r\nDISPLAY=\"Ready\"\r\nONLINE=TRUE\r\n\f"
        "@PJL USTATUS JOB\r\nSTART\r\nNAME=\"T\"\r\n\f"
        "@PJL USTATUS DEVICE\r\nCODE=41002\r\nDISPLAY=\"Load paper\"\r\nONLINE=FALSE\r\n\f"
        "@PJL USTATUS PAGE\r\n1\r\n\f"
        "@PJL USTATUS DEVICE\r\nCODE=10002\r\n\f"
        "@PJL USTATUS JOB\r\nEND\r\nNAME=\"T\"\r\nPAGES=1\r\n\f";
    assert(write(channel[1], printer, sizeof(printer) - 1U) == (ssize_t)(sizeof(printer) - 1U));
    double start = now_seconds();
    assert(p1600_status_reader_finish(&reader, 5.0, 5.0));
    assert(now_seconds() - start < 3.0);
    assert(!reader.running);
    char *text = read_stream(log);
    assert(strstr(text, "DEBUG: Printer status code 10001\nSTATE: -cover-open-error,") != NULL);
    assert(strstr(text, "INFO: Printer Ready\n") != NULL);
    assert(strstr(text, "INFO: Out of Paper: Refill the paper tray, then press Go on the printer.\n"
                        "STATE: +media-empty-error\n") != NULL);
    assert(strstr(text, "DEBUG: Printer started the job\n") != NULL);
    assert(strstr(text, "DEBUG: Printer reported a printed page\n") != NULL);
    assert(strstr(text, "DEBUG: Printer status code 10002\nDEBUG: Printer finished the job\n") != NULL);
    free(text);
    fclose(log);
    close(channel[0]);
    close(channel[1]);

    /* Canceled at the printer. */
    assert(pipe(channel) == 0);
    log = tmpfile();
    assert(p1600_status_reader_start(&reader, channel[0], &texts, log) == 0);
    static const char canceled[] = "@PJL USTATUS JOB\r\nCANCELED\r\n\f";
    assert(write(channel[1], canceled, sizeof(canceled) - 1U) == (ssize_t)(sizeof(canceled) - 1U));
    assert(p1600_status_reader_finish(&reader, 5.0, 5.0));
    text = read_stream(log);
    assert(strstr(text, "DEBUG: Printer canceled the job\n") != NULL);
    free(text);
    fclose(log);
    close(channel[0]);
    close(channel[1]);

    /* Job end while the filter is already waiting. */
    assert(pipe(channel) == 0);
    log = tmpfile();
    assert(p1600_status_reader_start(&reader, channel[0], &texts, log) == 0);
    DelayedWrite late = {channel[1], 300, "@PJL USTATUS DEVICE\r\nCODE=10001\r\n\f@PJL USTATUS JOB\r\nEND\r\n\f"};
    pthread_t writer;
    assert(pthread_create(&writer, NULL, delayed_write, &late) == 0);
    start = now_seconds();
    assert(p1600_status_reader_finish(&reader, 5.0, 5.0));
    double waited = now_seconds() - start;
    assert(waited >= 0.25 && waited < 3.0);
    pthread_join(writer, NULL);
    fclose(log);
    close(channel[0]);
    close(channel[1]);

    /* The backend closes the backchannel. */
    assert(pipe(channel) == 0);
    log = tmpfile();
    assert(p1600_status_reader_start(&reader, channel[0], &texts, log) == 0);
    assert(write(channel[1], "@PJL USTATUS DEVICE\r\nCODE=10001\r\n\f", 34) == 34);
    close(channel[1]);
    start = now_seconds();
    assert(!p1600_status_reader_finish(&reader, 30.0, 30.0));
    assert(now_seconds() - start < 3.0);
    fclose(log);
    close(channel[0]);

    /* A printer that never answers: quiet limit. */
    assert(pipe(channel) == 0);
    log = tmpfile();
    assert(p1600_status_reader_start(&reader, channel[0], &texts, log) == 0);
    start = now_seconds();
    assert(!p1600_status_reader_finish(&reader, 0.3, 30.0));
    waited = now_seconds() - start;
    assert(waited >= 0.29 && waited < 3.0);
    fclose(log);
    close(channel[0]);
    close(channel[1]);

    /* A printer that answered and went silent: idle limit. */
    assert(pipe(channel) == 0);
    log = tmpfile();
    assert(p1600_status_reader_start(&reader, channel[0], &texts, log) == 0);
    assert(write(channel[1], "@PJL USTATUS DEVICE\r\nCODE=40121\r\n\f", 34) == 34);
    sleep_ms(200);
    start = now_seconds();
    assert(!p1600_status_reader_finish(&reader, 30.0, 0.4));
    waited = now_seconds() - start;
    assert(waited >= 0.39 && waited < 3.0);
    text = read_stream(log);
    assert(strstr(text, "STATE: +media-needed-error\n") != NULL);
    free(text);
    fclose(log);
    close(channel[0]);
    close(channel[1]);

    /* Misuse. */
    memset(&reader, 0, sizeof(reader));
    assert(!p1600_status_reader_finish(&reader, 1.0, 1.0));
    assert(!p1600_status_reader_finish(NULL, 1.0, 1.0));
    assert(p1600_status_reader_start(NULL, 0, &texts, stderr) == -1);
    assert(p1600_status_reader_start(&reader, -1, &texts, stderr) == -1);
    assert(p1600_status_reader_start(&reader, 0, &texts, NULL) == -1);
    puts("[PASS] Reader thread: job end, cancel, late end, backchannel EOF, quiet and idle limits");
}

/* Interrupt and idle wake-ups of this process (macOS power accounting). */
static uint64_t process_wakeups(void) {
    struct task_power_info power;
    mach_msg_type_number_t count = TASK_POWER_INFO_COUNT;
    assert(task_info(mach_task_self(), TASK_POWER_INFO, (task_info_t)&power, &count) == KERN_SUCCESS);
    return power.task_interrupt_wakeups + power.task_platform_idle_wakeups;
}

/* Waiting for the printer must not wake the CPU: the reader blocks in
 * poll() and the filter on a condition variable (no polling timers). */
static void test_reader_energy(void) {
    P1600StatusTexts texts;
    assert(p1600_status_texts_load(&texts, ppd_path, "") == 0);
    int channel[2];
    assert(pipe(channel) == 0);
    FILE *log = tmpfile();
    P1600StatusReader reader;
    assert(p1600_status_reader_start(&reader, channel[0], &texts, log) == 0);
    assert(write(channel[1], "@PJL USTATUS DEVICE\r\nCODE=40121\r\n\f", 34) == 34);
    sleep_ms(100);
    uint64_t before = process_wakeups();
    double start = now_seconds();
    assert(!p1600_status_reader_finish(&reader, 30.0, 1.5));
    double waited = now_seconds() - start;
    uint64_t wakeups = process_wakeups() - before;
    assert(waited >= 1.49 && waited < 4.0);
    /* Sanitizer runtimes run their own periodic threads; only plain builds
     * can count the driver's wake-ups. */
    bool instrumented = false;
#if defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
    instrumented = true;
#endif
#endif
    if (!instrumented && wakeups > 15U) {
        fprintf(stderr, "%llu wake-ups during a %.1f s wait\n", (unsigned long long)wakeups, waited);
        assert(0 && "waiting for the printer must not poll");
    }
    fclose(log);
    close(channel[0]);
    close(channel[1]);
    printf("[PASS] Waiting for the printer is event driven (%llu wake-ups in %.1f s)\n",
           (unsigned long long)wakeups, waited);
}

/* --- rastertop1600dn with a simulated printer --------------------------- */

static void write_gray_raster(const char *path, unsigned int pages) {
    int fd = open(path, O_WRONLY | O_TRUNC);
    assert(fd >= 0);
    cups_raster_t *raster = cupsRasterOpen(fd, CUPS_RASTER_WRITE);
    assert(raster != NULL);
    for (unsigned int page = 0; page < pages; page++) {
        cups_page_header2_t header;
        memset(&header, 0, sizeof(header));
        header.HWResolution[0] = 600;
        header.HWResolution[1] = 600;
        header.cupsWidth = 640;
        header.cupsHeight = 300;
        header.cupsBitsPerColor = 8;
        header.cupsBitsPerPixel = 8;
        header.cupsBytesPerLine = 640;
        header.cupsColorOrder = CUPS_ORDER_CHUNKED;
        header.cupsColorSpace = CUPS_CSPACE_W;
        header.cupsNumColors = 1;
        header.cupsCompression = 1;
        header.cupsInteger[0] = 1606;
        header.PageSize[0] = 595;
        header.PageSize[1] = 842;
        header.NumCopies = 1;
        header.MediaPosition = 7;
        header.cupsMediaType = 1;
        assert(cupsRasterWriteHeader2(raster, &header));
        unsigned char row[640];
        for (unsigned int y = 0; y < header.cupsHeight; y++) {
            for (unsigned int x = 0; x < sizeof(row); x++) row[x] = (unsigned char)((x * 7U + y * 3U + page) & 0xffU);
            assert(cupsRasterWritePixels(raster, row, sizeof(row)) == sizeof(row));
        }
    }
    cupsRasterClose(raster);
    assert(close(fd) == 0);
}

typedef struct {
    int status;
    char *output;
    size_t output_length;
    char *diagnostics;
    double seconds;
} FilterRun;

typedef enum {
    PRINTER_ANSWERS,      /* status, then JOB END after EOJ */
    PRINTER_HANGS_UP,     /* backchannel closed after the first message */
    PRINTER_NO_CHANNEL    /* fd 3 open, but DEVICE_URI is ipp:// */
} PrinterBehaviour;

static bool contains_bytes(const char *data, size_t length, const char *needle) {
    return data && memmem(data, length, needle, strlen(needle)) != NULL;
}

static FilterRun run_with_printer(const char *filter, const char *raster, PrinterBehaviour behaviour,
                                  const char *language) {
    int backchannel[2], output[2];
    assert(pipe(backchannel) == 0 && pipe(output) == 0);
    char *diagnostics_path = temp_path("status-diagnostics");
    fflush(stdout);
    double start = now_seconds();
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        int input = open(raster, O_RDONLY);
        int errors = open(diagnostics_path, O_WRONLY | O_TRUNC);
        if (input < 0 || errors < 0 || dup2(input, STDIN_FILENO) < 0 || dup2(output[1], STDOUT_FILENO) < 0 ||
            dup2(errors, STDERR_FILENO) < 0 || dup2(backchannel[0], P1600_BACKCHANNEL_FD) < 0) _exit(126);
        for (int fd = 4; fd < 64; fd++) close(fd);
        setenv("DEVICE_URI", behaviour == PRINTER_NO_CHANNEL ? "ipp://192.0.2.10/ipp/print"
                                                             : "usb://HP/LaserJet%20Professional%20P1606dn?serial=TEST",
               1);
        setenv("PPD", ppd_path, 1);
        unsetenv("LC_ALL");
        unsetenv("LC_MESSAGES");
        if (language) setenv("LANG", language, 1);
        else unsetenv("LANG");
        alarm(60);
        execl(filter, "HP_P1606dn", "7", "test-user", "Status Test", "1", "", (char *)NULL);
        _exit(127);
    }
    close(backchannel[0]);
    close(output[1]);

    if (behaviour != PRINTER_NO_CHANNEL) {
        static const char hello[] =
            "@PJL INFO STATUS\r\nCODE=10001\r\nDISPLAY=\"Ready\"\r\nONLINE=TRUE\r\n\f"
            "@PJL USTATUS DEVICE\r\nCODE=41002\r\nDISPLAY=\"Load paper\"\r\nONLINE=FALSE\r\n\f";
        assert(write(backchannel[1], hello, sizeof(hello) - 1U) == (ssize_t)(sizeof(hello) - 1U));
        if (behaviour == PRINTER_HANGS_UP) close(backchannel[1]);
    }

    static const char eoj[] = "@PJL EOJ\r\n\033%-12345X";
    FilterRun run = {0};
    size_t capacity = 0;
    bool ended = false;
    double eoj_seen = 0.0;
    for (;;) {
        struct pollfd channel = {.fd = output[0], .events = POLLIN};
        int ready = poll(&channel, 1, 100);
        assert(ready >= 0 || errno == EINTR);
        if (ready > 0) {
            if (run.output_length + 65536U > capacity) {
                capacity = capacity ? capacity * 2U : 1U << 20;
                run.output = realloc(run.output, capacity);
                assert(run.output != NULL);
            }
            ssize_t received = read(output[0], run.output + run.output_length, 65536U);
            assert(received >= 0);
            if (received == 0) break;
            run.output_length += (size_t)received;
        }
        if (behaviour == PRINTER_ANSWERS && !ended && contains_bytes(run.output, run.output_length, eoj)) {
            if (eoj_seen == 0.0) eoj_seen = now_seconds();
            /* The filter keeps USTATUSOFF back until the printer ends the job. */
            if (now_seconds() - eoj_seen >= 0.3) {
                assert(!contains_bytes(run.output, run.output_length, "USTATUSOFF"));
                static const char done[] = "@PJL USTATUS JOB\r\nEND\r\nNAME=\"Status Test\"\r\nPAGES=2\r\n\f";
                assert(write(backchannel[1], done, sizeof(done) - 1U) == (ssize_t)(sizeof(done) - 1U));
                ended = true;
            }
        }
    }
    close(output[0]);
    if (behaviour != PRINTER_HANGS_UP) close(backchannel[1]);

    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    run.seconds = now_seconds() - start;
    run.status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    FILE *diagnostics = fopen(diagnostics_path, "r");
    assert(diagnostics != NULL);
    assert(fseek(diagnostics, 0, SEEK_END) == 0);
    run.diagnostics = read_stream(diagnostics);
    fclose(diagnostics);
    unlink(diagnostics_path);
    free(diagnostics_path);
    if (run.status != 0) fprintf(stderr, "filter status %d:\n%s\n", run.status, run.diagnostics);
    return run;
}

static void free_run(FilterRun *run) {
    free(run->output);
    free(run->diagnostics);
    memset(run, 0, sizeof(*run));
}

static void test_filter_with_printer(const char *filter) {
    char *raster = temp_path("status-raster");
    write_gray_raster(raster, 2);
    static const char trailer[] = "\033%-12345X@PJL EOJ\r\n\033%-12345X@PJL USTATUSOFF\r\n\033%-12345X";

    FilterRun answered = run_with_printer(filter, raster, PRINTER_ANSWERS, "tr_TR.UTF-8");
    assert(answered.status == 0);
    assert(answered.output_length > sizeof(trailer));
    assert(memcmp(answered.output + answered.output_length - (sizeof(trailer) - 1U), trailer, sizeof(trailer) - 1U) == 0);
    assert(strstr(answered.diagnostics, "INFO: Yazıcı Hazır\n") != NULL);
    assert(strstr(answered.diagnostics, "INFO: Kâğıt Bitti: Kâğıt tepsisini doldurun, ardından yazıcıdaki Go düğmesine basın.\n"
                                        "STATE: +media-empty-error\n") != NULL);
    assert(strstr(answered.diagnostics, "INFO: Sayfa Yazdırılıyor 1, 14% Tamamlandı...\n") != NULL);
    assert(strstr(answered.diagnostics, "INFO: Sayfa Yazdırılıyor 2, 100% Tamamlandı...\n") != NULL);
    assert(strstr(answered.diagnostics, "PAGE: 2 1\n") != NULL);
    assert(strstr(answered.diagnostics, "DEBUG: Printer finished the job\n") != NULL);

    /* The printer's status never changes the job bytes. */
    FilterRun plain = run_with_printer(filter, raster, PRINTER_NO_CHANNEL, NULL);
    assert(plain.status == 0);
    assert(plain.output_length == answered.output_length);
    assert(memcmp(plain.output, answered.output, plain.output_length) == 0);
    assert(strstr(plain.diagnostics, "Printer status code") == NULL);
    assert(strstr(plain.diagnostics, "INFO: Printing Page 1, 14% Complete...\n") != NULL);
    assert(plain.seconds < 10.0);

    FilterRun hung_up = run_with_printer(filter, raster, PRINTER_HANGS_UP, "en_US.UTF-8");
    assert(hung_up.status == 0);
    assert(hung_up.output_length == plain.output_length);
    assert(memcmp(hung_up.output, plain.output, plain.output_length) == 0);
    assert(strstr(hung_up.diagnostics, "INFO: Printer Ready\n") != NULL);
    assert(strstr(hung_up.diagnostics, "DEBUG: Printer finished the job") == NULL);
    assert(hung_up.seconds < 10.0);

    free_run(&answered);
    free_run(&plain);
    free_run(&hung_up);
    unlink(raster);
    free(raster);
    puts("[PASS] Filter with a simulated printer: USTATUSOFF after JOB END, status, progress, unchanged job bytes");
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <rastertop1600dn> <ppd>\n", argv[0]);
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    ppd_path = argv[2];
    test_language();
    test_repository_texts();
    test_synthetic_texts();
    test_code_table();
    test_code_messages();
    test_progress();
    test_classify();
    test_framing();
    test_backchannel_detection();
    test_reader_thread();
    test_reader_energy();
    test_filter_with_printer(argv[1]);
    puts("[PASS] Printer status suite");
    return 0;
}
